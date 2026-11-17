#!/usr/bin/env python3
"""pairs.py - paired, block-by-block comparisons that REPORT.md does not print.

analyze.py pairs KvAsm with every system inside E1-E7 and N1/N3, but gives
S1 only per-system medians. S1 is this session's one quiet-host run of the
4-core GET cell (E1's cell, ten blocks, run last), while E1 itself ran mostly
under 13-25 CPUs of outside load. This script applies the report's own
method to S1 - ratio of medians, [min-max of per-block ratios], exact
two-sided sign test over blocks (METHODOLOGY.md section 7) - and adds the
client/server saturation figures that decide whether KvAsm's cells measure
the server or the load generator.

Usage: python pairs.py [RAW.jsonl]   (default: raw.jsonl next to this file)
"""

from __future__ import annotations

import json
import math
import pathlib
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
RIVALS = ("redis", "valkey", "dragonfly", "garnet", "memcached")


def sign_test(wins: int, n: int) -> float:
    """Exact two-sided sign test.

    Args:
        wins: blocks in which KvAsm's ratio exceeded 1.
        n: blocks compared (ties, i.e. ratio exactly 1, never occur with
            continuous throughput and are not special-cased).

    Returns:
        P(a split at least this uneven | both directions equally likely),
        capped at 1.
    """
    k = min(wins, n - wins)
    tail = sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n
    return min(1.0, 2 * tail)


def load(raw: pathlib.Path) -> list[dict]:
    """Valid kvbench records of a session.

    Args:
        raw: raw.jsonl written by run.py.

    Returns:
        Records whose status is "ok" and whose measured phase is valid.
    """
    out = []
    for line in raw.read_text(encoding="utf-8").splitlines():
        if line.strip():
            r = json.loads(line)
            if r.get("status") == "ok" and r.get("kvbench", {}).get("result", {}).get("valid"):
                out.append(r)
    return out


def paired(records: list[dict], experiment: str, cores: int, factor: float) -> None:
    """Prints KvAsm against every rival for one experiment's GET cell.

    Args:
        records: valid records.
        experiment: experiment id (S1 or E1).
        cores: the cell's core budget.
        factor: the session's CPU clock factor (server CPU is divided by it,
            as analyze.py does).
    """
    cell = [r for r in records if r["experiment"] == experiment and r["cores"] == cores]
    ops = {(r["system"], r["rep"]): r["kvbench"]["result"]["ops_per_sec"] for r in cell}
    blocks = sorted({r["rep"] for r in cell})
    kv = [ops[("kvasm", b)] for b in blocks if ("kvasm", b) in ops]
    loads = [r["host_load"]["mean"] for r in cell if r.get("host_load")]
    print(f"{experiment} {cores} cores: {len(blocks)} blocks, outside load median "
          f"{statistics.median(loads):.1f} CPUs; KvAsm median {statistics.median(kv) / 1e6:.2f} M")
    for rival in RIVALS:
        # Only blocks where both ran count: a missing run drops its block's
        # pair and nothing else (METHODOLOGY.md section 7).
        both = [b for b in blocks if ("kvasm", b) in ops and (rival, b) in ops]
        ratios = [ops[("kvasm", b)] / ops[(rival, b)] for b in both]
        med_ratio = (statistics.median(ops[("kvasm", b)] for b in both)
                     / statistics.median(ops[(rival, b)] for b in both))
        wins = sum(x > 1 for x in ratios)
        print(f"  KvAsm / {rival:<9} {med_ratio:5.2f}x [{min(ratios):.2f}-{max(ratios):.2f}] "
              f"{wins}/{len(ratios)} blocks, sign p={sign_test(wins, len(ratios)):.3f}  "
              f"({rival} median {statistics.median(ops[(rival, b)] for b in both) / 1e6:.2f} M)")
    # Saturation: which side of the loopback was the limit? kvbench reports
    # its own CPU share of the 16 client vCPUs; the server's comes from its
    # cgroup (seconds of CPU over the 10-s window, clock-corrected).
    for system in ("kvasm",):
        rs = [r["kvbench"]["result"] for r in cell if r["system"] == system]
        server = statistics.median(x["server_cpu_s"] / x["seconds"] / factor / cores for x in rs)
        client = statistics.median(x["client_cpu_util"] for x in rs)
        print(f"  {system}: server busy {server:.2f} of its {cores} cores, client busy {client:.2f} "
              f"of 16 vCPUs (client-bound flag: client > 0.85 and server < 0.90)")


def main() -> int:
    """Entry point.

    Returns:
        0.
    """
    raw = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "raw.jsonl"
    env = json.loads((raw.parent / "environment.json").read_text(encoding="utf-8"))
    factor = env["cpu_clock"]["factor"]
    records = load(raw)
    paired(records, "S1", 4, factor)
    paired(records, "E1", 4, factor)
    paired(records, "E1", 8, factor)
    return 0


if __name__ == "__main__":
    sys.exit(main())
