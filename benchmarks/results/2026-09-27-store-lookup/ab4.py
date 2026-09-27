#!/usr/bin/env python3
"""ab2.py — concurrent paired A/B of two kvnode binaries.

Usage: ab2.py BLOCKS CONFIG... with CONFIG = MODE:VALUE_BYTES:CLIENT_THREADS,
e.g. "blocking:100:8", "busy:100:6", "blocking:16:8".

Why concurrent: on this host (WSL2, other programs using ~15-20 of 32 CPUs)
a sequential pair showed a position effect larger than the effect under
test. Here A and B run AT THE SAME TIME, each server pinned to its own vCPU
with its own client vCPU set, so time-varying host noise hits both equally.
The two server vCPUs (2, 6) and client sets (16-23, 24-31) swap between A
and B every block, so a systematically faster vCPU or client set cancels
over an even number of blocks.

Per run: fresh servers (1 worker, capacity 1.25M), each preloaded with 1M
keys, then both kvbench clients start together: 64 connections x 16 deep,
3 s warmup, 10 s measured. Reported per config: medians, per-block B/A
throughput ratios with min-max, and the exact two-sided sign test.
"""
import math
import os
import re
import statistics
import subprocess
import sys
import time

KVB = "/src/benchmarks/kvbench/build/kvbench"
BIN = {"A": "/tmp/head/server/build/kvnode", "B": "/tmp/head/server/build/kvnode"}
TUN = {"A": "", "B": "glibc.malloc.arena_max=1:glibc.malloc.hugetlb=1:glibc.malloc.top_pad=268435456"}
SLOTS = [("2", "16-23", 50241), ("6", "24-31", 50242)]  # (server vCPU, client vCPUs, port)


def start(variant, cpu, port, log):
    """Starts one pinned server; returns its Popen once it listens."""
    env = dict(os.environ, KVNODE_PORT=str(port), KVNODE_WORKERS="1", KVNODE_CAPACITY="1250000",
               GLIBC_TUNABLES=TUN[variant])
    f = open(log, "w")
    p = subprocess.Popen(["taskset", "-c", cpu, BIN[variant]], env=env, stderr=f)
    for _ in range(300):
        if 'msg="listening"' in open(log).read():
            return p
        time.sleep(0.02)
    sys.exit(f"{variant} not listening")


def kvb(cpus, port, vsize, extra):
    """kvbench command line pinned to the given client vCPUs."""
    return ["taskset", "-c", cpus, KVB, "--protocol", "grpc", "--port", str(port), "--keys", "1000000",
            "--value-size", str(vsize)] + extra


def ops_of(out):
    """Measured-phase ops/s from kvbench's JSON (the last ops_per_sec)."""
    if '"valid":true' not in out:
        sys.exit("invalid kvbench run: " + out[-400:])
    return float(re.findall(r'"ops_per_sec":([0-9.]+)', out)[-1])


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def block(blk, mode, vsize, threads):
    """One block of one config: A and B concurrently; returns {variant: ops/s}."""
    order = ("A", "B") if blk % 2 == 0 else ("B", "A")      # who gets slot 0 this block
    procs = {}
    for v, (cpu, ccpus, port) in zip(order, SLOTS):
        procs[v] = (start(v, cpu, port, f"/tmp/ab2.{v}.log"), ccpus, port)
    for v, (_, ccpus, port) in procs.items():               # preload one after the other
        subprocess.run(kvb(ccpus, port, vsize, ["--load-only", "--settle", "0.2"]), check=True,
                       capture_output=True, text=True)
    extra = ["--threads", str(threads), "--connections", "64", "--depth", "16", "--warmup", "3",
             "--duration", "10"] + (["--busy-poll"] if mode == "busy" else [])
    runs = {v: subprocess.Popen(kvb(ccpus, port, vsize, extra), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
            for v, (_, ccpus, port) in procs.items()}
    res = {v: ops_of(r.communicate()[0]) for v, r in runs.items()}
    for v, (p, _, _) in procs.items():
        p.terminate()
        p.wait()
    return res, order


def main():
    blocks = int(sys.argv[1])
    configs = sys.argv[2:]
    data = {c: [] for c in configs}
    for blk in range(blocks):
        for c in configs:
            mode, vsize, threads = c.split(":")
            res, order = block(blk, mode, int(vsize), int(threads))
            data[c].append(res)
            print(f"block {blk + 1} {c:16s} A {res['A']:12,.0f}  B {res['B']:12,.0f}  "
                  f"B/A {res['B'] / res['A']:.3f}  (slot0={order[0]})", flush=True)
    print()
    for c in configs:
        a = [r["A"] for r in data[c]]
        b = [r["B"] for r in data[c]]
        ratios = [r["B"] / r["A"] for r in data[c]]
        wins = sum(x > 1 for x in ratios)
        gm = math.exp(statistics.mean(math.log(x) for x in ratios))
        print(f"{c:16s} A {statistics.median(a):12,.0f}  B {statistics.median(b):12,.0f}  "
              f"B/A median {statistics.median(ratios):.3f} geomean {gm:.3f} "
              f"[{min(ratios):.3f}-{max(ratios):.3f}]  {wins}/{len(ratios)} blocks B faster, "
              f"sign p={sign_p(wins, len(ratios)):.3f}")


main()
