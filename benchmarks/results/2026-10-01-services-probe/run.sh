#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run services_probe inside kvasm-dev:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvasm-dev bash /src/benchmarks/results/2026-10-01-services-probe/run.sh [CPU] [REPS]
#
# What it does, and why this way:
#   * Assembles the store modules fresh from /src/server/src with the
#     server's NASM flags into a scratch directory, so the probe measures
#     the checked-out store code and never touches /src/server/build (other
#     sessions share it).
#   * Compiles the probe with the golden probe's flags (-O2, -Werror). The
#     SIMD paths use per-function target attributes, so the binary runs its
#     baseline code on any x86-64 and the probe itself checks for AVX2, FMA,
#     POPCNT, BMI2 and SSE4.2 before it starts; AVX-VNNI and WAITPKG rows
#     are skipped (and say so) where the CPU or the hypervisor lacks them.
#   * Runs the whole probe at 10M keys on one pinned P-core (threads on
#     CPUs 1-7: distinct P-cores; vCPU 0 carries VM interrupt work). Needs
#     ~5 GB of free memory; a full run takes a few minutes.
#   * QUICK=1 runs a 1M-key smoke test with one repetition instead (checks
#     every structure, timings meaningless).
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/services-probe.XXXXXX)"
# Removes the scratch directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I "$SRC/tests" \
  -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
  -o "$OUT/services_probe" "$HERE/services_probe.c" "$OUT"/*.o -pthread -lm

# The container has no git: the caller passes the checked-out revision.
echo "revision ${REV:-unknown}; gcc $(gcc -dumpfullversion); nasm $(nasm -v | awk '{print $3}')"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
if [ "${QUICK:-0}" = 1 ]; then
  echo
  echo "== services_probe --keys 1000000 (smoke test) =="
  "$OUT/services_probe" --keys 1000000 --cpu "$CPU" --reps 1 --threads 1,2
  exit 0
fi
echo
echo "== services_probe --keys 10000000 =="
"$OUT/services_probe" --keys 10000000 --cpu "$CPU" --reps "$REPS" --threads 1,2,4,7
