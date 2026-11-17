#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run export_probe (see export_probe.c) inside the
# kvasm-dev container:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvasm-dev bash /src/benchmarks/results/2026-09-29-export-probe/run.sh [CPU] [REPS]
#
# Why a scratch build directory: other sessions share /src/server/build, and
# this probe must neither read stale objects from there nor leave anything
# behind in the repo. The store modules are assembled fresh from
# /src/server/src with the server's own NASM flags, so the probe measures
# exactly the code on the checked-out revision.
#
# Output: one block per key count (1M, 4M, 16M) on stdout; output.txt next to
# this file is a copy of the run the proposal cites. Needs ~3.5 GB of free
# memory for the 16M-key run.
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/export-probe.XXXXXX)"
# Removes the scratch build directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I "$SRC/tests" \
  -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
  -o "$OUT/export_probe" "$HERE/export_probe.c" "$OUT"/*.o -pthread

# The container has no git: the caller passes the revision it checked out
# (docker exec -e REV="$(git rev-parse --short HEAD)" ...).
echo "revision ${REV:-unknown}"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
for keys in 1000000 4000000 16000000; do
  echo
  "$OUT/export_probe" --keys "$keys" --cpu "$CPU" --reps "$REPS"
done
