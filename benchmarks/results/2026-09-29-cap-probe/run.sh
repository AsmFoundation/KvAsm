#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run cap_probe inside kvasm-dev:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvasm-dev bash /src/benchmarks/results/2026-09-29-cap-probe/run.sh [CPU] [REPS]
#
# Assembles the store modules fresh from /src/server/src with the server's
# NASM flags into a scratch directory (never touching /src/server/build,
# which other sessions share), builds cap_probe, and runs it at 100k keys
# (the question) and 1M keys (to show how each capability scales). The
# probe's AVX2 routine is compiled for AVX2 only inside that function
# (target attribute) and the probe refuses to run without AVX2.
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-7}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/cap-probe.XXXXXX)"
# Removes the scratch directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I "$SRC/tests" \
  -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
  -o "$OUT/cap_probe" "$HERE/cap_probe.c" "$OUT"/*.o -pthread

# The container has no git: the caller passes the checked-out revision.
echo "revision ${REV:-unknown}"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
for keys in 100000 1000000; do
  echo
  echo "== cap_probe --keys $keys =="
  "$OUT/cap_probe" --keys "$keys" --cpu "$CPU" --reps "$REPS"
done
