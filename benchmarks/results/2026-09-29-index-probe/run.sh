#!/usr/bin/env bash
# =============================================================================
# run.sh — build and run seq_probe and index_probe inside kvnode-dev:
#   MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
#     kvnode-dev bash /src/benchmarks/results/2026-09-29-index-probe/run.sh [CPU] [REPS]
#
# What it does, and why this way:
#   * Fetches libart (the rival ART implementation) at a pinned commit and
#     refuses to build if any file's SHA-256 differs — the same rule the
#     Dockerfile applies to every third-party download. libart is BSD-3-Clause
#     and is not committed here; it only ever lives in the scratch directory.
#   * Assembles the store modules fresh from /src/server/src with the
#     server's NASM flags into a scratch directory, so the probes measure the
#     checked-out code and never touch /src/server/build (other sessions
#     share it).
#   * seq_probe runs on CPUs 1-7 (distinct P-cores on this host; vCPU 0
#     carries VM interrupt work): once for timing, once with its watermark
#     verifier on CPU 16. index_probe runs on one pinned CPU at 1M and 16M
#     keys and needs ~6 GB of free memory. SEQ_ONLY=1 stops after seq_probe.
# =============================================================================
set -euo pipefail

CPU="${1:-4}"
REPS="${2:-5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC=/src/server
OUT="$(mktemp -d /tmp/index-probe.XXXXXX)"
# Removes the scratch directory however the script ends.
trap 'rm -rf "$OUT"' EXIT

ART_SHA=301046804af165269e37da6725f5a4aec9ecc881
declare -A ART_SUM=(
  [art.c]=b65f20fdc160a6e4c0b1b390086f682d381e3638d8690594158ad75cda33396e
  [art.h]=4443de6fd42581982167f7826f63bd956d21a958f204575e52512e007deab5af
)
for f in art.c art.h; do
  curl -fsSL -o "$OUT/$f" "https://raw.githubusercontent.com/armon/libart/$ART_SHA/src/$f"
  echo "${ART_SUM[$f]}  $OUT/$f" | sha256sum -c --quiet -
done

NASMFLAGS="-f elf64 -g -F dwarf -I $SRC/include/ -w+all -w-reloc-rel-dword -w-reloc-abs-qword -w+error"
for m in siphash util cuckoo wlock store; do
  nasm $NASMFLAGS -o "$OUT/$m.o" "$SRC/src/$m.asm"
done
CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I $SRC/tests -I $OUT"
LDFLAGS="-Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack"
# Third-party code builds without -Werror: its warnings are not ours to fix.
gcc -std=c11 -O2 -g -D_GNU_SOURCE -w -c -o "$OUT/art.o" "$OUT/art.c"
gcc $CFLAGS $LDFLAGS -o "$OUT/seq_probe" "$HERE/seq_probe.c" "$OUT/wlock.o" -pthread
gcc $CFLAGS $LDFLAGS -o "$OUT/index_probe" "$HERE/index_probe.c" "$OUT/art.o" \
  "$OUT/siphash.o" "$OUT/util.o" "$OUT/cuckoo.o" "$OUT/wlock.o" "$OUT/store.o" -pthread

# The container has no git: the caller passes the checked-out revision.
echo "revision ${REV:-unknown}; libart $ART_SHA"
echo "cpu $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "thp $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo
echo "== seq_probe (timing) =="
"$OUT/seq_probe" --ops 2000000 --cpus 1,2,3,4,5,6,7 --reps "$REPS"
echo
echo "== seq_probe (verification: a watermark reader on CPU 16 checks every revision) =="
"$OUT/seq_probe" --ops 2000000 --cpus 1,2,3,4,5,6,7 --reps 2 --verify 16
[ "${SEQ_ONLY:-0}" = 1 ] && exit 0
for keys in 1000000 16000000; do
  echo
  echo "== index_probe --keys $keys =="
  "$OUT/index_probe" --keys "$keys" --cpu "$CPU" --reps "$REPS"
done
