#!/usr/bin/env bash
# =============================================================================
# abseq.sh — two server builds measured in ONE slot, alternating in a balanced
# order (ABBA ABBA ...), for hosts where no two vCPU sets are equivalent.
#
# Usage (inside kvasm-dev):
#   abseq.sh BIN_A BIN_B QUADS KEYS VALUE_SIZE "KVBENCH_RUN_ARGS"
# Environment (defaults in parentheses):
#   AB_SCPU     server vCPU                         (2)
#   AB_CCPU     client vCPU list                    (16-23)
#   AB_THREADS  kvbench threads                     (8)
#   AB_CONNS    kvbench connections                 (32)
#   AB_SECS     measured seconds per run            (4)
# Runs QUADS x (A B B A): within a quad each build is measured once early and
# once late, so a load that drifts over the quad cancels in the quad's ratio
#   (B1 + B2) / (A1 + A2)      on server CPU per request (schedstat, ns-exact).
# When to use it instead of abpair.sh: when the two slots of a concurrent
# pair differ (here one slot ran 2-3x slower under busy-polling clients), a
# concurrent pair measures the slots, not the builds. The price: load that
# changes faster than a quad (~40 s) is not cancelled — check the quads'
# spread before trusting the mean.
# =============================================================================
set -euo pipefail
A=$1; B=$2; QUADS=$3; KEYS=$4; VS=$5; RUNARGS=$6
KVB=/src/benchmarks/kvbench/build/kvbench
SCPU=${AB_SCPU:-2}; CCPU=${AB_CCPU:-16-23}
THREADS=${AB_THREADS:-8}; CONNS=${AB_CONNS:-32}; SECS=${AB_SECS:-4}
PORT=50357

one() {  # binary -> "ops_per_sec cpu_ns_per_req busy errors bad"
  local bin=$1
  KVASM_PORT=$PORT KVASM_WORKERS=1 KVASM_CAPACITY=$((KEYS * 2 + 1000)) \
      taskset -c "$SCPU" "$bin" 2> /tmp/abseq.log &
  local spid=$!
  for _ in $(seq 1 200); do grep -q 'msg="listening"' /tmp/abseq.log && break; sleep 0.05; done
  taskset -c "$CCPU" "$KVB" --protocol grpc --port $PORT --keys "$KEYS" --value-size "$VS" \
      --load-only --settle 0.2 --threads 4 --connections 16 > /dev/null
  taskset -c "$CCPU" "$KVB" --protocol grpc --port $PORT --keys "$KEYS" --value-size "$VS" \
      --threads "$THREADS" --connections "$CONNS" --depth 16 $RUNARGS --warmup 1 --duration "$SECS" \
      --server-pid "$spid" > /tmp/abseq.json
  kill -TERM "$spid" 2>/dev/null || true
  wait "$spid" 2>/dev/null || true
  python3 -c '
import json
r = json.load(open("/tmp/abseq.json"))["result"]
print(r["ops_per_sec"], r["server_cpu_s"] / r["ops"] * 1e9, r["server_cpu_s"] / r["seconds"], r["errors"], r["bad_values"])'
}

ROWS=""
for q in $(seq 1 "$QUADS"); do
  for v in A B B A; do
    if [ "$v" = A ]; then res=$(one "$A"); else res=$(one "$B"); fi
    ROWS+="$q $v $res"$'\n'
  done
done
python3 - <<EOF
import math
rows = [l.split() for l in """$ROWS""".strip().split("\n")]
quads = {}
for q, v, ops, cpu, busy, err, bad in rows:
    quads.setdefault(int(q), {"A": [], "B": []})[v].append((float(ops), float(cpu), float(busy), int(err) + int(bad)))
cr, rr = [], []
for q in sorted(quads):
    a, b = quads[q]["A"], quads[q]["B"]
    ca, cb = sum(x[1] for x in a) / 2, sum(x[1] for x in b) / 2
    oa, ob = sum(x[0] for x in a) / 2, sum(x[0] for x in b) / 2
    cr.append(cb / ca); rr.append(ob / oa)
    print(f"quad {q}: A {oa:9,.0f} req/s {ca:6.0f} ns/req (runs {a[0][1]:.0f}, {a[1][1]:.0f}; busy {a[0][2]:.2f}) | "
          f"B {ob:9,.0f} req/s {cb:6.0f} ns/req (runs {b[0][1]:.0f}, {b[1][1]:.0f}; busy {b[0][2]:.2f}) | "
          f"B/A CPU {cb / ca:.3f} req/s {ob / oa:.3f} errors+bad {sum(x[3] for x in a + b)}")
g = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
print(f"B/A server CPU per request: geometric mean {g(cr):.3f} over {len(cr)} quads; B cheaper in "
      f"{sum(1 for x in cr if x < 1)}/{len(cr)}; range {min(cr):.3f}-{max(cr):.3f}")
print(f"B/A requests per second:    geometric mean {g(rr):.3f}; B faster in {sum(1 for x in rr if x > 1)}/{len(rr)}; "
      f"range {min(rr):.3f}-{max(rr):.3f}")
EOF
