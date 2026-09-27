#!/usr/bin/env python3
"""ab_client.py - is KvNode's 8-core result limited by the load generator?

The 2026-09-27 standard session found KvNode's 8-core cells client-bound:
KvNode's cores were only 0.52-0.70 busy while kvbench's 16 client threads
were 80-92% busy, and its server CPU per GET stayed flat from 4 to 8 cores.
That is an inference from utilization. This script tests it directly: the
same server, the same workload, only the client's CPU budget differs.

Arms (paired, one fresh server per run):

* A - the suite's standard client: kvbench on vCPUs 16-31, 16 threads;
* B - a bigger client: kvbench on vCPUs 8-31, 24 threads. vCPUs 8-15 are
  never used by the suite (servers take 0-7), so B adds 8 vCPUs without
  touching the server's.

Server: KvNode on vCPUs 0-7 with 8 workers, E1's workload (100% GET,
100-byte values, 1M preloaded keys, 64 connections x 16 in flight = 1,024
requests in flight in BOTH arms - only threads and vCPUs change).

Reading the result:

* B faster AND KvNode's utilization higher -> the standard client was the
  ceiling, and A's number is a floor of KvNode's capacity;
* B no faster -> the server (or the host) is the limit.

Caveat that biases AGAINST B: with B the VM has all 32 vCPUs busy, so any
outside load on the Windows host oversubscribes the 32 logical CPUs (and
some vCPUs share a hyperthreaded P-core or sit on E-cores). A gain for B is
therefore conservative.

Order: blocks alternate AB, BA, AB, ... so a position effect (a host
settling or warming within a block) hits both arms equally often. Paired
B/A ratios are tested with the exact two-sided sign test.

Usage (from the repository root, with the images of the standard session):
    python benchmarks/results/2026-09-28-client-ceiling/ab_client.py [blocks]
Writes ab_client.jsonl (one record per run) next to this script and prints
the summary (tee it to ab_client.out).
"""

from __future__ import annotations

import datetime
import json
import math
import pathlib
import statistics
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))  # benchmarks/: run.py and suite.py

import run  # noqa: E402  (needs the path above)
import suite  # noqa: E402

ARMS = {
    "A": {"cpus": "16-31", "threads": 16, "label": "standard client, 16 vCPUs"},
    "B": {"cpus": "8-31", "threads": 24, "label": "bigger client, 24 vCPUs"},
}
"""Client placement per arm; everything else is identical."""

SERVER_CORES = 8
SERVER_WORKERS = 8
WARMUP = 5.0
DURATION = 10.0


def sign_test(wins: int, n: int) -> float:
    """Exact two-sided sign test.

    Args:
        wins: pairs in which B beat A.
        n: pairs (ties excluded by the caller).

    Returns:
        The two-sided p-value, capped at 1.
    """
    k = min(wins, n - wins)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def one_run(arm: str, block: int, cg: run.Cgroups) -> dict:
    """Runs one fresh-server measurement with the given client arm.

    run.py's client() and Workload.kvbench_args() read suite.CLIENT_CPUS and
    suite.CLIENT_THREADS at call time, so setting them here moves only the
    client; they are restored even if the run fails, so a crash can never
    leak a non-standard placement into a later run.

    Args:
        arm: "A" or "B" (see ARMS).
        block: block number; also seeds kvbench's key sequence.
        cg: cgroup locator.

    Returns:
        A flat record: arm, block, throughput, server and client CPU
        figures, outside load, validity.

    Raises:
        subprocess.CalledProcessError: docker could not start the server.
    """
    saved = (suite.CLIENT_CPUS, suite.CLIENT_THREADS)
    suite.CLIENT_CPUS, suite.CLIENT_THREADS = ARMS[arm]["cpus"], ARMS[arm]["threads"]
    try:
        cell = {"system": "kvnode", "cores": SERVER_CORES, "threads": SERVER_WORKERS,
                "workload_obj": suite.BASE, "warmup": WARMUP, "duration": DURATION,
                "rep": block, "run_id": f"client-ceiling/{arm}/r{block}"}
        started = datetime.datetime.now(datetime.timezone.utc).isoformat()
        body = run.run_kvbench_cell(cell, cg)
    finally:
        suite.CLIENT_CPUS, suite.CLIENT_THREADS = saved
    res = body["kvbench"].get("result", {})
    ok = run.kvbench_valid(body["kvbench"])
    return {
        "arm": arm, "block": block, "started": started, "valid": ok,
        "client_cpus": ARMS[arm]["cpus"], "client_threads": ARMS[arm]["threads"],
        "ops_per_sec": res.get("ops_per_sec"), "ops": res.get("ops"),
        "seconds": res.get("seconds"),
        "server_cpu_s": res.get("server_cpu_s"), "client_cpu_s": res.get("client_cpu_s"),
        "client_cpu_util": res.get("client_cpu_util"),
        "p50_us": (res.get("latency_us") or {}).get("p50"),
        "p99_us": (res.get("latency_us") or {}).get("p99"),
        "host_load": body.get("host_load"),
        "error": None if ok else (body["kvbench"].get("error_text") or "invalid"),
    }


def derived(rec: dict, factor: float) -> dict:
    """Utilization and per-operation costs of one valid run.

    Args:
        rec: a record from one_run().
        factor: the CPU-clock calibration factor (cgroup CPU seconds per
            wall second per vCPU), by which server CPU is divided exactly as
            analyze.py does.

    Returns:
        {"mops", "srv_util", "srv_ns", "cli_util", "cli_ns", "load"}.
    """
    ops = rec["ops"]
    srv = rec["server_cpu_s"] / factor
    return {"mops": rec["ops_per_sec"] / 1e6,
            "srv_util": srv / (rec["seconds"] * SERVER_CORES),
            "srv_ns": srv / ops * 1e9,
            "cli_util": rec["client_cpu_util"],
            "cli_ns": rec["client_cpu_s"] / ops * 1e9,
            "load": (rec["host_load"] or {}).get("mean")}


def main() -> int:
    """Runs the blocks, then prints per-run figures and the paired summary.

    Returns:
        0 when every run was valid, 1 otherwise.
    """
    blocks = int(sys.argv[1]) if len(sys.argv) > 1 else 6
    cg = run.Cgroups()
    docker_rm = ["docker", "rm", "-f", run.SERVER]
    subprocess.run(docker_rm, capture_output=True)  # a stale server would hold the port
    run.HOST = run.HostLoad()
    run.HOST.start()
    rev = subprocess.run(["git", "-C", str(HERE.parents[2]), "rev-parse", "--short=12", "HEAD"],
                         capture_output=True, text=True).stdout.strip()
    image = run.image_identity(suite.SYSTEMS["kvnode"].image)["id"][:19]
    calib = run.calibrate_cpu_clock(cg)
    factor = calib["factor"]
    print(f"revision {rev}, image {image}, cpu clock factor {factor:.4f}, {blocks} blocks", flush=True)

    out = HERE / "ab_client.jsonl"
    recs = []
    with out.open("w", encoding="utf-8") as sink:
        for block in range(blocks):
            # Alternate the order so each arm runs first in half the blocks.
            for arm in ("A", "B") if block % 2 == 0 else ("B", "A"):
                rec = one_run(arm, block, cg)
                rec["cpu_clock_factor"] = factor
                sink.write(json.dumps(rec) + "\n")
                sink.flush()
                recs.append(rec)
                d = derived(rec, factor) if rec["valid"] else None
                print(f"r{block} {arm}: " + (f"{d['mops']:5.2f}M/s  server {d['srv_util']:.2f} busy "
                      f"{d['srv_ns']:4.0f} ns/op  client {d['cli_util']:.2f} busy {d['cli_ns']:5.0f} ns/op  "
                      f"outside {d['load']:.1f}" if d else f"INVALID {rec['error']}"), flush=True)

    valid = [r for r in recs if r["valid"]]
    print("\nmedians (runs):")
    for arm in ("A", "B"):
        ds = [derived(r, factor) for r in valid if r["arm"] == arm]
        med = {k: statistics.median(d[k] for d in ds) for k in ds[0]}
        print(f"  {arm} {ARMS[arm]['label']:<27} {med['mops']:5.2f}M/s  server {med['srv_util']:.2f} busy "
              f"{med['srv_ns']:4.0f} ns/op  client {med['cli_util']:.2f} busy {med['cli_ns']:5.0f} ns/op  "
              f"outside {med['load']:.1f}  (n={len(ds)})")

    # Pair A and B within each block; a block missing either arm drops out.
    ratios = {"mops": [], "srv_util": []}
    for block in range(blocks):
        pair = {r["arm"]: derived(r, factor) for r in valid if r["block"] == block}
        if len(pair) == 2:
            for k in ratios:
                ratios[k].append(pair["B"][k] / pair["A"][k])
    for k, name in (("mops", "throughput"), ("srv_util", "KvNode core utilization")):
        rs = ratios[k]
        wins = sum(x > 1 for x in rs)
        ties = sum(x == 1 for x in rs)
        print(f"B/A {name}: median {statistics.median(rs):.3f}x [{min(rs):.3f}-{max(rs):.3f}], "
              f"B higher in {wins}/{len(rs)} blocks, sign p = {sign_test(wins, len(rs) - ties):.3f}")
    return 0 if len(valid) == len(recs) else 1


if __name__ == "__main__":
    sys.exit(main())
