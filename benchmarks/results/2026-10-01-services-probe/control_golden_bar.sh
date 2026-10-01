#!/usr/bin/env bash
# =============================================================================
# control_golden_bar.sh — the control for this probe's bar, inside kvnode-dev:
#   MSYS_NO_PATHCONV=1 docker exec kvnode-dev \
#     bash /src/benchmarks/results/2026-10-01-services-probe/control_golden_bar.sh
#
# Builds the committed golden probe exactly as its run.sh does (the pinned,
# checksummed binary fuse filter header included) and runs only its bar
# (--bar-only) at 10M keys, twice. Why: the services probe's first run read
# its bars at 440/430/83 ns against the golden probe's certified 347/327/54.
# If today's golden bar reads the same, the machine is slower today (host
# load); where it does not (B read 56-64 here), the new probe measured
# differently (it did: a rep movsq value copy, fixed). Output: control.txt.
# =============================================================================
set -euo pipefail
SRC=/src/server
GP=/src/benchmarks/results/2026-09-29-golden-probe
OUT="$(mktemp -d /tmp/golden-control.XXXXXX)"
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
  -o "$OUT/golden_probe" "$GP/golden_probe.c" "$OUT"/*.o -pthread -lm
for i in 1 2; do
  echo "== golden_probe --bar-only, run $i =="
  "$OUT/golden_probe" --keys 10000000 --cpu 4 --reps 5 --bar-only | grep -E "BARS|budget"
done
