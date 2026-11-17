#!/usr/bin/env python3
"""dnseq.py — sequential, alternating A/B of two builds of the .NET client
(or of one build with two flag sets), for when concurrent pairs are too noisy.

Usage (inside kvasm-dev):
  dnseq.py OUT.jsonl ROUNDS CONFIG...
  CONFIG = MODE:SIZE:CONCURRENCY[:CHUNK[:WINDOW]]   (as abdotnet.py)
Env: A_BENCH, B_BENCH, A_ARGS, B_ARGS, SERVER, DN_ENV, WARMUP (2), SECONDS (6)
     as abdotnet.py; SERVER_CPUS ("2") and WORKERS ("1") place the server,
     e.g. "2,4,6,8" and 4 to see what spreading connections over server
     workers buys (KvAsm pins each connection to one worker).
MODE get (unary Get of SIZE-byte values) uses at most 10,000 keys, as does
every mode: the server holds 100,000.

Why a second method: two .NET clients running at once each keep 4-5 cores
busy, much of it thread-pool spinning, and disturb each other through the
shared cache and memory — concurrent pairs of the 2026-09-30 client A/B
swung 0.83-1.15 within one config. Here one client runs at a time, alone,
on all 16 E-core vCPUs (16-31), against a fresh server pinned to vCPU 2;
A and B alternate in ABBA order across rounds (A B | B A | A B ...) so a
drift in host load hits both evenly, and each round yields one paired B/A
ratio. Summary: medians, per-round ratios with geomean and min-max, the
exact sign test; allocations are exact counts and are reported as such.
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
WARMUP = os.environ.get("WARMUP", "2")
SECONDS = os.environ.get("SECONDS", "6")
SERVER_CPU = os.environ.get("SERVER_CPUS", "2")
WORKERS = os.environ.get("WORKERS", "1")
CLIENT_CPUS, PORT = "16-31", 50223
RECORD_BYTES = 64 << 20
MAX_KEYS = 10000


def parse_cfg(text):
    """MODE:SIZE:CONCURRENCY[:CHUNK[:WINDOW]] -> tuple."""
    p = text.split(":")
    return (p[0], int(p[1]), int(p[2]), int(p[3]) if len(p) > 3 else 0, int(p[4]) if len(p) > 4 else 0)


def run(variant, cfg):
    """One run of one variant against a fresh pinned server; returns the bench's JSON."""
    env = dict(os.environ, KVASM_PORT=str(PORT), KVASM_WORKERS=WORKERS, KVASM_CAPACITY="100000",
               KVASM_MAX_UPLOAD_BYTES="67108864")
    log = open("/tmp/dnseq.server.log", "w")
    server = subprocess.Popen(["taskset", "-c", SERVER_CPU, SERVER], env=env, stderr=log)
    for _ in range(500):
        if 'msg="listening"' in open("/tmp/dnseq.server.log").read():
            break
        time.sleep(0.02)
    mode, size, conc, chunk, window = cfg
    # 64 MiB of values (a working set beyond the caches), but never more keys
    # than the server's capacity comfortably holds or a preload takes long.
    keys = min(MAX_KEYS, max(8, RECORD_BYTES // max(size, 1)))
    cenv = dict(os.environ, DOTNET_gcServer="0")
    for kv in DN_ENV.split():
        k, val = kv.split("=", 1)
        cenv[k] = val
    cmd = ["taskset", "-c", CLIENT_CPUS, BENCH[variant], "--address", f"http://127.0.0.1:{PORT}",
           "--mode", mode, "--size", str(size), "--concurrency", str(conc), "--keys", str(keys),
           "--warmup", WARMUP, "--seconds", SECONDS, "--chunk", str(chunk), "--window", str(window),
           "--server-pid", str(server.pid)] + os.environ.get(f"{variant}_ARGS", "").split()
    r = subprocess.run(cmd, env=cenv, capture_output=True, text=True)
    server.terminate()
    server.wait()
    try:
        j = json.loads(r.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        sys.exit(f"{variant}: unreadable bench output: {r.stdout[-400:]} {r.stderr[-400:]}")
    if not j.get("valid"):
        sys.exit(f"{variant}: invalid run: {j.get('error')}")
    return j


def metrics(j):
    """The compared numbers from one bench JSON result."""
    secs, gb = j["seconds"], j["value_bytes_per_sec"] * j["seconds"] / 1e9
    return {"mbps": j["value_bytes_per_sec"] / 1e6, "cpu_per_gb": j["client_cpu_s"] / gb,
            "client_cores": j["client_cpu_s"] / secs, "alloc": j["alloc_per_transfer"],
            "gc0_per_s": j["gc0"] / secs, "server_util": j["server_cpu_s"] / secs,
            "p50": j["p50_us"], "p99": j["p99_us"]}


def sign_p(k, n):
    """Exact two-sided sign test p for k successes of n."""
    tail = sum(math.comb(n, i) for i in range(0, min(k, n - k) + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def main():
    path, rounds, configs = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
    data = {c: [] for c in configs}
    with open(path, "a") as out:
        for rnd in range(rounds):
            order = ("A", "B") if rnd % 2 == 0 else ("B", "A")
            for c in configs:
                res = {}
                for v in order:
                    j = run(v, parse_cfg(c))
                    res[v] = metrics(j)
                    out.write(json.dumps({"round": rnd, "config": c, "variant": v, "position": order.index(v),
                                          "bench": j}) + "\n")
                    out.flush()
                data[c].append(res)
                a, b = res["A"], res["B"]
                print(f"round {rnd + 1} {c:24s} A {a['mbps']:7.0f} MB/s {a['cpu_per_gb']:5.2f} cpu-s/GB "
                      f"{a['alloc']:9.0f} B/xfer | B {b['mbps']:7.0f} MB/s {b['cpu_per_gb']:5.2f} cpu-s/GB "
                      f"{b['alloc']:9.0f} B/xfer | B/A {b['mbps'] / a['mbps']:.3f} (first={order[0]})", flush=True)
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
            pos = [x for x in ratios if x > 0]
            gm = math.exp(statistics.mean(math.log(x) for x in pos)) if pos and len(pos) == len(ratios) else float("nan")
            line = (f"   {label:26s} A {statistics.median(a):12.3f}  B {statistics.median(b):12.3f}  "
                    f"B/A geomean {gm:.3f}" + (f" [{min(ratios):.3f}-{max(ratios):.3f}]" if ratios else ""))
            if better != "none" and ratios:
                wins = sum((x > 1) if better == "higher" else (x < 1) for x in ratios)
                line += f"  B better in {wins}/{len(ratios)}, sign p={sign_p(wins, len(ratios)):.3f}"
            print(line)


main()
