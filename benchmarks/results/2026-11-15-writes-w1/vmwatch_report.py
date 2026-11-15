#!/usr/bin/env python3
"""vmwatch_report.py - interference evidence from vmwatch.sh's samples.

vmwatch.sh logs cumulative per-vCPU busy jiffies of the whole Docker VM and
cumulative CPU of every container cgroup, every few seconds. This script
differences consecutive samples into intervals and answers one of two
questions:

* ``--tail N`` (live polls): did anything that is not the benchmark suite
  run inside the VM during the last N intervals?
* ``--runs RAW.jsonl`` (after a session): which runs' measured windows
  overlapped such work? Those runs, or their whole blocks, are candidates
  for a re-run; everything else is certified clean of in-VM interference.

Why a separate tool: run.py's host-load sampler sees only CPU used OUTSIDE
the VM. Another Claude session building or benchmarking in the kvnode-dev
container, a BuildKit build, or a helper container competes for the same
vCPUs and is invisible to it.

What counts as interference in an interval (thresholds below):
* the dev container used CPU (its idle state is ``sleep infinity``, so any
  real use means someone exec'd into it: a build, a test, an A/B);
* BuildKit used CPU (a ``docker build``);
* more than the suite's own two containers (server + client) were active,
  judged only on intervals fully inside a run's window, because at window
  edges the previous or next run's containers legitimately appear;
* the "side" vCPUs - the ones neither the servers (0-7) nor the client
  (16-31) are pinned to, minus the monitor's own - were busy in a
  SUSTAINED way: unpinned work lands on idle vCPUs first, so these are
  where stray load shows, but single intervals legitimately spike at run
  boundaries and during polls (see SIDE_MAX), so a run window is judged by
  its interior mean and a live poll by two or more consecutive intervals.

Usage:
    docker logs kvnode-vmwatch | python vmwatch_report.py --tail 50 \\
        --watch 3719a2e3beb5 --dev bae3fd5a0c43
    docker logs kvnode-vmwatch | python vmwatch_report.py --runs raw.jsonl \\
        --watch 3719a2e3beb5 --dev bae3fd5a0c43 [--offset -0.45]
"""

from __future__ import annotations

import argparse
import datetime
import json
import sys
from dataclasses import dataclass, field

USER_HZ = 100
"""/proc/stat's jiffy rate (Linux USER_HZ; 100 on x86-64 kernels)."""

DEV_MS = 20.0
"""Dev-container CPU (ms per interval) above which it counts as active. Its
idle state is ``sleep infinity``, measured at exactly 0 ms; 20 ms per 5-s
interval is 0.4% of one vCPU - far above idle, far below a build or bench."""

BUILDKIT_MS = 20.0
"""BuildKit CPU (ms per interval) above which a docker build was running."""

OTHER_MS = 1.0
"""A non-dev container counts as active in an interval above this CPU (ms);
it filters out cgroups that merely exist (a stopped container's residue)."""

OWN_CONTAINERS = 2
"""Containers the suite itself runs at once: the server and one client."""

SIDE_MAX = 0.25
"""Side-vCPU busy (in vCPUs) above which stray work is assumed - judged on a
SUSTAINED basis, never on one interval. An idle VM measured 0.00, but single
5-s intervals legitimately reach 0.3-0.5 when the suite starts or removes
containers at a run boundary, or when the observer itself calls the docker
API (`docker logs` for a poll); measured 2026-09-27 by aligning intervals
with run boundaries and poll times. Real interference (a WSL-distro job, an
unpinned build) keeps the side vCPUs busy for many intervals, so run windows
are judged by their mean over interior intervals and live polls by two or
more consecutive intervals above this level."""


@dataclass
class Sample:
    """One monitor line.

    Attributes:
        t: VM epoch seconds when the sample was taken.
        cpus: cumulative busy jiffies per vCPU (index = vCPU number).
        ctrs: cumulative CPU nanoseconds per container-id prefix (or the
            literal "buildkit" cgroup).
    """

    t: float
    cpus: list[int]
    ctrs: dict[str, int]


@dataclass
class Interval:
    """The difference between two consecutive samples.

    Attributes:
        t0: interval start (VM epoch seconds).
        t1: interval end.
        side: average busy vCPUs over the side vCPUs.
        dev_ms: CPU the dev container used (ms).
        buildkit_ms: CPU BuildKit used (ms).
        others: CPU (ms) per other active container (the suite's own and
            anything foreign - they cannot be told apart by id alone).
    """

    t0: float
    t1: float
    side: float
    dev_ms: float
    buildkit_ms: float
    others: dict[str, float] = field(default_factory=dict)


def parse_cpu_set(spec: str) -> list[int]:
    """Parses a cpuset-style list such as ``8-14`` or ``8-11,13``.

    Args:
        spec: comma-separated vCPU numbers and inclusive ranges.

    Returns:
        The vCPU numbers, in order.

    Raises:
        ValueError: a component is not an integer or a range of integers.
    """
    out: list[int] = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        out.extend(range(int(lo), int(hi or lo) + 1))
    return out


def parse(lines) -> list[Sample]:
    """Reads monitor lines, skipping anything that is not a sample.

    Args:
        lines: iterable of text lines (``docker logs`` output).

    Returns:
        Samples in log order.
    """
    samples = []
    for line in lines:
        line = line.strip()
        # Anything else (a stray error line from awk, a truncated last line
        # while the container is being read) is not a sample.
        if not line[:1].isdigit() or " cpu " not in line or "|" not in line:
            continue
        head, _, tail = line.partition("|")
        words = head.split()
        try:
            t = float(words[0])
            cpus = [int(w) for w in words[2:]]
            ctrs = {}
            for kv in tail.split():
                k, _, v = kv.partition("=")
                ctrs[k] = int(v)
        except ValueError:
            continue
        samples.append(Sample(t, cpus, ctrs))
    return samples


def intervals(samples: list[Sample], side: list[int], watch: str, dev: str) -> list[Interval]:
    """Differences consecutive samples.

    A container absent from the earlier sample was created inside the
    interval, so its whole counter is attributed to it. A container absent
    from the later sample was removed inside it; its last partial interval
    is lost (only short tails at run boundaries, never a measured window's
    interior, because runs remove their containers after measuring).

    Args:
        samples: parsed samples, in time order.
        side: vCPUs whose busy time signals stray work.
        watch: the monitor's own container-id prefix (excluded everywhere).
        dev: the dev container's id prefix.

    Returns:
        One interval per consecutive pair with positive duration.
    """
    out = []
    for a, b in zip(samples, samples[1:]):
        dt = b.t - a.t
        if dt <= 0 or len(a.cpus) != len(b.cpus):
            continue  # clock step or a VM with a changed vCPU count: unusable
        busy = sum(b.cpus[i] - a.cpus[i] for i in side if i < len(b.cpus))
        deltas = {k: (v - a.ctrs.get(k, 0)) / 1e6 for k, v in b.ctrs.items()}
        others = {k: ms for k, ms in deltas.items()
                  if k not in (watch, dev, "buildkit") and ms > OTHER_MS}
        out.append(Interval(a.t, b.t, busy / USER_HZ / dt, deltas.get(dev, 0.0),
                            deltas.get("buildkit", 0.0), others))
    return out


def flags(iv: Interval, interior: bool) -> list[str]:
    """Reasons an interval counts as interfered, side vCPUs excepted.

    Side-vCPU load is deliberately not judged here: one busy interval is
    normal at run boundaries and during polls (see SIDE_MAX), so callers
    judge it over several intervals.

    Args:
        iv: the interval.
        interior: True when the interval lies fully inside the window being
            judged, so the suite's own containers are exactly two.

    Returns:
        Human-readable reasons; empty when the interval is clean.
    """
    why = []
    if iv.dev_ms > DEV_MS:
        why.append(f"dev container {iv.dev_ms:.0f} ms")
    if iv.buildkit_ms > BUILDKIT_MS:
        why.append(f"buildkit {iv.buildkit_ms:.0f} ms")
    if interior and len(iv.others) > OWN_CONTAINERS:
        why.append(f"{len(iv.others)} active containers")
    return why


def sustained_side(ivs: list[Interval]) -> list[Interval]:
    """Intervals that are part of a run of two or more consecutive intervals
    with side-vCPU load above SIDE_MAX.

    Args:
        ivs: consecutive intervals.

    Returns:
        The intervals in such runs (empty when load only blipped).
    """
    hot = [iv.side > SIDE_MAX for iv in ivs]
    return [iv for i, iv in enumerate(ivs)
            if hot[i] and ((i > 0 and hot[i - 1]) or (i + 1 < len(ivs) and hot[i + 1]))]


def tail_report(ivs: list[Interval], n: int) -> None:
    """Prints a compact summary of the last n intervals (live polls).

    Args:
        ivs: all intervals.
        n: how many of the most recent intervals to summarize.
    """
    last = ivs[-n:]
    if not last:
        print("vmwatch: no intervals yet")
        return
    # Interior-ness is unknown without run windows, so the container count
    # is shown but not judged: at a run boundary the old and the new run's
    # containers can share an interval.
    bad = [(iv, flags(iv, interior=False)) for iv in last]
    bad = [(iv, w) for iv, w in bad if w]
    bad += [(iv, [f"sustained side vCPUs {iv.side:.2f} busy"]) for iv in sustained_side(last)]
    bad.sort(key=lambda b: b[0].t1)
    age = datetime.datetime.now().timestamp() - last[-1].t1
    print(f"vmwatch: {len(last)} intervals, {last[0].t0:.0f}..{last[-1].t1:.0f} (last sample {age:.0f}s ago); "
          f"side max {max(iv.side for iv in last):.2f} vCPU; dev {sum(iv.dev_ms for iv in last):.0f} ms; "
          f"buildkit {sum(iv.buildkit_ms for iv in last):.0f} ms; "
          f"max active containers {max(len(iv.others) for iv in last)}; flagged {len(bad)}")
    for iv, why in bad[-5:]:
        stamp = datetime.datetime.fromtimestamp(iv.t1).strftime("%H:%M:%S")
        print(f"  {stamp} {'; '.join(why)}")


def run_windows(raw_path: str, offset: float) -> list[dict]:
    """Measured windows of every run in a results file, on the VM clock.

    kvbench runs measure their last ``duration`` seconds, ending just before
    the server is removed; the window used here is
    [end - duration - 3 s, end] with end = started + seconds, a superset that
    also covers the removal. Validation runs (E8) time their own tools, so
    their whole run is the window.

    Args:
        raw_path: raw.jsonl written by run.py.
        offset: VM clock minus Windows clock, in seconds (run.py stamps runs
            with the Windows clock; the monitor with the VM's).

    Returns:
        One dict per run: run_id, experiment, system, rep, status, w0, w1.
    """
    out = []
    with open(raw_path, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            rec = json.loads(line)
            start = datetime.datetime.fromisoformat(rec["started"]).timestamp() + offset
            end = start + rec.get("seconds", 0.0)
            w0 = start if rec.get("kind") == "validation" else end - rec["duration"] - 3.0
            out.append({"run_id": rec["run_id"], "experiment": rec["experiment"],
                        "system": rec["system"], "rep": rec["rep"], "status": rec.get("status"),
                        "w0": w0, "w1": end})
    return out


def runs_report(ivs: list[Interval], raw_path: str, offset: float) -> int:
    """Prints which runs overlapped in-VM interference.

    Args:
        ivs: all intervals.
        raw_path: raw.jsonl written by run.py.
        offset: VM clock minus Windows clock, in seconds.

    Returns:
        The number of flagged runs (0 = the session is clean).
    """
    runs = run_windows(raw_path, offset)
    flagged = []
    uncovered = 0
    side_by_exp: dict[str, list[float]] = {}
    for r in runs:
        touching = [iv for iv in ivs if iv.t0 < r["w1"] and iv.t1 > r["w0"]]
        if not touching:
            uncovered += 1  # the monitor was not running then: no evidence
            continue
        reasons = []
        for iv in touching:
            interior = iv.t0 >= r["w0"] and iv.t1 <= r["w1"]
            reasons += flags(iv, interior)
        # Side load is judged on the window's interior only: its edges hold
        # the suite's own container start/removal. A window too short to
        # contain a whole interval falls back to every interval touching it.
        inner = [iv for iv in touching if iv.t0 >= r["w0"] and iv.t1 <= r["w1"]] or touching
        side = sum(iv.side * (iv.t1 - iv.t0) for iv in inner) / sum(iv.t1 - iv.t0 for iv in inner)
        if side > SIDE_MAX:
            reasons.append(f"side vCPUs {side:.2f} busy (window mean)")
        side_by_exp.setdefault(r["experiment"], []).append(side)
        if reasons:
            flagged.append((r, sorted(set(reasons))))
    print(f"{len(runs)} runs; {len(runs) - uncovered - len(flagged)} clean, {len(flagged)} flagged, "
          f"{uncovered} without monitor coverage")
    for exp, vals in side_by_exp.items():
        vals.sort()
        print(f"  {exp}: side-vCPU busy per run window (interior mean), median "
              f"{vals[len(vals) // 2]:.3f}, max {vals[-1]:.3f} vCPU")
    for r, why in flagged:
        print(f"  FLAG {r['run_id']} (block {r['rep']}, {r['status']}): {'; '.join(why)}")
    return len(flagged)


def main() -> int:
    """Entry point.

    Returns:
        0 when clean or in --tail mode; 1 when --runs flagged any run.
    """
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", help="monitor log file (default: stdin)")
    ap.add_argument("--watch", required=True, help="the monitor's own container-id prefix")
    ap.add_argument("--dev", required=True, help="the dev container's id prefix")
    ap.add_argument("--side", default="8-14", help="vCPUs where stray work shows (default 8-14)")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--tail", type=int, help="summarize the last N intervals")
    mode.add_argument("--runs", help="raw.jsonl whose run windows to check")
    ap.add_argument("--offset", type=float, default=0.0, help="VM clock minus Windows clock (s)")
    args = ap.parse_args()

    src = open(args.log, encoding="utf-8") if args.log else sys.stdin
    with src:
        samples = parse(src)
    ivs = intervals(samples, parse_cpu_set(args.side), args.watch, args.dev)
    if args.tail:
        tail_report(ivs, args.tail)
        return 0
    return 1 if runs_report(ivs, args.runs, args.offset) else 0


if __name__ == "__main__":
    sys.exit(main())
