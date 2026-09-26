#!/usr/bin/env python3
"""analyze.py — statistics and report for one results directory.

Usage:
    python benchmarks/analyze.py benchmarks/results/<dir>

Writes <dir>/REPORT.md (and <dir>/charts/*.svg when matplotlib is
installed). Standard library only otherwise.

Statistics, chosen for small samples (3-10 repetitions per cell):
  * median and min-max range per cell (no normality assumption);
  * KvNode vs each system within one experiment: ratio of medians, plus a
    *paired* analysis by block — repetition r of every cell runs in block r
    (run.plan), so within-block ratios cancel the host's slow drift. The
    range shown is the min-max of those per-block ratios and the p-value is
    the exact two-sided sign test (6 blocks all agreeing: p = 0.031);
  * comparisons across experiments (S1 vs E1) or with too few shared blocks
    fall back to the conservative unpaired range [min(KvNode)/max(other),
    max(KvNode)/min(other)] and the exact Mann-Whitney U p-value;
  * a run is excluded if kvbench flagged it invalid (any error, miss or
    wrong value); counts of excluded runs are reported, never hidden;
  * server CPU is divided by the host's measured CPU-clock factor; a run is
    flagged client-bound if the client used > 85% of its CPU while the
    server used < 90% of its budget.
"""

from __future__ import annotations

import itertools
import json
import math
import pathlib
import statistics
import sys
from collections import defaultdict

ORDER = ["kvnode", "redis", "valkey", "dragonfly", "garnet", "memcached"]


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------


def midranks(values: list[float]) -> list[float]:
    """Ranks with ties sharing their average rank.

    Args:
        values: observations.

    Returns:
        Rank of each observation (1-based).
    """
    order = sorted(range(len(values)), key=lambda i: values[i])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return ranks


def mann_whitney_p(a: list[float], b: list[float]) -> float | None:
    """Exact two-sided Mann-Whitney U test by full enumeration.

    Args:
        a: sample A.
        b: sample B.

    Returns:
        p-value, or None when a sample is empty or the enumeration would be
        too large (n > 16 combined); with 3 vs 3 the smallest possible p is
        0.1, with 5 vs 5 it is 0.0079.
    """
    if not a or not b or len(a) + len(b) > 16:
        return None
    pooled = a + b
    ranks = midranks(pooled)
    na = len(a)
    observed = sum(ranks[:na])
    mean = na * (len(pooled) + 1) / 2
    dev = abs(observed - mean)
    total = hits = 0
    for combo in itertools.combinations(range(len(pooled)), na):
        total += 1
        if abs(sum(ranks[i] for i in combo) - mean) >= dev - 1e-9:
            hits += 1
    return hits / total


def samples(values: dict | list) -> list[float]:
    """Plain observations of a cell, whatever form they arrive in.

    Footgun this exists for: Results.values() returns {block: value} dicts,
    and statistics.median/pstdev of a dict silently iterate its *keys* (the
    block ids), not the measurements. Every consumer that wants numbers goes
    through here.

    Args:
        values: {block: value} from Results.values(), or a list.

    Returns:
        The observations as a list (block order is irrelevant here).
    """
    return list(values.values()) if isinstance(values, dict) else list(values)


def summary(values: dict | list) -> dict | None:
    """Median and range of a cell.

    Args:
        values: observations, as a list or a {block: value} dict.

    Returns:
        {"n", "median", "min", "max"}, or None when empty.
    """
    values = samples(values)
    if not values:
        return None
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values)}


def sign_test_p(ratios: list[float]) -> float:
    """Exact two-sided sign test of paired ratios against 1.

    Args:
        ratios: per-block ratios (ties at exactly 1.0 are dropped).

    Returns:
        p-value; 6 pairs all on one side give 2/64 = 0.031, the smallest n
        that can reach p < 0.05.
    """
    signs = [r > 1 for r in ratios if r != 1.0]
    n = len(signs)
    if n == 0:
        return 1.0
    k = min(sum(signs), n - sum(signs))
    return min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2**n)


def compare(kv: dict | list, other: dict | list, higher_is_better: bool = True) -> dict | None:
    """KvNode relative to another system, paired by block when possible.

    Why pairing: on a noisy host the absolute level of *every* system drifts
    by tens of percent over hours, which swamps unpaired tests. Runs of the
    same repetition form one block, run close together in random order
    (run.plan), so the ratio inside a block cancels that shared drift.

    Args:
        kv: KvNode observations — a {block key: value} dict from
            Results.values() for paired analysis, or a plain list to force
            the unpaired test (e.g. comparing different experiments, whose
            blocks were not run together).
        other: the other system's observations, in the same form.
        higher_is_better: orientation (throughput True, latency False).

    Returns:
        None when either side is empty or non-positive; otherwise
        {"ratio", "lo", "hi", "p", "kind", "n", "verdict"} with
        ratio = median(KvNode) / median(other) and:
        * kind "paired" (≥ 3 shared blocks): lo..hi is the range of the
          within-block ratios, p the exact two-sided sign test, n the
          number of pairs;
        * kind "unpaired": lo..hi = [min(kv)/max(other), max(kv)/min(other)],
          which contains every pairwise run ratio, p the exact Mann-Whitney
          (None when the enumeration is too large);
        * verdict "better"/"worse" only when p < 0.05 (unpaired: and the
          range excludes 1), else "no clear difference". With 6 pairs that
          means all six blocks agree; 3 repetitions can never reach it.
    """
    kv_v, ot_v = samples(kv), samples(other)
    if not kv_v or not ot_v or min(ot_v) <= 0 or min(kv_v) <= 0:
        return None
    ratio = statistics.median(kv_v) / statistics.median(ot_v)
    # Pair only when both sides carry block keys; a block missing on one side
    # (an invalid run) just drops that pair rather than the whole comparison.
    shared = sorted(set(kv) & set(other)) if isinstance(kv, dict) and isinstance(other, dict) else []
    if len(shared) >= 3:
        pairs = [kv[b] / other[b] for b in shared]
        lo, hi, p, kind, n = min(pairs), max(pairs), sign_test_p(pairs), "paired", len(pairs)
        clear = p < 0.05
    else:
        lo, hi = min(kv_v) / max(ot_v), max(kv_v) / min(ot_v)
        p, kind, n = mann_whitney_p(kv_v, ot_v), "unpaired", min(len(kv_v), len(ot_v))
        clear = (lo > 1 or hi < 1) and p is not None and p < 0.05
    better = ratio > 1 if higher_is_better else ratio < 1
    verdict = ("better" if better else "worse") if clear else "no clear difference"
    return {"ratio": ratio, "lo": lo, "hi": hi, "p": p, "kind": kind, "n": n, "verdict": verdict}


# ---------------------------------------------------------------------------
# Records
# ---------------------------------------------------------------------------


class Results:
    """Loaded results with per-run derived metrics."""

    def __init__(self, directory: pathlib.Path) -> None:
        """Loads environment.json and raw.jsonl.

        Args:
            directory: results directory.
        """
        self.dir = directory
        self.env = json.loads((directory / "environment.json").read_text(encoding="utf-8"))
        self.factor = self.env.get("cpu_clock", {}).get("factor", 1.0) or 1.0
        lines = (directory / "raw.jsonl").read_text(encoding="utf-8").splitlines()
        self.records = [json.loads(l) for l in lines if l.strip()]
        # A resumed suite may have retried a failed run: keep the last record per id.
        last: dict[str, dict] = {}
        for r in self.records:
            last[r["run_id"]] = r
        self.records = list(last.values())
        for r in self.records:
            r["m"] = self.metrics(r)

    def metrics(self, rec: dict) -> dict:
        """Derived metrics of one run (empty unless it is a valid kvbench run).

        Args:
            rec: run record.

        Returns:
            ops, latency percentiles (µs), corrected server CPU utilization,
            ops per server CPU-second, client utilization, memory per key.
        """
        if rec.get("kind") == "validation":
            return {"ops": rec.get("tool_ops_per_sec")} if rec.get("status") == "ok" else {}
        kv = rec.get("kvbench", {})
        res, pre = kv.get("result", {}), kv.get("preload", {})
        if rec.get("status") != "ok" or not res.get("valid"):
            return {}
        m = {"ops": res["ops_per_sec"], "seconds": res["seconds"]}
        lat = res.get("latency_us", {})
        for q in ("p50", "p99", "p999", "mean", "max"):
            m[q] = lat.get(q)
        srv = res.get("server_cpu_s", -1)
        if srv and srv > 0:
            cpu = srv / self.factor
            m["server_util"] = cpu / (res["seconds"] * rec["cores"])
            m["ops_per_cpu_s"] = res["ops"] / cpu
            m["cpu_ns_per_op"] = cpu * 1e9 / res["ops"]
        m["client_util"] = res.get("client_cpu_util")
        m["client_bound"] = bool(m["client_util"] and m["client_util"] > 0.85
                                 and m.get("server_util", 1) < 0.9)
        before, after = pre.get("server_mem_before", -1), pre.get("server_mem_after", -1)
        keys = rec["workload"]["keys"]
        if before >= 0 and after >= 0 and keys:
            m["mem_per_key"] = (after - before) / keys
            m["mem_total"] = after
        m["achieved"] = res["ops_per_sec"]
        m["stalls"] = res.get("stalls", 0)
        tl = [x for x in res.get("timeline_ops_per_sec", []) if x > 0]
        if len(tl) >= 3:
            mean = statistics.fmean(tl)
            m["within_cv"] = statistics.pstdev(tl) / mean
            # least-squares slope of the per-interval samples, as % of the
            # mean per 10 samples: sustained drift inside one run
            xs = range(len(tl))
            xm = statistics.fmean(xs)
            slope = sum((x - xm) * (y - mean) for x, y in zip(xs, tl)) / sum((x - xm) ** 2 for x in xs)
            m["trend_pct_per_10"] = slope * 10 / mean * 100
            m["min_sample_ratio"] = min(tl) / mean
        return m

    def cells(self, experiment: str) -> dict[tuple, list[dict]]:
        """Runs of an experiment grouped by (system, cores, threads, workload, tool).

        Args:
            experiment: experiment id.

        Returns:
            Mapping of cell key to its records.
        """
        out: dict[tuple, list[dict]] = defaultdict(list)
        for r in self.records:
            if r.get("experiment") == experiment:
                out[(r["system"], r["cores"], r["threads"], r["workload_key"], r.get("tool"))].append(r)
        return out

    def values(self, experiment: str, metric: str, **match) -> dict[str, dict[tuple, float]]:
        """Metric values per system and block for runs matching the fields.

        The block key is (repetition, cores, workload, tool): everything that
        identifies a run except the system and its thread count (threads are
        tuned per system, so including them would prevent any pairing). Two
        systems' values with the same key ran in the same block on the same
        configuration, which is what compare() pairs.

        Args:
            experiment: experiment id.
            metric: key in the derived metrics.
            **match: record fields that must be equal (e.g. cores=4).

        Returns:
            {system: {block key: value}} for valid runs. Use samples() for
            plain numbers — statistics functions iterate a dict's keys.

        Raises:
            ValueError: two runs of one system share a block key, i.e. the
                experiment varies a field the key omits (E0's thread sweep).
                Silently keeping one of them would drop data, so such
                experiments must use cells() instead.
        """
        out: dict[str, dict[tuple, float]] = defaultdict(dict)
        for r in self.records:
            if r.get("experiment") != experiment or any(r.get(k) != v for k, v in match.items()):
                continue
            v = r["m"].get(metric)
            if v is None:
                continue
            key = (r["rep"], r["cores"], r["workload_key"], r.get("tool"))
            if key in out[r["system"]]:
                raise ValueError(f"{experiment}: two {r['system']} runs share block {key}; use cells()")
            out[r["system"]][key] = v
        return out


# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------


def fmt_ops(v: float) -> str:
    """Throughput with a unit.

    Args:
        v: ops/s.

    Returns:
        e.g. "1.23 M" or "845 k".
    """
    return f"{v / 1e6:.2f} M" if v >= 1e6 else f"{v / 1e3:.0f} k"


def cell_text(s: dict | None, fmt=fmt_ops) -> str:
    """Median with range, or a dash.

    Args:
        s: summary() result.
        fmt: number formatter.

    Returns:
        e.g. "1.23 M (1.20–1.25)".
    """
    if not s:
        return "—"
    if s["n"] == 1:
        return fmt(s["median"])
    return f"{fmt(s['median'])} ({fmt(s['min']).split()[0]}–{fmt(s['max'])})"


def cmp_text(c: dict | None) -> str:
    """KvNode-relative comparison as table text.

    Args:
        c: compare() result.

    Returns:
        e.g. "1.31× [1.22–1.40] p=0.008 ✓".
    """
    if not c:
        return "—"
    p = "n/a" if c["p"] is None else f"{c['p']:.3f}"
    mark = {"better": "▲", "worse": "▼"}.get(c["verdict"], "≈")
    # The test name travels with the p-value: a paired range and a
    # conservative unpaired range answer different questions.
    test = "sign" if c.get("kind") == "paired" else "MW"
    return f"{c['ratio']:.2f}× [{c['lo']:.2f}–{c['hi']:.2f}] {test} p={p} {mark}"


def systems_in(values: dict[str, dict | list]) -> list[str]:
    """Systems present, in the canonical order.

    Args:
        values: per-system values (only the keys are used).

    Returns:
        Ordered system names.
    """
    return [s for s in ORDER if s in values] + sorted(s for s in values if s not in ORDER)


# ---------------------------------------------------------------------------
# Report sections
# ---------------------------------------------------------------------------


class Report:
    """Builds REPORT.md section by section."""

    def __init__(self, res: Results) -> None:
        """Prepares an empty report.

        Args:
            res: loaded results.
        """
        self.res = res
        self.lines: list[str] = []
        self.charts = res.dir / "charts"
        try:
            import matplotlib

            matplotlib.use("Agg")
            import matplotlib.pyplot as plt

            self.plt = plt
            self.charts.mkdir(exist_ok=True)
        except ImportError:
            self.plt = None

    def title(self, s: str) -> str:
        """Display name of a system.

        Args:
            s: system id.

        Returns:
            Title with version, from the environment record.
        """
        return self.res.env["systems"].get(s, {}).get("title", s)

    def w(self, *lines: str) -> None:
        """Appends lines to the report.

        Args:
            *lines: markdown lines.
        """
        self.lines.extend(lines)

    def comparison_table(self, header: str, columns: list[tuple[str, dict[str, dict | list]]],
                         fmt=fmt_ops, higher_is_better: bool = True) -> None:
        """A systems x columns table of medians, plus KvNode ratios per column.

        Args:
            header: first column label.
            columns: (label, per-system values) per column; block-keyed dicts
                from Results.values() give paired ratios, lists unpaired ones.
            fmt: number formatter.
            higher_is_better: orientation for the verdicts.
        """
        systems = []
        for _, vals in columns:
            systems += [s for s in systems_in(vals) if s not in systems]
        self.w(f"| {header} | " + " | ".join(c for c, _ in columns) + " |",
               "|---|" + "---|" * len(columns))
        for s in systems:
            self.w(f"| {self.title(s)} | " + " | ".join(cell_text(summary(v.get(s, {})), fmt) for _, v in columns) + " |")
        if any("kvnode" in v for _, v in columns):
            for s in systems:
                if s == "kvnode":
                    continue
                self.w(f"| *KvNode ÷ {self.title(s).split()[0]}* | " + " | ".join(
                    cmp_text(compare(v.get("kvnode", {}), v.get(s, {}), higher_is_better)) for _, v in columns) + " |")
        self.w("")

    def chart(self, name: str, xlabel: str, ylabel: str, series: dict[str, list[tuple[float, float]]],
              logx: bool = False) -> None:
        """Writes an SVG line chart of per-system medians and links it.

        Args:
            name: file stem.
            xlabel: x-axis label.
            ylabel: y-axis label.
            series: {system: [(x, median y), ...]}.
            logx: logarithmic x axis.
        """
        if not self.plt or not series:
            return
        plt = self.plt
        fig, ax = plt.subplots(figsize=(7, 4.2))
        for s in systems_in(series):
            pts = sorted(series[s])
            ax.plot([p[0] for p in pts], [p[1] for p in pts], marker="o",
                    linewidth=2.6 if s == "kvnode" else 1.4, label=self.title(s))
        ax.set_xlabel(xlabel)
        ax.set_ylabel(ylabel)
        if logx:
            ax.set_xscale("log", base=2)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(self.charts / f"{name}.svg")
        plt.close(fig)
        self.w(f"![{name}](charts/{name}.svg)", "")

    # -- sections -------------------------------------------------------------

    def header(self) -> None:
        """Environment, systems and validity overview."""
        env = self.res.env
        h = env["host"]
        self.w("# KvNode benchmark report", "",
               f"Profile **{env['profile']}**, recorded {env['created'][:19]} UTC, repository "
               f"revision `{env['repository']['revision'][:12]}`"
               f"{' (uncommitted changes)' if env['repository']['dirty'] else ''}. "
               "Method: [METHODOLOGY.md](../../METHODOLOGY.md).", "",
               "## Environment", "",
               f"- CPU: {h['cpu_model']}, {h['vcpus']} vCPUs seen by the container VM; "
               f"{h['os']}, kernel {h['kernel']}",
               f"- Docker {env['docker']['server_version']}, cgroup v{env['docker']['cgroup_version']} "
               f"({env['docker']['cgroup_driver']}), security options {env['docker']['security_options']}",
               f"- Placement: server on vCPUs 0..N-1, kvbench on {env['placement']['client_cpus']} "
               f"with {env['placement']['client_threads']} threads, loopback in the server's network namespace",
               f"- CPU clock factor {self.res.factor:.4f} (cgroup CPU time per wall second on a known "
               "load; server CPU figures are divided by it)", "",
               "| System | Image | Configuration |", "|---|---|---|")
        for s in ORDER:
            meta = env["systems"].get(s)
            if not meta:
                continue
            img = env["images"].get(s, {})
            ident = (img.get("digests") or [img.get("id", "")])[0]
            self.w(f"| {meta['title']} | `{meta['image']}`<br/>`{ident[-19:]}` | {meta['notes']} |")
        self.w("")
        counts: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
        bad = []
        for r in self.res.records:
            counts[r["experiment"]][r["status"]] += 1
            if r["status"] != "ok":
                why = r.get("error") or r.get("kvbench", {}).get("result", {}).get("error_text") \
                      or r.get("kvbench", {}).get("error_text") or r.get("raw", "")[-120:]
                bad.append(f"- `{r['run_id']}`: {r['status']} — {why}")
        cb = [r["run_id"] for r in self.res.records if r["m"].get("client_bound")]
        self.w("## Run validity", "", "| Experiment | ok | invalid | failed |", "|---|---|---|---|")
        for e in sorted(counts):
            c = counts[e]
            self.w(f"| {e} | {c['ok']} | {c['invalid']} | {c['failed']} |")
        self.w("", f"Client-bound runs (client > 85% busy, server < 90%): {len(cb)}"
               + ("".join(f"\n- `{x}`" for x in cb) if cb else "."), "")
        if bad:
            self.w("Excluded runs:", "", *bad, "")

    def e0(self) -> None:
        """Thread-count tuning table."""
        cells = self.res.cells("E0")
        if not cells:
            return
        self.w("## E0 — Thread-count tuning", "",
               "Median GET throughput for each thread setting; the better one (bold) is used "
               "by every later experiment.", "",
               "| System | Cores | threads = N−1 | threads = N |", "|---|---|---|---|")
        table: dict[tuple[str, int], dict[int, list[float]]] = defaultdict(lambda: defaultdict(list))
        for (s, c, t, _, _), recs in cells.items():
            table[(s, c)][t] += [r["m"]["ops"] for r in recs if "ops" in r["m"]]
        for (s, c) in sorted(table, key=lambda k: (ORDER.index(k[0]) if k[0] in ORDER else 99, k[1])):
            by = table[(s, c)]
            meds = {t: statistics.median(v) for t, v in by.items() if v}
            best = max(meds, key=meds.get) if meds else None
            cols = []
            for t in (c - 1, c):
                txt = cell_text(summary(by.get(t, [])))
                cols.append(f"**{txt}**" if t == best else txt)
            self.w(f"| {self.title(s)} | {c} | {cols[0]} | {cols[1]} |")
        self.w("")

    def e1(self) -> None:
        """Throughput scaling, CPU efficiency and memory per key."""
        cores = sorted({r["cores"] for r in self.res.records if r["experiment"] == "E1"})
        if not cores:
            return
        self.w("## E1 — Throughput scaling (100% GET, 100-byte values, 1M keys, uniform)", "")
        cols = [(f"{c} core{'s' if c > 1 else ''}", self.res.values("E1", "ops", cores=c)) for c in cores]
        self.comparison_table("Throughput (ops/s)", cols)
        series = defaultdict(list)
        for c, (_, vals) in zip(cores, cols):
            for s, v in vals.items():
                series[s].append((c, statistics.median(samples(v))))
        self.chart("e1_throughput", "server cores (vCPUs)", "GET ops/s (median)", series)
        self.w("**CPU efficiency** — operations per server CPU-second (higher is better):", "")
        self.comparison_table("ops per CPU-second", [(f"{c} core{'s' if c > 1 else ''}",
                                                      self.res.values("E1", "ops_per_cpu_s", cores=c))
                                                     for c in cores])
        self.w("**Server CPU utilization** of its core budget (1.0 = all given cores busy):", "")
        self.comparison_table("utilization", [(f"{c} core{'s' if c > 1 else ''}",
                                               self.res.values("E1", "server_util", cores=c))
                                              for c in cores], fmt=lambda v: f"{v:.2f}")
        mem = {s: v for s, v in self.res.values("E1", "mem_per_key").items()}
        self.w("**Memory per key** after preloading 1M keys (anonymous memory delta ÷ keys; "
               "16-byte key + 100-byte value = 116 raw bytes; lower is better):", "")
        self.comparison_table("bytes / key", [("all E1 runs", mem)], fmt=lambda v: f"{v:.0f} B",
                              higher_is_better=False)

    def by_workload(self, exp: str, title: str, note: str, labels: dict[str, str]) -> None:
        """A systems x workloads throughput table for one experiment.

        Args:
            exp: experiment id.
            title: section title.
            note: explanatory sentence.
            labels: workload key -> column label.
        """
        keys = [k for k in labels if any(r["workload_key"] == k for r in self.res.records if r["experiment"] == exp)]
        if not keys:
            return
        self.w(f"## {exp} — {title}", "", note, "")
        self.comparison_table("Throughput (ops/s)", [(labels[k], self.res.values(exp, "ops", workload_key=k))
                                                     for k in keys])

    def e3(self) -> None:
        """No-pipelining throughput and latency."""
        recs = [r for r in self.res.records if r["experiment"] == "E3"]
        if not recs:
            return
        self.w("## E3 — No pipelining (256 connections × 1 request in flight, 4 cores)", "")
        self.comparison_table("metric", [("ops/s", self.res.values("E3", "ops")),
                                         ("p50 µs", self.res.values("E3", "p50")),
                                         ("p99 µs", self.res.values("E3", "p99"))],
                              fmt=lambda v: f"{v:,.0f}" if v > 1000 else f"{v:.1f}")

    def e4(self) -> None:
        """Latency at fixed offered load."""
        recs = [r for r in self.res.records if r["experiment"] == "E4"]
        if not recs:
            return
        rates = sorted({r["workload"]["rate"] for r in recs})
        self.w("## E4 — Latency at identical offered load (open loop, Poisson, 4 cores)", "",
               "Latency is measured from each request's intended send time, so queueing "
               "behind a slow response counts (no coordinated omission). Lower is better.", "")
        for q in ("p50", "p99", "p999"):
            cols = [(f"{fmt_ops(rt)}/s", self.res.values("E4", q, workload_key=next(
                r["workload_key"] for r in recs if r["workload"]["rate"] == rt))) for rt in rates]
            self.w(f"**{q} latency (µs)**", "")
            self.comparison_table(q, cols, fmt=lambda v: f"{v:.1f}", higher_is_better=False)
        series = defaultdict(list)
        for rt in rates:
            key = next(r["workload_key"] for r in recs if r["workload"]["rate"] == rt)
            for s, v in self.res.values("E4", "p99", workload_key=key).items():
                series[s].append((rt, statistics.median(samples(v))))
        self.chart("e4_p99", "offered load (ops/s)", "p99 latency (µs, median of runs)", series)
        cols = []
        for rt in rates:
            key = next(r["workload_key"] for r in recs if r["workload"]["rate"] == rt)
            cols.append((f"{fmt_ops(rt)}/s", self.res.values("E4", "achieved", workload_key=key)))
        self.w("Achieved throughput (should equal the offered load when the system keeps up):", "")
        self.comparison_table("achieved", cols)

    def e5(self) -> None:
        """Per-request floor."""
        if not any(r["experiment"] == "E5" for r in self.res.records):
            return
        self.w("## E5 — Per-request floor (no-op round trip, 1 core)", "",
               "RESP `PING`, memcached meta `mn`, gRPC `grpc.health.v1.Health/Check`: the cost "
               "of protocol, framing and event loop without any data access.", "")
        self.comparison_table("metric", [("ops/s", self.res.values("E5", "ops")),
                                         ("server CPU ns/op", self.res.values("E5", "cpu_ns_per_op"))],
                              fmt=lambda v: fmt_ops(v) if v > 10000 else f"{v:.0f}")

    def e6(self) -> None:
        """Value size effects."""
        recs = [r for r in self.res.records if r["experiment"] == "E6"]
        if not recs:
            return
        sizes = sorted({r["workload"]["value_size"] for r in recs})
        self.w("## E6 — Value size (4 cores, 100% GET, about 1 GiB of data)", "")
        cols = []
        for v in sizes:
            key = next(r["workload_key"] for r in recs if r["workload"]["value_size"] == v)
            cols.append((f"{v} B values", self.res.values("E6", "ops", workload_key=key)))
        self.comparison_table("Throughput (ops/s)", cols)
        cols = []
        for v in sizes:
            key = next(r["workload_key"] for r in recs if r["workload"]["value_size"] == v)
            cols.append((f"{v} B", self.res.values("E6", "mem_per_key", workload_key=key)))
        self.w("Memory per key (lower is better):", "")
        self.comparison_table("bytes / key", cols, fmt=lambda x: f"{x:,.0f} B", higher_is_better=False)

    def e7(self) -> None:
        """Concurrency sweep: throughput and median latency per in-flight level."""
        recs = [r for r in self.res.records if r["experiment"] == "E7"]
        if not recs:
            return
        levels = sorted({(r["workload"]["connections"], r["workload"]["depth"]) for r in recs},
                        key=lambda cd: (cd[0] * cd[1], cd[0]))
        key_of = {(r["workload"]["connections"], r["workload"]["depth"]): r["workload_key"] for r in recs}
        self.w("## E7 — Concurrency sweep (4 cores, 100% GET)", "",
               "Requests in flight = connections × depth. At 1 in flight the throughput is "
               "exactly 1 / round-trip time; beyond that, each system shows how far "
               "concurrency carries it.", "")
        cols = [(f"{c}×{d} = {c * d}", self.res.values("E7", "ops", workload_key=key_of[(c, d)])) for c, d in levels]
        self.comparison_table("Throughput (ops/s)", cols)
        cols = [(f"{c * d} in flight", self.res.values("E7", "p50", workload_key=key_of[(c, d)])) for c, d in levels]
        self.w("Median latency (µs; closed loop, so it grows with the queue by Little's law):", "")
        self.comparison_table("p50 µs", cols, fmt=lambda v: f"{v:.1f}", higher_is_better=False)
        series = defaultdict(list)
        for c, d in levels:
            for s, v in self.res.values("E7", "ops", workload_key=key_of[(c, d)]).items():
                series[s].append((c * d, statistics.median(samples(v))))
        self.chart("e7_concurrency", "requests in flight (log scale)", "GET ops/s (median)", series, logx=True)

    def stability(self) -> None:
        """How repeatable the measurements are (S1 plus all runs' timelines)."""
        s1 = self.res.values("S1", "ops")
        within = [r["m"]["within_cv"] for r in self.res.records if "within_cv" in r["m"]]
        if not s1 and not within:
            return
        self.w("## S1 — Measurement stability", "")
        if within:
            wq = statistics.quantiles(within, n=10) if len(within) >= 10 else [max(within)] * 9
            self.w(f"**Within runs** (per-second throughput samples, all {len(within)} timed runs): "
                   f"median coefficient of variation {statistics.median(within) * 100:.1f}%, "
                   f"90th percentile {wq[8] * 100:.1f}%, worst {max(within) * 100:.1f}%.", "")
        cvs = []
        for key, recs in itertools.chain(self.res.cells("E1").items(), self.res.cells("S1").items()):
            vals = [r["m"]["ops"] for r in recs if "ops" in r["m"]]
            if len(vals) >= 3:
                cvs.append((statistics.pstdev(vals) / statistics.fmean(vals), key))
        if cvs:
            worst = max(cvs)
            self.w(f"**Between runs** (fresh server each, E1 and S1 cells with ≥ 3 runs): median CV "
                   f"{statistics.median(c for c, _ in cvs) * 100:.1f}%, worst {worst[0] * 100:.1f}% "
                   f"({worst[1][0]}, {worst[1][1]} cores).", "")
        if s1:
            self.w("| System | runs | median ops/s | between-run CV | median within-run CV | "
                   "worst 1-s sample ÷ mean | median trend per 10 s | S1 ÷ E1 (same cell, hours apart) |",
                   "|---|---|---|---|---|---|---|---|")
            e1 = self.res.values("E1", "ops", cores=4)
            for s in systems_in(s1):
                v = samples(s1[s])
                runs = [r for r in self.res.records if r["experiment"] == "S1" and r["system"] == s and r["m"]]
                wcv = [r["m"]["within_cv"] for r in runs if "within_cv" in r["m"]]
                mins = [r["m"]["min_sample_ratio"] for r in runs if "min_sample_ratio" in r["m"]]
                trends = [r["m"]["trend_pct_per_10"] for r in runs if "trend_pct_per_10" in r["m"]]
                cv = statistics.pstdev(v) / statistics.fmean(v) if len(v) > 1 else 0
                # Plain lists on purpose: S1 block r and E1 block r ran hours
                # apart, so pairing them would be meaningless — unpaired test.
                drift = cmp_text(compare(v, samples(e1[s]))) if e1.get(s) else "—"
                self.w(f"| {self.title(s)} | {len(v)} | {fmt_ops(statistics.median(v))} | {cv * 100:.1f}% | "
                       f"{(statistics.median(wcv) * 100 if wcv else 0):.1f}% | "
                       f"{(min(mins) if mins else 0):.2f} | {(statistics.median(trends) if trends else 0):+.1f}% | "
                       f"{drift} |")
            self.w("", "Reading guide: a between-run CV of a few percent means a single run is "
                   "representative to within that margin; differences between systems smaller than "
                   "the CVs involved are noise. The S1 ÷ E1 column should be ≈ 1.00 (≈): the same "
                   "cell measured at the start and the end of the session.", "")

    def e8(self) -> None:
        """Client validation against standard tools."""
        recs = [r for r in self.res.records if r["experiment"] == "E8"]
        if not recs:
            return
        self.w("## E8 — Client validation (kvbench vs each ecosystem's standard tool, 4 cores)", "",
               "If kvbench reaches at least the throughput of the standard tool on the same "
               "server and workload, it does not handicap that system.", "",
               "| Server | Tool / keys | Throughput (ops/s) |", "|---|---|---|")
        # Keyed by block (repetition): every tool of a server ran once per
        # block, so kvbench ÷ tool is paired like the system comparisons.
        by: dict[tuple[str, str], dict[int, float]] = defaultdict(dict)
        for r in recs:
            if r["m"].get("ops"):
                by[(r["system"], r["tool"])][r["rep"]] = r["m"]["ops"]
        for (s, tool) in sorted(by, key=lambda k: (ORDER.index(k[0]), k[1])):
            self.w(f"| {self.title(s)} | {tool} | {cell_text(summary(by[(s, tool)]))} |")
        self.w("")
        pairs = [("redis", "kvbench-random", "redis-benchmark-random"),
                 ("redis", "kvbench-random", "memtier-random"),
                 ("redis", "kvbench-onekey", "redis-benchmark-onekey"),
                 ("kvnode", "kvbench-onekey", "h2load-onekey")]
        for s, a, b in pairs:
            if by.get((s, a)) and by.get((s, b)):
                c = compare(by[(s, a)], by[(s, b)])
                self.w(f"- {self.title(s)}: kvbench ÷ {b.split('-')[0]} = {cmp_text(c)}")
        self.w("")

    def build(self) -> str:
        """Assembles every section.

        Returns:
            The report markdown.
        """
        self.header()
        self.e0()
        self.e1()
        self.by_workload("E2", "Read/write mixes (4 cores)",
                         "100-byte values, 1M keys; Zipf uses YCSB's scrambled Zipfian with θ = 0.99.",
                         {"get90-v100-k1000000-uni-c64d16": "90% GET", "get50-v100-k1000000-uni-c64d16": "50% GET",
                          "get0-v100-k1000000-uni-c64d16": "100% SET",
                          "get90-v100-k1000000-zipf0.99-c64d16": "90% GET, Zipf 0.99",
                          "get90-v100-k100000-uni-c64d16": "90% GET", "get50-v100-k100000-uni-c64d16": "50% GET",
                          "get0-v100-k100000-uni-c64d16": "100% SET",
                          "get90-v100-k100000-zipf0.99-c64d16": "90% GET, Zipf 0.99"})
        self.e3()
        self.e4()
        self.e5()
        self.e6()
        self.e7()
        self.e8()
        self.stability()
        self.w("---", "", "Legend: `median (min–max)` over repetitions, each with a fresh server. "
               "KvNode ÷ X: ratio of medians, then either **sign** — [min–max of the per-block "
               "ratios] and the exact two-sided sign test over blocks (paired: both systems ran in "
               "the same block, so shared drift cancels) — or **MW** — [conservative range "
               "containing every pairwise run ratio] and the exact two-sided Mann–Whitney p "
               "(unpaired). ▲ KvNode clearly better, ▼ clearly worse (p < 0.05; for MW also the "
               "range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six "
               "agree; cells with 3 repetitions are descriptive only — no test can reach "
               "p < 0.05 with them.")
        return "\n".join(self.lines) + "\n"


def main() -> int:
    """Entry point.

    Returns:
        0 on success, 2 on usage errors.
    """
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    res = Results(pathlib.Path(sys.argv[1]))
    report = Report(res).build()
    (res.dir / "REPORT.md").write_text(report, encoding="utf-8")
    print(f"wrote {res.dir / 'REPORT.md'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
