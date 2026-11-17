#!/usr/bin/env python3
"""summarize.py — per-variant and paired statistics of an ablation file.

Usage:
    python summarize.py rotated.txt [fixed-order.txt ...]

Each input line is one run, as printed by ablation.sh:
    cores=<n> variant=<s0-base|s1-cache|s2-pool|s3-batch> rep=<r> ops=<ops/s> valid=<bool>

For every (cores, variant) cell it prints the median, min, max and the
between-run coefficient of variation, then the paired ratio variant ÷
baseline *within the same repetition*: its median, its range and in how
many repetitions the variant was faster. Why paired: the host's absolute
level drifts by tens of percent between runs, but every variant of one
repetition ran back to back, so their ratio cancels that drift.

Standard library only.
"""

from __future__ import annotations

import statistics
import sys
from collections import defaultdict

VARIANTS = ("s0-base", "s1-cache", "s2-pool", "s3-batch")


def load(path: str) -> dict[tuple[int, str], dict[int, float]]:
    """Reads one ablation file.

    Args:
        path: file with one ``key=value`` run line per row.

    Returns:
        {(cores, variant): {rep: ops/s}}.

    Raises:
        ValueError: a line is malformed or a run is marked invalid — an
            invalid run (any error, miss or wrong value in kvbench) must
            never be averaged in silently.
        OSError: the file cannot be read.
    """
    cells: dict[tuple[int, str], dict[int, float]] = defaultdict(dict)
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            if not line.strip():
                continue
            try:
                kv = dict(field.split("=", 1) for field in line.split())
                if kv["valid"] != "true":
                    raise ValueError("run marked invalid")
                cells[(int(kv["cores"]), kv["variant"])][int(kv["rep"])] = float(kv["ops"])
            except (KeyError, ValueError) as e:
                raise ValueError(f"{path}:{n}: {e}: {line.strip()}") from None
    return cells


def report(path: str) -> None:
    """Prints the summary table of one file.

    Args:
        path: ablation file.

    Raises:
        ValueError: see load().
        OSError: see load().
    """
    cells = load(path)
    print(f"== {path}")
    print(f"{'cores':>5} {'variant':9} {'n':>2} {'median':>8} {'min':>7} {'max':>7} {'CV':>6}"
          f"   {'paired median [range]':26} faster")
    for cores in sorted({c for c, _ in cells}):
        base = cells.get((cores, "s0-base"), {})
        for v in VARIANTS:
            runs = cells.get((cores, v))
            if not runs:
                continue
            vals = list(runs.values())
            cv = statistics.pstdev(vals) / statistics.fmean(vals)
            # Pair only repetitions present for both; the baseline pairs with
            # itself (ratio 1), which keeps the table's shape uniform.
            pairs = [runs[r] / base[r] for r in runs if r in base]
            paired = (f"{statistics.median(pairs):.3f} [{min(pairs):.3f}–{max(pairs):.3f}]"
                      if pairs else "—")
            faster = f"{sum(p > 1 for p in pairs)}/{len(pairs)}" if v != "s0-base" else ""
            print(f"{cores:>5} {v:9} {len(vals):>2} {statistics.median(vals) / 1e6:>7.3f}M "
                  f"{min(vals) / 1e6:>6.3f}M {max(vals) / 1e6:>6.3f}M {cv * 100:>5.1f}%   {paired:26} {faster}")


def main() -> int:
    """Entry point.

    Returns:
        0 on success, 2 on usage errors.
    """
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        report(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
