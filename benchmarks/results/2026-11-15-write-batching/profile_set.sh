#!/usr/bin/env bash
# =============================================================================
# profile_set.sh — where a SET's server time goes: perf cpu-clock samples of
# one kvnode under a 100% SET load, by symbol, plus the user/kernel split.
#
# Usage (inside the kvnode-dev container):
#   profile_set.sh BINARY [WORKERS] [SECONDS]
# defaults: 1 worker (pinned to vCPU 2), 10 s of samples. The client (kvbench,
# 16 threads x 64 connections x 16 deep on vCPUs 16-31) replaces random keys
# of a 1M-key preloaded table, 100-byte values: the steady state of a write
# workload, where every SET allocates a record and retires the old one.
#
# Why cpu-clock and perf -p: WSL2 exposes no PMU (no cycles/cache events), and
# `perf record -a` needs privileges the dev container lacks; per-PID
# cpu-clock sampling works and attributes user-space time exactly. Kernel
# time shows up as the split read from /proc (utime vs stime), not by symbol.
# =============================================================================
set -euo pipefail

BIN=${1:?usage: profile_set.sh BINARY [WORKERS] [SECONDS]}
WORKERS=${2:-1}
SECS=${3:-10}
PORT=50299
KVB=/src/benchmarks/kvbench/build/kvbench
OUT=/tmp/profile_set
mkdir -p "$OUT"

if [ "$WORKERS" = 1 ]; then SCPU=2; else SCPU="2-$((1 + WORKERS))"; fi

# Server: pinned, capacity for 1M keys + headroom.
KVNODE_PORT=$PORT KVNODE_WORKERS=$WORKERS KVNODE_CAPACITY=1250000 \
    taskset -c "$SCPU" "$BIN" 2> "$OUT/server.log" &
SPID=$!
trap 'kill -TERM $SPID 2>/dev/null || true; wait $SPID 2>/dev/null || true' EXIT
for _ in $(seq 1 200); do grep -q 'msg="listening"' "$OUT/server.log" && break; sleep 0.05; done

taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
    --load-only --settle 0.2 --threads 8 --connections 16 > /dev/null

# The load runs warm-up + samples + a margin; perf starts after the warm-up.
taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
    --threads 16 --connections 64 --depth 16 --read-ratio 0 --warmup 3 \
    --duration $((SECS + 4)) > "$OUT/kvbench.json" &
CPID=$!
sleep 3.5

read_ticks() { awk '{print $14, $15}' "/proc/$SPID/stat"; }
read -r U0 S0 < <(read_ticks)
perf record -e cpu-clock -F 4999 -g -p "$SPID" -o "$OUT/perf.data" -- sleep "$SECS" 2> /dev/null
read -r U1 S1 < <(read_ticks)
wait $CPID

HZ=$(getconf CLK_TCK)
OPS=$(grep -o '"ops_per_sec":[0-9.]*' "$OUT/kvbench.json" | tail -1 | cut -d: -f2)
echo "SET/s: $OPS"
awk -v u0="$U0" -v s0="$S0" -v u1="$U1" -v s1="$S1" -v hz="$HZ" -v ops="$OPS" -v secs="$SECS" 'BEGIN {
    u = (u1 - u0) / hz; s = (s1 - s0) / hz; reqs = ops * secs;
    printf "server CPU per SET: %.0f ns (user %.0f, kernel %.0f); cores busy %.2f\n",
           (u + s) * 1e9 / reqs, u * 1e9 / reqs, s * 1e9 / reqs, (u + s) / secs }'
perf report -i "$OUT/perf.data" --no-children --sort symbol --stdio 2> /dev/null \
    | grep -E '^ +[0-9]' | head -40
