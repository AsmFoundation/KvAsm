#!/usr/bin/env bash
# build_bench.sh VARIANT — build bench_h2 against one server variant's objects
# (inside kvnode-dev; the variant was built in /pc/VARIANT/server by make).
#   output: /pc/probe/bench_h2-VARIANT
# h2_process and conn_window_refill are internal to h2n.asm: a copy of the
# object gets them made global (objcopy only edits the symbol table), so the
# code under test is byte-for-byte the variant's.
# hashes.o (built by the hashbench step) supplies calib_add, the cycle-time
# calibration; a variant's own aeshash.o is linked when it has one.
set -euo pipefail
V=$1
S=/pc/$V/server
O=$S/build/obj
W=/pc/probe/obj-$V
mkdir -p "$W"
nasm -f elf64 -I "$S/include/" -o "$W/benchabi.o" /pc/probe/benchabi.asm
objcopy --globalize-symbol=h2_process --globalize-symbol=conn_window_refill "$O/h2n.o" "$W/h2n_g.o"
EXTRA=""
if [ -f "$O/aeshash.o" ]; then EXTRA="$O/aeshash.o"; fi
gcc -std=c11 -O2 -g -Wall -Wextra -I "$S/tests" -o /pc/probe/bench_h2-"$V" /pc/probe/bench_h2.c \
    "$W/benchabi.o" "$W/h2n_g.o" "$O/grpc.o" "$O/config.o" \
    "$O/siphash.o" "$O/util.o" "$O/cuckoo.o" "$O/wlock.o" "$O/store.o" "$O/proto.o" "$O/log.o" \
    "$O/snapshot.o" "$O/abi.o" "$O/pool.o" "$O/hcache.o" "$O/hpack.o" "$O/crc32c.o" $EXTRA \
    /pc/probe/hashes.o -pthread
ls -la /pc/probe/bench_h2-"$V"
