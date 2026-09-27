#!/usr/bin/env python3
"""ab3.py — concurrent paired A/B of two kvnode binaries, with mechanism metrics.

Usage: ab3.py BLOCKS CONFIG... with CONFIG = MODE:VALUE_BYTES:THREADS:CONNS
(MODE blocking|busy), e.g. "blocking:100:8:64" (8 client threads x 8
connections each) or "blocking:100:64:64" (one connection per thread).

A and B run AT THE SAME TIME, each server pinned to its own vCPU with its
own client vCPU set; the two slots (server vCPU 2 or 6, clients 16-23 or
24-31) swap between A and B every block, so a faster slot cancels over an
even number of blocks and time-varying host noise hits both equally.

Per run: fresh servers (1 worker, capacity 1.25M), 1M keys preloaded into
each, then both kvbench clients start together (64 x 16 deep by default
connections, 3 s warmup, 10 s measured). Per variant it records:
  * ops/s and p50/p99 latency (kvbench's measured phase);
  * server CPU ns per request (utime + stime over the window / requests);
  * client wakeups per 1,000 requests: voluntary context switches summed
    over kvbench's threads during the window (a thread that blocks in
    epoll_wait and is woken again counts one).
Summary per config: medians, per-block B/A ratios (geomean, min-max) and
the exact two-sided sign test.
"""
import math
import os
import re
import statistics
import subprocess
import sys
import time

KVB = "/src/benchmarks/kvbench/build/kvbench"
BIN = {"A": "/tmp/kvnode-A", "B": "/tmp/kvnode-B"}
SLOTS = [("2", "16-23", 50211), ("6", "24-31", 50212)]  # (server vCPU, client vCPUs, port)
HZ = os.sysconf("SC_CLK_TCK")


def start(variant, cpu, port, log):
    """Starts one pinned server; returns its Popen once it listens."""
    env = dict(os.environ, KVNODE_PORT=str(port), KVNODE_WORKERS="1", KVNODE_CAPACITY="1250000")
    f = open(log, "w")
    p = subprocess.Popen(["taskset", "-c", cpu, BIN[variant]], env=env, stderr=f)
    for _ in range(500):
        if 'msg="listening"' in open(log).read():
            return p
        time.sleep(0.02)
    sys.exit(f"{variant} not listening")


def ticks(pid):
    """utime + stime of a process, in clock ticks."""
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def vswitches(pid):
    """Voluntary context switches summed over a process's threads."""
    total = 0
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            for line in open(f"/proc/{pid}/task/{tid}/status"):
                if line.startswith("voluntary_ctxt_switches"):
                    total += int(line.split()[1])
        except FileNotFoundError:
            pass
    return total


def kvb(cpus, port, vsize, extra):
    """kvbench command line pinned to the given client vCPUs (taskset execs it: same pid)."""
    return ["taskset", "-c", cpus, KVB, "--protocol", "grpc", "--port", str(port), "--keys", "1000000",
            "--value-size", str(vsize)] + extra


def parse(out):
    """(ops/s, p50 us, p99 us) of kvbench's measured phase."""
    if '"valid":true' not in out:
        sys.exit("invalid kvbench run: " + out[-400:])
    ops = float(re.findall(r'"ops_per_sec":([0-9.]+)', out)[-1])
    lat = re.search(r'"latency_us":\{[^}]*"p50":([0-9.]+),"p90":[0-9.]+,"p99":([0-9.]+)', out)
    return ops, float(lat.group(1)), float(lat.group(2))


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def block(blk, mode, vsize, threads, conns):
    """One block of one config: A and B concurrently; returns {variant: metrics}."""
    order = ("A", "B") if blk % 2 == 0 else ("B", "A")      # who gets slot 0 this block
    t_start = time.time()
    procs = {}
    for v, (cpu, ccpus, port) in zip(order, SLOTS):
        procs[v] = (start(v, cpu, port, f"/tmp/ab3.{v}.log"), ccpus, port)
    for v, (_, ccpus, port) in procs.items():               # preload one after the other
        subprocess.run(kvb(ccpus, port, vsize, ["--load-only", "--settle", "0.2", "--threads", "8",
                                                 "--connections", "16"]),
                       check=True, capture_output=True, text=True)
    t_loaded = time.time()
    extra = ["--threads", str(threads), "--connections", str(conns), "--depth", "16",
             "--warmup", "3", "--duration", "10"] + (["--busy-poll"] if mode == "busy" else [])
    runs = {v: subprocess.Popen(kvb(ccpus, port, vsize, extra), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
            for v, (_, ccpus, port) in procs.items()}
    time.sleep(3.3)                                          # past the warmup
    w0 = {v: (time.time(), ticks(procs[v][0].pid), vswitches(r.pid)) for v, r in runs.items()}
    time.sleep(9.0)                                          # inside the 10 s window
    w1 = {v: (time.time(), ticks(procs[v][0].pid), vswitches(r.pid)) for v, r in runs.items()}
    res = {}
    for v, r in runs.items():
        ops, p50, p99 = parse(r.communicate()[0])
        dt = w1[v][0] - w0[v][0]
        reqs = ops * dt
        res[v] = {"ops": ops, "p50": p50, "p99": p99,
                  "ns": (w1[v][1] - w0[v][1]) / HZ * 1e9 / reqs,
                  "wake": (w1[v][2] - w0[v][2]) * 1000.0 / reqs}
    for v, (p, _, _) in procs.items():
        p.terminate()
        p.wait()
    return res, order, t_loaded - t_start, time.time() - t_start


def main():
    blocks = int(sys.argv[1])
    configs = sys.argv[2:]
    data = {c: [] for c in configs}
    for blk in range(blocks):
        for c in configs:
            mode, vsize, threads, conns = c.split(":")
            res, order, t_load, t_all = block(blk, mode, int(vsize), int(threads), int(conns))
            data[c].append(res)
            a, b = res["A"], res["B"]
            print(f"block {blk + 1} {c:20s} A {a['ops']:10,.0f}/s p50 {a['p50']:6.0f} p99 {a['p99']:6.0f} "
                  f"{a['ns']:4.0f} ns {a['wake']:5.1f} wk | B {b['ops']:10,.0f}/s p50 {b['p50']:6.0f} "
                  f"p99 {b['p99']:6.0f} {b['ns']:4.0f} ns {b['wake']:5.1f} wk | B/A {b['ops'] / a['ops']:.3f} "
                  f"(slot0={order[0]}, load {t_load:.0f}s, total {t_all:.0f}s)", flush=True)
    print()
    for c in configs:
        rows = data[c]
        print(f"== {c}")
        for key, label, better in (("ops", "ops/s", "higher"), ("p50", "p50 us", "lower"),
                                   ("p99", "p99 us", "lower"), ("ns", "server CPU ns/req", "lower"),
                                   ("wake", "client wakeups per 1k req", "lower")):
            a = [r["A"][key] for r in rows]
            b = [r["B"][key] for r in rows]
            ratios = [rb / ra for ra, rb in zip(a, b) if ra > 0]
            wins = sum((x > 1) if better == "higher" else (x < 1) for x in ratios)
            gm = math.exp(statistics.mean(math.log(x) for x in ratios))
            print(f"   {label:26s} A {statistics.median(a):12,.1f}  B {statistics.median(b):12,.1f}  "
                  f"B/A geomean {gm:.3f} [{min(ratios):.3f}-{max(ratios):.3f}]  "
                  f"B better in {wins}/{len(ratios)}, sign p={sign_p(wins, len(ratios)):.3f}")


main()
