#!/usr/bin/env bash
# Does GLIBC_TUNABLES put KvAsm's records on transparent huge pages?
# Starts the committed server (1 worker) with and without the tunables,
# preloads 1M keys x 100 B through kvbench, and prints resident and huge
# memory from smaps_rollup. Runs inside kvasm-dev.
set -u
SRV=/tmp/head/server/build/kvasm
KVB=/src/benchmarks/kvbench/build/kvbench
PORT=50231
TUN="glibc.malloc.arena_max=1:glibc.malloc.hugetlb=1:glibc.malloc.top_pad=268435456"
for mode in default thp; do
  if [ "$mode" = thp ]; then T="$TUN"; else T=""; fi
  env GLIBC_TUNABLES="$T" KVASM_PORT=$PORT KVASM_WORKERS=1 KVASM_CAPACITY=1250000 \
    taskset -c 2 $SRV 2>/tmp/thp.log &
  SPID=$!
  for _ in $(seq 100); do grep -q 'msg="listening"' /tmp/thp.log && break; sleep 0.05; done
  taskset -c 16-31 $KVB --protocol grpc --port $PORT --keys 1000000 --load-only > /dev/null || exit 1
  sleep 1
  echo "$mode: $(grep -E '^(Rss|AnonHugePages):' /proc/$SPID/smaps_rollup | tr -s ' ' | tr '\n' ' ')"
  kill $SPID
  wait $SPID 2>/dev/null
done
