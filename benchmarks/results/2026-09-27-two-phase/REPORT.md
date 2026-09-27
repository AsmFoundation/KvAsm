# KvNode benchmark report

Profile **standard**, recorded 2026-09-27T05:14:37 UTC, repository revision `ac100eb89083` (uncommitted changes). Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0886 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvNode 0.1.0 | `kvnode:0.1.0`<br/>`3fa31d893381e8b8a99` | gRPC over h2c; one worker thread per core (KVNODE_WORKERS). |
| Redis 8.10.2 | `redis:8.10.2`<br/>`426335161ea6c23f3b3` | Persistence off (--save '' --appendonly no); io-threads = tuned thread count (commands still execute on the main thread). |
| Valkey 9.1.2 | `valkey/valkey:9.1.2`<br/>`faa14c828e611eb9dfd` | Persistence off; io-threads = tuned thread count. 9.2 was only a release candidate at test time. |
| Dragonfly 2.0.0 | `ghcr.io/dragonflydb/dragonfly:v2.0.0`<br/>`9a0d5f8f474b3631233` | Snapshots off (--dbfilename=); proactor_threads = tuned thread count; memlock unlimited as its docs require; io_uring is available on this host (seccomp unconfined), so Dragonfly uses its preferred I/O engine. |
| Garnet 2.1.8 | `kvbench/garnet:2.1.8`<br/>`8a297dea4465e17cfcc` | Built from Microsoft's own Dockerfile at tag v2.1.8 (the newest published image, 2.1.0, is eight releases older; 'latest' is an unreleased main build). Defaults: AOF off; the .NET thread pool sizes itself from the cpuset, so there is no thread knob to tune. |
| Memcached 1.6.45 | `memcached:1.6.45`<br/>`3de3fa823e6482be4ad` | Meta protocol (mg/ms/mn); -t = tuned thread count; -m sized so nothing is evicted; UDP off. |

## Run validity

| Experiment | ok | invalid | failed |
|---|---|---|---|
| N1 | 18 | 0 | 0 |
| N3 | 18 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 0.

## Host interference

Before the first run, programs outside the container VM used **11.1 of 32 logical CPUs** (peak 13.3).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| N1 | 18 | 9.0 | 17.2 | 19.1 | 18 |
| N3 | 18 | 5.9 | 7.8 | 9.4 | 18 |

**Effect:** Spearman ρ = -0.41 between outside load and each run's throughput relative to its cell's median (36 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

## N — one core: KvNode vs Redis and Valkey

E1's and E5's 1-core cells for the single-threaded-execution servers, run on their own so the headline is re-checked with full pairing. Ratios are KvNode ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvNode 0.1.0 | 1.65 M (1.18–1.73 M) | 1.65 M (1.18–1.72 M) | 608 (580–844) | 545 (528–569) | 1393 (1171–9765) |
| Redis 8.10.2 | 1.26 M (975–1.43 M) | 1.25 M (978–1.42 M) | 799 (706–1023) | 729 (684–807) | 1888 (1270–8716) |
| Valkey 9.1.2 | 1.34 M (1.15–1.40 M) | 1.35 M (1.14–1.40 M) | 740 (716–880) | 725 (700–799) | 1368 (1221–1991) |
| *KvNode ÷ Redis* | 1.32× [0.82–1.57] sign p=0.219 ≈ | 1.31× [0.84–1.60] sign p=0.219 ≈ | 0.76× [0.63–1.20] sign p=0.219 ≈ | 0.75× [0.71–0.78] sign p=0.031 ▲ | 0.74× [0.17–7.69] sign p=0.219 ≈ |
| *KvNode ÷ Valkey* | 1.23× [0.86–1.44] sign p=0.219 ≈ | 1.22× [0.85–1.46] sign p=0.219 ≈ | 0.82× [0.68–1.17] sign p=0.219 ≈ | 0.75× [0.68–0.79] sign p=0.031 ▲ | 1.02× [0.70–7.89] sign p=0.688 ≈ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvNode 0.1.0 | 2.03 M (2.01–2.16 M) | 492 (463–501) |
| Redis 8.10.2 | 2.11 M (2.05–2.14 M) | 476 (462–488) |
| Valkey 9.1.2 | 1.95 M (1.90–1.97 M) | 516 (512–527) |
| *KvNode ÷ Redis* | 0.96× [0.95–1.01] sign p=0.219 ≈ | 1.03× [0.99–1.05] sign p=0.219 ≈ |
| *KvNode ÷ Valkey* | 1.04× [1.03–1.10] sign p=0.031 ▲ | 0.95× [0.90–0.96] sign p=0.031 ▲ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 36 timed runs): median coefficient of variation 4.2%, 90th percentile 18.4%, worst 28.5%.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvNode ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvNode clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
