#!/usr/bin/env python3
"""abw.py — concurrent paired A/B of two kvnode binaries on write-heavy mixes.

Usage: abw.py BLOCKS CONFIG... with
CONFIG = WORKERS:READ_RATIO:VALUE_BYTES[:CONNECTIONS:DEPTH]
e.g. "1:0:100" (one worker, 100% SET, 100-byte values, 64 connections x 16
in flight), "4:0.5:100", or "1:0:100:256:1" (256 connections with one
request each: no pipelining, nothing to batch within a read).

Why concurrent: the host is a desktop whose other programs load 4-25 of its
32 logical CPUs, unseen from inside the VM (METHODOLOGY.md §7). A and B run
AT THE SAME TIME, each server pinned to its own vCPUs with its own client
vCPU set, and the two slots swap between A and B every block, so a faster
slot cancels over an even number of blocks and host noise hits both equally.
Paired ratios per block and an exact sign test decide; absolute numbers are
only indicative.

Slots (server vCPUs, client vCPUs, port): the two SMT siblings of every
core used go one to each slot, so A and B always run on the same physical
cores, a hyperthread each:
  1 worker:  vCPU 2 or its sibling 3
  4 workers: vCPUs 2,4,6,8 or their siblings 3,5,7,9
  clients:   the even vCPUs of 16-31 or the odd ones (8 each)
Why siblings: the VM presents vCPU pairs (0-1, 2-3, ...) as SMT siblings,
and Hyper-V places each virtual core on a host core of its choosing. A first
attempt gave each slot its own cores (2-5 vs 8-11, clients 16-23 vs 24-31)
and measured whole-slot differences of up to 2.8x at 4 workers, which no
number of blocks averages out (ab-batching-attempt1-slot-biased.out).
Sharing every core makes the hardware identical for A and B by
construction; the per-block slot swap still cancels any even/odd sibling
difference. Absolute rates are lower (each variant gets half of each core),
the ratios fair. vCPU 0-1 are avoided (vCPU 0 takes the VM's interrupts).

Per run: fresh servers (KVNODE_WORKERS as configured, capacity 1.25M), 1M
keys preloaded into each (so a SET replaces an existing key, the common
case), then both kvbench clients start together: 16 threads, 64 connections x 16
requests in flight by default (1,024), 3 s warm-up, 10 s measured. Recorded per
variant:
  * ops/s and p50/p99 latency of kvbench's measured phase;
  * server CPU ns per request (utime + stime over the window / requests);
  * client CPU busy (kvbench's cores used / cores given): near 1.0 means
    the CLIENT limits the run, and the block is flagged.
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
EVEN_CLIENTS = ",".join(str(c) for c in range(16, 32, 2))
ODD_CLIENTS = ",".join(str(c) for c in range(17, 32, 2))
SLOTS = {
    1: [("2", EVEN_CLIENTS, 50211), ("3", ODD_CLIENTS, 50212)],
    4: [("2,4,6,8", EVEN_CLIENTS, 50211), ("3,5,7,9", ODD_CLIENTS, 50212)],
}
CLIENT_CPUS = 8                                  # vCPUs per client set above
HZ = os.sysconf("SC_CLK_TCK")


def start(variant, cpus, port, workers, log):
    """Start one pinned server and wait until it listens.

    Args:
        variant: "A" or "B", the key of BIN naming the binary to run.
        cpus: taskset CPU list the server is pinned to (e.g. "2-5").
        port: TCP port the server listens on (KVNODE_PORT).
        workers: KVNODE_WORKERS, the number of event-loop threads.
        log: path of the file that receives the server's stderr (its log).

    Returns:
        The server's subprocess.Popen, once its log says "listening".

    Raises:
        SystemExit: the server did not log "listening" within 10 s.
        OSError: the log file or the binary could not be opened.
    """
    env = dict(os.environ, KVNODE_PORT=str(port), KVNODE_WORKERS=str(workers),
               KVNODE_CAPACITY="1250000")
    f = open(log, "w")
    p = subprocess.Popen(["taskset", "-c", cpus, BIN[variant]], env=env, stderr=f)
    for _ in range(500):
        if 'msg="listening"' in open(log).read():
            return p
        time.sleep(0.02)
    sys.exit(f"{variant} not listening")


def ticks(pid):
    """Read the CPU time a process (all its threads) has used.

    Args:
        pid: the process id.

    Returns:
        utime + stime from /proc/PID/stat, in clock ticks (HZ per second).

    Raises:
        OSError: the process no longer exists.
    """
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def kvb(cpus, port, vsize, extra):
    """Build a kvbench command line pinned to client vCPUs.

    taskset execs kvbench, so the Popen's pid is kvbench's own and its CPU
    time can be read with ticks().

    Args:
        cpus: taskset CPU list for the client.
        port: the server's port.
        vsize: value size in bytes.
        extra: further kvbench arguments (workload, threads, durations).

    Returns:
        The argument list for subprocess.
    """
    return ["taskset", "-c", cpus, KVB, "--protocol", "grpc", "--port", str(port), "--keys", "1000000",
            "--value-size", str(vsize)] + extra


def parse(out):
    """Extract throughput and latency of kvbench's measured phase.

    Args:
        out: kvbench's stdout (JSON lines).

    Returns:
        (ops per second, p50 latency in us, p99 latency in us).

    Raises:
        SystemExit: kvbench marked its run invalid (errors, refused calls).
        AttributeError: the output lacks a latency object (kvbench changed).
    """
    if '"valid":true' not in out:
        sys.exit("invalid kvbench run: " + out[-400:])
    ops = float(re.findall(r'"ops_per_sec":([0-9.]+)', out)[-1])
    lat = re.search(r'"latency_us":\{[^}]*"p50":([0-9.]+),"p90":[0-9.]+,"p99":([0-9.]+)', out)
    return ops, float(lat.group(1)), float(lat.group(2))


def sign_p(k, n):
    """Exact two-sided sign test.

    Args:
        k: blocks in which B was better.
        n: blocks.

    Returns:
        The two-sided p-value of k successes in n fair coin flips (6/6 gives
        0.031, the least that six blocks can show).
    """
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def block(blk, workers, ratio, vsize, conns, depth):
    """Run one block of one config: A and B at the same time.

    Args:
        blk: block index; its parity decides which variant takes slot 0.
        workers: KVNODE_WORKERS for both servers (selects SLOTS[workers]).
        ratio: kvbench --read-ratio (0 = 100% SET, 1 = 100% GET).
        vsize: value size in bytes.
        conns: kvbench --connections (spread over 16 client threads).
        depth: kvbench --depth, requests in flight per connection.

    Returns:
        ({variant: metrics dict}, (variant in slot 0, variant in slot 1)).

    Raises:
        subprocess.CalledProcessError: a preload failed.
        SystemExit: a server did not start or a run was invalid.
    """
    order = ("A", "B") if blk % 2 == 0 else ("B", "A")      # who gets slot 0 this block
    procs = {}
    for v, (cpus, ccpus, port) in zip(order, SLOTS[workers]):
        procs[v] = (start(v, cpus, port, workers, f"/tmp/abw.{v}.log"), ccpus, port)
    for v, (_, ccpus, port) in procs.items():               # preload one after the other
        subprocess.run(kvb(ccpus, port, vsize, ["--load-only", "--settle", "0.2", "--threads", "8",
                                                 "--connections", "16"]),
                       check=True, capture_output=True, text=True)
    extra = ["--threads", "16", "--connections", str(conns), "--depth", str(depth), "--read-ratio", str(ratio),
             "--warmup", "3", "--duration", "10"]
    runs = {v: subprocess.Popen(kvb(ccpus, port, vsize, extra), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
            for v, (_, ccpus, port) in procs.items()}
    time.sleep(3.3)                                          # past the warm-up
    w0 = {v: (time.time(), ticks(procs[v][0].pid), ticks(r.pid)) for v, r in runs.items()}
    time.sleep(9.0)                                          # inside the 10 s window
    w1 = {v: (time.time(), ticks(procs[v][0].pid), ticks(r.pid)) for v, r in runs.items()}
    res = {}
    for v, r in runs.items():
        ops, p50, p99 = parse(r.communicate()[0])
        dt = w1[v][0] - w0[v][0]
        reqs = ops * dt
        res[v] = {"ops": ops, "p50": p50, "p99": p99,
                  "ns": (w1[v][1] - w0[v][1]) / HZ * 1e9 / reqs,
                  "cbusy": (w1[v][2] - w0[v][2]) / HZ / dt / CLIENT_CPUS}
    for v, (p, _, _) in procs.items():
        p.terminate()
        p.wait()
    return res, order


def main():
    """Run every config for the given number of blocks and print the summary.

    Reads BLOCKS and CONFIG... from sys.argv (see the module docstring).

    Raises:
        ValueError: a config is not WORKERS:READ_RATIO:VALUE_BYTES[:CONNECTIONS:DEPTH].
        KeyError: WORKERS has no slot definition (only 1 and 4 do).
    """
    blocks = int(sys.argv[1])
    configs = sys.argv[2:]
    data = {c: [] for c in configs}
    for blk in range(blocks):
        for c in configs:
            parts = c.split(":")
            w, ratio, vsize = parts[:3]
            conns, depth = (parts[3], parts[4]) if len(parts) == 5 else ("64", "16")
            res, order = block(blk, int(w), float(ratio), int(vsize), int(conns), int(depth))
            data[c].append(res)
            a, b = res["A"], res["B"]
            flag = " CLIENT-BOUND" if max(a["cbusy"], b["cbusy"]) > 0.92 else ""
            print(f"block {blk + 1} {c:14s} A {a['ops']:10,.0f}/s p50 {a['p50']:6.0f} p99 {a['p99']:6.0f} "
                  f"{a['ns']:5.0f} ns cl {a['cbusy']:.2f} | B {b['ops']:10,.0f}/s p50 {b['p50']:6.0f} "
                  f"p99 {b['p99']:6.0f} {b['ns']:5.0f} ns cl {b['cbusy']:.2f} | B/A {b['ops'] / a['ops']:.3f} "
                  f"(slot0={order[0]}){flag}", flush=True)
    print()
    for c in configs:
        rows = data[c]
        print(f"== {c} (workers:read_ratio:value_bytes[:connections:depth])")
        for key, label, better in (("ops", "ops/s", "higher"), ("p50", "p50 us", "lower"),
                                   ("p99", "p99 us", "lower"), ("ns", "server CPU ns/req", "lower"),
                                   ("cbusy", "client CPU busy", "info")):
            a = [r["A"][key] for r in rows]
            b = [r["B"][key] for r in rows]
            ratios = [rb / ra for ra, rb in zip(a, b) if ra > 0]
            gm = math.exp(statistics.mean(math.log(x) for x in ratios))
            if better == "info":
                print(f"   {label:26s} A {statistics.median(a):12,.2f}  B {statistics.median(b):12,.2f}")
                continue
            wins = sum((x > 1) if better == "higher" else (x < 1) for x in ratios)
            print(f"   {label:26s} A {statistics.median(a):12,.1f}  B {statistics.median(b):12,.1f}  "
                  f"B/A geomean {gm:.3f} [{min(ratios):.3f}-{max(ratios):.3f}]  "
                  f"B better in {wins}/{len(ratios)}, sign p={sign_p(wins, len(ratios)):.3f}")


main()
