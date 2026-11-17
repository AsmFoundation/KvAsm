#!/usr/bin/env bash
# =============================================================================
# abpair.sh — two server builds measured AT THE SAME TIME, slots swapped every
# block: the concurrent paired A/B that cancels load that varies over time
# (other benchmarks, VMs on the host), which sequential runs cannot.
#
# Usage (inside kvasm-dev):
#   abpair.sh BIN_A BIN_B BLOCKS KEYS VALUE_SIZE "KVBENCH_RUN_ARGS"
# Environment (defaults in parentheses):
#   AB_SCPU0 / AB_SCPU1   server vCPU of slot 0 / slot 1          (12 / 13)
#   AB_CCPU0 / AB_CCPU1   client vCPU list of slot 0 / slot 1     (5,14 / 11,15)
#   AB_THREADS            kvbench threads per side                (2)
#   AB_CONNS              kvbench connections per side            (8)
#   AB_SECS               measured seconds per block              (5)
# Block b puts A in slot b%2 and B in the other. Each side gets the same
# client; the metric is the server's CPU per request (schedstat, ns-exact),
# which does not depend on which side's client happened to be faster. When
# the clients saturate both servers, requests/s is a second, independent
# reading of the same difference.
# Prints one line per block and the geometric means of B/A at the end.
# =============================================================================
set -euo pipefail
A=$1; B=$2; BLOCKS=$3; KEYS=$4; VS=$5; RUNARGS=$6
KVB=/src/benchmarks/kvbench/build/kvbench
SCPU=("${AB_SCPU0:-12}" "${AB_SCPU1:-13}")
CCPU=("${AB_CCPU0:-5,14}" "${AB_CCPU1:-11,15}")
THREADS=${AB_THREADS:-2}; CONNS=${AB_CONNS:-8}; SECS=${AB_SECS:-5}
PORT=(50355 50356)

run_side() {  # slot binary tag: start a server, preload it, remember its pid
  local s=$1 bin=$2 tag=$3
  KVASM_PORT=${PORT[$s]} KVASM_WORKERS=1 KVASM_CAPACITY=$((KEYS * 2 + 1000)) \
      taskset -c "${SCPU[$s]}" "$bin" 2> /tmp/abpair.$tag.log &
  local spid=$!
  for _ in $(seq 1 200); do grep -q 'msg="listening"' /tmp/abpair.$tag.log && break; sleep 0.05; done
  taskset -c "${CCPU[$s]}" "$KVB" --protocol grpc --port "${PORT[$s]}" --keys "$KEYS" --value-size "$VS" \
      --load-only --settle 0.2 --threads 2 --connections 4 > /dev/null
  echo "$spid" > /tmp/abpair.$tag.pid
}
measure_side() {  # slot tag: the measured run against that side's server
  local s=$1 tag=$2
  taskset -c "${CCPU[$s]}" "$KVB" --protocol grpc --port "${PORT[$s]}" --keys "$KEYS" --value-size "$VS" \
      --threads "$THREADS" --connections "$CONNS" --depth 16 $RUNARGS --warmup 1 --duration "$SECS" \
      --server-pid "$(cat /tmp/abpair.$tag.pid)" > /tmp/abpair.$tag.json
}

LINES=""
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
busy = {t: r[t]["server_cpu_s"] / r[t]["seconds"] for t in "AB"}
print(f"block {b} (A in slot {sa}): A {r['A']['ops_per_sec']:9,.0f} req/s {cpu['A']:6.0f} ns/req busy {busy['A']:.2f} | "
      f"B {r['B']['ops_per_sec']:9,.0f} req/s {cpu['B']:6.0f} ns/req busy {busy['B']:.2f} | "
      f"B/A CPU {cpu['B'] / cpu['A']:.3f} req/s {r['B']['ops_per_sec'] / r['A']['ops_per_sec']:.3f} "
      f"errors {r['A']['errors'] + r['B']['errors']} bad {r['A']['bad_values'] + r['B']['bad_values']}")
EOF
)
  echo "$line"
  LINES+="$line"$'\n'
done
python3 - <<EOF
import math, re
cpu, rps = [], []
for line in """$LINES""".strip().split("\n"):
    m = re.search(r"B/A CPU ([0-9.]+) req/s ([0-9.]+)", line)
    cpu.append(float(m.group(1))); rps.append(float(m.group(2)))
g = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
print(f"B/A server CPU per request: geometric mean {g(cpu):.3f} over {len(cpu)} blocks; "
      f"B cheaper in {sum(1 for x in cpu if x < 1)}/{len(cpu)}; range {min(cpu):.3f}-{max(cpu):.3f}")
print(f"B/A requests per second:    geometric mean {g(rps):.3f}; B faster in {sum(1 for x in rps if x > 1)}/{len(rps)}; "
      f"range {min(rps):.3f}-{max(rps):.3f}")
EOF
