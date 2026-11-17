# KvAsm benchmark report

Profile **standard**, recorded 2026-09-26T18:17:13 UTC, repository revision `ec83e0e2f252` (uncommitted changes). Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0364 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvAsm 0.1.0 | `kvasm:0.1.0`<br/>`8de4b9b7ced2715c798` | gRPC over h2c; one worker thread per core (KVASM_WORKERS). |
| KvAsm 0.1.0 native-h2 | `kvasm:0.1.0`<br/>`8de4b9b7ced2715c798` | KvAsm with its own HTTP/2 engine (server/src/h2n.asm, KVASM_H2_ENGINE=native). |
| KvAsm 0.1.0 nghttp2 | `kvasm:0.1.0`<br/>`8de4b9b7ced2715c798` | KvAsm with nghttp2 1.70 as its HTTP/2 engine (KVASM_H2_ENGINE=nghttp2). |
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

Before the first run, programs outside the container VM used **13.3 of 32 logical CPUs** (peak 25.2).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| N1 | 24 | 7.0 | 11.2 | 13.2 | 24 |
| N2 | 24 | 8.0 | 10.9 | 14.2 | 24 |
| N3 | 24 | 6.6 | 9.4 | 13.1 | 24 |

**Effect:** Spearman ρ = -0.44 between outside load and each run's throughput relative to its cell's median (72 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

## N — KvAsm's native HTTP/2 engine (h2n.asm) vs nghttp2

The same image, one variable: `KVASM_H2_ENGINE=native` (KvAsm's own HTTP/2 engine, ADR 0007) or `nghttp2` (nghttp2 1.70). Ratios are native ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvAsm 0.1.0 native-h2 | 1.51 M (1.43–1.78 M) | 1.50 M (1.42–1.77 M) | 665 (564–705) | 586 (518–643) | 1630 (1319–1974) |
| KvAsm 0.1.0 nghttp2 | 744 k (644–796 k) | 742 k (642–794 k) | 1348 (1260–1557) | 1278 (1171–1368) | 2720 (2572–5931) |
| Redis 8.10.2 | 1.27 M (1.23–1.35 M) | 1.27 M (1.23–1.34 M) | 788 (747–814) | 741 (717–774) | 1671 (1516–2146) |
| Valkey 9.1.2 | 1.24 M (1.11–1.35 M) | 1.23 M (1.11–1.35 M) | 813 (741–899) | 762 (717–807) | 1704 (1434–2507) |
| *KvAsm native ÷ nghttp2* | 2.03× [1.92–2.52] sign p=0.031 ▲ | 2.03× [1.92–2.52] sign p=0.031 ▲ | 0.49× [0.40–0.52] sign p=0.031 ▲ | 0.46× [0.38–0.51] sign p=0.031 ▲ | 0.60× [0.33–0.71] sign p=0.031 ▲ |
| *KvAsm native ÷ Redis* | 1.19× [1.10–1.35] sign p=0.031 ▲ | 1.19× [1.11–1.35] sign p=0.031 ▲ | 0.84× [0.74–0.90] sign p=0.031 ▲ | 0.79× [0.71–0.86] sign p=0.031 ▲ | 0.98× [0.83–1.12] sign p=1.000 ≈ |
| *KvAsm native ÷ Valkey* | 1.22× [1.14–1.34] sign p=0.031 ▲ | 1.22× [1.14–1.34] sign p=0.031 ▲ | 0.82× [0.75–0.88] sign p=0.031 ▲ | 0.77× [0.72–0.83] sign p=0.031 ▲ | 0.96× [0.79–1.15] sign p=0.688 ≈ |

### N2 — scaling (E1's workload)

| Throughput (ops/s) | 2 cores | 4 cores |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 2.56 M (1.86–3.37 M) | 5.45 M (4.10–7.46 M) |
| KvAsm 0.1.0 nghttp2 | 1.31 M (770–1.40 M) | 2.63 M (1.48–2.76 M) |
| *KvAsm native ÷ nghttp2* | 1.96× [1.36–2.66] sign p=0.031 ▲ | 2.07× [1.63–2.96] sign p=0.031 ▲ |

| ops per CPU-second | 2 cores | 4 cores |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 1.28 M (927–1.68 M) | 1.40 M (1.05–1.94 M) |
| KvAsm 0.1.0 nghttp2 | 651 k (384–695 k) | 655 k (368–686 k) |
| *KvAsm native ÷ nghttp2* | 1.96× [1.35–2.66] sign p=0.031 ▲ | 2.13× [1.68–3.06] sign p=0.031 ▲ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 1.93 M (1.05–1.97 M) | 520 (508–959) |
| KvAsm 0.1.0 nghttp2 | 804 k (413–884 k) | 1250 (1136–2433) |
| Redis 8.10.2 | 1.92 M (990–1.99 M) | 523 (504–1015) |
| Valkey 9.1.2 | 1.81 M (912–1.88 M) | 556 (536–1102) |
| *KvAsm native ÷ nghttp2* | 2.40× [2.21–3.94] sign p=0.031 ▲ | 0.42× [0.25–0.45] sign p=0.031 ▲ |
| *KvAsm native ÷ Redis* | 1.00× [0.99–1.14] sign p=1.000 ≈ | 1.00× [0.88–1.01] sign p=0.688 ≈ |
| *KvAsm native ÷ Valkey* | 1.07× [1.05–1.15] sign p=0.031 ▲ | 0.94× [0.87–0.95] sign p=0.031 ▲ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 72 timed runs): median coefficient of variation 3.2%, 90th percentile 17.2%, worst 29.4%.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvAsm ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvAsm clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
