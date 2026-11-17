#!/usr/bin/env bash
# =============================================================================
# syscount.sh — syscalls one kvasm worker makes per request, by name, counted
# with `strace -c -f` under an open-loop load slow enough that the worker
# sleeps between requests (so the count is the per-wakeup pattern, not a
# property of how saturated the traced server happens to be).
#
# Usage (inside kvasm-dev):
#   syscount.sh BINARY LABEL [RATE] [SECONDS] [SRV_CPU] [CLI_CPUS] [PORT]
# Defaults: 2,000 requests/s for 4 s, server on vCPU 12, client on 13-15.
# Why strace: it counts exactly; its slowdown does not matter at this rate.
# =============================================================================
set -euo pipefail
BIN=$1; LABEL=$2; RATE=${3:-2000}; SECS=${4:-4}
SCPU=${5:-12}; CCPUS=${6:-13-15}; PORT=${7:-50353}
KVB=/src/benchmarks/kvbench/build/kvbench
OUT=/tmp/syscount.$PORT

KVASM_PORT=$PORT KVASM_WORKERS=1 KVASM_CAPACITY=50000 \
    taskset -c "$SCPU" strace -f -c -o "$OUT.strace" "$BIN" 2> "$OUT.log" &
SPID=$!
trap 'pkill -TERM -P $SPID 2>/dev/null || true; kill -TERM $SPID 2>/dev/null || true; wait $SPID 2>/dev/null || true' EXIT
for _ in $(seq 1 200); do grep -q 'msg="listening"' "$OUT.log" && break; sleep 0.05; done

taskset -c "$CCPUS" "$KVB" --protocol grpc --port "$PORT" --keys 20000 --value-size 100 \
    --load-only --settle 0.2 --threads 2 --connections 4 > /dev/null
taskset -c "$CCPUS" "$KVB" --protocol grpc --port "$PORT" --keys 20000 --value-size 100 \
    --threads 2 --connections 8 --depth 1 --rate "$RATE" --warmup 0 --duration "$SECS" > "$OUT.json"
# SIGTERM the server (strace's child) so strace prints its table.
pkill -TERM -P $SPID || true
wait $SPID 2>/dev/null || true
trap - EXIT
python3 - "$LABEL" "$OUT.json" "$OUT.strace" <<'EOF'
import json, re, sys
label, jpath, spath = sys.argv[1:]
ops = json.load(open(jpath))["result"]["ops"]
rows = []
for line in open(spath):
    m = re.match(r"\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\w+)\s*$", line)
    if m and m.group(2) != "total":
        rows.append((int(m.group(1)), m.group(2)))
rows.sort(reverse=True)
# The preload (20,000 Puts over 4 connections, pipelined) adds a few thousand
# calls to read/sendto/epoll_wait; the measured run dominates at these rates
# only if ops is large, so report both raw counts and per measured request.
print(f"{label}: {ops} measured requests")
for n, name in rows[:6]:
    print(f"   {name:14s} {n:8d} calls   {n / ops:6.2f} per measured request")
EOF
