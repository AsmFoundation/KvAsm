#!/usr/bin/env python3
"""abstream.py — concurrent paired A/B of two kvnode binaries on the streaming
calls (Upload, Download), measured with kvbench's streaming workloads.

Usage (inside kvnode-dev):
  abstream.py OUT.jsonl BLOCKS CONFIG...
  CONFIG = WORKLOAD:VALUE_BYTES:CONNS:DEPTH[:CHUNK[:RATE]]
    WORKLOAD  upload | download | get (unary GETs of 100k preloaded keys:
              the guard that a streaming change does not cost the small-
              request path; 8 client threads share the connections, as in
              the standard suite's shape)
    CHUNK     kvbench --chunk (0 = default: 64 KiB upload messages, the
              server's 64 KiB download chunks)
    RATE      0 = closed loop (peak throughput); > 0 = open loop at RATE
              transfers/s in total, which fixes the work so that server CPU
              per byte compares variants at equal load
  e.g. upload:1048576:4:1  download:65536:8:4  upload:1048576:4:1:0:3000
Env: A_BIN, B_BIN (default /tmp/kvnode-A, /tmp/kvnode-B), KVB (kvbench),
     WARMUP (2), DURATION (6), A_ENV / B_ENV (extra VAR=VALUE for one side).

Method (as ab3.py of 2026-09-27, whose reasoning holds here): A and B run
AT THE SAME TIME, each server pinned to its own P-core vCPU (2 or 6) with its
own client vCPU set (16-23 or 24-31, E-cores); the slots swap every block, so
a faster slot cancels over an even number of blocks, and host noise that
varies in time hits both variants alike. Caveat specific to streaming: both
runs share the memory bus, and a streaming server moves several bytes of
DRAM traffic per byte transferred — so a faster B also slows A a little,
which UNDERSTATES B's gain rather than inventing one. Open-loop configs
(RATE > 0) hold the traffic equal and compare CPU per byte instead.

Per run: fresh servers (1 worker; capacity 100k; 64 MiB upload limit);
download configs first preload their keys with Upload (one server after
the other); then both kvbench clients start together. The key count gives
~64 MiB of records per server (8 keys at least), more than the 36 MB L3,
as a store serving large values would have. Per variant: value MB/s,
server CPU ns per value byte (the server's threads' schedstat time over
kvbench's measured window — the VM's scheduler clock runs ~4.4% fast
against wall time, which cancels in ratios), client CPU, p50/p99 latency
of one transfer. Summary per config: medians, per-block B/A geomean with
min-max, and the exact two-sided sign test.
"""
import json
import math
import os
import statistics
import subprocess
import sys
import time

KVB = os.environ.get("KVB", "/src/.claude/worktrees/upload-download/benchmarks/kvbench/build/kvbench")
BIN = {"A": os.environ.get("A_BIN", "/tmp/kvnode-A"), "B": os.environ.get("B_BIN", "/tmp/kvnode-B")}
EXTRA_ENV = {"A": os.environ.get("A_ENV", ""), "B": os.environ.get("B_ENV", "")}
SLOTS = [("2", "16-23", 50211), ("6", "24-31", 50212)]  # (server vCPU, client vCPUs, port)
WARMUP = float(os.environ.get("WARMUP", "2"))
DURATION = float(os.environ.get("DURATION", "6"))
RECORD_BYTES = 64 << 20


def start(variant, cpu, port, log):
    """Starts one pinned server with the variant's binary and extra settings;
    returns its Popen once it logs "listening" (exits the run otherwise)."""
    env = dict(os.environ, KVNODE_PORT=str(port), KVNODE_WORKERS="1", KVNODE_CAPACITY="100000",
               KVNODE_MAX_UPLOAD_BYTES="67108864")
    for kv in EXTRA_ENV[variant].split():
        k, v = kv.split("=", 1)
        env[k] = v
    f = open(log, "w")
    p = subprocess.Popen(["taskset", "-c", cpu, BIN[variant]], env=env, stderr=f)
    for _ in range(500):
        if 'msg="listening"' in open(log).read():
            return p
        time.sleep(0.02)
    sys.exit(f"{variant} not listening: {open(log).read()[-400:]}")


def kvb(cpus, port, cfg, keys, server_pid, extra):
    """kvbench command line for one config, pinned to the client vCPUs."""
    wl, vsize, conns, depth, chunk, rate = cfg
    # get: kvbench's default workload (read ratio 1.0) with 8 threads at most;
    # streaming: one thread per connection.
    mode = ["--read-ratio", "1"] if wl == "get" else [f"--{wl}"]
    threads = min(conns, 8) if wl == "get" else conns
    cmd = ["taskset", "-c", cpus, KVB, "--protocol", "grpc", "--port", str(port)] + mode + [
           "--value-size", str(vsize), "--keys", str(keys), "--connections", str(conns),
           "--threads", str(threads), "--depth", str(depth), "--server-pid", str(server_pid)]
    if chunk:
        cmd += ["--chunk", str(chunk)]
    if rate:
        cmd += ["--rate", str(rate)]
    return cmd + extra


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def parse_cfg(text):
    """WORKLOAD:VALUE:CONNS:DEPTH[:CHUNK[:RATE]] -> tuple."""
    parts = text.split(":")
    wl, vsize, conns, depth = parts[0], int(parts[1]), int(parts[2]), int(parts[3])
    chunk = int(parts[4]) if len(parts) > 4 else 0
    rate = float(parts[5]) if len(parts) > 5 else 0.0
    if wl not in ("upload", "download", "get"):
        sys.exit("workload must be upload, download or get: " + text)
    return wl, vsize, conns, depth, chunk, rate


def metrics(j):
    """The per-run numbers compared, from one kvbench JSON result. For get
    runs, "mbps" holds thousands of GETs per second and "ns_per_byte" the
    server CPU ns per GET (so the columns keep one meaning per config)."""
    r, c = j["result"], j["config"]
    secs = r["seconds"]
    if c["workload"] == "mixed":
        ops = r["ops_per_sec"] * secs
        lat = r["get_latency_us"]
        return {"mbps": r["ops_per_sec"] / 1e3,
                "ns_per_byte": r["server_cpu_s"] * 1e9 / ops if ops else float("nan"),
                "server_util": r["server_cpu_s"] / secs,
                "client_cpu": r["client_cpu_s"] / secs,
                "p50": lat["p50"], "p99": lat["p99"]}
    moved = r["value_bytes_per_sec"] * secs
    lat = r[c["workload"] + "_latency_us"]
    return {"mbps": r["value_bytes_per_sec"] / 1e6,
            "ns_per_byte": r["server_cpu_s"] * 1e9 / moved if moved else float("nan"),
            "server_util": r["server_cpu_s"] / secs,
            "client_cpu": r["client_cpu_s"] / secs,
            "p50": lat["p50"], "p99": lat["p99"]}


def block(blk, text, out):
    """One block of one config: A and B concurrently. Returns {variant: metrics}."""
    cfg = parse_cfg(text)
    wl, vsize = cfg[0], cfg[1]
    keys = 100000 if wl == "get" else max(8, RECORD_BYTES // max(vsize, 1))
    order = ("A", "B") if blk % 2 == 0 else ("B", "A")
    procs = {}
    for v, (cpu, ccpus, port) in zip(order, SLOTS):
        procs[v] = (start(v, cpu, port, f"/tmp/abstream.{v}.log"), ccpus, port)
    if wl == "get":
        # Unary preload (Put), 16 connections x 16 deep, one server at a time.
        for v, (p, ccpus, port) in procs.items():
            pre = subprocess.run(kvb(ccpus, port, cfg, keys, p.pid,
                                     ["--load-only", "--settle", "0.2", "--threads", "8",
                                      "--connections", "16", "--depth", "16"]),
                                 capture_output=True, text=True)
            if pre.returncode != 0:
                sys.exit(f"preload failed for {v}: {pre.stdout[-400:]}")
    if wl == "download":
        for v, (p, ccpus, port) in procs.items():
            pre = subprocess.run(kvb(ccpus, port, cfg, keys, p.pid,
                                     ["--load-only", "--settle", "0.2", "--threads", "4",
                                      "--connections", "4", "--depth", "1"]),
                                 capture_output=True, text=True)
            if pre.returncode != 0:
                sys.exit(f"preload failed for {v}: {pre.stdout[-400:]}")
    # The measured runs (a download's keys are already stored: no --load).
    runs = {v: subprocess.Popen(kvb(ccpus, port, cfg, keys, p.pid,
                                    ["--warmup", str(WARMUP), "--duration", str(DURATION)]),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            for v, (p, ccpus, port) in procs.items()}
    res = {}
    for v, r in runs.items():
        stdout, _ = r.communicate()
        try:
            j = json.loads(stdout)
        except json.JSONDecodeError:
            sys.exit(f"{v}: unreadable kvbench output: {stdout[-400:]}")
        if not j.get("result", {}).get("valid"):
            sys.exit(f"{v}: invalid run: {stdout[-600:]}")
        res[v] = metrics(j)
        out.write(json.dumps({"block": blk, "config": text, "variant": v,
                              "slot": order.index(v), "kvbench": j}) + "\n")
    for v, (p, _, _) in procs.items():
        p.terminate()
        p.wait()
        log = open(f"/tmp/abstream.{v}.log").read()
        if "pinned=0 deferred=0" not in log:
            sys.exit(f"{v}: server did not stop cleanly: {log[-400:]}")
    out.flush()
    return res, order


def main():
    path, blocks, configs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    data = {c: [] for c in configs}
    with open(path, "a") as out:
        for blk in range(blocks):
            for c in configs:
                res, order = block(blk, c, out)
                data[c].append(res)
                a, b = res["A"], res["B"]
                unit, cost = ("kGET/s", "ns/GET") if c.startswith("get:") else ("MB/s", "ns/B")
                print(f"block {blk + 1} {c:26s} A {a['mbps']:8.0f} {unit} {a['ns_per_byte']:6.3f} {cost} "
                      f"p50 {a['p50']:8.0f} | B {b['mbps']:8.0f} {unit} {b['ns_per_byte']:6.3f} {cost} "
                      f"p50 {b['p50']:8.0f} | B/A {b['mbps'] / a['mbps']:.3f} (slot0={order[0]})",
                      flush=True)
    print()
    for c in configs:
        rows = data[c]
        print(f"== {c}")
        get = c.startswith("get:")
        for key, label, better in (("mbps", "kGET/s" if get else "value MB/s", "higher"),
                                   ("ns_per_byte", "server CPU ns per GET" if get
                                    else "server CPU ns per byte", "lower"),
                                   ("server_util", "server CPU utilization", "none"),
                                   ("client_cpu", "client CPUs busy", "none"),
                                   ("p50", "p50 us per transfer", "lower"),
                                   ("p99", "p99 us per transfer", "lower")):
            a = [r["A"][key] for r in rows]
            b = [r["B"][key] for r in rows]
            ratios = [rb / ra for ra, rb in zip(a, b) if ra > 0]
            gm = math.exp(statistics.mean(math.log(x) for x in ratios))
            line = (f"   {label:26s} A {statistics.median(a):10.3f}  B {statistics.median(b):10.3f}  "
                    f"B/A geomean {gm:.3f} [{min(ratios):.3f}-{max(ratios):.3f}]")
            if better != "none":
                wins = sum((x > 1) if better == "higher" else (x < 1) for x in ratios)
                line += f"  B better in {wins}/{len(ratios)}, sign p={sign_p(wins, len(ratios)):.3f}"
            print(line)


main()
