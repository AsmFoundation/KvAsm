#!/usr/bin/env python3
"""transfer.py — carry the 2026-09-27 comparisons over to a new KvAsm build.

The 2026-11-15 session ran the standard suite for two KvAsm builds only,
paired in the same blocks (``run.py --systems kvasm --control-image
kvasm:0.1.0``):

* ``kvasm``: the new build (ADR 0011);
* ``kvasm-control``: the very image the 2026-09-27 suite measured.

The other stores were not re-measured, and the host itself had moved: the
control image ran ~1.5x slower per GET than it did on 2026-09-27 (pass 1's
control). So no absolute number of this session may be set against the other
stores' 2026-09-27 numbers. What does carry over is a RATIO:

    new ÷ X  ≈  (new ÷ control, paired today)  ×  (control ÷ X, paired 2026-09-27)

The first factor is measured within this session's blocks; the second is the
published comparison, within that session's blocks; the control is the same
image in both. The estimate assumes only that the new build's advantage over
the old one is the same on both days' hosts — far weaker than assuming the
host did not move, but still an assumption, which is why the result is
labelled an estimate. The bracket multiplies the two factors' block-ratio
ranges (the extremes of each), so it is conservative.

Usage:
    python transfer.py NEW_DIR OLD_DIR > transfer.md

Args (command line):
    NEW_DIR: the paired two-build session (kvasm and kvasm-control).
    OLD_DIR: the 2026-09-27 full suite (kvasm and the other stores).

Returns:
    Exit code 0; Markdown on stdout.

Raises:
    SystemExit: with the usage text on wrong arguments.
    FileNotFoundError / json.JSONDecodeError: a directory lacks or has a
        malformed environment.json or raw.jsonl (analyze.Results).
"""

from __future__ import annotations

import pathlib
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))  # benchmarks/: analyze.py
import analyze  # noqa: E402

NEW, CONTROL = "kvasm", "kvasm-control"
RIVALS = ["redis", "valkey", "dragonfly", "garnet", "memcached"]
"""Other stores, in the report's column order."""

# (experiment, metric, label, higher_is_better). E0 is a thread sweep (its
# runs share block keys, which analyze.values refuses), and E4's throughput is
# fixed by design (its result is latency), so neither appears with ops.
ROWS = [
    ("E1", "ops", "GET/s", True), ("E1", "cpu_ns_per_op", "CPU ns per GET", False),
    ("E2", "ops", "ops/s", True),
    ("E3", "ops", "ops/s", True), ("E3", "p99", "p99 µs", False),
    ("E4", "p50", "p50 µs", False), ("E4", "p99", "p99 µs", False),
    ("E5", "ops", "ops/s", True), ("E5", "cpu_ns_per_op", "CPU ns/op", False),
    ("E6", "ops", "GET/s", True),
    ("E7", "ops", "GET/s", True), ("E7", "p99", "p99 µs", False),
    ("E8", "ops", "ops/s", True),
    ("N1", "ops", "GET/s", True), ("N1", "cpu_ns_per_op", "CPU ns per GET", False),
    ("N3", "ops", "ops/s", True), ("N3", "cpu_ns_per_op", "CPU ns/op", False),
    ("S1", "ops", "GET/s", True),
]


def cells(res: analyze.Results, experiment: str) -> list[tuple[int, str, str | None, dict]]:
    """The (cores, workload_key, tool) cells of an experiment that KvAsm ran.

    Args:
        res: a loaded session.
        experiment: experiment id.

    Returns:
        Sorted (cores, workload_key, tool, workload dict) per cell.
    """
    seen: dict[tuple, dict] = {}
    for r in res.records:
        if r["experiment"] == experiment and r["system"] == NEW:
            seen.setdefault((r["cores"], r["workload_key"], r.get("tool")), r["workload"])
    return [(c, k, t, w) for (c, k, t), w in sorted(seen.items(), key=lambda x: (x[0][0], x[0][1], x[0][2] or ""))]


def fmt(metric: str, v: float | None) -> str:
    """Formats a median for a table cell.

    Args:
        metric: metric name (decides the unit).
        v: value, or None.

    Returns:
        e.g. "8.84 M", "576", "117.2" or "—".
    """
    if v is None:
        return "—"
    if metric == "ops":
        return analyze.fmt_ops(v)
    return f"{v:,.0f}" if metric == "cpu_ns_per_op" else f"{v:,.1f}"


def label(experiment: str, cores: int, tool: str | None, workload: dict) -> str:
    """Human label of a cell.

    Args:
        experiment: experiment id.
        cores: core budget.
        tool: E8's tool, else None.
        workload: the cell's workload dict.

    Returns:
        e.g. "4 cores", "100% SET", "836 k/s offered", "h2load-onekey".
    """
    if tool:
        return tool
    if experiment == "E4":
        return f"{analyze.fmt_ops(workload['rate'])}/s offered"
    if experiment in ("N1", "N3", "S1"):
        return f"{cores} core{'s' if cores > 1 else ''}"
    return analyze.cell_label(experiment, cores, workload)


def main() -> int:
    """Writes the transfer tables.

    Returns:
        0 on success.

    Raises:
        SystemExit: on wrong arguments.
    """
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.stdout.reconfigure(encoding="utf-8")
    new = analyze.Results(pathlib.Path(sys.argv[1]))
    old = analyze.Results(pathlib.Path(sys.argv[2]))
    w = sys.stdout.write
    w("# The new build against every store, carried over from 2026-09-27\n\n"
      "Columns: this session's medians; **new ÷ control**, paired by block today (range of block "
      "ratios, sign test); **control today ÷ 2026-09-27**, the same image across sessions (the host's "
      "move); then per store the estimate new ÷ X = (new ÷ control) × (2026-09-27's paired "
      "KvAsm ÷ X), with the product of both ranges. For throughput, above 1 favors KvAsm; for "
      "latency and CPU, below 1 does.\n")
    last = None
    for exp, metric, mlabel, higher in ROWS:
        cs = cells(new, exp)
        if not cs:
            continue
        if exp != last:
            w(f"\n## {exp}\n")
            last = exp
        rivals = [s for s in RIVALS if any(r["system"] == s and r["experiment"] == exp for r in old.records)]
        w(f"\n**{mlabel}**\n\n| cell | new | control | new ÷ control | control ÷ 09-27 |"
          + "".join(f" ≈ new ÷ {s} |" for s in rivals) + "\n|---|---|---|---|---|" + "---|" * len(rivals) + "\n")
        for cores, wkey, tool, wl in cs:
            match = {"cores": cores, "workload_key": wkey, "tool": tool}
            nv = new.values(exp, metric, **match)
            ov = old.values(exp, metric, **match)
            a, c = nv.get(NEW, {}), nv.get(CONTROL, {})
            ma = statistics.median(a.values()) if a else None
            mc = statistics.median(c.values()) if c else None
            today = analyze.compare(a, c, higher)
            o_kv = ov.get("kvasm", {})
            host = (mc / statistics.median(o_kv.values())) if (mc and o_kv) else None
            row = (f"| {label(exp, cores, tool, wl)} | {fmt(metric, ma)} | {fmt(metric, mc)} | "
                   + (f"{today['ratio']:.2f}× [{today['lo']:.2f}–{today['hi']:.2f}] p={today['p']:.3f}"
                      if today and today["p"] is not None else "—")
                   + f" | {f'{host:.2f}×' if host else '—'} |")
            for s in rivals:
                then = analyze.compare(o_kv, ov.get(s, {}), higher)
                if today and then:
                    est, lo, hi = today["ratio"] * then["ratio"], today["lo"] * then["lo"], today["hi"] * then["hi"]
                    row += f" {est:.2f}× [{lo:.2f}–{hi:.2f}] |"
                else:
                    row += " — |"
            w(row + "\n")
    w("\nThe 2026-09-27 factors are that session's paired KvAsm ÷ X (analyze.compare over its blocks); "
      "cells it did not run for a store show —.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
