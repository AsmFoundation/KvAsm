#!/usr/bin/env python3
"""compare_sessions.py — a KvAsm-only session set against an earlier full suite.

The 2026-11-15 session re-measured only KvAsm (``run.py --systems kvasm``),
after ADR 0011, so its own report has no other store to pair with. This script
puts every KvAsm cell of that session next to:

* the same cell of KvAsm in the earlier session (before the change), and
* the same cell of every other store in the earlier session.

Both comparisons are UNPAIRED and ACROSS SESSIONS: the runs did not share
blocks, so host drift between the sessions is not cancelled. They hold only to
the extent the two sessions' hosts were equally quiet; the script therefore
prints each session's outside load next to the numbers, and the
new-vs-old KvAsm column carries an exact two-sided Mann–Whitney p over the
runs (analyze.mann_whitney_p), which at 6 vs 6 runs can reach 0.002 and
says only whether the two sets of runs separate, not why.

Usage:
    python compare_sessions.py NEW_DIR OLD_DIR [> compare.md]

Args (command line):
    NEW_DIR: the KvAsm-only results directory (raw.jsonl, environment.json).
    OLD_DIR: the earlier full-suite results directory.

Returns:
    Exit code 0; writes a Markdown report to stdout.

Raises:
    SystemExit: with a usage message when the arguments are wrong.
    FileNotFoundError / json.JSONDecodeError: when a directory lacks its
        environment.json or raw.jsonl, or they are malformed (analyze.Results).
"""

from __future__ import annotations

import pathlib
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
# Reuse analyze.py's loader (CPU-clock correction, last record per run id) and
# statistics, so both sessions' numbers are derived exactly as in REPORT.md.
sys.path.insert(0, str(HERE.parent.parent))
import analyze  # noqa: E402  (path set just above)

ORDER = ["redis", "valkey", "dragonfly", "garnet", "memcached"]
"""Column order of the other stores (analyze.ORDER without KvAsm variants)."""

# Per experiment: (metric, label, higher_is_better) rows to show. E4 runs at a
# fixed offered load, so its throughput says nothing: latency is the result.
METRICS = {
    "E0": [("ops", "ops/s", True)],
    "E1": [("ops", "ops/s", True), ("cpu_ns_per_op", "CPU ns/op", False)],
    "E2": [("ops", "ops/s", True)],
    "E3": [("ops", "ops/s", True), ("p50", "p50 µs", False), ("p99", "p99 µs", False)],
    "E4": [("p50", "p50 µs", False), ("p99", "p99 µs", False), ("p999", "p99.9 µs", False)],
    "E5": [("ops", "ops/s", True), ("cpu_ns_per_op", "CPU ns/op", False)],
    "E6": [("ops", "ops/s", True)],
    "E7": [("ops", "ops/s", True), ("p99", "p99 µs", False)],
    "E8": [("ops", "ops/s", True)],
    "N1": [("ops", "ops/s", True), ("cpu_ns_per_op", "CPU ns/op", False), ("p50", "p50 µs", False),
           ("p99", "p99 µs", False)],
    "N3": [("ops", "ops/s", True), ("cpu_ns_per_op", "CPU ns/op", False)],
    "S1": [("ops", "ops/s", True)],
}


def fmt(metric: str, v: float | None) -> str:
    """Formats one median for a table cell.

    Args:
        metric: metric name (decides the unit).
        v: the value, or None when the cell has no valid run.

    Returns:
        "—" for None, else e.g. "8.84 M", "576", "117.2".
    """
    if v is None:
        return "—"
    if metric == "ops":
        return analyze.fmt_ops(v)
    if metric == "cpu_ns_per_op":
        return f"{v:,.0f}"
    return f"{v:,.1f}"


def cell_key(rec: dict) -> tuple:
    """Identifies a cell across sessions and systems.

    Threads are left out on purpose: they are tuned per system (E0) and may
    differ between sessions; E0 itself keeps them, because there the thread
    count IS the cell.

    Args:
        rec: run record.

    Returns:
        (experiment, cores, workload_key, tool[, threads for E0]).
    """
    key = (rec["experiment"], rec["cores"], rec["workload_key"], rec.get("tool"))
    return key + ((rec["threads"],) if rec["experiment"] == "E0" else ())


def values_by_cell(res: analyze.Results, system: str, metric: str) -> dict[tuple, list[float]]:
    """All valid values of one metric for one system, grouped by cell.

    Args:
        res: a loaded session.
        system: system name.
        metric: metric name (see analyze.Results.metrics).

    Returns:
        {cell_key: [value per run]} for runs where the metric exists.
    """
    out: dict[tuple, list[float]] = {}
    for r in res.records:
        v = r.get("m", {}).get(metric)
        if r["system"] == system and v is not None:
            out.setdefault(cell_key(r), []).append(v)
    return out


def outside_load(res: analyze.Results, experiment: str, system: str | None = None) -> str:
    """Median outside-VM load (logical CPUs) over an experiment's runs.

    Args:
        res: a loaded session.
        experiment: experiment id.
        system: restrict to one system's runs, or None for all.

    Returns:
        e.g. "4.7", or "—" when the runs carry no host_load.
    """
    xs = [r["m"]["host_external"] for r in res.records
          if r["experiment"] == experiment and "host_external" in r.get("m", {})
          and (system is None or r["system"] == system)]
    return f"{statistics.median(xs):.1f}" if xs else "—"


def label(rec_key: tuple, workload: dict) -> str:
    """Human label of a cell, extending analyze.cell_label.

    Args:
        rec_key: cell_key of the cell.
        workload: the workload dict of one of its runs.

    Returns:
        e.g. "4 cores", "100% SET", "418 k/s offered", "kvbench-onekey".
    """
    experiment, cores, _, tool = rec_key[:4]
    if tool:
        return tool
    if experiment == "E0":
        return f"{cores} cores, {rec_key[4]} threads"
    if experiment == "E4":
        return f"{analyze.fmt_ops(workload['rate'])}/s offered"
    if experiment in ("N1", "N3", "S1"):
        return f"{cores} core{'s' if cores > 1 else ''}"
    return analyze.cell_label(experiment, cores, workload)


def main() -> int:
    """Writes the comparison as Markdown to stdout.

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
    workloads = {cell_key(r): r["workload"] for r in new.records if r["system"] == "kvasm"}
    w = sys.stdout.write
    w(f"# KvAsm re-measured ({new.env['repository']['revision'][:7]}) against "
      f"{old.env['created'][:10]} ({old.env['repository']['revision'][:7]})\n\n")
    w("Unpaired, across sessions: new ÷ old KvAsm with an exact Mann–Whitney p over the runs; "
      "KvAsm ÷ X compares this session's KvAsm median with the other store's median of the "
      "earlier session. Outside load = median logical CPUs busy outside the VM per run.\n")
    for exp in METRICS:
        keys = sorted(k for k in workloads if k[0] == exp)
        if not keys:
            continue
        w(f"\n## {exp}\n\nOutside load: this session {outside_load(new, exp, 'kvasm')}, "
          f"earlier session {outside_load(old, exp)} (all systems).\n\n")
        rivals = [s for s in ORDER if any(r["system"] == s and r["experiment"] == exp for r in old.records)]
        for metric, mlabel, higher in METRICS[exp]:
            nv = values_by_cell(new, "kvasm", metric)
            ov = values_by_cell(old, "kvasm", metric)
            rv = {s: values_by_cell(old, s, metric) for s in rivals}
            w(f"**{mlabel}**\n\n| cell | KvAsm now | KvAsm before | now ÷ before (MW p) |"
              + "".join(f" {s} | KvAsm ÷ {s} |" for s in rivals) + "\n")
            w("|---|---|---|---|" + "---|---|" * len(rivals) + "\n")
            for k in keys:
                a, b = nv.get(k, []), ov.get(k, [])
                ma = statistics.median(a) if a else None
                mb = statistics.median(b) if b else None
                if ma is not None and mb:
                    p = analyze.mann_whitney_p(a, b)
                    ratio = f"{ma / mb:.2f}× (p={p:.3f})" if p is not None else f"{ma / mb:.2f}×"
                else:
                    ratio = "—"
                row = f"| {label(k, workloads[k])} | {fmt(metric, ma)} | {fmt(metric, mb)} | {ratio} |"
                for s in rivals:
                    xs = rv[s].get(k, [])
                    mx = statistics.median(xs) if xs else None
                    rr = f"{ma / mx:.2f}×" if (ma is not None and mx) else "—"
                    row += f" {fmt(metric, mx)} | {rr} |"
                w(row + "\n")
            w("\n")
    w("For ops/s, ratios above 1 favor the first-named; for latency and CPU, below 1 does.\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
