#!/usr/bin/env bash
# =============================================================================
# abpair.sh — two server builds measured AT THE SAME TIME, slots swapped every
# block: the concurrent paired A/B that cancels load that varies over time
# (other benchmarks, VMs on the host), which sequential runs cannot.
#
# Usage (inside kvnode-dev):
#   abpair.sh BIN_A BIN_B BLOCKS KEYS VALUE_SIZE "KVBENCH_RUN_ARGS"
# Slot 0: server on vCPU 12, client on vCPUs 5,14, port 50355.
# Slot 1: server on vCPU 13, client on vCPUs 11,15, port 50356.
# Block b puts A in slot b%2 and B in the other. Each side gets the same
# client; the metric is the server's CPU per request (schedstat, ns-exact),
# which does not depend on which side's client happened to be faster.
# Prints one line per block and the geometric mean of B/A at the end.
# =============================================================================
set -euo pipefail
A=$1; B=$2; BLOCKS=$3; KEYS=$4; VS=$5; RUNARGS=$6
KVB=/src/benchmarks/kvbench/build/kvbench
SCPU=(12 13); CCPU=("5,14" "11,15"); PORT=(50355 50356)

run_side() {  # slot binary tag -> writes /tmp/abpair.$tag.json
  local s=$1 bin=$2 tag=$3
  KVNODE_PORT=${PORT[$s]} KVNODE_WORKERS=1 KVNODE_CAPACITY=$((KEYS * 2 + 1000)) \
      taskset -c "${SCPU[$s]}" "$bin" 2> /tmp/abpair.$tag.log &
  local spid=$!
  for _ in $(seq 1 200); do grep -q 'msg="listening"' /tmp/abpair.$tag.log && break; sleep 0.05; done
  taskset -c "${CCPU[$s]}" "$KVB" --protocol grpc --port "${PORT[$s]}" --keys "$KEYS" --value-size "$VS" \
      --load-only --settle 0.2 --threads 2 --connections 4 > /dev/null
  echo "$spid" > /tmp/abpair.$tag.pid
}
measure_side() {  # slot tag
  local s=$1 tag=$2
  taskset -c "${CCPU[$s]}" "$KVB" --protocol grpc --port "${PORT[$s]}" --keys "$KEYS" --value-size "$VS" \
      --threads 2 --connections 8 --depth 16 $RUNARGS --warmup 1 --duration 5 \
      --server-pid "$(cat /tmp/abpair.$tag.pid)" > /tmp/abpair.$tag.json
}

RATIOS=""
for b in $(seq 0 $((BLOCKS - 1))); do
  sa=$((b % 2)); sb=$((1 - sa))
  run_side $sa "$A" A; run_side $sb "$B" B
  measure_side $sa A & pa=$!
  measure_side $sb B & pb=$!
  wait $pa $pb
  kill -TERM "$(cat /tmp/abpair.A.pid)" "$(cat /tmp/abpair.B.pid)" 2>/dev/null || true
  wait 2>/dev/null || true
  line=$(python3 - "$b" "$sa" <<'EOF'
import json, sys
b, sa = sys.argv[1], sys.argv[2]
r = {t: json.load(open(f"/tmp/abpair.{t}.json"))["result"] for t in "AB"}
cpu = {t: r[t]["server_cpu_s"] / r[t]["ops"] * 1e9 for t in "AB"}
print(f"block {b} (A in slot {sa}): A {r['A']['ops_per_sec']:9,.0f} req/s {cpu['A']:6.0f} ns/req | "
      f"B {r['B']['ops_per_sec']:9,.0f} req/s {cpu['B']:6.0f} ns/req | B/A CPU {cpu['B'] / cpu['A']:.3f} "
      f"errors {r['A']['errors'] + r['B']['errors']} bad {r['A']['bad_values'] + r['B']['bad_values']}")
EOF
)
  echo "$line"
  RATIOS+="$(echo "$line" | sed -E 's/.*B\/A CPU ([0-9.]+).*/\1/') "
done
python3 - $RATIOS <<'EOF'
import math, sys
xs = list(map(float, sys.argv[1:]))
g = math.exp(sum(map(math.log, xs)) / len(xs))
print(f"B/A server CPU per request: geometric mean {g:.3f} over {len(xs)} blocks; "
      f"B cheaper in {sum(1 for x in xs if x < 1)}/{len(xs)}; range {min(xs):.3f}-{max(xs):.3f}")
EOF
