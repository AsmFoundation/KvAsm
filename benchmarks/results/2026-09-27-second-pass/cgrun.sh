#!/usr/bin/env bash
# cgrun.sh VARIANT WORKLOAD — one callgrind measurement (run from Git Bash).
#   VARIANT   B or C: /src/server/build/kvnode-$VARIANT
#   WORKLOAD  get | set | ping
# The server runs under callgrind in the kvnode-valgrind helper, sharing
# kvnode-dev's network namespace; kvbench runs in kvnode-dev. Counters are
# zeroed after the 20k-key preload, the workload runs for 8 s, and the dump
# is written to server/build/cg-$VARIANT-$WORKLOAD.out.
set -u
V=$1
W=$2
PORT=50251
export MSYS_NO_PATHCONV=1
docker rm -f kvnode-cg > /dev/null 2>&1
docker run -d --name kvnode-cg --cap-add=SYS_PTRACE --security-opt seccomp=unconfined --net=container:kvnode-dev -v "K:/source/KvNode:/src" \
  -e KVNODE_PORT=$PORT -e KVNODE_WORKERS=1 -e KVNODE_CAPACITY=40000 kvnode-valgrind:local \
  valgrind --tool=callgrind --callgrind-out-file=/src/server/build/cg-$V-$W.out \
  /src/server/build/kvnode-$V > /dev/null
for _ in $(seq 120); do docker logs kvnode-cg 2>&1 | grep -q 'msg="listening"' && break; sleep 0.5; done
KVB="/src/benchmarks/kvbench/build/kvbench --protocol grpc --port $PORT --keys 20000 --wait-ready 60"
docker exec kvnode-dev bash -c "$KVB --load-only --threads 2 --connections 4 > /dev/null"
docker exec kvnode-cg callgrind_control -z > /dev/null
case $W in
  get)  EXTRA="" ;;
  set)  EXTRA="--read-ratio 0" ;;
  ping) EXTRA="--ping" ;;
esac
docker exec kvnode-dev bash -c "$KVB $EXTRA --threads 2 --connections 8 --depth 16 --warmup 0 --duration 8 > /tmp/cg.json"
docker exec kvnode-cg callgrind_control -d > /dev/null
sleep 1
docker stop -t 20 kvnode-cg > /dev/null
docker rm kvnode-cg > /dev/null
ls /k/source/KvNode/server/build/cg-$V-$W.out* | tail -1
