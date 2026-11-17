#!/usr/bin/env python3
"""run.py — executes a benchmark profile and records every run.

Usage:
    python benchmarks/run.py [--profile standard|quick|writes] [--out DIR] [--only E1,E2]
                             [--systems kvasm] [--e4-rates 418000,836000,1254000]
    python benchmarks/run.py --out benchmarks/results/<existing>   # resume

--systems re-measures some systems alone (e.g. KvAsm after a change) to set
against an earlier session's results for the others; such comparisons are
unpaired across sessions, so the host must be as quiet as it was then.
--e4-rates gives E4 explicit offered loads, e.g. that earlier session's, which
a subset could not derive (E4 normally takes 25/50/75% of the slowest
system's E1 median, and the slowest system may not be in the subset).
--control-image adds a second KvAsm, system "kvasm-control" running that
image, to every experiment that has kvasm, in the same blocks: run the image
an earlier session measured, and each cell's kvasm ÷ kvasm-control ratio is
paired within this session while the control's own numbers show how far the
host moved since then (2026-11-15: the 2026-09-27 image itself ran 1.5x
slower per GET than it did that day).

Each run: start a fresh server container on vCPUs 0..N-1, run kvbench in the
server's network namespace (loopback) on vCPUs 16-31, preload every key,
warm up, measure, remove the server. Runs of one experiment execute in
blocks: repetition r of every cell forms block r, in a seeded random order,
so slow drift hits every system of a block alike and cancels in paired
comparisons. Results are appended to DIR/raw.jsonl one JSON object per run;
an interrupted suite resumes where it stopped.

With psutil on a Docker Desktop host, CPU used by other programs outside the
container VM is sampled every second and recorded per run (host_load), and a
busy host is reported at startup: such load competes for the same cores and
cannot be seen from inside the containers.

Requires Python 3.10+ and the docker CLI; psutil is optional.
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
import threading
import time
from collections import deque
from dataclasses import asdict, replace

import suite

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
SERVER = "kvb-srv"
KVBENCH_IMAGE = "kvasm-kvbench:dev"
CONTROL = "kvasm-control"
"""System name of --control-image's KvAsm (a variant; never in suite.ALL)."""
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
        out = docker("run", "--rm", "-v", "/sys/fs/cgroup:/host-cgroup:ro", "kvasm-toolchain",
                     "bash", "-c", script, timeout=120)
        samples = []
        for line in out.splitlines():
            a, b, t0, t1 = map(int, line.split())
            usage = (b - a) if cg.version == "1" else (b - a) * 1000
            samples.append(usage / (t1 - t0) / 4.0)
        return {"factor": statistics.median(samples), "samples": samples}
    finally:
        docker("rm", "-f", "kvb-burn", check=False)


HOST_VM_PROCESSES = ("vmmemwsl", "vmmem")
"""Host process names of the VM that runs the containers (Docker Desktop on
Windows: WSL2 → vmmemWSL, Hyper-V backend → vmmem)."""

HOST_BUSY_WARN = 2.0
"""Logical CPUs of outside load above which the host counts as busy (a quiet
desktop idles well below 1). run.py warns at startup; the analysis flags such
runs and adds a sensitivity check restricted to quiet runs, but does not
exclude busy runs outright. Decided before the 2026-09-26 monitored run,
which had to run on a shared machine: a hard cut there would have dropped
nearly every run."""


class HostLoad:
    """Samples, once per period, how much host CPU is used *outside* the VM
    that runs the containers.

    Why it exists: cgroups only account for the containers, so on a shared
    workstation every other program competing for the same physical cores
    shows up as unexplained noise. On 2026-09-26 this testbed had about 23 of
    its 32 logical CPUs busy with unrelated work (optimization jobs, a game,
    an emulator, a runaway process), which produced 3× swings between
    identical runs. Recording the external load of every run makes such
    contamination visible, and lets the analysis flag the affected runs.

    It works where psutil is importable and the VM is a host process psutil
    can read (Docker Desktop on Windows). Elsewhere it is disabled and
    reports None — it never guesses. Linux hosts running containers
    natively would need cgroup-aware accounting instead; not implemented.

    Thread-safety: one daemon thread appends samples; readers copy them
    under the lock, because iterating a deque that is being appended to
    raises RuntimeError.
    """

    def __init__(self, period: float = 1.0) -> None:
        """Finds the VM process; does not start sampling.

        Args:
            period: seconds between samples.
        """
        self.period = period
        self.samples: deque[tuple[float, float, float]] = deque(maxlen=20_000)
        self.lock = threading.Lock()
        self.psutil = None
        self.vm: list = []
        try:
            import psutil  # optional dependency: absent → monitoring disabled

            self.vm = [p for p in psutil.process_iter(["name"])
                       if (p.info["name"] or "").lower() in HOST_VM_PROCESSES]
            if self.vm:
                self.psutil = psutil
                self.logical_cpus = psutil.cpu_count()
        except Exception:  # psutil missing or process table unreadable
            self.psutil = None

    @property
    def enabled(self) -> bool:
        """True when samples are being (or can be) taken."""
        return self.psutil is not None

    def _sample(self) -> tuple[float, float, float] | None:
        """One cumulative reading.

        Returns:
            (monotonic seconds, host busy CPU-seconds, VM CPU-seconds), or
            None if the VM process vanished (Docker Desktop restarted).
        """
        try:
            t = self.psutil.cpu_times()
            # On Windows psutil's "system" already contains interrupt and DPC
            # time; adding those fields again would double count them.
            vm = 0.0
            for p in self.vm:
                ct = p.cpu_times()
                vm += ct.user + ct.system
            return time.monotonic(), t.user + t.system, vm
        except Exception:
            return None

    def _loop(self) -> None:
        """Sampler thread body: append a reading every period, forever."""
        while True:
            s = self._sample()
            if s is not None:
                with self.lock:
                    self.samples.append(s)
            time.sleep(self.period)

    def start(self) -> None:
        """Starts the daemon sampler thread (no-op when disabled)."""
        if self.enabled:
            threading.Thread(target=self._loop, name="host-load", daemon=True).start()

    def external(self, t0: float, t1: float) -> dict | None:
        """External CPU load between two monotonic instants.

        Args:
            t0: window start (time.monotonic()).
            t1: window end.

        Returns:
            {"mean": cores, "max": cores, "intervals": n} where one core = one
            logical CPU fully busy outside the VM; None when disabled or when
            fewer than two samples fall inside the window.
        """
        if not self.enabled:
            return None
        with self.lock:
            pts = [s for s in self.samples if t0 <= s[0] <= t1]
        loads = [((b[1] - a[1]) - (b[2] - a[2])) / (b[0] - a[0])
                 for a, b in zip(pts, pts[1:]) if b[0] > a[0]]
        if not loads:
            return None
        span = pts[-1][0] - pts[0][0]
        mean = ((pts[-1][1] - pts[0][1]) - (pts[-1][2] - pts[0][2])) / span
        return {"mean": round(mean, 2), "max": round(max(loads), 2), "intervals": len(loads)}


HOST: HostLoad | None = None
"""Process-wide host-load sampler, created and started by main() (not at
import, so importing this module for its helpers has no side effects)."""


def host_load(t0: float, t1: float) -> dict | None:
    """External host load over a window, if monitoring is active.

    Args:
        t0: window start (time.monotonic()).
        t1: window end.

    Returns:
        HostLoad.external()'s result, or None without a sampler.
    """
    return HOST.external(t0, t1) if HOST else None


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
        # kvbench's measured window is its last `duration` seconds; docker
        # adds up to ~1 s of teardown after it, hence the 1.5 s allowance.
        t_end = time.monotonic()
        out = {"kvbench": result, "host_load": host_load(t_end - cell["duration"] - 1.5, t_end)}
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
      kvasm/kvbench-onekey vs kvasm/h2load-onekey (h2load cannot vary the
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
        # h2load against KvAsm, single key
        script = (f"printf '{H2_GET_ONE_KEY}' > /tmp/body && h2load -n 20000000 -c 64 -m 16 -t 16 "
                  "-d /tmp/body -H 'content-type: application/grpc' -H 'te: trailers' "
                  "http://127.0.0.1:50051/kvasm.v1.KvAsm/Get")
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
    "kvasm": ["kvbench-onekey", "h2load-onekey"],
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


def plan(exp: suite.Experiment, records: list[dict], e4_override: list[float] | None = None) -> list[dict]:
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
        e4_override: explicit E4 offered loads in ops/s (``--e4-rates``),
            used instead of rates derived from this session's E1. They apply
            to E1's standard workload (suite.BASE's key count), which is
            what derived rates assume as well.

    Returns:
        Cells with a unique run_id each; cell["rep"] is its block.

    Raises:
        RuntimeError: E4 without an override and without E1 results to
            derive its rates from (see e4_rates); main skips the experiment.
    """
    tuning = tuned_threads(records)
    workloads = exp.workloads
    if exp.id == "E4":
        # E1's workload, open loop: the rates were derived from exactly it —
        # or given, to meet an earlier session's loads (--e4-rates).
        if e4_override:
            rates, keys = e4_override, suite.BASE.keys
        else:
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
    ap.add_argument("--systems", help="comma-separated systems to run, of each experiment's own "
                    "(default: all); e.g. kvasm to re-measure KvAsm alone")
    ap.add_argument("--e4-rates", help="comma-separated E4 offered loads in ops/s, instead of "
                    "25/50/75%% of the slowest system's E1 median (e.g. an earlier session's)")
    ap.add_argument("--control-image", help="also run this KvAsm image as system 'kvasm-control' "
                    "wherever kvasm runs, in the same blocks (e.g. the image an earlier session measured)")
    args = ap.parse_args()

    experiments = suite.PROFILES[args.profile]
    if args.only:
        wanted = set(args.only.split(","))
        experiments = [e for e in experiments if e.id in wanted]
    if args.systems:
        # Validate names first: a typo would otherwise silently drop every
        # experiment and "finish" a session that measured nothing.
        keep = set(args.systems.split(","))
        unknown = keep - set(suite.SYSTEMS)
        if unknown:
            ap.error(f"unknown systems: {', '.join(sorted(unknown))} (known: {', '.join(suite.SYSTEMS)})")
        experiments = [replace(e, systems=tuple(s for s in e.systems if s in keep)) for e in experiments]
        experiments = [e for e in experiments if e.systems]  # e.g. no rival of a one-system subset left
    e4_override = None
    if args.e4_rates:
        try:
            e4_override = [float(r) for r in args.e4_rates.split(",")]
        except ValueError:
            ap.error("--e4-rates takes comma-separated numbers (ops/s)")
        if any(r <= 0 for r in e4_override):
            ap.error("--e4-rates must be positive")
    if args.control_image:
        # Identical to the kvasm system in every setting but the image, so a
        # cell's kvasm ÷ kvasm-control ratio isolates the build. Registered
        # before environment() records every system's image identity, and
        # with kvasm's E8 tools, which plan() looks up by system name.
        suite.SYSTEMS[CONTROL] = replace(suite.SYSTEMS["kvasm"], name=CONTROL,
                                         title=f"KvAsm control ({args.control_image})",
                                         image=args.control_image)
        VALIDATION_TOOLS[CONTROL] = VALIDATION_TOOLS["kvasm"]
        experiments = [replace(e, systems=e.systems + (CONTROL,)) if "kvasm" in e.systems else e
                       for e in experiments]
    stamp = datetime.datetime.now().strftime("%Y-%m-%d-%H%M")
    out = pathlib.Path(args.out) if args.out else HERE / "results" / f"{stamp}-{args.profile}"
    out.mkdir(parents=True, exist_ok=True)
    raw = out / "raw.jsonl"
    cg = Cgroups()
    docker("rm", "-f", SERVER, check=False)
    global HOST
    HOST = HostLoad()
    HOST.start()
    # Baseline before any load of ours: everything the host does on its own.
    t_idle = time.monotonic()
    time.sleep(6 if HOST.enabled else 0)
    idle = host_load(t_idle, time.monotonic())
    if idle is None:
        print("host load: not measurable here (needs psutil and Docker Desktop's VM process)", flush=True)
    else:
        print(f"host load outside the container VM: {idle['mean']:.1f} of {HOST.logical_cpus} logical CPUs",
              flush=True)
        if idle["mean"] > HOST_BUSY_WARN:
            print(f"  WARNING: other programs are using {idle['mean']:.1f} logical CPUs; they compete with "
                  "the servers and the client, and results will carry that noise. Close them for a "
                  "publishable run.", flush=True)
    if not (out / "environment.json").exists():
        print("recording environment and calibrating the CPU clock ...", flush=True)
        env = environment(args.profile, experiments, cg)
        env["host_load_idle"] = idle
        env["host_logical_cpus"] = HOST.logical_cpus if HOST.enabled else None
        # A subset session must say so: its report has no rivals to pair with,
        # and its E4 loads may come from another session.
        env["selection"] = {"only": args.only, "systems": args.systems, "e4_rates": e4_override,
                            "control_image": args.control_image}
        (out / "environment.json").write_text(json.dumps(env, indent=2), encoding="utf-8")
        print(f"  cpu clock factor {env['cpu_clock']['factor']:.4f}", flush=True)

    records = load_records(raw)
    done = {r["run_id"] for r in records if r.get("status") in ("ok", "invalid")}
    with raw.open("a", encoding="utf-8") as sink:
        for exp in experiments:
            try:
                cells = [c for c in plan(exp, records, e4_override) if c["run_id"] not in done]
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
                t0_mono = time.monotonic()
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
                # Whole run (server start to removal); kvbench cells also carry
                # "host_load" for just the measured window.
                rec["host_load_run"] = host_load(t0_mono, time.monotonic())
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
