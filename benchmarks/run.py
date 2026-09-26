#!/usr/bin/env python3
"""run.py — executes a benchmark profile and records every run.

Usage:
    python benchmarks/run.py [--profile standard|quick] [--out DIR] [--only E1,E2]
    python benchmarks/run.py --out benchmarks/results/<existing>   # resume

Each run: start a fresh server container on vCPUs 0..N-1, run kvbench in the
server's network namespace (loopback) on vCPUs 16-31, preload every key,
warm up, measure, remove the server. Runs of one experiment execute in a
seeded random order so slow drift (thermals, background load) spreads over
all systems instead of biasing whichever ran last. Results are appended to
DIR/raw.jsonl one JSON object per run; an interrupted suite resumes where it
stopped. Requires Python 3.10+ and the docker CLI; nothing else.
"""

from __future__ import annotations

import argparse
import datetime
import json
import pathlib
import platform
import random
import re
import statistics
import subprocess
import sys
import time
from dataclasses import asdict, replace

import suite

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
SERVER = "kvb-srv"
KVBENCH_IMAGE = "kvnode-kvbench:dev"
SEED = 20260926


def sh(cmd: list[str], timeout: float | None = None, check: bool = True) -> subprocess.CompletedProcess:
    """Runs a command, capturing text output.

    Args:
        cmd: argv.
        timeout: seconds before the command is killed.
        check: raise on a non-zero exit status.

    Returns:
        The completed process.

    Raises:
        subprocess.CalledProcessError: check is set and the command failed.
        subprocess.TimeoutExpired: the command outlived the timeout.
    """
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=check,
                          encoding="utf-8", errors="replace")


def docker(*args: str, timeout: float = 180, check: bool = True) -> str:
    """Runs ``docker <args>``.

    Args:
        *args: docker arguments.
        timeout: seconds before giving up.
        check: raise on failure.

    Returns:
        Stripped standard output.

    Raises:
        subprocess.CalledProcessError: check is set and docker failed.
    """
    return sh(["docker", *args], timeout=timeout, check=check).stdout.strip()


# ---------------------------------------------------------------------------
# Environment and instruments
# ---------------------------------------------------------------------------


class Cgroups:
    """Locates a container's cgroup accounting files as seen by kvbench."""

    def __init__(self) -> None:
        """Reads the daemon's cgroup version and driver once."""
        info = docker("info", "--format", "{{.CgroupVersion}} {{.CgroupDriver}}").split()
        self.version, self.driver = info[0], info[1]

    def paths(self, cid: str) -> tuple[str, str]:
        """Accounting files of a container, under the /host-cgroup mount.

        Args:
            cid: full container id.

        Returns:
            (cpu file, memory.stat file) for kvbench's --cgroup-cpu/--cgroup-mem.
        """
        if self.version == "1":
            return (f"/host-cgroup/cpuacct/docker/{cid}/cpuacct.usage",
                    f"/host-cgroup/memory/docker/{cid}/memory.stat")
        base = (f"/host-cgroup/system.slice/docker-{cid}.scope" if self.driver == "systemd"
                else f"/host-cgroup/docker/{cid}")
        return f"{base}/cpu.stat", f"{base}/memory.stat"


def calibrate_cpu_clock(cg: Cgroups) -> dict:
    """Measures how cgroup CPU time relates to wall time on this host.

    A container busy-loops on 4 vCPUs; the cgroup's CPU counter is read over
    three 4-second windows timed by the same (guest) clock. On bare metal the
    ratio is 4.0; virtualized hosts can deviate (this WSL2 host reads about
    6.5% high). Server CPU numbers are divided by the factor.

    Args:
        cg: cgroup locator.

    Returns:
        {"factor": median ratio / 4, "samples": [...]}.
    """
    docker("rm", "-f", "kvb-burn", check=False)
    cid = docker("run", "-d", "--name", "kvb-burn", "--cpuset-cpus", "0-3", "alpine:3", "sh", "-c",
                 "for i in 1 2 3 4 5 6 7; do (while :; do :; done) & done; while :; do :; done")
    try:
        time.sleep(2)
        cpu_file, _ = cg.paths(cid)
        script = (f"f={cpu_file}; for i in 1 2 3; do a=$(cat $f); t0=$(date +%s%N); sleep 4; "
                  "b=$(cat $f); t1=$(date +%s%N); echo $a $b $t0 $t1; done")
        out = docker("run", "--rm", "-v", "/sys/fs/cgroup:/host-cgroup:ro", "kvnode-toolchain",
                     "bash", "-c", script, timeout=120)
        samples = []
        for line in out.splitlines():
            a, b, t0, t1 = map(int, line.split())
            usage = (b - a) if cg.version == "1" else (b - a) * 1000
            samples.append(usage / (t1 - t0) / 4.0)
        return {"factor": statistics.median(samples), "samples": samples}
    finally:
        docker("rm", "-f", "kvb-burn", check=False)


def image_identity(image: str) -> dict:
    """Records exactly which image ran.

    Args:
        image: image reference.

    Returns:
        {"image", "id", "digests"}; digests are empty for locally built images.
    """
    raw = docker("image", "inspect", "--format", "{{.Id}}|{{json .RepoDigests}}", image)
    image_id, digests = raw.split("|", 1)
    return {"image": image, "id": image_id, "digests": json.loads(digests)}


def environment(profile: str, experiments: list[suite.Experiment], cg: Cgroups) -> dict:
    """Captures everything needed to interpret or reproduce the results.

    Args:
        profile: profile name.
        experiments: the profile's experiments.
        cg: cgroup locator.

    Returns:
        A JSON-serializable description of host, runtime, images and plan.
    """
    info = json.loads(docker("info", "--format", "{{json .}}"))
    cpuinfo = docker("run", "--rm", "alpine:3", "cat", "/proc/cpuinfo")
    model = next((l.split(":", 1)[1].strip() for l in cpuinfo.splitlines() if l.startswith("model name")), "?")
    rev = sh(["git", "-C", str(ROOT), "rev-parse", "HEAD"], check=False).stdout.strip()
    dirty = bool(sh(["git", "-C", str(ROOT), "status", "--porcelain"], check=False).stdout.strip())
    images = {name: image_identity(s.image) for name, s in suite.SYSTEMS.items()}
    images["kvbench"] = image_identity(KVBENCH_IMAGE)
    for name, image in suite.VALIDATION_IMAGES.items():
        images[name] = image_identity(image)
    return {
        "created": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "profile": profile,
        "seed": SEED,
        "host": {"platform": platform.platform(), "python": platform.python_version(),
                 "cpu_model": model, "vcpus": info.get("NCPU"), "memory_bytes": info.get("MemTotal"),
                 "kernel": info.get("KernelVersion"), "os": info.get("OperatingSystem")},
        "docker": {"server_version": info.get("ServerVersion"), "cgroup_version": cg.version,
                   "cgroup_driver": cg.driver, "security_options": info.get("SecurityOptions")},
        "placement": {"client_cpus": suite.CLIENT_CPUS, "client_threads": suite.CLIENT_THREADS,
                      "server_cpus": "0..N-1", "network": "server network namespace (loopback)"},
        "cpu_clock": calibrate_cpu_clock(cg),
        "repository": {"revision": rev, "dirty": dirty},
        "images": images,
        "systems": {n: {"title": s.title, "image": s.image, "protocol": s.protocol,
                        "notes": s.notes, "build": s.build} for n, s in suite.SYSTEMS.items()},
        "experiments": [asdict(e) for e in experiments],
    }


# ---------------------------------------------------------------------------
# One run
# ---------------------------------------------------------------------------


def start_server(system: suite.System, cores: int, threads: int, workload: suite.Workload) -> str:
    """Starts a fresh server container pinned to vCPUs 0..cores-1.

    Args:
        system: system under test.
        cores: core budget.
        threads: thread count to configure (ignored without a knob).
        workload: workload (sizes capacity/memory settings).

    Returns:
        Full container id.

    Raises:
        subprocess.CalledProcessError: docker could not start it.
    """
    docker("rm", "-f", SERVER, check=False)
    env = [x for k, v in system.env(threads, workload).items() for x in ("-e", f"{k}={v}")]
    return docker("run", "-d", "--name", SERVER, "--cpuset-cpus", suite.server_cpus(cores),
                  *system.docker_flags, *env, system.image, *system.args(threads, workload))


def client(image: str, *args: str, timeout: float = 600) -> subprocess.CompletedProcess:
    """Runs a client container in the server's network namespace on client vCPUs.

    Args:
        image: client image.
        *args: container arguments.
        timeout: seconds before it is killed.

    Returns:
        The completed process (not checked).
    """
    return sh(["docker", "run", "--rm", "--network", f"container:{SERVER}", "--cpuset-cpus",
               suite.CLIENT_CPUS, "-v", "/sys/fs/cgroup:/host-cgroup:ro", image, *args],
              timeout=timeout, check=False)


def kvbench(system: suite.System, cid: str, cg: Cgroups, args: list[str], timeout: float) -> dict:
    """Runs kvbench against the current server.

    Args:
        system: system under test (protocol, port).
        cid: server container id (for its cgroup files).
        cg: cgroup locator.
        args: workload and phase arguments.
        timeout: seconds.

    Returns:
        kvbench's JSON result, or {"valid": False, "error_text": ...} when it
        produced none.
    """
    cpu_file, mem_file = cg.paths(cid)
    p = client(KVBENCH_IMAGE, "--protocol", system.protocol, "--port", str(system.port),
               "--cgroup-cpu", cpu_file, "--cgroup-mem", mem_file, "--wait-ready", "90", *args,
               timeout=timeout)
    lines = [l for l in p.stdout.splitlines() if l.startswith("{")]
    if not lines:
        return {"valid": False, "error_text": f"kvbench exit {p.returncode}: {(p.stderr or p.stdout)[-400:]}"}
    return json.loads(lines[-1])


def kvbench_valid(kv: dict) -> bool:
    """Whether a kvbench result counts: no errors, misses or wrong values in
    the preload (when there was one) and in the measured phase.

    Args:
        kv: kvbench JSON (validity lives inside "preload" and "result").

    Returns:
        True for a usable run.
    """
    return bool(kv.get("preload", {}).get("valid", True) and kv.get("result", {}).get("valid", False))


def server_log_tail() -> str:
    """Last lines of the server's log, kept with failed runs for diagnosis.

    Returns:
        Up to 20 log lines.
    """
    return docker("logs", "--tail", "20", SERVER, check=False, timeout=30)[-2000:]


def run_kvbench_cell(cell: dict, cg: Cgroups) -> dict:
    """Executes one kvbench run: fresh server, preload, warm-up, measure.

    Args:
        cell: run description (system, cores, threads, workload, phases).
        cg: cgroup locator.

    Returns:
        The run record's result part.
    """
    system = suite.SYSTEMS[cell["system"]]
    wl: suite.Workload = cell["workload_obj"]
    cid = start_server(system, cell["cores"], cell["threads"], wl)
    try:
        args = wl.kvbench_args() + ["--load", "--warmup", str(cell["warmup"]),
                                    "--duration", str(cell["duration"]), "--seed", str(cell["rep"] + 1),
                                    "--label", cell["run_id"]]
        budget = 120 + cell["warmup"] + cell["duration"] + wl.keys / 20_000
        result = kvbench(system, cid, cg, args, timeout=budget)
        out = {"kvbench": result}
        if not kvbench_valid(result):
            out["server_log"] = server_log_tail()
        return out
    finally:
        docker("rm", "-f", SERVER, check=False)


# ---------------------------------------------------------------------------
# Validation experiment: kvbench vs the ecosystems' standard tools
# ---------------------------------------------------------------------------

H2_GET_ONE_KEY = r"\x00\x00\x00\x00\x12\x0a\x10key:000000000000"
"""gRPC-framed GetRequest{key: "key:000000000000"} (kvbench's key 0)."""


def run_validation_cell(cell: dict, cg: Cgroups) -> dict:
    """Runs one tool configuration of the validation experiment.

    Tools and workloads (4 cores, 64 connections x 16 in flight, 16 threads):
      redis/kvbench-random, redis/redis-benchmark-random (same preloaded
      keys: redis-benchmark's -r keys are kvbench's key format),
      redis/memtier-random (memtier preloads its own keys),
      redis/kvbench-onekey vs redis/redis-benchmark-onekey, and
      kvnode/kvbench-onekey vs kvnode/h2load-onekey (h2load cannot vary the
      request body, so the comparison uses a single key for both).

    Args:
        cell: run description; cell["tool"] selects the configuration.
        cg: cgroup locator.

    Returns:
        {"tool_ops_per_sec": float | None, "raw": tool output excerpt, ...}.
    """
    system = suite.SYSTEMS[cell["system"]]
    wl = suite.BASE
    one_key = cell["tool"].endswith("onekey")
    keys = 1 if one_key else wl.keys
    cid = start_server(system, cell["cores"], cell["threads"], replace(wl, keys=keys))
    try:
        base_args = replace(wl, keys=keys).kvbench_args()
        tool = cell["tool"].split("-")[0]
        if tool == "kvbench":
            r = kvbench(system, cid, cg, base_args + ["--load", "--warmup", str(cell["warmup"]),
                                                      "--duration", str(cell["duration"])], 600)
            ok = r.get("result", {}).get("valid", False)
            return {"tool_ops_per_sec": r.get("result", {}).get("ops_per_sec") if ok else None, "kvbench": r}
        if tool == "memtier":
            common = ["-s", "127.0.0.1", "-p", "6379", "--protocol=redis", "-t", "16", "-c", "4",
                      "--data-size=100", "--key-minimum=1", "--key-maximum=1000000", "--hide-histogram"]
            load = client(suite.VALIDATION_IMAGES["memtier"], *common, "--ratio=1:0", "--key-pattern=P:P",
                          "--requests=allkeys", "--pipeline=16")
            if load.returncode != 0:
                return {"tool_ops_per_sec": None, "raw": load.stderr[-800:]}
            p = client(suite.VALIDATION_IMAGES["memtier"], *common, "--ratio=0:1", "--key-pattern=R:R",
                       "--pipeline=16", "--distinct-client-seed", f"--test-time={int(cell['duration'])}")
            m = re.search(r"^Totals\s+([\d.]+)", p.stdout, re.M)
            return {"tool_ops_per_sec": float(m.group(1)) if m else None, "raw": p.stdout[-1500:]}
        # redis-benchmark and h2load address kvbench-preloaded keys
        load = kvbench(system, cid, cg, base_args + ["--load-only"], 600)
        if not load.get("preload", {}).get("valid", False):
            return {"tool_ops_per_sec": None, "raw": json.dumps(load)[-800:]}
        if tool == "redis":  # redis-benchmark
            rand = [] if one_key else ["-r", str(keys)]
            if one_key:  # without -r the key is literally "key:__rand_int__"
                client(system.image, "redis-benchmark", "-h", "127.0.0.1", "-p", "6379", "-t", "set",
                       "-n", "1", "-d", "100", "-q")
            p = client(system.image, "redis-benchmark", "-h", "127.0.0.1", "-p", "6379", "-t", "get",
                       *rand, "-d", "100", "-P", "16", "-c", "64", "--threads", "16", "-n", "30000000",
                       "--csv", "-q")
            m = re.search(r'^"GET","([\d.]+)"', p.stdout, re.M)
            return {"tool_ops_per_sec": float(m.group(1)) if m else None, "raw": p.stdout[-800:]}
        # h2load against KvNode, single key
        script = (f"printf '{H2_GET_ONE_KEY}' > /tmp/body && h2load -n 20000000 -c 64 -m 16 -t 16 "
                  "-d /tmp/body -H 'content-type: application/grpc' -H 'te: trailers' "
                  "http://127.0.0.1:50051/kvnode.v1.KvNode/Get")
        p = client(suite.VALIDATION_IMAGES["h2load"], "bash", "-c", script, timeout=900)
        m = re.search(r"finished in [\d.]+m?s, ([\d.]+) req/s", p.stdout)
        failed = re.search(r"(\d+) failed", p.stdout)
        ok = m and failed and failed.group(1) == "0"
        return {"tool_ops_per_sec": float(m.group(1)) if ok else None, "raw": p.stdout[-1500:]}
    finally:
        docker("rm", "-f", SERVER, check=False)


VALIDATION_TOOLS = {
    "redis": ["kvbench-random", "redis-benchmark-random", "memtier-random", "kvbench-onekey",
              "redis-benchmark-onekey"],
    "kvnode": ["kvbench-onekey", "h2load-onekey"],
}

# ---------------------------------------------------------------------------
# Planning
# ---------------------------------------------------------------------------


def load_records(path: pathlib.Path) -> list[dict]:
    """Reads raw.jsonl.

    Args:
        path: results file (may not exist yet).

    Returns:
        All records, in file order.
    """
    if not path.exists():
        return []
    return [json.loads(l) for l in path.read_text(encoding="utf-8").splitlines() if l.strip()]


def ops_of(rec: dict) -> float | None:
    """Throughput of a successful kvbench record.

    Args:
        rec: run record.

    Returns:
        ops/s, or None for invalid or failed runs.
    """
    r = rec.get("kvbench", {}).get("result", {})
    return r.get("ops_per_sec") if rec.get("status") == "ok" and r.get("valid") else None


def tuned_threads(records: list[dict]) -> dict[str, dict[int, int]]:
    """Best thread count per (system, cores) from E0 (median throughput).

    Args:
        records: all records so far.

    Returns:
        {system: {cores: threads}}; missing entries default to cores.
    """
    by: dict[tuple[str, int, int], list[float]] = {}
    for rec in records:
        if rec.get("experiment") == "E0" and (ops := ops_of(rec)) is not None:
            by.setdefault((rec["system"], rec["cores"], rec["threads"]), []).append(ops)
    best: dict[str, dict[int, tuple[int, float]]] = {}
    for (system, cores, threads), vals in by.items():
        med = statistics.median(vals)
        cur = best.setdefault(system, {}).get(cores)
        if cur is None or med > cur[1]:
            best[system][cores] = (threads, med)
    return {s: {c: t for c, (t, _) in d.items()} for s, d in best.items()}


def e4_rates(records: list[dict], cores: int) -> tuple[list[float], int]:
    """E4 offered loads from the slowest system's E1 median.

    Uses E1 at E4's own core budget, or the largest E1 budget below it when
    that one was not run (the quick profile stops at 2 cores).

    Args:
        records: all records so far.
        cores: E4's core budget.

    Returns:
        (absolute rates in ops/s for suite.E4_LOAD_FRACTIONS, key count of
        the reference E1 runs — E4 must use the same workload).

    Raises:
        RuntimeError: E1 has no valid results at or below that budget.
    """
    per: dict[int, dict[str, list[float]]] = {}
    keys = {}
    for rec in records:
        if rec.get("experiment") == "E1" and (ops := ops_of(rec)) is not None:
            per.setdefault(rec["cores"], {}).setdefault(rec["system"], []).append(ops)
            keys[rec["cores"]] = rec["workload"]["keys"]
    usable = [c for c in per if c <= cores]
    if not usable:
        raise RuntimeError(f"E4 needs E1 results at {cores} cores or fewer")
    ref = max(usable)
    slowest = min(statistics.median(v) for v in per[ref].values())
    return [round(slowest * f, -3) for f in suite.E4_LOAD_FRACTIONS], keys[ref]


def plan(exp: suite.Experiment, records: list[dict]) -> list[dict]:
    """Expands an experiment into run cells, in blocked random order.

    Repetition r of every cell forms block r; blocks run one after another,
    each in its own seeded random order. Every system is therefore measured
    once per block, minutes apart from all the others. On a host whose speed
    drifts (a virtualized hybrid CPU moves vCPUs between P- and E-cores),
    comparing systems *within* a block cancels the drift, which fully random
    ordering would only average out.

    Args:
        exp: experiment.
        records: previous records (for tuning and E4 rates).

    Returns:
        Cells with a unique run_id each; cell["rep"] is its block.
    """
    tuning = tuned_threads(records)
    workloads = exp.workloads
    if exp.id == "E4":
        # E1's workload, open loop: the rates were derived from exactly it.
        rates, keys = e4_rates(records, exp.cores[0])
        workloads = tuple(replace(suite.BASE, keys=keys, rate=r, depth=64) for r in rates)
    cells = []
    for system in exp.systems:
        for cores in exp.cores:
            if exp.threads == "sweep":
                variants = sorted({cores, max(1, cores - 1)})
            else:
                variants = [tuning.get(system, {}).get(cores, cores) if suite.SYSTEMS[system].thread_knob else cores]
            tools = VALIDATION_TOOLS[system] if exp.kind == "validation" else [None]
            for threads in variants:
                for wl in workloads:
                    for tool in tools:
                        for rep in range(exp.reps):
                            tag = f"-{tool}" if tool else ""
                            cells.append({
                                "run_id": f"{exp.id}/{system}/c{cores}/t{threads}/{wl.key()}{tag}/r{rep}",
                                "experiment": exp.id, "system": system, "cores": cores,
                                "threads": threads, "workload": asdict(wl), "workload_key": wl.key(),
                                "workload_obj": wl, "tool": tool, "rep": rep,
                                "warmup": exp.warmup, "duration": exp.duration, "kind": exp.kind})
    rng = random.Random(f"{SEED}-{exp.id}")
    ordered: list[dict] = []
    for rep in range(exp.reps):
        block = [c for c in cells if c["rep"] == rep]
        rng.shuffle(block)
        ordered += block
    return ordered


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    """Entry point.

    Returns:
        Process exit code: 0 when every planned run finished (valid or not).
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default="standard", choices=sorted(suite.PROFILES))
    ap.add_argument("--out", help="results directory (existing = resume)")
    ap.add_argument("--only", help="comma-separated experiment ids")
    args = ap.parse_args()

    experiments = suite.PROFILES[args.profile]
    if args.only:
        wanted = set(args.only.split(","))
        experiments = [e for e in experiments if e.id in wanted]
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M")
    out = pathlib.Path(args.out) if args.out else HERE / "results" / f"{stamp}-{args.profile}"
    out.mkdir(parents=True, exist_ok=True)
    raw = out / "raw.jsonl"
    cg = Cgroups()
    docker("rm", "-f", SERVER, check=False)
    if not (out / "environment.json").exists():
        print("recording environment and calibrating the CPU clock ...", flush=True)
        env = environment(args.profile, experiments, cg)
        (out / "environment.json").write_text(json.dumps(env, indent=2), encoding="utf-8")
        print(f"  cpu clock factor {env['cpu_clock']['factor']:.4f}", flush=True)

    records = load_records(raw)
    done = {r["run_id"] for r in records if r.get("status") in ("ok", "invalid")}
    with raw.open("a", encoding="utf-8") as sink:
        for exp in experiments:
            try:
                cells = [c for c in plan(exp, records) if c["run_id"] not in done]
            except RuntimeError as e:  # e.g. E4 without E1 results: skip, keep going
                print(f"== {exp.id} skipped: {e}", flush=True)
                continue
            if not cells:
                continue
            per_run = exp.warmup + exp.duration + 12
            print(f"== {exp.id} {exp.title}: {len(cells)} runs, ~{len(cells) * per_run / 60:.0f} min", flush=True)
            t_exp = time.time()
            for i, cell in enumerate(cells, 1):
                t0 = time.time()
                rec = {k: v for k, v in cell.items() if k != "workload_obj"}
                rec["started"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                try:
                    body = (run_validation_cell if cell["kind"] == "validation" else run_kvbench_cell)(cell, cg)
                    rec.update(body)
                    if cell["kind"] == "validation":
                        rec["status"] = "ok" if body.get("tool_ops_per_sec") else "invalid"
                    else:
                        rec["status"] = "ok" if kvbench_valid(body["kvbench"]) else "invalid"
                except Exception as e:  # keep the suite going; the record says why
                    rec["status"] = "failed"
                    rec["error"] = f"{type(e).__name__}: {e}"
                    docker("rm", "-f", SERVER, check=False)
                rec["seconds"] = round(time.time() - t0, 1)
                sink.write(json.dumps(rec) + "\n")
                sink.flush()
                records.append(rec)
                ops = rec.get("tool_ops_per_sec") or ops_of(rec)
                eta = (time.time() - t_exp) / i * (len(cells) - i) / 60
                print(f"[{exp.id} {i}/{len(cells)}] {cell['system']:<9} c{cell['cores']} t{cell['threads']} "
                      f"{cell['workload_key']:<36} {cell.get('tool') or '':<24} "
                      f"{(f'{ops:,.0f}/s' if ops else rec['status']):>14}  eta {eta:.0f}m", flush=True)
    print(f"results: {raw}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
