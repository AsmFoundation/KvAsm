#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run endpoints_probe inside kvnode-dev:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvnode-dev bash /src/benchmarks/results/2026-10-01-endpoints-probe/run.sh [CPU] [REPS]
#
# What it does, and why this way:
#   * Assembles the store modules and the server's CRC-32C (crc32c.asm) fresh
#     from /src/server/src with the server's NASM flags into a scratch
#     directory: the probe measures the checked-out code (the ETag rows call
#     the server's own folding CRC) and never touches /src/server/build,
#     which other sessions share.
#   * Compiles with the golden probe's flags (-O2, -Werror). SIMD paths use
#     per-function target attributes; the probe checks for AVX2, BMI1,
#     SSE4.2, PCLMULQDQ and SHA-NI before it starts.
#   * Runs at 10M keys on one pinned P-core (threads on CPUs 1-7: distinct
#     P-cores; vCPU 0 carries VM interrupt work). Needs ~6 GB of free memory;
#     a full run takes a few minutes.
#   * QUICK=1 runs a 1M-key smoke test with one repetition instead (checks
#     every structure, timings meaningless).
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/endpoints-probe.XXXXXX)"
# Removes the scratch directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store crc32c; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I "$SRC/tests" \
  -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
  -o "$OUT/endpoints_probe" "$HERE/endpoints_probe.c" "$OUT"/*.o -pthread -lm

# The container has no git: the caller passes the checked-out revision.
echo "revision ${REV:-unknown}; gcc $(gcc -dumpfullversion); nasm $(nasm -v | awk '{print $3}')"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
if [ "${QUICK:-0}" = 1 ]; then
  echo
  echo "== endpoints_probe --keys 1000000 (smoke test) =="
  "$OUT/endpoints_probe" --keys 1000000 --cpu "$CPU" --reps 1
  exit 0
fi
echo
echo "== endpoints_probe --keys 10000000 =="
"$OUT/endpoints_probe" --keys 10000000 --cpu "$CPU" --reps "$REPS"
