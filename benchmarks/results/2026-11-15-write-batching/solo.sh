#!/usr/bin/env bash
# =============================================================================
# solo.sh — one kvnode alone with the whole client: absolute write
# throughput, for the headline numbers abw.py's side-by-side runs cannot give
# (there each variant gets half of every core, and a fast variant's client
# saturates first).
#
# Usage (inside the kvnode-dev container):
#   solo.sh ROUNDS WORKERS READ_RATIO BINARY_A BINARY_B
# Runs A, B, A, B, ... (ROUNDS pairs, order alternating per round so drift
# and position effects cancel), each: a fresh server with WORKERS workers on
# their own cores (vCPUs 2,4,6,8 for 4; vCPU 2 for 1), 1M keys preloaded,
# then kvbench on all 16 client vCPUs (16-31), 64 connections x 16 in
# flight, 3 s warm-up, 10 s measured. Prints per run: requests/s, server CPU
# per request, client CPU busy; then per variant the median and B/A per pair.
# Sequential runs feel the host's load changes; compare within one session.
# =============================================================================
set -euo pipefail

ROUNDS=${1:?usage: solo.sh ROUNDS WORKERS READ_RATIO BINARY_A BINARY_B}
WORKERS=${2:?}
RATIO=${3:?}
BIN_A=${4:?}
BIN_B=${5:?}
PORT=50297
KVB=/src/benchmarks/kvbench/build/kvbench
HZ=$(getconf CLK_TCK)
case "$WORKERS" in
    1) SCPU=2 ;;
    4) SCPU=2,4,6,8 ;;
    *) echo "WORKERS must be 1 or 4" >&2; exit 2 ;;
esac

# run BINARY -> prints "ops ns_per_req client_busy"
run() {
    local bin=$1 log=/tmp/solo.log
    KVNODE_PORT=$PORT KVNODE_WORKERS=$WORKERS KVNODE_CAPACITY=1250000 \
        taskset -c "$SCPU" "$bin" 2> "$log" &
    local spid=$!
    for _ in $(seq 1 200); do grep -q 'msg="listening"' "$log" && break; sleep 0.05; done
    taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
        --load-only --settle 0.2 --threads 8 --connections 16 > /dev/null
    taskset -c 16-31 "$KVB" --protocol grpc --port $PORT --keys 1000000 --value-size 100 \
        --threads 16 --connections 64 --depth 16 --read-ratio "$RATIO" --warmup 3 \
        --duration 10 > /tmp/solo.json &
    local cpid=$!
    sleep 3.3
    local s0 c0 s1 c1 t0 t1
    s0=$(awk '{print $14 + $15}' "/proc/$spid/stat"); c0=$(awk '{print $14 + $15}' "/proc/$cpid/stat")
    t0=$(date +%s.%N)
    sleep 9
    s1=$(awk '{print $14 + $15}' "/proc/$spid/stat"); c1=$(awk '{print $14 + $15}' "/proc/$cpid/stat")
    t1=$(date +%s.%N)
    wait $cpid
    kill -TERM $spid; wait $spid || true
    local ops
    ops=$(grep -o '"ops_per_sec":[0-9.]*' /tmp/solo.json | tail -1 | cut -d: -f2)
    awk -v ops="$ops" -v s="$((s1 - s0))" -v c="$((c1 - c0))" -v hz="$HZ" -v t0="$t0" -v t1="$t1" \
        'BEGIN { dt = t1 - t0; printf "%.0f %.0f %.2f\n", ops, s / hz * 1e9 / (ops * dt), c / hz / dt / 16 }'
}

declare -a A B
for r in $(seq 1 "$ROUNDS"); do
    if (( r % 2 )); then order="A B"; else order="B A"; fi
    for v in $order; do
        if [ "$v" = A ]; then out=$(run "$BIN_A"); A+=("$out"); else out=$(run "$BIN_B"); B+=("$out"); fi
        read -r ops ns cb <<< "$out"
        printf "round %d %s: %10.0f req/s  %5.0f ns/req  client busy %.2f\n" "$r" "$v" "$ops" "$ns" "$cb"
    done
done
printf "%s\n" "${A[@]}" > /tmp/solo.A
printf "%s\n" "${B[@]}" > /tmp/solo.B
python3 - "$WORKERS" "$RATIO" <<'EOF'
import math, statistics, sys
a = [list(map(float, l.split())) for l in open("/tmp/solo.A")]
b = [list(map(float, l.split())) for l in open("/tmp/solo.B")]
r = [y[0] / x[0] for x, y in zip(a, b)]
print(f"== {sys.argv[1]} workers, read ratio {sys.argv[2]}: "
      f"A median {statistics.median(x[0] for x in a):,.0f} req/s ({statistics.median(x[1] for x in a):,.0f} ns/req), "
      f"B median {statistics.median(x[0] for x in b):,.0f} req/s ({statistics.median(x[1] for x in b):,.0f} ns/req), "
      f"B/A per pair {', '.join(f'{x:.2f}' for x in r)} (geomean {math.exp(statistics.mean(map(math.log, r))):.2f}); "
      f"client busy A {statistics.median(x[2] for x in a):.2f} B {statistics.median(x[2] for x in b):.2f}")
EOF
