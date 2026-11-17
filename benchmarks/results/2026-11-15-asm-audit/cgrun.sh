#!/usr/bin/env bash
# =============================================================================
# cgrun.sh — one callgrind measurement of a server binary with a free
# workload shape: exact user-space instructions, per instruction address.
#
# Usage (from Git Bash on the host):
#   AUDIT_WORK=<host dir> cgrun.sh BIN TAG KEYS VALUE_SIZE "LOAD_ARGS" "RUN_ARGS" [SECONDS]
#   AUDIT_WORK  host directory holding the binaries (mounted at /pc in the
#               helper); the dump is written there too
#   BIN         file name of the server binary inside AUDIT_WORK
#   TAG         names the dump: AUDIT_WORK/cg-BIN-TAG.out.1
#   KEYS        key space (preloaded before the counters are zeroed)
#   VALUE_SIZE  value bytes
#   LOAD_ARGS   extra kvbench arguments for the preload (e.g. "--upload")
#   RUN_ARGS    extra kvbench arguments for the measured run
#               (e.g. "--depth 1", "--read-ratio 0", "--ping", "--download")
#   SECONDS     measured seconds (default 8)
#
# How it runs: the server under callgrind in the kvasm-valgrind helper image
# (sharing kvasm-dev's network namespace, one worker), kvbench in kvasm-dev.
# Counters are zeroed after the preload (callgrind_control -z, which needs
# SYS_PTRACE), so the dump holds the workload only; --dump-instr=yes keeps a
# count per instruction, which cgannot.py turns into per-function and
# per-source-line tables.
# Why callgrind: instruction counts are exact and do not depend on how busy
# the host is — the only instrument that works while other benchmarks run.
# What it cannot see: kernel time and memory stalls (use getcost.sh, bench_h2).
# Pinning: the helper on vCPUs 12-13 and kvbench on 14-15 keep it off the
# vCPUs other sessions' benchmarks use (2-5 and 16-31 at the time).
# =============================================================================
set -u
BIN=$1; TAG=$2; KEYS=$3; VS=$4; LOADARGS=$5; RUNARGS=$6; SECS=${7:-8}
WORK=${AUDIT_WORK:?set AUDIT_WORK to the host directory holding the binaries}
PORT=${AUDIT_PORT:-50351}
export MSYS_NO_PATHCONV=1
OUT=cg-$BIN-$TAG.out
docker rm -f kvasm-cg2 > /dev/null 2>&1
rm -f "$WORK/$OUT" "$WORK/$OUT".*
docker run -d --name kvasm-cg2 --cpuset-cpus=12-13 --cap-add=SYS_PTRACE --security-opt seccomp=unconfined \
  --net=container:kvasm-dev -v "$WORK:/pc" \
  -e KVASM_PORT="$PORT" -e KVASM_WORKERS=1 -e KVASM_CAPACITY=40000 kvasm-valgrind:local \
  valgrind --tool=callgrind --dump-instr=yes --callgrind-out-file=/pc/"$OUT" \
  /pc/"$BIN" > /dev/null
for _ in $(seq 120); do docker logs kvasm-cg2 2>&1 | grep -q 'msg="listening"' && break; sleep 0.5; done
KVB="taskset -c 14-15 /src/benchmarks/kvbench/build/kvbench --protocol grpc --port $PORT --keys $KEYS --value-size $VS --wait-ready 60"
docker exec kvasm-dev bash -c "$KVB --load-only --threads 2 --connections 4 $LOADARGS > /dev/null"
docker exec kvasm-cg2 callgrind_control -z > /dev/null 2>&1
docker exec kvasm-dev bash -c "$KVB --threads 2 --connections 8 --depth 16 $RUNARGS --warmup 0 --duration $SECS > /tmp/cgrun.json; grep -o '\"ops_per_sec\":[0-9.]*' /tmp/cgrun.json | tail -1; grep -o '\"errors\":[0-9]*' /tmp/cgrun.json | tail -1"
docker exec kvasm-cg2 callgrind_control -d > /dev/null 2>&1
sleep 1
docker stop -t 20 kvasm-cg2 > /dev/null
docker rm kvasm-cg2 > /dev/null
ls -la "$WORK/" | grep "$OUT"
