#!/usr/bin/env bash
# Runs inside kvasm-dev: one KvAsm worker pinned to CPU $CPU under E1's GET
# load (1M keys, 100 B values, 64 x 16), long enough for an external perf
# window. Writes /tmp/prof.pid, /tmp/prof.ready, /tmp/prof.stat0/1 and the
# kvbench result to /tmp/prof.json.
# Env: CPU (default 2), SECS (measured seconds, default 30), PING (1 = no-op).
set -u
SRV=/src/server/build/kvasm
KVB=/src/benchmarks/kvbench/build/kvbench
PORT=50181
CPU=${CPU:-2}
SECS=${SECS:-30}
rm -f /tmp/prof.*
pkill -x kvasm 2>/dev/null; sleep 0.3
KVASM_PORT=$PORT KVASM_WORKERS=1 KVASM_CAPACITY=1250000 taskset -c "$CPU" $SRV 2>/tmp/prof.srv.log &
SPID=$!
for _ in $(seq 100); do grep -q 'msg="listening"' /tmp/prof.srv.log && break; sleep 0.05; done
echo "$SPID" > /tmp/prof.pid
taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --load-only > /tmp/prof.preload.json || exit 1
EXTRA=
[ "${PING:-0}" = 1 ] && EXTRA=--ping
[ "${BUSY:-0}" = 1 ] && EXTRA="$EXTRA --busy-poll"
taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --threads 8 --connections 64 --depth 16 \
  --warmup 3 --duration "$SECS" $EXTRA > /tmp/prof.json &
BPID=$!
sleep 4
awk '{print $14, $15}' /proc/"$SPID"/stat > /tmp/prof.stat0
date +%s.%N > /tmp/prof.ready
wait $BPID
awk '{print $14, $15}' /proc/"$SPID"/stat > /tmp/prof.stat1
date +%s.%N > /tmp/prof.done
kill "$SPID"
