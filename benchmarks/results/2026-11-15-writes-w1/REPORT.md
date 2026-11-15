# KvNode benchmark report

Profile **writes**, recorded 2026-11-15T12:52:23 UTC, repository revision `5735d7a47e54` (uncommitted changes). Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0329 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvNode (kvnode:adr0011) | `kvnode:adr0011`<br/>`fcce5d2fbbcf3160b22` | gRPC over h2c; one worker thread per core (KVNODE_WORKERS). |
| Redis 8.10.2 | `redis:8.10.2`<br/>`426335161ea6c23f3b3` | Persistence off (--save '' --appendonly no); io-threads = tuned thread count (commands still execute on the main thread). |
| Valkey 9.1.2 | `valkey/valkey:9.1.2`<br/>`faa14c828e611eb9dfd` | Persistence off; io-threads = tuned thread count. 9.2 was only a release candidate at test time. |
| Dragonfly 2.0.0 | `ghcr.io/dragonflydb/dragonfly:v2.0.0`<br/>`9a0d5f8f474b3631233` | Snapshots off (--dbfilename=); proactor_threads = tuned thread count; memlock unlimited as its docs require; io_uring is available on this host (seccomp unconfined), so Dragonfly uses its preferred I/O engine. |
| Garnet 2.1.8 | `kvbench/garnet:2.1.8`<br/>`8a297dea4465e17cfcc` | Built from Microsoft's own Dockerfile at tag v2.1.8 (the newest published image, 2.1.0, is eight releases older; 'latest' is an unreleased main build). Defaults: AOF off; the .NET thread pool sizes itself from the cpuset, so there is no thread knob to tune. |
| Memcached 1.6.45 | `memcached:1.6.45`<br/>`3de3fa823e6482be4ad` | Meta protocol (mg/ms/mn); -t = tuned thread count; -m sized so nothing is evicted; UDP off. |

## Run validity

| Experiment | ok | invalid | failed |
|---|---|---|---|
| W1 | 72 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 0.

## Host interference

Before the first run, programs outside the container VM used **7.7 of 32 logical CPUs** (peak 9.0).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| W1 | 72 | 10.3 | 17.2 | 24.0 | 72 |

**Effect:** Spearman ρ = -0.67 between outside load and each run's throughput relative to its cell's median (72 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

**Quiet-only check** (declared in advance): 0 of 60 KvNode-vs-other pairs had at most 2 CPUs of outside load on both runs, too few to recompute anything.

**Adjusted to equal outside load.** Each experiment's pairs are corrected with one common slope of log(KvNode ÷ X) on the difference in outside load between the two runs of a block, estimated within comparisons (analysis of covariance). The slope column gives the change of the ratio per logical CPU of extra outside load on KvNode's run. Raw, unadjusted results are the tables in each section below. Where the two disagree, the difference is load, not the servers.

| Experiment · cell | slope | KvNode ÷ Redis | KvNode ÷ Valkey | KvNode ÷ Dragonfly | KvNode ÷ Garnet | KvNode ÷ Memcached |
|---|---|---|---|---|---|---|
| W1 · 50% GET | -2.8% | 3.11× [2.30–4.00] ▲ | 3.19× [2.84–3.51] ▲ | 3.02× [2.85–4.01] ▲ | 1.69× [1.63–1.90] ▲ | 3.09× [2.88–3.93] ▲ |
| W1 · 100% SET | -2.8% | 2.85× [2.66–3.44] ▲ | 3.70× [3.46–4.28] ▲ | 2.92× [2.76–3.12] ▲ | 1.71× [1.40–1.90] ▲ | 4.40× [3.96–5.02] ▲ |

## W1 — Writes (4 cores)

E2's 100% SET and 50% GET cells: 100-byte values, 1M keys, uniform keys.

| Throughput (ops/s) | 100% SET | 50% GET |
|---|---|---|
| KvNode (kvnode:adr0011) | 3.10 M (2.04–3.68 M) | 3.36 M (3.03–3.68 M) |
| Redis 8.10.2 | 1.11 M (752–1.14 M) | 983 k (662–1.23 M) |
| Valkey 9.1.2 | 824 k (674–891 k) | 1.11 M (821–1.13 M) |
| Dragonfly 2.0.0 | 1.07 M (738–1.26 M) | 1.07 M (889–1.16 M) |
| Garnet 2.1.8 | 1.80 M (1.66–2.05 M) | 1.91 M (1.32–2.11 M) |
| Memcached 1.6.45 | 743 k (653–788 k) | 1.07 M (852–1.20 M) |
| *KvNode ÷ Redis* | 2.79× [1.82–4.45] sign p=0.031 ▲ | 3.42× [2.62–5.56] sign p=0.031 ▲ |
| *KvNode ÷ Valkey* | 3.76× [2.35–5.46] sign p=0.031 ▲ | 3.04× [2.77–3.69] sign p=0.031 ▲ |
| *KvNode ÷ Dragonfly* | 2.91× [2.46–3.66] sign p=0.031 ▲ | 3.15× [2.89–3.66] sign p=0.031 ▲ |
| *KvNode ÷ Garnet* | 1.72× [1.00–1.98] sign p=0.219 ≈ | 1.76× [1.46–2.43] sign p=0.031 ▲ |
| *KvNode ÷ Memcached* | 4.17× [2.61–4.94] sign p=0.031 ▲ | 3.13× [2.98–3.61] sign p=0.031 ▲ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 72 timed runs): median coefficient of variation 4.9%, 90th percentile 14.6%, worst 28.7%.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvNode ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvNode clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
