# KvNode benchmark report

Profile **standard**, recorded 2026-09-26T17:44:08 UTC, repository revision `ec83e0e2f252` (uncommitted changes). Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0358 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvNode 0.1.0 | `kvnode:0.1.0`<br/>`093e181df167f5310cc` | gRPC over h2c; one worker thread per core (KVNODE_WORKERS). |
| KvNode 0.1.0 native-h2 | `kvnode:0.1.0`<br/>`093e181df167f5310cc` | KvNode with its own HTTP/2 engine (server/src/h2n.asm, KVNODE_H2_ENGINE=native). |
| KvNode 0.1.0 nghttp2 | `kvnode:0.1.0`<br/>`093e181df167f5310cc` | KvNode with nghttp2 1.70 as its HTTP/2 engine (KVNODE_H2_ENGINE=nghttp2). |
| Redis 8.10.2 | `redis:8.10.2`<br/>`426335161ea6c23f3b3` | Persistence off (--save '' --appendonly no); io-threads = tuned thread count (commands still execute on the main thread). |
| Valkey 9.1.2 | `valkey/valkey:9.1.2`<br/>`faa14c828e611eb9dfd` | Persistence off; io-threads = tuned thread count. 9.2 was only a release candidate at test time. |
| Dragonfly 2.0.0 | `ghcr.io/dragonflydb/dragonfly:v2.0.0`<br/>`9a0d5f8f474b3631233` | Snapshots off (--dbfilename=); proactor_threads = tuned thread count; memlock unlimited as its docs require; io_uring is available on this host (seccomp unconfined), so Dragonfly uses its preferred I/O engine. |
| Garnet 2.1.8 | `kvbench/garnet:2.1.8`<br/>`8a297dea4465e17cfcc` | Built from Microsoft's own Dockerfile at tag v2.1.8 (the newest published image, 2.1.0, is eight releases older; 'latest' is an unreleased main build). Defaults: AOF off; the .NET thread pool sizes itself from the cpuset, so there is no thread knob to tune. |
| Memcached 1.6.45 | `memcached:1.6.45`<br/>`3de3fa823e6482be4ad` | Meta protocol (mg/ms/mn); -t = tuned thread count; -m sized so nothing is evicted; UDP off. |

## Run validity

| Experiment | ok | invalid | failed |
|---|---|---|---|
| N1 | 24 | 0 | 0 |
| N2 | 24 | 0 | 0 |
| N3 | 24 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 0.

## Host interference

Before the first run, programs outside the container VM used **4.5 of 32 logical CPUs** (peak 5.6).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| N1 | 24 | 5.0 | 9.5 | 11.2 | 24 |
| N2 | 24 | 7.7 | 13.1 | 14.2 | 24 |
| N3 | 24 | 5.0 | 6.7 | 10.5 | 24 |

**Effect:** Spearman ρ = -0.66 between outside load and each run's throughput relative to its cell's median (72 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

## N — KvNode's native HTTP/2 engine (h2n.asm) vs nghttp2

The same image, one variable: `KVNODE_H2_ENGINE=native` (KvNode's own HTTP/2 engine, ADR 0007) or `nghttp2` (nghttp2 1.70). Ratios are native ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvNode 0.1.0 native-h2 | 1.61 M (1.39–1.64 M) | 1.61 M (1.39–1.64 M) | 622 (611–720) | 586 (569–610) | 1368 (1286–2056) |
| KvNode 0.1.0 nghttp2 | 785 k (671–823 k) | 784 k (670–821 k) | 1275 (1218–1494) | 1253 (1122–1303) | 2540 (2310–3555) |
| Redis 8.10.2 | 1.42 M (1.15–1.51 M) | 1.42 M (1.15–1.51 M) | 708 (660–869) | 692 (659–750) | 1368 (1204–2441) |
| Valkey 9.1.2 | 1.36 M (1.17–1.46 M) | 1.36 M (1.17–1.46 M) | 736 (685–856) | 696 (684–750) | 1450 (1106–2343) |
| *KvNode native ÷ nghttp2* | 2.05× [1.72–2.44] sign p=0.031 ▲ | 2.05× [1.72–2.45] sign p=0.031 ▲ | 0.49× [0.41–0.58] sign p=0.031 ▲ | 0.47× [0.44–0.52] sign p=0.031 ▲ | 0.54× [0.39–0.84] sign p=0.031 ▲ |
| *KvNode native ÷ Redis* | 1.14× [1.00–1.26] sign p=0.031 ▲ | 1.14× [1.00–1.26] sign p=0.031 ▲ | 0.88× [0.79–1.00] sign p=0.031 ▲ | 0.85× [0.77–0.90] sign p=0.031 ▲ | 1.00× [0.82–1.10] sign p=1.000 ≈ |
| *KvNode native ÷ Valkey* | 1.18× [1.03–1.38] sign p=0.031 ▲ | 1.18× [1.03–1.38] sign p=0.031 ▲ | 0.85× [0.72–0.97] sign p=0.031 ▲ | 0.84× [0.76–0.87] sign p=0.031 ▲ | 0.94× [0.63–1.40] sign p=1.000 ≈ |

### N2 — scaling (E1's workload)

| Throughput (ops/s) | 2 cores | 4 cores |
|---|---|---|
| KvNode 0.1.0 native-h2 | 3.16 M (2.81–3.33 M) | 7.80 M (6.14–8.09 M) |
| KvNode 0.1.0 nghttp2 | 1.38 M (1.19–1.46 M) | 2.55 M (2.33–2.76 M) |
| *KvNode native ÷ nghttp2* | 2.29× [2.09–2.62] sign p=0.031 ▲ | 3.06× [2.55–3.34] sign p=0.031 ▲ |

| ops per CPU-second | 2 cores | 4 cores |
|---|---|---|
| KvNode 0.1.0 native-h2 | 1.58 M (1.40–1.66 M) | 1.99 M (1.57–2.07 M) |
| KvNode 0.1.0 nghttp2 | 688 k (592–727 k) | 637 k (583–688 k) |
| *KvNode native ÷ nghttp2* | 2.29× [2.08–2.62] sign p=0.031 ▲ | 3.13× [2.60–3.42] sign p=0.031 ▲ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvNode 0.1.0 native-h2 | 2.00 M (1.86–2.17 M) | 501 (461–537) |
| KvNode 0.1.0 nghttp2 | 799 k (789–861 k) | 1254 (1164–1272) |
| Redis 8.10.2 | 2.02 M (1.99–2.06 M) | 495 (486–503) |
| Valkey 9.1.2 | 1.87 M (1.75–1.92 M) | 535 (521–573) |
| *KvNode native ÷ nghttp2* | 2.50× [2.34–2.73] sign p=0.031 ▲ | 0.40× [0.37–0.43] sign p=0.031 ▲ |
| *KvNode native ÷ Redis* | 0.99× [0.92–1.07] sign p=0.688 ≈ | 1.01× [0.93–1.09] sign p=0.688 ≈ |
| *KvNode native ÷ Valkey* | 1.07× [1.00–1.15] sign p=0.031 ▲ | 0.94× [0.87–1.00] sign p=0.031 ▲ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 72 timed runs): median coefficient of variation 3.4%, 90th percentile 17.8%, worst 30.3%.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvNode ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvNode clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
