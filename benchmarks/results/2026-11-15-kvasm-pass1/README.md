# KvAsm-only suite, first pass (2026-11-15) — stopped: the host moved, not the code

**What it was:** the standard suite for KvAsm alone, after ADR 0011
(`run.py --systems kvasm --e4-rates 418000,836000,1254000`). Its numbers
were to be set against the other stores' numbers of 2026-09-27.
- Image: `kvasm:adr0011`, built from `5735d7a`.
- 45 of 136 runs completed: E0, E1, and 3 of E2.

**Why it stopped.** Clean E1 runs on one core measured the following:

| | GET/s | Server CPU per GET |
|---|---|---|
| This pass | 0.91–0.95M | 1,048–1,098 ns |
| 2026-09-27, E1 (loaded host) | 1.39M | 720 ns |
| 2026-09-27, N1 (quiet host) | 1.76M | 576 ns |

The difference was either the host or a GET regression. The suite was
paused to tell them apart.

## The control (`control_builds.py` → `control-1core-get.out`)

The E1 one-core cell was run in the suite's own placement for three images,
interleaved, 3 rounds, with the image order rotated each round.

| Image | GET/s, median | CPU per GET | ÷ the 2026-09-27 image, per round |
|---|---|---|---|
| `kvasm:0.1.0` (`3728b25d…`), **the very image the 2026-09-27 suite measured** (a0264f7) | 0.934M | 1,072 ns | — |
| `kvasm:0.2.0`, the release | 0.918M | 1,083 ns | 0.973, 0.965, 1.049 |
| `kvasm:adr0011`, this change | 0.911M | 1,092 ns | 1.052, 0.976, 0.964 |

- **No regression:** the three builds are equal within ±5%, with no
  consistent direction.
- **The host is ~1.5× slower per GET today than on 2026-09-27:**
  - the 2026-09-27 image now needs 1,072 ns per GET on one core, where it
    needed 720 ns (E1) and 576 ns (N1) that day;
  - outside load was not the cause: it was 4–6 CPUs today against 16
    during that E1.

  The likeliest cause is where Hyper-V places the VM's vCPUs on this
  hybrid P/E-core CPU. The suite cannot see that placement.
- **Consequence:** KvAsm's absolute numbers from today cannot be set
  against the other stores' numbers from 2026-09-27. The suite was
  restarted with that image as a paired control in every cell
  ([2026-11-15-kvasm-standard](../2026-11-15-kvasm-standard/README.md)).

## What the pass measured anyway

These are the new build's numbers on today's slower host
(`REPORT.md`, `compare.md`):
- **E1** (GET/s): 1 core 0.92M, 2 cores 2.05M, 4 cores 4.49M, 8 cores
  5.25M.
  - E0 picked 8 threads at 8 cores. On 2026-09-27 it picked 7.
- **E2, 100% SET at 4 cores:** 4.00M in its one run, against 2.23M on
  2026-09-27.
- **Interference** (`vmwatch-verdict.txt`, with the host's idle daemons
  declared via `--idle`): 29 of 45 runs clean.
  - E0's first two blocks overlapped another session's `make test itest`
    in `kvasm-dev`.

## Files

| File | What |
|---|---|
| `raw.jsonl`, `environment.json`, `REPORT.md`, `run.log` | The pass (`run.log`'s last runs were cut by the stop) |
| `control_builds.py` → `control-1core-get.out` | The three-image control |
| `compare_sessions.py` → `compare.md` | Every cell against 2026-09-27, KvAsm and the other stores (unpaired, across sessions) |
| `vmwatch.sh`, `vmwatch_report.py` → `vmwatch-verdict.txt` | The in-VM watcher and its verdict |

`vmwatch_report.py` here gained `--idle`. Long-lived idle containers (a
desktop-fleet agent, a BuildKit builder, an SDK container) each use 1–10 ms
per 5-s interval, above the old 1-ms bar. Without the option they flagged
every run as "3–4 active containers". Declared idle, they flag a run only
above 20 ms per interval (`IDLE_MS`).

```sh
python vmwatch_report.py --log vmwatch.log --runs raw.jsonl --watch ffbafb7fae4a \
    --dev bae3fd5a0c43 --idle 577cf5134f8d,57e53854c733,a19fb32fe59d,buildx --offset 0
```
