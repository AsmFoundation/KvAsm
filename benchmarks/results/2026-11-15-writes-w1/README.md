# Writes against every store after ADR 0011 (W1), 2026-11-15 — indicative only

**Question:** after
[ADR 0011](../../../docs/adr/0011-batched-writes-per-worker-reclamation.md)
(batched writes, per-worker reclamation), where do KvAsm's writes stand
against Redis, Valkey, Dragonfly, Garnet and Memcached?

**Status: indicative, not evidence for a claim.**
- The in-VM watcher flagged every one of the 72 run windows.
- Programs outside the VM kept a median 10.3 of 32 logical CPUs busy.
- The direction is clear and agrees with an independent estimate (below).
  The magnitudes need a clean re-run before
  [docs/CLAIMS.md](../../../docs/CLAIMS.md) changes.

## Result

`run.py --profile writes`: E2's 100% SET and 50% GET cells at 4 cores
(vCPUs 0–3, one thread per core), all six stores, 6 blocks. Medians, with
the range over blocks:

| Throughput | 100% SET | 50% GET |
|---|---|---|
| KvAsm (ADR 0011, `5735d7a`) | **3.10 M** (2.04–3.68 M) | **3.36 M** (3.03–3.68 M) |
| Redis 8.10.2 | 1.11 M | 983 k |
| Valkey 9.1.2 | 824 k | 1.11 M |
| Dragonfly 2.0.0 | 1.07 M | 1.07 M |
| Garnet 2.1.8 | 1.80 M | 1.91 M |
| Memcached 1.6.45 | 743 k | 1.07 M |

KvAsm ÷ each store:

| KvAsm ÷ | 100% SET, raw | adjusted for outside load | 50% GET, raw | adjusted |
|---|---|---|---|---|
| Redis | 2.79× (6/6) | 2.85× | 3.42× (6/6) | 3.11× |
| Valkey | 3.76× (6/6) | 3.70× | 3.04× (6/6) | 3.19× |
| Dragonfly | 2.91× (6/6) | 2.92× | 3.15× (6/6) | 3.02× |
| Garnet | 1.72× (5/6, p = 0.22) | 1.71× [1.40–1.90] | 1.76× (6/6) | 1.69× |
| Memcached | 4.17× (6/6) | 4.40× | 3.13× (6/6) | 3.09× |

- **"Adjusted"** is analyze.py's analysis of covariance on the difference in
  outside load between the two runs of a block (REPORT.md).
- **The one block that was not a win:** Garnet, 100% SET, block 1, at
  1.00×. KvAsm's run in that block had the session's highest outside load,
  20.4 CPUs, and dropped to 2.04M.

**An independent estimate agrees.** Take the quiet-host suite's 0.2.0
ratios (2026-09-27, E2 100% SET) and multiply by this change's paired gain
over 0.2.0 (2.6–2.8×,
[write-batching](../2026-11-15-write-batching/README.md)):

| Store | 0.2.0, 2026-09-27 | × 2.6–2.8 | W1 |
|---|---|---|---|
| Redis | 0.87× | 2.3–2.4× | 2.79× |
| Valkey | 1.26× | 3.3–3.5× | 3.76× |
| Dragonfly | 0.90× | 2.3–2.5× | 2.91× |
| Garnet | 0.60× | 1.6–1.7× | 1.72× |
| Memcached | 1.45× | 3.8–4.1× | 4.17× |

Both lines of evidence put KvAsm's writes ahead of all five stores. W1
reads 2–24% above the estimate. That is within what this host's load can
do to a ratio; the direction of load bias is not fixed (see
`benchmarks/README.md`).

## Why it is not publishable

- **Outside the VM** (`host_load`):
  - a median 10.3 of 32 logical CPUs were busy per measured window
    (5.1–24.0);
  - every run was above the 2-CPU "busy" mark;
  - Spearman ρ = −0.67 between that load and throughput.
- **Inside the VM** (`vmwatch_report.py` → `vmwatch-verdict.txt`): 72 of
  72 runs were flagged, at every clock offset from −1 to +1 s.
  - Another session's Windows desktop container (dockurr/windows) and its
    gateway were active throughout. More than the suite's two containers
    were therefore busy in every window.
  - Another session ran `make -j8 test itest` in `kvasm-dev` during 7
    runs, at up to 6 vCPUs.
  - 37 runs saw load on the side vCPUs (median 0.39, max 5.2 vCPUs).
- **Absolute rates:** the other stores ran at 0.43–0.48× their quiet-host E2
  rates of 2026-09-27 (Redis 2.58M → 1.11M SET/s). The host lost about
  half its capacity.

## Setup

- **Images:** every other store's image is the exact one the 2026-09-27
  suite measured (`environment.json` ids equal that session's).
  - The `redis:8.10.2` and `memcached:1.6.45` tags had since been
    re-pushed, so those two images were pulled by digest and re-tagged.
  - kvbench is the same image, `952a6003…`.
- **KvAsm** is `kvasm:adr0011`, built from the tree committed as
  `5735d7a`. That build passed every server suite, and the .NET suite
  passed 380/380 against it.
  - `environment.json` says "uncommitted changes" only because this folder
    held the watcher's scripts when the run started.
  - `KVASM_BENCH_IMAGE=kvasm:adr0011` names the image in the report
    instead of "0.2.0".
- **Placement and method:** unchanged from METHODOLOGY.md. The thread
  count is one per core, as E2 used.

## Files

| File | What |
|---|---|
| `raw.jsonl`, `environment.json`, `REPORT.md` | The session, its environment, and analyze.py's report |
| `run.log` | run.py's console output |
| `vmwatch.sh`, `vmwatch.log` | The in-VM watcher (pinned to vCPU 15) and its samples |
| `vmwatch_report.py` → `vmwatch-verdict.txt` | The per-run interference verdict |

## Reproduce, on a quiet host

```sh
docker build --target runtime -t kvasm:adr0011 .
KVASM_BENCH_IMAGE=kvasm:adr0011 python benchmarks/run.py --profile writes \
    --out benchmarks/results/<date>-writes
KVASM_BENCH_IMAGE=kvasm:adr0011 python benchmarks/analyze.py benchmarks/results/<date>-writes
python vmwatch_report.py --log vmwatch.log --runs raw.jsonl \
    --watch f90f10d9fc38 --dev bae3fd5a0c43 --offset 0
```

Before running, check the following:
- `docker stats`: nothing but the suite's containers is busy;
- `run.py`'s first line: outside load below 2 CPUs.
