#!/usr/bin/env bash
# =============================================================================
# faults_set.sh — page faults and syscalls behind a SET: minor/major faults
# per request, read from /proc/PID/stat around a measured window, with and
# without extra environment (e.g. GLIBC_TUNABLES), to test whether glibc's
# heap trimming makes record memory churn through the kernel.
#
# Usage (inside the kvasm-dev container):
#   [READ_RATIO=r] faults_set.sh BINARY [ENV=VALUE ...]   (READ_RATIO: kvbench
#   --read-ratio, default 0 = 100% SET; 1 = 100% GET, the comparison)
# One worker on vCPU 2, the same 100% SET load as profile_set.sh, 8 s window.
# Why /proc counters: they cost nothing and count exactly; a profiler cannot
# see kernel symbols in the dev container (perf_event_paranoid=2).
# =============================================================================
set -euo pipefail

BIN=${1:?usage: faults_set.sh BINARY [ENV=VALUE ...]}
shift
PORT=50298
KVB=/src/benchmarks/kvbench/build/kvbench
LOG=/tmp/faults_set.log

env "$@" KVASM_PORT=$PORT KVASM_WORKERS=1 KVASM_CAPACITY=1250000 \
    taskset -c 2 "$BIN" 2> "$LOG" &
SPID=$!
trap 'kill -TERM $SPID 2>/dev/null || true; wait $SPID 2>/dev/null || true' EXIT
for _ in $(seq 1 200); do grep -q 'msg="listening"' "$LOG" && break; sleep 0.05; done

taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
    --load-only --settle 0.2 --threads 8 --connections 16 > /dev/null
taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
    --threads 16 --connections 64 --depth 16 --read-ratio "${READ_RATIO:-0}" --warmup 3 --duration 12 \
    > /tmp/faults_set.json &
CPID=$!
sleep 3.5
# /proc/PID/stat fields: 10 minflt, 12 majflt, 14 utime, 15 stime.
read -r F0 M0 U0 S0 < <(awk '{print $10, $12, $14, $15}' "/proc/$SPID/stat")
RSS0=$(awk '/^Rss:/ {print $2}' "/proc/$SPID/smaps_rollup")
sleep 8
read -r F1 M1 U1 S1 < <(awk '{print $10, $12, $14, $15}' "/proc/$SPID/stat")
RSS1=$(awk '/^Rss:/ {print $2}' "/proc/$SPID/smaps_rollup")
wait $CPID
OPS=$(grep -o '"ops_per_sec":[0-9.]*' /tmp/faults_set.json | tail -1 | cut -d: -f2)
HZ=$(getconf CLK_TCK)
awk -v f="$((F1 - F0))" -v m="$((M1 - M0))" -v u="$((U1 - U0))" -v s="$((S1 - S0))" \
    -v hz="$HZ" -v ops="$OPS" -v r0="$RSS0" -v r1="$RSS1" -v env="$*" -v rr="${READ_RATIO:-0}" 'BEGIN {
    reqs = ops * 8;
    printf "read-ratio %s env [%s]: %.0f req/s; minor faults %.3f per request, major %d; CPU per request %.0f ns (user %.0f, kernel %.0f); RSS %d -> %d MiB\n",
           rr, env, ops, f / reqs, m, (u + s) / hz * 1e9 / reqs, u / hz * 1e9 / reqs, s / hz * 1e9 / reqs, r0 / 1024, r1 / 1024 }'
