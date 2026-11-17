#!/usr/bin/env bash
# =============================================================================
# abbench.sh — compare bench_h2 builds of several server variants on a host
# that other work shares.
#
# Usage (inside kvasm-dev): abbench.sh MODE CPU ROUNDS VARIANT [VARIANT..]
#   MODE    get | ping (bench_h2's workload)   CPU  the vCPU to pin to
#   ROUNDS  interleaved rounds (each round runs every variant once, in order)
#
# Why this shape: a vCPU here is time-sliced, moves between P- and E-cores and
# changes frequency with the host's load, so one run's minimum is only as good
# as the best moment that run happened to get. Many short interleaved runs
# give every variant the same chances at a good moment; the minimum over all
# rounds is then each variant's cost on the best core state seen, and the
# number of rounds within 2% of that minimum shows how often it was reached
# (a minimum seen once is weak evidence, one seen in most rounds is strong).
# =============================================================================
set -euo pipefail
MODE=$1; CPU=$2; ROUNDS=$3; shift 3
declare -A all
for v in "$@"; do all[$v]=""; done
for r in $(seq 1 "$ROUNDS"); do
  for v in "$@"; do
    # The first line carries the figure (a second one names the layout);
    # BENCH_LAYOUT and BENCH_HASH reach bench_h2 through the environment.
    ns=$(taskset -c "$CPU" /pc/probe/bench_h2-"$v" "$MODE" 1024 40 1000 | head -1 | sed -E 's/^[a-z]+: ([0-9.]+) ns.*/\1/')
    all[$v]+="$ns "
  done
done
for v in "$@"; do
  python3 - "$v" ${all[$v]} <<'EOF'
import sys
v, xs = sys.argv[1], sorted(map(float, sys.argv[2:]))
m = xs[0]
near = sum(1 for x in xs if x <= m * 1.02)
print(f"{v:6s} min {m:7.2f} ns/request   median {xs[len(xs) // 2]:7.2f}   rounds within 2% of min: {near}/{len(xs)}")
EOF
done
