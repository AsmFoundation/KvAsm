#!/usr/bin/env bash
# Runs inside kvnode-dev: one worker pinned to vCPU 2 under E1's GET load,
# then samples, over a 5 s window inside the measured phase:
#   * the server's and the client's voluntary context switches (summed over
#     threads): each one is a thread that blocked (epoll_wait) and had to be
#     woken again;
#   * requests completed (kvbench's ops/s x window).
# Signals /tmp/lp.ready when the window starts (an external perf stat can
# attach to /tmp/lp.pid then) and writes /tmp/lp.result at the end.
# Env: SRV (binary, default build/kvnode), THREADS (16), CONNS (64),
#      BUSY (1 = busy-polling client).
set -u
SRV=${SRV:-/src/server/build/kvnode}
KVB=/src/benchmarks/kvbench/build/kvbench
PORT=50231
THREADS=${THREADS:-16}
CONNS=${CONNS:-64}
rm -f /tmp/lp.*
KVNODE_PORT=$PORT KVNODE_WORKERS=1 KVNODE_CAPACITY=1250000 taskset -c 2 "$SRV" 2>/tmp/lp.srv.log &
SPID=$!
for _ in $(seq 200); do grep -q 'msg="listening"' /tmp/lp.srv.log && break; sleep 0.05; done
echo "$SPID" > /tmp/lp.pid
taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --load-only > /dev/null || exit 1
EXTRA=
[ "${BUSY:-0}" = 1 ] && EXTRA=--busy-poll
taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --threads "$THREADS" \
  --connections "$CONNS" --depth 16 --warmup 3 --duration 12 $EXTRA > /tmp/lp.json &
CPID=$!
vsw() {  # voluntary context switches summed over a process's threads
  local t=0
  for f in /proc/"$1"/task/*/status; do
    t=$((t + $(awk '/^voluntary_ctxt_switches/ {print $2}' "$f")))
  done
  echo "$t"
}
sleep 4
s0=$(vsw "$SPID"); c0=$(vsw "$CPID"); t0=$(date +%s.%N)
touch /tmp/lp.ready
sleep 5
s1=$(vsw "$SPID"); c1=$(vsw "$CPID"); t1=$(date +%s.%N)
wait $CPID
OPS=$(grep -o '"ops_per_sec":[0-9.]*' /tmp/lp.json | tail -1 | cut -d: -f2)
python3 - "$OPS" "$s0" "$s1" "$c0" "$c1" "$t0" "$t1" > /tmp/lp.result << 'PY'
import sys
ops, s0, s1, c0, c1, t0, t1 = map(float, sys.argv[1:])
req = ops * (t1 - t0)
print(f"ops/s {ops:,.0f}; per 1,000 requests: server wakeups {(s1 - s0) * 1000 / req:.2f}, "
      f"client wakeups {(c1 - c0) * 1000 / req:.2f}")
PY
kill "$SPID"
