#!/usr/bin/env python3
"""abdotnet.py — concurrent paired A/B of two builds of the .NET client
(client/dotnet/bench/KvAsm.Client.Bench, published self-contained for
linux-x64) against the same KvAsm server binary.

Usage (inside kvasm-dev):
  abdotnet.py OUT.jsonl BLOCKS CONFIG...
  CONFIG = MODE:SIZE:CONCURRENCY[:CHUNK[:WINDOW]]
    MODE    upload | download
    CHUNK   the bench's --chunk (0 = the client's defaults)
    WINDOW  the bench's --window (HTTP/2 stream receive window; 0 = default)
Env: A_BENCH, B_BENCH (bench executables), SERVER (kvasm binary; default
     the worktree's server/build/kvasm), DN_ENV (extra VAR=VALUE for both
     clients, e.g. DOTNET_ThreadPool_UnfairSemaphoreSpinLimit=0),
     A_ARGS / B_ARGS (extra bench flags for one side only, e.g. B_ARGS=
     "--window 4194304" to A/B a setting with one build; appended last, so
     they override the config's), WARMUP (2), SECONDS (6).

Method: as abstream.py — A and B run at the same time, each against its own
server pinned to a P-core vCPU (2 or 6), each client on its own E-core set
(16-23 or 24-31), slots swapped every block. The server has spare capacity
here (the .NET client is the bottleneck by several times), so the figures
compare the clients: value MB/s, client CPU-seconds per GB moved, bytes the
client allocated per transfer, gen0 collections, p50/p99. Allocation counts
are exact (GC.GetTotalAllocatedBytes, precise) and need no statistics; the
throughput and CPU figures get the per-block ratios and the sign test.
"""
import json
import math
import os
import statistics
import subprocess
import sys
import time

ROOT = "/src/.claude/worktrees/upload-download"
SERVER = os.environ.get("SERVER", ROOT + "/server/build/kvasm")
BENCH = {"A": os.environ["A_BENCH"], "B": os.environ["B_BENCH"]}
DN_ENV = os.environ.get("DN_ENV", "")
SLOTS = [("2", "16-23", 50221), ("6", "24-31", 50222)]  # (server vCPU, client vCPUs, port)
WARMUP = os.environ.get("WARMUP", "2")
SECONDS = os.environ.get("SECONDS", "6")
RECORD_BYTES = 64 << 20


def start(cpu, port, log):
    """Starts one pinned server (1 worker); returns its Popen once it listens."""
    env = dict(os.environ, KVASM_PORT=str(port), KVASM_WORKERS="1", KVASM_CAPACITY="100000",
               KVASM_MAX_UPLOAD_BYTES="67108864")
    f = open(log, "w")
    p = subprocess.Popen(["taskset", "-c", cpu, SERVER], env=env, stderr=f)
    for _ in range(500):
        if 'msg="listening"' in open(log).read():
            return p
        time.sleep(0.02)
    sys.exit(f"server not listening: {open(log).read()[-400:]}")


def bench_cmd(variant, ccpus, port, server_pid, cfg):
    """The bench command line for one variant and config, pinned to the client vCPUs."""
    mode, size, conc, chunk, window = cfg
    keys = max(8, RECORD_BYTES // max(size, 1))
    return ["taskset", "-c", ccpus, BENCH[variant], "--address", f"http://127.0.0.1:{port}",
            "--mode", mode, "--size", str(size), "--concurrency", str(conc), "--keys", str(keys),
            "--warmup", WARMUP, "--seconds", SECONDS, "--chunk", str(chunk), "--window", str(window),
            "--server-pid", str(server_pid)] + os.environ.get(f"{variant}_ARGS", "").split()


def parse_cfg(text):
    """MODE:SIZE:CONCURRENCY[:CHUNK[:WINDOW]] -> tuple."""
    p = text.split(":")
    return (p[0], int(p[1]), int(p[2]), int(p[3]) if len(p) > 3 else 0, int(p[4]) if len(p) > 4 else 0)


def metrics(j):
    """The compared numbers from one bench JSON result."""
    secs, gb = j["seconds"], j["value_bytes_per_sec"] * j["seconds"] / 1e9
    return {"mbps": j["value_bytes_per_sec"] / 1e6,
            "cpu_per_gb": j["client_cpu_s"] / gb if gb else float("nan"),
            "client_cores": j["client_cpu_s"] / secs,
            "alloc": j["alloc_per_transfer"],
            "gc0_per_s": j["gc0"] / secs,
            "server_util": j["server_cpu_s"] / secs if j["server_cpu_s"] >= 0 else float("nan"),
            "p50": j["p50_us"], "p99": j["p99_us"]}


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def block(blk, text, out):
    """One block of one config: A and B concurrently. Returns {variant: metrics}, slot order."""
    cfg = parse_cfg(text)
    order = ("A", "B") if blk % 2 == 0 else ("B", "A")
    servers = {}
    for v, (cpu, ccpus, port) in zip(order, SLOTS):
        servers[v] = (start(cpu, port, f"/tmp/abdotnet.{v}.log"), ccpus, port)
    env = dict(os.environ, DOTNET_gcServer="0")
    for kv in DN_ENV.split():
        k, val = kv.split("=", 1)
        env[k] = val
    runs = {v: subprocess.Popen(bench_cmd(v, ccpus, port, p.pid, cfg), env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
            for v, (p, ccpus, port) in servers.items()}
    res = {}
    for v, r in runs.items():
        stdout, stderr = r.communicate()
        try:
            j = json.loads(stdout.strip().splitlines()[-1])
        except (json.JSONDecodeError, IndexError):
            sys.exit(f"{v}: unreadable bench output: {stdout[-400:]} {stderr[-400:]}")
        if not j.get("valid"):
            sys.exit(f"{v}: invalid run: {j.get('error')}")
        res[v] = metrics(j)
        out.write(json.dumps({"block": blk, "config": text, "variant": v, "slot": order.index(v),
                              "bench": j}) + "\n")
    for v, (p, _, _) in servers.items():
        p.terminate()
        p.wait()
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
                print(f"block {blk + 1} {c:24s} A {a['mbps']:7.0f} MB/s {a['cpu_per_gb']:5.2f} cpu-s/GB "
                      f"{a['alloc']:9.0f} B/xfer | B {b['mbps']:7.0f} MB/s {b['cpu_per_gb']:5.2f} cpu-s/GB "
                      f"{b['alloc']:9.0f} B/xfer | B/A {b['mbps'] / a['mbps']:.3f} (slot0={order[0]})", flush=True)
    print()
    for c in configs:
        rows = data[c]
        print(f"== {c}")
        for key, label, better in (("mbps", "value MB/s", "higher"),
                                   ("cpu_per_gb", "client CPU-s per GB", "lower"),
                                   ("client_cores", "client cores busy", "none"),
                                   ("alloc", "allocated B per transfer", "lower"),
                                   ("gc0_per_s", "gen0 GCs per second", "lower"),
                                   ("server_util", "server CPU utilization", "none"),
                                   ("p50", "p50 us per transfer", "lower"),
                                   ("p99", "p99 us per transfer", "lower")):
            a = [r["A"][key] for r in rows]
            b = [r["B"][key] for r in rows]
            ratios = [rb / ra for ra, rb in zip(a, b) if ra > 0]
            gm = math.exp(statistics.mean(math.log(x) for x in ratios)) if ratios and all(x > 0 for x in ratios) else float("nan")
            line = (f"   {label:26s} A {statistics.median(a):12.3f}  B {statistics.median(b):12.3f}  "
                    f"B/A geomean {gm:.3f}" + (f" [{min(ratios):.3f}-{max(ratios):.3f}]" if ratios else ""))
            if better != "none" and ratios:
                wins = sum((x > 1) if better == "higher" else (x < 1) for x in ratios)
                line += f"  B better in {wins}/{len(ratios)}, sign p={sign_p(wins, len(ratios)):.3f}"
            print(line)


main()
