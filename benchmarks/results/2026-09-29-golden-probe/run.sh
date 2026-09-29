#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run golden_probe inside kvnode-dev:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvnode-dev bash /src/benchmarks/results/2026-09-29-golden-probe/run.sh [CPU] [REPS]
#
# What it does, and why this way:
#   * Fetches binaryfusefilter.h (FastFilter/xor_singleheader, Apache-2.0)
#     at a pinned commit and refuses to build if its SHA-256 differs, the
#     same rule the Dockerfile applies to every third-party download. It is
#     not committed here: it only lives in the scratch directory, and it is
#     included with -isystem so its own warnings cannot trip -Werror.
#   * Assembles the store modules fresh from /src/server/src with the
#     server's NASM flags into a scratch directory, so the probe measures
#     the checked-out code and never touches /src/server/build (other
#     sessions share it).
#   * Runs the whole probe at 10M keys on one pinned P-core (threads on
#     CPUs 1-7: distinct P-cores; vCPU 0 carries VM interrupt work). Then,
#     unless FULL_ONLY=1, a paired A/B of the bar alone: glibc's default heap
#     against the huge-page heap tunables measured on 2026-09-27
#     (results/2026-09-27-store-lookup), in alternating order over three
#     blocks, since unpaired runs on this host differ by more than the
#     effect. Needs ~7 GB of free memory; a full run takes a few minutes.
#   * QUICK=1 runs a 1M-key smoke test with one repetition instead (checks
#     every structure, timings meaningless).
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/golden-probe.XXXXXX)"
# Removes the scratch directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

BFF_SHA=3e10fb185a0c51e5866518d13b403e07db63d8d8
BFF_SUM=26f192bdff8a971c9db49afce4c9cf51cc17477ca7047c6ae7c5a272a1b6985f
mkdir -p "$OUT/bff"
curl -fsSL -o "$OUT/bff/binaryfusefilter.h" \
  "https://raw.githubusercontent.com/FastFilter/xor_singleheader/$BFF_SHA/include/binaryfusefilter.h"
echo "$BFF_SUM  $OUT/bff/binaryfusefilter.h" | sha256sum -c --quiet -

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
gcc -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I "$SRC/tests" -isystem "$OUT/bff" \
  -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
  -o "$OUT/golden_probe" "$HERE/golden_probe.c" "$OUT"/*.o -pthread -lm

# The container has no git: the caller passes the checked-out revision.
echo "revision ${REV:-unknown}; binary fuse filter $BFF_SHA"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
if [ "${QUICK:-0}" = 1 ]; then
  echo
  echo "== golden_probe --keys 1000000 (smoke test) =="
  "$OUT/golden_probe" --keys 1000000 --cpu "$CPU" --reps 1 --threads 1,2
  exit 0
fi
echo
echo "== golden_probe --keys 10000000 =="
"$OUT/golden_probe" --keys 10000000 --cpu "$CPU" --reps "$REPS" --threads 1,2,4,7
[ "${FULL_ONLY:-0}" = 1 ] && exit 0

# The huge-page heap: glibc madvises its heap for THP and grows it in 256 MiB
# steps, so whole 2 MiB regions fault in as huge pages (records included).
HUGE='glibc.malloc.arena_max=1:glibc.malloc.hugetlb=1:glibc.malloc.top_pad=268435456'
for block in 1 2 3; do
  if [ $((block % 2)) = 1 ]; then order="default huge"; else order="huge default"; fi
  for heap in $order; do
    echo
    echo "== block $block: the bar alone, $heap heap =="
    if [ "$heap" = huge ]; then
      GLIBC_TUNABLES="$HUGE" "$OUT/golden_probe" --keys 10000000 --cpu "$CPU" --reps "$REPS" --bar-only
    else
      "$OUT/golden_probe" --keys 10000000 --cpu "$CPU" --reps "$REPS" --bar-only
    fi
  done
done
