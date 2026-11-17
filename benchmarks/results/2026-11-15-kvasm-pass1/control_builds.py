#!/usr/bin/env python3
"""control_builds.py — several KvAsm images on one cell, interleaved, in the suite's placement.

Why it exists: the 2026-11-15 KvAsm-only suite measured clean, consistent
1-core GET runs at ~1,080 ns of server CPU per GET, where the 2026-09-27 suite
measured 720 (E1) and 576 (N1). A cross-session difference like that is
either the host (vCPU placement on a hybrid CPU, clocks) or the code. Running
the very image the 2026-09-27 suite measured next to the newer builds, in the
same placement and the same minutes, tells the two apart: if the old image is
as slow as the new one today, the host moved; if not, the code did.

Each round runs every image once on the same cell, in a rotated order
(round r starts at image r mod n), so position effects and slow drift spread
evenly. Every run is a full suite run: run.run_kvbench_cell with a fresh
server container on vCPUs 0..cores-1, kvbench on 16-31, preload, warm-up,
measurement, removal.

Usage (repository root, no other benchmark running):
    python benchmarks/results/2026-11-15-kvasm-pass1/control_builds.py \\
        ROUNDS CORES IMAGE [IMAGE ...] [--read-ratio R] > control.out

Args (command line):
    ROUNDS: rounds; each runs every image once.
    CORES: core budget (vCPUs 0..CORES-1, one worker per core).
    IMAGE: KvAsm image tags to compare, e.g. kvasm:0.1.0 kvasm:adr0011.
    --read-ratio R: GET fraction of the workload (default 1.0, E1's).

Returns:
    Exit code 0; prints one line per run, then per image the median ops/s
    and server CPU per op, and each image's per-round ratio to the first.

Raises:
    SystemExit: on wrong arguments.
    subprocess.CalledProcessError: when docker cannot start a server
        (run.start_server), e.g. a missing image.
"""

from __future__ import annotations

import json
import pathlib
import statistics
import sys
from dataclasses import replace

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent))  # benchmarks/: run.py and suite.py
import run  # noqa: E402
import suite  # noqa: E402


def main() -> int:
    """Runs the interleaved rounds and prints the summary.

    Returns:
        0 when every run was valid, 1 otherwise.

    Raises:
        SystemExit: on wrong arguments.
    """
    args = sys.argv[1:]
    ratio = 1.0
    if "--read-ratio" in args:
        i = args.index("--read-ratio")
        ratio = float(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        raise SystemExit(__doc__)
    rounds, cores, images = int(args[0]), int(args[1]), args[2:]
    sys.stdout.reconfigure(encoding="utf-8")
    # One variant system per image, identical to the suite's kvasm system in
    # every setting but the image, so only the build differs between runs.
    names = []
    for img in images:
        name = "kvasm@" + img.split(":", 1)[-1]
        suite.SYSTEMS[name] = replace(suite.SYSTEMS["kvasm"], name=name, title=f"KvAsm ({img})", image=img)
        names.append(name)
    run.HOST = run.HostLoad()
    run.HOST.start()
    cg = run.Cgroups()
    # The suite divides server CPU by the cgroup clock factor of its session;
    # use this session's (environment.json), so the numbers match REPORT.md.
    env = json.loads((HERE / "environment.json").read_text(encoding="utf-8"))
    factor = env.get("cpu_clock", {}).get("factor", 1.0) or 1.0
    wl = replace(suite.BASE, read_ratio=ratio)
    rows: dict[str, list[tuple[int, float, float]]] = {n: [] for n in names}
    ok = True
    for r in range(rounds):
        order = names[r % len(names):] + names[:r % len(names)]
        for name in order:
            cell = {"system": name, "cores": cores, "threads": cores, "workload_obj": wl,
                    "warmup": 5.0, "duration": 10.0, "rep": r,
                    "run_id": f"control/{name}/c{cores}/{wl.key()}/r{r}"}
            out = run.run_kvbench_cell(cell, cg)
            res = out["kvbench"].get("result", {})
            if not run.kvbench_valid(out["kvbench"]):
                ok = False
                print(f"round {r} {name}: INVALID", flush=True)
                continue
            ops = res["ops_per_sec"]
            cpu = res.get("server_cpu_s", -1)
            ns = cpu / factor * 1e9 / res["ops"] if cpu and cpu > 0 else float("nan")
            hl = (out.get("host_load") or {}).get("mean")
            rows[name].append((r, ops, ns))
            print(f"round {r} {name:<16} {ops / 1e6:6.3f} M ops/s  {ns:7.0f} ns/op  "
                  f"client {res.get('client_cpu_util') or 0:.2f}  outside load {hl if hl is not None else float('nan'):.1f}",
                  flush=True)
    print()
    base = {r: (o, n) for r, o, n in rows[names[0]]}
    for name in names:
        rs = rows[name]
        if not rs:
            continue
        line = (f"{name:<16} median {statistics.median(o for _, o, _ in rs) / 1e6:6.3f} M ops/s, "
                f"{statistics.median(n for _, _, n in rs):6.0f} ns/op")
        if name != names[0]:
            ratios = [o / base[r][0] for r, o, _ in rs if r in base]
            line += "; ops ÷ " + names[0] + " per round " + ", ".join(f"{x:.3f}" for x in ratios)
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
