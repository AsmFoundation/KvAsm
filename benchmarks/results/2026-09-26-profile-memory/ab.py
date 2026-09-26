#!/usr/bin/env python3
"""ab.py — paired A/B of two kvnode binaries on one pinned core.

Usage: ab.py BLOCKS CONFIG... where CONFIG is "blocking:100", "busy:100",
"blocking:16" (client mode : value bytes).

Every block runs A and B once per config, the order alternating by block
(A,B then B,A) so drift and position cancel; each run is a fresh server on
CPU 2 with 1M keys preloaded, then kvbench 8 threads x 64 conns x 16 deep
for 10 s (3 s warmup) on CPUs 16-31. Reports per config: median ops/s of
each variant, per-block B/A ratios, min-max and the exact two-sided sign
test over blocks. Server CPU ns/op is computed from /proc/PID/stat ticks
over the measured window (the core is saturated in every run).
"""
import math
import os
import re
import statistics
import subprocess
import sys
import time

KVB = "/src/benchmarks/kvbench/build/kvbench"
PORT = 50191
BIN = {"A": "/tmp/kvnode-A", "B": "/tmp/kvnode-B"}
TICK = os.sysconf("SC_CLK_TCK")


def ticks(pid):
    """user + system ticks of a process."""
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def run(variant, mode, vsize):
    """One fresh-server run; returns (ops/s, server CPU ns per op)."""
    env = dict(os.environ, KVNODE_PORT=str(PORT), KVNODE_WORKERS="1", KVNODE_CAPACITY="1250000")
    log = open("/tmp/ab.srv.log", "w")
    srv = subprocess.Popen(["taskset", "-c", "2", BIN[variant]], env=env, stderr=log)
    for _ in range(200):
        if 'msg="listening"' in open("/tmp/ab.srv.log").read():
            break
        time.sleep(0.02)
    base = ["taskset", "-c", "16-31", KVB, "--protocol", "grpc", "--port", str(PORT),
            "--keys", "1000000", "--value-size", str(vsize)]
    subprocess.run(base + ["--load-only", "--settle", "0.2"], check=True, capture_output=True)
    args = base + ["--threads", "8", "--connections", "64", "--depth", "16", "--warmup", "3",
                   "--duration", "10"]
    if mode == "busy":
        args.append("--busy-poll")
    b = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(3.0)                     # warmup
    t0, c0 = time.time(), ticks(srv.pid)
    time.sleep(9.5)                     # inside the 10 s measured window
    t1, c1 = time.time(), ticks(srv.pid)
    out, _ = b.communicate()
    srv.terminate()
    srv.wait()
    # kvbench prints one JSON document; its measured-phase throughput is the
    # last "ops_per_sec" (a preload section, when present, comes first).
    if '"valid":true' not in out:
        sys.exit(f"invalid run {variant} {mode} {vsize}: {out[-500:]}")
    ops = float(re.findall(r'"ops_per_sec":([0-9.]+)', out)[-1])
    cpu_s = (c1 - c0) / TICK
    ns_per_op = cpu_s / (t1 - t0) * 1e9 / ops
    return ops, ns_per_op


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def main():
    blocks = int(sys.argv[1])
    configs = sys.argv[2:]
    data = {c: {"A": [], "B": []} for c in configs}
    for blk in range(blocks):
        order = ("A", "B") if blk % 2 == 0 else ("B", "A")
        for c in configs:
            mode, vsize = c.split(":")
            for v in order:
                ops, ns = run(v, mode, int(vsize))
                data[c][v].append((ops, ns))
                print(f"block {blk + 1} {c:12s} {v}: {ops:12,.0f} ops/s  {ns:5.0f} ns/op", flush=True)
    print()
    for c in configs:
        a = [x[0] for x in data[c]["A"]]
        b = [x[0] for x in data[c]["B"]]
        an = [x[1] for x in data[c]["A"]]
        bn = [x[1] for x in data[c]["B"]]
        ratios = [bb / aa for aa, bb in zip(a, b)]
        wins = sum(r > 1 for r in ratios)
        print(f"{c:12s} A {statistics.median(a):12,.0f} ops/s ({statistics.median(an):.0f} ns/op)  "
              f"B {statistics.median(b):12,.0f} ops/s ({statistics.median(bn):.0f} ns/op)  "
              f"B/A {statistics.median(ratios):.3f} [{min(ratios):.3f}-{max(ratios):.3f}]  "
              f"{wins}/{len(ratios)} blocks B faster, sign p={sign_p(wins, len(ratios)):.3f}")


main()
