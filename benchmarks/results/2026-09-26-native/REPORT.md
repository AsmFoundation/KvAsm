# KvAsm benchmark report

Profile **standard**, recorded 2026-09-26T17:04:23 UTC, repository revision `ec83e0e2f252` (uncommitted changes). Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0332 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvAsm 0.1.0 | `kvasm:0.1.0`<br/>`b511e9860db33ec48ca` | gRPC over h2c; one worker thread per core (KVASM_WORKERS). |
| KvAsm 0.1.0 native-h2 | `kvasm:0.1.0`<br/>`b511e9860db33ec48ca` | KvAsm with its own HTTP/2 engine (server/src/h2n.asm, KVASM_H2_ENGINE=native). |
| KvAsm 0.1.0 nghttp2 | `kvasm:0.1.0`<br/>`b511e9860db33ec48ca` | KvAsm with nghttp2 1.70 as its HTTP/2 engine (KVASM_H2_ENGINE=nghttp2). |
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

Before the first run, programs outside the container VM used **27.2 of 32 logical CPUs** (peak 28.3).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| N1 | 24 | 27.2 | 28.5 | 30.0 | 24 |
| N2 | 24 | 6.1 | 20.3 | 25.6 | 24 |
| N3 | 24 | 5.1 | 10.4 | 17.1 | 24 |

**Effect:** Spearman ρ = -0.31 between outside load and each run's throughput relative to its cell's median (72 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

## N — KvAsm's native HTTP/2 engine (h2n.asm) vs nghttp2

The same image, one variable: `KVASM_H2_ENGINE=native` (KvAsm's own HTTP/2 engine, ADR 0007) or `nghttp2` (nghttp2 1.70). Ratios are native ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvAsm 0.1.0 native-h2 | 369 k (357–399 k) | 369 k (358–399 k) | 2710 (2505–2793) | 2359 (2245–2376) | 8405 (7438–9372) |
| KvAsm 0.1.0 nghttp2 | 206 k (186–248 k) | 207 k (186–248 k) | 4842 (4027–5368) | 4489 (3981–4882) | 12 k (8847–13 k) |
| Redis 8.10.2 | 319 k (280–343 k) | 319 k (281–343 k) | 3137 (2914–3561) | 3031 (2736–3326) | 7832 (7242–9765) |
| Valkey 9.1.2 | 314 k (248–370 k) | 314 k (249–371 k) | 3188 (2697–4023) | 2884 (2572–3031) | 10 k (7438–14 k) |
| *KvAsm native ÷ nghttp2* | 1.79× [1.55–1.98] sign p=0.031 ▲ | 1.79× [1.54–1.98] sign p=0.031 ▲ | 0.56× [0.51–0.65] sign p=0.031 ▲ | 0.53× [0.49–0.59] sign p=0.031 ▲ | 0.70× [0.60–0.93] sign p=0.031 ▲ |
| *KvAsm native ÷ Redis* | 1.16× [1.08–1.29] sign p=0.031 ▲ | 1.16× [1.08–1.29] sign p=0.031 ▲ | 0.86× [0.77–0.92] sign p=0.031 ▲ | 0.78× [0.71–0.86] sign p=0.031 ▲ | 1.07× [0.92–1.15] sign p=1.000 ≈ |
| *KvAsm native ÷ Valkey* | 1.18× [1.04–1.46] sign p=0.031 ▲ | 1.18× [1.03–1.46] sign p=0.031 ▲ | 0.85× [0.68–0.97] sign p=0.031 ▲ | 0.82× [0.78–0.91] sign p=0.031 ▲ | 0.84× [0.67–1.11] sign p=0.219 ≈ |

### N2 — scaling (E1's workload)

| Throughput (ops/s) | 2 cores | 4 cores |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 2.77 M (2.09–2.88 M) | 7.29 M (2.46–7.55 M) |
| KvAsm 0.1.0 nghttp2 | 1.28 M (753–1.46 M) | 2.75 M (883–2.79 M) |
| *KvAsm native ÷ nghttp2* | 2.16× [1.89–3.82] sign p=0.031 ▲ | 2.66× [2.32–2.78] sign p=0.031 ▲ |

| ops per CPU-second | 2 cores | 4 cores |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 1.38 M (1.04–1.44 M) | 1.83 M (641–1.89 M) |
| KvAsm 0.1.0 nghttp2 | 640 k (377–728 k) | 686 k (221–696 k) |
| *KvAsm native ÷ nghttp2* | 2.16× [1.89–3.82] sign p=0.031 ▲ | 2.66× [2.35–2.90] sign p=0.031 ▲ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvAsm 0.1.0 native-h2 | 1.68 M (1.19–1.84 M) | 594 (545–842) |
| KvAsm 0.1.0 nghttp2 | 843 k (802–872 k) | 1188 (1148–1247) |
| Redis 8.10.2 | 2.06 M (1.94–2.20 M) | 487 (454–515) |
| Valkey 9.1.2 | 1.89 M (1.63–1.96 M) | 529 (512–613) |
| *KvAsm native ÷ nghttp2* | 2.00× [1.40–2.15] sign p=0.031 ▲ | 0.50× [0.46–0.72] sign p=0.031 ▲ |
| *KvAsm native ÷ Redis* | 0.82× [0.54–0.94] sign p=0.031 ▼ | 1.22× [1.06–1.85] sign p=0.031 ▼ |
| *KvAsm native ÷ Valkey* | 0.89× [0.61–1.04] sign p=0.219 ≈ | 1.12× [0.96–1.64] sign p=0.219 ≈ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 72 timed runs): median coefficient of variation 6.0%, 90th percentile 16.7%, worst 66.2%.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvAsm ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvAsm clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
