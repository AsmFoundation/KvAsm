# KvAsm benchmark report

Profile **standard**, recorded 2026-09-27T17:23:27 UTC, repository revision `a0264f783730`. Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0508 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvAsm 0.1.0 | `kvasm:0.1.0`<br/>`bcb8a8ab03267ecf9f3` | gRPC over h2c; one worker thread per core (KVASM_WORKERS). |
| Redis 8.10.2 | `redis:8.10.2`<br/>`426335161ea6c23f3b3` | Persistence off (--save '' --appendonly no); io-threads = tuned thread count (commands still execute on the main thread). |
| Valkey 9.1.2 | `valkey/valkey:9.1.2`<br/>`faa14c828e611eb9dfd` | Persistence off; io-threads = tuned thread count. 9.2 was only a release candidate at test time. |
| Dragonfly 2.0.0 | `ghcr.io/dragonflydb/dragonfly:v2.0.0`<br/>`9a0d5f8f474b3631233` | Snapshots off (--dbfilename=); proactor_threads = tuned thread count; memlock unlimited as its docs require; io_uring is available on this host (seccomp unconfined), so Dragonfly uses its preferred I/O engine. |
| Garnet 2.1.8 | `kvbench/garnet:2.1.8`<br/>`8a297dea4465e17cfcc` | Built from Microsoft's own Dockerfile at tag v2.1.8 (the newest published image, 2.1.0, is eight releases older; 'latest' is an unreleased main build). Defaults: AOF off; the .NET thread pool sizes itself from the cpuset, so there is no thread knob to tune. |
| Memcached 1.6.45 | `memcached:1.6.45`<br/>`3de3fa823e6482be4ad` | Meta protocol (mg/ms/mn); -t = tuned thread count; -m sized so nothing is evicted; UDP off. |

## Run validity

| Experiment | ok | invalid | failed |
|---|---|---|---|
| E0 | 90 | 0 | 0 |
| E1 | 144 | 0 | 0 |
| E2 | 144 | 0 | 0 |
| E3 | 36 | 0 | 0 |
| E4 | 54 | 0 | 0 |
| E5 | 36 | 0 | 0 |
| E6 | 36 | 0 | 0 |
| E7 | 90 | 0 | 0 |
| E8 | 21 | 0 | 0 |
| N1 | 18 | 0 | 0 |
| N3 | 18 | 0 | 0 |
| S1 | 60 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 4
- `E0/kvasm/c8/t8/get100-v100-k1000000-uni-c64d16/r2`
- `E0/kvasm/c8/t7/get100-v100-k1000000-uni-c64d16/r2`
- `E1/kvasm/c8/t7/get100-v100-k1000000-uni-c64d16/r4`
- `E1/kvasm/c8/t7/get100-v100-k1000000-uni-c64d16/r5`

## Host interference

Before the first run, programs outside the container VM used **8.1 of 32 logical CPUs** (peak 8.9).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| E0 | 90 | 16.7 | 18.5 | 20.2 | 90 |
| E1 | 144 | 15.8 | 18.4 | 25.3 | 144 |
| E2 | 144 | 4.5 | 6.3 | 12.0 | 144 |
| E3 | 36 | 6.4 | 8.4 | 9.6 | 36 |
| E4 | 54 | 5.8 | 7.1 | 8.4 | 54 |
| E5 | 36 | 3.6 | 4.5 | 5.9 | 36 |
| E6 | 36 | 4.3 | 5.7 | 6.9 | 36 |
| E7 | 90 | 4.4 | 5.9 | 8.4 | 90 |
| N1 | 18 | 4.6 | 8.3 | 8.6 | 18 |
| N3 | 18 | 4.6 | 7.1 | 7.3 | 18 |
| S1 | 60 | 5.2 | 8.1 | 11.7 | 60 |

**Effect:** Spearman ρ = -0.32 between outside load and each run's throughput relative to its cell's median (672 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

**Quiet-only check** (declared in advance): 0 of 405 KvAsm-vs-other pairs had at most 2 CPUs of outside load on both runs, too few to recompute anything.

**Adjusted to equal outside load.** Each experiment's pairs are corrected with one common slope of log(KvAsm ÷ X) on the difference in outside load between the two runs of a block, estimated within comparisons (analysis of covariance). The slope column gives the change of the ratio per logical CPU of extra outside load on KvAsm's run. Raw, unadjusted results are the tables in each section below. Where the two disagree, the difference is load, not the servers.

| Experiment · cell | slope | KvAsm ÷ Redis | KvAsm ÷ Valkey | KvAsm ÷ Dragonfly | KvAsm ÷ Garnet | KvAsm ÷ Memcached |
|---|---|---|---|---|---|---|
| E1 · 1 core | -3.2% | 1.29× [1.15–1.53] ▲ | 1.33× [1.18–1.50] ▲ | 2.24× [2.17–2.37] ▲ | 2.16× [2.00–2.49] ▲ | 2.31× [2.10–2.46] ▲ |
| E1 · 2 cores | -3.2% | 2.30× [2.12–2.79] ▲ | 1.82× [1.74–2.03] ▲ | 2.78× [2.64–3.12] ▲ | 2.05× [1.92–2.17] ▲ | 2.44× [2.29–2.78] ▲ |
| E1 · 4 cores | -3.2% | 3.09× [2.90–3.73] ▲ | 2.17× [2.08–2.60] ▲ | 3.07× [3.02–3.60] ▲ | 1.55× [1.54–2.15] ▲ | 2.10× [2.07–2.83] ▲ |
| E1 · 8 cores | -3.2% | 3.06× [2.95–3.59] ▲ | 3.28× [2.98–4.16] ▲ | 2.64× [2.35–3.15] ▲ | 1.15× [0.97–1.26] ≈ | 1.32× [1.12–1.66] ▲ |
| E2 · 90% GET | -2.2% | 2.74× [2.51–2.85] ▲ | 2.58× [2.42–2.76] ▲ | 3.23× [2.94–3.37] ▲ | 1.97× [1.84–2.06] ▲ | 2.64× [2.48–2.78] ▲ |
| E2 · 90% GET, Zipf | -2.2% | 2.67× [2.54–2.79] ▲ | 2.60× [2.49–2.68] ▲ | 3.08× [2.99–3.23] ▲ | 1.89× [1.73–2.01] ▲ | 1.89× [1.80–1.99] ▲ |
| E2 · 50% GET | -2.2% | 1.45× [1.41–1.47] ▲ | 1.71× [1.68–1.75] ▲ | 1.63× [1.62–1.67] ▲ | 0.99× [0.98–1.03] ≈ | 1.62× [1.58–1.64] ▲ |
| E2 · 100% SET | -2.2% | 0.87× [0.86–0.96] ▼ | 1.25× [1.23–1.29] ▲ | 0.90× [0.88–0.94] ▼ | 0.59× [0.58–0.59] ▼ | 1.46× [1.44–1.51] ▲ |
| E3 · 256×1 | -0.5% | 2.08× [1.96–2.12] ▲ | 1.39× [1.36–1.44] ▲ | 1.86× [1.79–1.89] ▲ | 1.95× [1.72–2.01] ▲ | 1.34× [1.28–1.41] ▲ |
| E5 · no-op | -2.0% | 1.02× [0.99–1.04] ≈ | 1.09× [1.09–1.13] ▲ | 1.16× [1.13–1.17] ▲ | 1.64× [1.57–1.69] ▲ | 3.92× [3.80–3.96] ▲ |
| E6 · 1024 B values | -3.8% | 2.97× [2.94–3.10] ≈ | 3.10× [3.07–3.13] ≈ | 3.71× [3.25–3.89] ≈ | 1.80× [1.80–1.81] ≈ | 2.30× [2.26–2.34] ≈ |
| E6 · 16384 B values | -3.8% | 1.09× [1.07–1.09] ≈ | 1.86× [1.76–1.86] ≈ | 1.13× [1.08–1.13] ≈ | 0.52× [0.49–0.52] ≈ | 0.51× [0.50–0.51] ≈ |
| E7 · 1×1 | -2.8% | 1.85× [1.84–1.95] ≈ | 1.01× [0.98–1.05] ≈ | 1.70× [1.70–1.73] ≈ | 1.02× [1.01–1.07] ≈ | 0.97× [0.96–0.97] ≈ |
| E7 · 16×1 | -2.8% | 1.81× [1.81–1.92] ≈ | 1.14× [1.07–1.17] ≈ | 1.32× [1.29–1.41] ≈ | 1.25× [1.18–1.34] ≈ | 0.88× [0.87–0.93] ≈ |
| E7 · 64×1 | -2.8% | 2.47× [2.43–2.47] ≈ | 1.76× [1.60–1.78] ≈ | 1.76× [1.59–1.76] ≈ | 1.69× [1.60–1.69] ≈ | 1.38× [1.31–1.42] ≈ |
| E7 · 64×4 | -2.8% | 2.82× [2.73–2.92] ≈ | 1.92× [1.83–1.98] ≈ | 2.84× [2.71–2.90] ≈ | 2.06× [1.89–2.09] ≈ | 2.04× [2.00–2.04] ≈ |
| E7 · 64×16 | -2.8% | 3.02× [2.88–3.27] ≈ | 2.67× [2.55–2.87] ≈ | 3.68× [3.52–3.88] ≈ | 2.24× [2.10–2.37] ≈ | 2.89× [2.77–3.08] ≈ |

## E0 — Thread-count tuning

Median GET throughput for each thread setting; the better one (bold) is used by every later experiment.

| System | Cores | threads = N−1 | threads = N |
|---|---|---|---|
| KvAsm 0.1.0 | 2 | 1.41 M (1.36–1.47 M) | **3.08 M (2.76–3.18 M)** |
| KvAsm 0.1.0 | 4 | 4.21 M (3.99–5.07 M) | **5.02 M (4.40–5.07 M)** |
| KvAsm 0.1.0 | 8 | **5.30 M (4.70–8.82 M)** | 4.62 M (4.36–5.64 M) |
| Redis 8.10.2 | 2 | 1.00 M (939–1.26 M) | **1.38 M (1.31–1.44 M)** |
| Redis 8.10.2 | 4 | 1.66 M (1.61–2.24 M) | **1.71 M (1.51–1.77 M)** |
| Redis 8.10.2 | 8 | **2.65 M (1.88–2.82 M)** | 1.83 M (1.67–2.79 M) |
| Valkey 9.1.2 | 2 | 1.26 M (896–1.30 M) | **1.69 M (1.68–1.83 M)** |
| Valkey 9.1.2 | 4 | 2.19 M (1.88–3.01 M) | **2.75 M (2.21–3.01 M)** |
| Valkey 9.1.2 | 8 | **2.03 M (1.72–2.13 M)** | 1.74 M (1.63–2.00 M) |
| Dragonfly 2.0.0 | 2 | 631 k (596–696 k) | **1.07 M (1.00–1.09 M)** |
| Dragonfly 2.0.0 | 4 | 1.76 M (1.35–1.81 M) | **2.03 M (1.67–2.25 M)** |
| Dragonfly 2.0.0 | 8 | **2.62 M (2.07–3.19 M)** | 2.23 M (2.16–3.25 M) |
| Memcached 1.6.45 | 2 | 747 k (701–806 k) | **1.24 M (1.23–1.25 M)** |
| Memcached 1.6.45 | 4 | 2.16 M (2.05–2.28 M) | **2.73 M (2.37–2.96 M)** |
| Memcached 1.6.45 | 8 | 4.13 M (3.60–5.22 M) | **5.46 M (4.22–5.74 M)** |

## E1 — Throughput scaling (100% GET, 100-byte values, 1M keys, uniform)

| Throughput (ops/s) | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 1.39 M (1.21–1.81 M) | 3.06 M (2.43–4.24 M) | 4.98 M (4.88–8.39 M) | 5.99 M (4.40–9.33 M) |
| Redis 8.10.2 | 1.01 M (856–1.42 M) | 1.29 M (1.19–1.53 M) | 1.67 M (1.36–2.92 M) | 1.86 M (1.45–2.83 M) |
| Valkey 9.1.2 | 1.04 M (882–1.42 M) | 1.68 M (1.34–2.11 M) | 2.46 M (2.03–3.37 M) | 1.82 M (1.58–2.25 M) |
| Dragonfly 2.0.0 | 629 k (528–786 k) | 1.08 M (899–1.32 M) | 1.68 M (1.43–2.43 M) | 2.18 M (1.77–3.30 M) |
| Garnet 2.1.8 | 642 k (560–841 k) | 1.51 M (1.46–1.91 M) | 3.32 M (1.95–4.02 M) | 4.96 M (4.58–7.87 M) |
| Memcached 1.6.45 | 587 k (475–754 k) | 1.23 M (1.11–1.55 M) | 2.47 M (2.16–3.07 M) | 4.34 M (2.82–6.36 M) |
| *KvAsm ÷ Redis* | 1.37× [1.27–1.52] sign p=0.031 ▲ | 2.36× [2.04–3.04] sign p=0.031 ▲ | 2.98× [2.87–4.70] sign p=0.031 ▲ | 3.22× [2.78–5.00] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.33× [1.28–1.58] sign p=0.031 ▲ | 1.82× [1.61–2.38] sign p=0.031 ▲ | 2.02× [1.88–2.49] sign p=0.031 ▲ | 3.30× [2.71–4.32] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 2.21× [2.13–2.39] sign p=0.031 ▲ | 2.84× [2.37–3.71] sign p=0.031 ▲ | 2.97× [2.83–3.66] sign p=0.031 ▲ | 2.75× [2.19–3.26] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 2.17× [1.97–2.55] sign p=0.031 ▲ | 2.03× [1.64–2.55] sign p=0.031 ▲ | 1.50× [1.50–2.51] sign p=0.031 ▲ | 1.21× [0.88–1.29] sign p=0.219 ≈ |
| *KvAsm ÷ Memcached* | 2.37× [2.28–2.62] sign p=0.031 ▲ | 2.48× [2.10–3.24] sign p=0.031 ▲ | 2.02× [1.97–3.14] sign p=0.031 ▲ | 1.38× [1.03–2.04] sign p=0.031 ▲ |

![e1_throughput](charts/e1_throughput.svg)

**CPU efficiency** — operations per server CPU-second (higher is better):

| ops per CPU-second | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 1.39 M (1.21–1.80 M) | 1.53 M (1.22–2.12 M) | 1.33 M (1.29–2.19 M) | 1.35 M (1.05–1.68 M) |
| Redis 8.10.2 | 1.01 M (858–1.41 M) | 851 k (719–1.06 M) | 644 k (490–985 k) | 487 k (364–683 k) |
| Valkey 9.1.2 | 1.04 M (883–1.41 M) | 837 k (670–1.05 M) | 618 k (510–848 k) | 274 k (232–327 k) |
| Dragonfly 2.0.0 | 630 k (530–787 k) | 541 k (453–659 k) | 433 k (366–610 k) | 323 k (274–492 k) |
| Garnet 2.1.8 | 641 k (559–839 k) | 765 k (737–961 k) | 886 k (600–1.07 M) | 768 k (728–1.13 M) |
| Memcached 1.6.45 | 587 k (476–751 k) | 624 k (561–782 k) | 626 k (547–784 k) | 550 k (380–803 k) |
| *KvAsm ÷ Redis* | 1.37× [1.28–1.52] sign p=0.031 ▲ | 1.80× [1.62–2.29] sign p=0.031 ▲ | 2.06× [2.00–3.20] sign p=0.031 ▲ | 2.77× [2.46–3.51] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.33× [1.27–1.58] sign p=0.031 ▲ | 1.83× [1.61–2.39] sign p=0.031 ▲ | 2.15× [2.01–2.62] sign p=0.031 ▲ | 4.92× [4.11–5.55] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 2.21× [2.13–2.39] sign p=0.031 ▲ | 2.82× [2.36–3.68] sign p=0.031 ▲ | 3.07× [2.93–3.78] sign p=0.031 ▲ | 4.18× [3.42–4.66] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 2.17× [1.98–2.55] sign p=0.031 ▲ | 2.00× [1.61–2.49] sign p=0.031 ▲ | 1.50× [1.48–2.15] sign p=0.031 ▲ | 1.76× [1.37–1.87] sign p=0.031 ▲ |
| *KvAsm ÷ Memcached* | 2.37× [2.29–2.62] sign p=0.031 ▲ | 2.45× [2.08–3.21] sign p=0.031 ▲ | 2.12× [2.08–3.23] sign p=0.031 ▲ | 2.45× [1.95–3.35] sign p=0.031 ▲ |

**Server CPU utilization** of its core budget (1.0 = all given cores busy; ▲ only means KvAsm kept more of its cores busy, which is neither good nor bad by itself):

| utilization | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 1.00 (1.00–1.00) | 1.00 (0.99–1.00) | 0.95 (0.93–0.96) | 0.56 (0.52–0.70) |
| Redis 8.10.2 | 1.00 (1.00–1.01) | 0.76 (0.72–0.83) | 0.67 (0.65–0.74) | 0.49 (0.47–0.52) |
| Valkey 9.1.2 | 1.00 (1.00–1.00) | 1.00 (1.00–1.01) | 1.00 (0.99–1.00) | 0.85 (0.79–0.86) |
| Dragonfly 2.0.0 | 1.00 (1.00–1.00) | 0.99 (0.99–1.00) | 0.97 (0.97–1.00) | 0.84 (0.81–0.84) |
| Garnet 2.1.8 | 1.00 (1.00–1.01) | 0.99 (0.98–0.99) | 0.93 (0.81–0.94) | 0.81 (0.79–0.87) |
| Memcached 1.6.45 | 1.00 (1.00–1.00) | 0.99 (0.99–0.99) | 0.98 (0.98–0.99) | 0.99 (0.93–0.99) |
| *KvAsm ÷ Redis* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.31× [1.20–1.39] sign p=0.031 ▲ | 1.42× [1.29–1.47] sign p=0.031 ▲ | 1.14× [1.04–1.48] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.00× [1.00–1.00] sign p=0.688 ≈ | 1.00× [1.00–1.00] sign p=0.031 ▼ | 0.95× [0.93–0.96] sign p=0.031 ▼ | 0.66× [0.65–0.82] sign p=0.031 ▼ |
| *KvAsm ÷ Dragonfly* | 1.00× [1.00–1.00] sign p=0.031 ▲ | 1.01× [1.00–1.01] sign p=0.031 ▲ | 0.97× [0.95–0.98] sign p=0.031 ▼ | 0.67× [0.62–0.84] sign p=0.031 ▼ |
| *KvAsm ÷ Garnet* | 1.00× [0.99–1.00] sign p=0.688 ≈ | 1.01× [1.01–1.02] sign p=0.031 ▲ | 1.02× [0.99–1.17] sign p=0.219 ≈ | 0.69× [0.64–0.80] sign p=0.031 ▼ |
| *KvAsm ÷ Memcached* | 1.00× [1.00–1.00] sign p=0.688 ≈ | 1.01× [1.01–1.01] sign p=0.031 ▲ | 0.96× [0.94–0.98] sign p=0.031 ▼ | 0.57× [0.53–0.70] sign p=0.031 ▼ |

**Memory per key** after preloading 1M keys (anonymous memory delta ÷ keys; 16-byte key + 100-byte value = 116 raw bytes; lower is better):

| bytes / key | all E1 runs |
|---|---|
| KvAsm 0.1.0 | 163 B (162–163 B) |
| Redis 8.10.2 | 174 B (172–175 B) |
| Valkey 9.1.2 | 173 B (172–173 B) |
| Dragonfly 2.0.0 | 157 B (154–161 B) |
| Garnet 2.1.8 | 285 B (274–287 B) |
| Memcached 1.6.45 | 196 B (195–198 B) |
| *KvAsm ÷ Redis* | 0.94× [0.93–0.94] sign p<0.001 ▲ |
| *KvAsm ÷ Valkey* | 0.94× [0.94–0.95] sign p<0.001 ▲ |
| *KvAsm ÷ Dragonfly* | 1.04× [1.01–1.06] sign p<0.001 ▼ |
| *KvAsm ÷ Garnet* | 0.57× [0.57–0.59] sign p<0.001 ▲ |
| *KvAsm ÷ Memcached* | 0.83× [0.82–0.83] sign p<0.001 ▲ |

## E2 — Read/write mixes (4 cores)

100-byte values, 1M keys; Zipf uses YCSB's scrambled Zipfian with θ = 0.99.

| Throughput (ops/s) | 90% GET | 50% GET | 100% SET | 90% GET, Zipf 0.99 |
|---|---|---|---|---|
| KvAsm 0.1.0 | 7.66 M (7.36–8.13 M) | 3.79 M (3.65–3.84 M) | 2.23 M (2.22–2.27 M) | 7.87 M (7.55–8.20 M) |
| Redis 8.10.2 | 2.88 M (2.86–2.96 M) | 2.65 M (2.25–2.69 M) | 2.58 M (2.02–2.62 M) | 2.98 M (2.89–3.00 M) |
| Valkey 9.1.2 | 3.01 M (2.47–3.06 M) | 2.24 M (2.12–2.26 M) | 1.78 M (1.66–1.80 M) | 3.09 M (2.85–3.16 M) |
| Dragonfly 2.0.0 | 2.43 M (2.23–2.53 M) | 2.33 M (2.21–2.36 M) | 2.48 M (2.38–2.50 M) | 2.57 M (2.33–2.60 M) |
| Garnet 2.1.8 | 3.96 M (3.60–4.01 M) | 3.84 M (3.48–3.96 M) | 3.75 M (3.48–3.90 M) | 4.19 M (3.92–4.42 M) |
| Memcached 1.6.45 | 2.95 M (2.62–3.01 M) | 2.36 M (2.20–2.39 M) | 1.54 M (1.52–1.56 M) | 4.13 M (3.59–4.19 M) |
| *KvAsm ÷ Redis* | 2.66× [2.50–2.83] sign p=0.031 ▲ | 1.43× [1.40–1.62] sign p=0.031 ▲ | 0.87× [0.86–1.10] sign p=0.219 ≈ | 2.64× [2.53–2.79] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 2.54× [2.41–2.97] sign p=0.031 ▲ | 1.69× [1.67–1.72] sign p=0.031 ▲ | 1.26× [1.24–1.37] sign p=0.031 ▲ | 2.54× [2.43–2.66] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 3.16× [2.92–3.31] sign p=0.031 ▲ | 1.63× [1.61–1.66] sign p=0.031 ▲ | 0.90× [0.89–0.95] sign p=0.031 ▼ | 3.06× [2.91–3.24] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 1.94× [1.90–2.05] sign p=0.031 ▲ | 0.99× [0.97–1.05] sign p=1.000 ≈ | 0.60× [0.58–0.65] sign p=0.031 ▼ | 1.88× [1.71–2.01] sign p=0.031 ▲ |
| *KvAsm ÷ Memcached* | 2.60× [2.49–2.81] sign p=0.031 ▲ | 1.60× [1.58–1.65] sign p=0.031 ▲ | 1.45× [1.43–1.48] sign p=0.031 ▲ | 1.90× [1.82–2.10] sign p=0.031 ▲ |

## E3 — No pipelining (256 connections × 1 request in flight, 4 cores)

| metric | ops/s | p50 µs | p99 µs |
|---|---|---|---|
| KvAsm 0.1.0 | 815,202 (802,266–835,542) | 274.4 (268.3–284.7) | 790.5 (757.8–847.9) |
| Redis 8.10.2 | 392,914 (390,002–414,724) | 626.7 (593.9–634.9) | 1,417 (1,057–1,499) |
| Valkey 9.1.2 | 588,174 (579,754–602,884) | 419.8 (411.6–423.9) | 720.9 (626.7–741.4) |
| Dragonfly 2.0.0 | 439,650 (431,639–455,401) | 557.1 (544.8–561.1) | 1,180 (1,057–1,286) |
| Garnet 2.1.8 | 419,784 (399,393–477,263) | 573.4 (505.9–577.5) | 1,212 (1,020–1,647) |
| Memcached 1.6.45 | 608,830 (575,449–632,804) | 370.7 (358.4–391.2) | 1,073 (995.3–1,139) |
| *KvAsm ÷ Redis* | 2.07× [1.98–2.12] sign p=0.031 ▲ | 0.44× [0.42–0.47] sign p=0.031 ▲ | 0.56× [0.54–0.75] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.39× [1.35–1.44] sign p=0.031 ▲ | 0.65× [0.63–0.68] sign p=0.031 ▲ | 1.10× [1.02–1.26] sign p=0.031 ▼ |
| *KvAsm ÷ Dragonfly* | 1.85× [1.79–1.90] sign p=0.031 ▲ | 0.49× [0.48–0.51] sign p=0.031 ▲ | 0.67× [0.62–0.75] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 1.94× [1.71–2.01] sign p=0.031 ▲ | 0.48× [0.46–0.54] sign p=0.031 ▲ | 0.65× [0.51–0.77] sign p=0.031 ▲ |
| *KvAsm ÷ Memcached* | 1.34× [1.29–1.42] sign p=0.031 ▲ | 0.74× [0.71–0.77] sign p=0.031 ▲ | 0.74× [0.67–0.79] sign p=0.031 ▲ |

## E4 — Latency at identical offered load (open loop, Poisson, 4 cores)

Latency is measured from each request's intended send time, so queueing behind a slow response counts (no coordinated omission). Lower is better.

**p50 latency (µs)**

| p50 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 100.9 (99.8–103.9) | 117.2 (97.8–120.3) | 111.1 (111.1–117.2) |
| Redis 8.10.2 | 222.2 (201.7–232.4) | 317.4 (309.2–321.5) | 378.9 (350.2–383.0) |
| Valkey 9.1.2 | 92.7 (91.7–93.7) | 142.3 (140.3–150.5) | 173.1 (173.1–179.2) |
| Dragonfly 2.0.0 | 171.0 (164.9–171.0) | 387.1 (387.1–395.3) | 553.0 (553.0–569.3) |
| Garnet 2.1.8 | 107.0 (99.8–116.2) | 134.1 (132.1–136.2) | 134.1 (129.5–136.2) |
| Memcached 1.6.45 | 96.8 (93.7–107.0) | 107.0 (101.9–111.1) | 134.1 (134.1–142.3) |
| *KvAsm ÷ Redis* | 0.45× [0.43–0.52] sign p=0.250 ≈ | 0.37× [0.31–0.39] sign p=0.250 ≈ | 0.29× [0.29–0.32] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.09× [1.08–1.12] sign p=0.250 ≈ | 0.82× [0.65–0.86] sign p=0.250 ≈ | 0.64× [0.62–0.68] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 0.59× [0.59–0.61] sign p=0.250 ≈ | 0.30× [0.25–0.31] sign p=0.250 ≈ | 0.20× [0.20–0.21] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.94× [0.87–1.00] sign p=0.500 ≈ | 0.87× [0.72–0.91] sign p=0.250 ≈ | 0.83× [0.82–0.87] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 1.04× [0.97–1.07] sign p=1.000 ≈ | 1.10× [0.96–1.10] sign p=1.000 ≈ | 0.83× [0.82–0.83] sign p=0.250 ≈ |

**p99 latency (µs)**

| p99 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 374.8 (362.5–432.1) | 444.4 (395.3–489.5) | 428.0 (395.3–509.9) |
| Redis 8.10.2 | 651.3 (514.0–766.0) | 888.8 (766.0–970.8) | 913.4 (905.2–938.0) |
| Valkey 9.1.2 | 252.9 (228.3–292.9) | 358.4 (342.0–432.1) | 395.3 (378.9–456.7) |
| Dragonfly 2.0.0 | 847.9 (823.3–1028.1) | 1515.5 (1417.2–1597.4) | 2342.9 (2277.4–2408.4) |
| Garnet 2.1.8 | 1269.8 (1253.4–1646.6) | 8978.4 (8978.4–10551.3) | 92799.0 (61603.8–120061.9) |
| Memcached 1.6.45 | 428.0 (378.9–553.0) | 634.9 (544.8–725.0) | 1089.5 (1056.8–1695.7) |
| *KvAsm ÷ Redis* | 0.58× [0.47–0.84] sign p=0.250 ≈ | 0.50× [0.50–0.52] sign p=0.250 ≈ | 0.47× [0.43–0.56] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.48× [1.43–1.64] sign p=0.250 ≈ | 1.24× [1.03–1.43] sign p=0.250 ≈ | 1.08× [0.94–1.35] sign p=1.000 ≈ |
| *KvAsm ÷ Dragonfly* | 0.44× [0.42–0.44] sign p=0.250 ≈ | 0.29× [0.28–0.32] sign p=0.250 ≈ | 0.18× [0.17–0.22] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.30× [0.22–0.34] sign p=0.250 ≈ | 0.05× [0.04–0.05] sign p=0.250 ≈ | 0.00× [0.00–0.01] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.88× [0.78–0.99] sign p=0.250 ≈ | 0.70× [0.55–0.82] sign p=0.250 ≈ | 0.39× [0.23–0.48] sign p=0.250 ≈ |

**p999 latency (µs)**

| p999 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 569.3 (536.6–684.0) | 626.7 (618.5–1646.6) | 1155.1 (733.2–1384.5) |
| Redis 8.10.2 | 954.4 (798.7–3391.5) | 1515.5 (1286.1–8355.8) | 1286.1 (1237.0–9633.8) |
| Valkey 9.1.2 | 448.5 (346.1–610.3) | 700.4 (518.1–1368.1) | 643.1 (626.7–766.0) |
| Dragonfly 2.0.0 | 1646.6 (1302.5–1646.6) | 2506.8 (2375.7–4358.1) | 3391.5 (3325.9–3489.8) |
| Garnet 2.1.8 | 13434.9 (7634.9–15925.2) | 35389.4 (23199.7–52166.7) | 274726.9 (274726.9–333447.2) |
| Memcached 1.6.45 | 667.6 (626.7–1269.8) | 1105.9 (880.6–1499.1) | 2506.8 (1859.6–5210.1) |
| *KvAsm ÷ Redis* | 0.60× [0.16–0.86] sign p=0.250 ≈ | 0.41× [0.20–0.49] sign p=0.250 ≈ | 0.90× [0.12–1.12] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 1.27× [1.12–1.64] sign p=0.250 ≈ | 0.89× [0.46–3.18] sign p=1.000 ≈ | 1.80× [1.14–2.21] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 0.35× [0.35–0.42] sign p=0.250 ≈ | 0.25× [0.25–0.38] sign p=0.250 ≈ | 0.34× [0.21–0.42] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.04× [0.04–0.09] sign p=0.250 ≈ | 0.02× [0.01–0.05] sign p=0.250 ≈ | 0.00× [0.00–0.01] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.85× [0.54–0.91] sign p=0.250 ≈ | 0.57× [0.41–1.49] sign p=1.000 ≈ | 0.46× [0.14–0.74] sign p=0.250 ≈ |

![e4_p99](charts/e4_p99.svg)

Achieved throughput (should equal the offered load when the system keeps up):

| achieved | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| Redis 8.10.2 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| Valkey 9.1.2 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| Dragonfly 2.0.0 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| Garnet 2.1.8 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| Memcached 1.6.45 | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| *KvAsm ÷ Redis* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=0.250 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |
| *KvAsm ÷ Dragonfly* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |
| *KvAsm ÷ Garnet* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |
| *KvAsm ÷ Memcached* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |

## E5 — Per-request floor (no-op round trip, 1 core)

RESP `PING`, memcached meta `mn`, gRPC `grpc.health.v1.Health/Check`: the cost of protocol, framing and event loop without any data access.

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvAsm 0.1.0 | 2.13 M (2.08–2.17 M) | 472 (465–484) |
| Redis 8.10.2 | 2.11 M (2.09–2.13 M) | 479 (475–481) |
| Valkey 9.1.2 | 1.93 M (1.83–1.94 M) | 525 (521–550) |
| Dragonfly 2.0.0 | 1.86 M (1.84–1.89 M) | 541 (534–548) |
| Garnet 2.1.8 | 1.30 M (1.28–1.33 M) | 774 (757–791) |
| Memcached 1.6.45 | 550 k (545–559 k) | 1837 (1804–1852) |
| *KvAsm ÷ Redis* | 1.01× [0.98–1.03] sign p=0.688 ≈ | 0.99× [0.97–1.01] sign p=0.688 ≈ |
| *KvAsm ÷ Valkey* | 1.11× [1.07–1.16] sign p=0.031 ▲ | 0.90× [0.86–0.93] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 1.15× [1.12–1.16] sign p=0.031 ▲ | 0.87× [0.86–0.90] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 1.64× [1.56–1.68] sign p=0.031 ▲ | 0.61× [0.59–0.64] sign p=0.031 ▲ |
| *KvAsm ÷ Memcached* | 3.88× [3.75–3.93] sign p=0.031 ▲ | 0.26× [0.25–0.27] sign p=0.031 ▲ |

## E6 — Value size (4 cores, 100% GET, about 1 GiB of data)

| Throughput (ops/s) | 1024 B values | 16384 B values |
|---|---|---|
| KvAsm 0.1.0 | 5.59 M (5.47–5.78 M) | 481 k (473–492 k) |
| Redis 8.10.2 | 1.91 M (1.81–1.93 M) | 461 k (442–469 k) |
| Valkey 9.1.2 | 1.85 M (1.85–1.87 M) | 273 k (271–290 k) |
| Dragonfly 2.0.0 | 1.53 M (1.45–1.75 M) | 444 k (441–446 k) |
| Garnet 2.1.8 | 3.16 M (3.11–3.19 M) | 972 k (927–989 k) |
| Memcached 1.6.45 | 2.47 M (2.43–2.47 M) | 995 k (974–1.01 M) |
| *KvAsm ÷ Redis* | 2.93× [2.86–3.20] sign p=0.250 ≈ | 1.04× [1.04–1.07] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 3.02× [2.96–3.09] sign p=0.250 ≈ | 1.76× [1.70–1.76] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 3.66× [3.13–3.86] sign p=0.250 ≈ | 1.08× [1.07–1.10] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 1.77× [1.76–1.81] sign p=0.250 ≈ | 0.50× [0.48–0.53] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 2.26× [2.25–2.34] sign p=0.250 ≈ | 0.48× [0.47–0.49] sign p=0.250 ≈ |

Memory per key (lower is better):

| bytes / key | 1024 B | 16384 B |
|---|---|---|
| KvAsm 0.1.0 | 1,075 B (1,075–1,075 B) | 16,515 B (16,515–16,515 B) |
| Redis 8.10.2 | 1,351 B (1,351–1,352 B) | 20,727 B (20,725–20,744 B) |
| Valkey 9.1.2 | 1,350 B (1,350–1,350 B) | 20,741 B (20,738–20,744 B) |
| Dragonfly 2.0.0 | 1,065 B (1,064–1,065 B) | 16,653 B (16,651–16,694 B) |
| Garnet 2.1.8 | 1,207 B (1,207–1,207 B) | 18,430 B (18,397–18,546 B) |
| Memcached 1.6.45 | 1,192 B (1,192–1,192 B) | 17,567 B (17,567–17,569 B) |
| *KvAsm ÷ Redis* | 0.80× [0.79–0.80] sign p=0.250 ≈ | 0.80× [0.80–0.80] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 0.80× [0.80–0.80] sign p=0.250 ≈ | 0.80× [0.80–0.80] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.01× [1.01–1.01] sign p=0.250 ≈ | 0.99× [0.99–0.99] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.89× [0.89–0.89] sign p=0.250 ≈ | 0.90× [0.89–0.90] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.90× [0.90–0.90] sign p=0.250 ≈ | 0.94× [0.94–0.94] sign p=0.250 ≈ |

## E7 — Concurrency sweep (4 cores, 100% GET)

Requests in flight = connections × depth. At 1 in flight the throughput is exactly 1 / round-trip time; beyond that, each system shows how far concurrency carries it.

| Throughput (ops/s) | 1×1 = 1 | 16×1 = 16 | 64×1 = 64 | 64×4 = 256 | 64×16 = 1024 |
|---|---|---|---|---|---|
| KvAsm 0.1.0 | 35 k (34–37 k) | 314 k (305–321 k) | 714 k (675–757 k) | 2.94 M (2.75–3.01 M) | 9.11 M (8.30–9.60 M) |
| Redis 8.10.2 | 19 k (19–19 k) | 175 k (175–179 k) | 297 k (252–305 k) | 1.05 M (1.02–1.08 M) | 2.91 M (2.69–3.14 M) |
| Valkey 9.1.2 | 35 k (31–36 k) | 275 k (274–282 k) | 426 k (425–428 k) | 1.56 M (1.52–1.57 M) | 3.42 M (3.37–3.47 M) |
| Dragonfly 2.0.0 | 21 k (21–21 k) | 235 k (230–241 k) | 423 k (422–433 k) | 1.07 M (1.07–1.09 M) | 2.50 M (2.49–2.54 M) |
| Garnet 2.1.8 | 35 k (33–35 k) | 250 k (240–255 k) | 432 k (424–442 k) | 1.50 M (1.47–1.52 M) | 4.09 M (4.06–4.14 M) |
| Memcached 1.6.45 | 37 k (36–37 k) | 347 k (343–357 k) | 519 k (518–526 k) | 1.50 M (1.45–1.52 M) | 3.15 M (3.14–3.16 M) |
| *KvAsm ÷ Redis* | 1.83× [1.74–1.93] sign p=0.250 ≈ | 1.79× [1.74–1.80] sign p=0.250 ≈ | 2.41× [2.41–2.68] sign p=0.250 ≈ | 2.78× [2.60–2.95] sign p=0.250 ≈ | 3.13× [2.85–3.57] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.01× [0.93–1.19] sign p=1.000 ≈ | 1.14× [1.11–1.14] sign p=0.250 ≈ | 1.68× [1.58–1.78] sign p=0.250 ≈ | 1.89× [1.74–1.93] sign p=0.250 ≈ | 2.67× [2.39–2.85] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.67× [1.60–1.75] sign p=0.250 ≈ | 1.33× [1.27–1.36] sign p=0.250 ≈ | 1.69× [1.60–1.75] sign p=0.250 ≈ | 2.75× [2.53–2.82] sign p=0.250 ≈ | 3.64× [3.33–3.84] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 1.02× [1.01–1.07] sign p=0.250 ≈ | 1.26× [1.22–1.34] sign p=0.250 ≈ | 1.65× [1.59–1.75] sign p=0.250 ≈ | 1.96× [1.83–2.04] sign p=0.250 ≈ | 2.23× [2.00–2.35] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.96× [0.91–1.01] sign p=1.000 ≈ | 0.90× [0.89–0.90] sign p=0.250 ≈ | 1.38× [1.30–1.44] sign p=0.250 ≈ | 1.95× [1.83–2.03] sign p=0.250 ≈ | 2.89× [2.62–3.05] sign p=0.250 ≈ |

Median latency (µs; closed loop, so it grows with the queue by Little's law):

| p50 µs | 1 in flight | 16 in flight | 64 in flight | 256 in flight | 1024 in flight |
|---|---|---|---|---|---|
| KvAsm 0.1.0 | 24.7 (24.4–25.5) | 46.3 (45.3–47.9) | 78.3 (77.3–79.4) | 77.3 (77.3–82.4) | 88.6 (82.4–98.8) |
| Redis 8.10.2 | 48.4 (47.9–48.9) | 85.5 (84.5–85.5) | 209.9 (205.8–228.3) | 236.5 (232.4–244.7) | 337.9 (321.5–354.3) |
| Valkey 9.1.2 | 26.0 (25.7–27.3) | 55.5 (54.5–56.1) | 146.4 (146.4–146.4) | 158.7 (158.7–160.8) | 288.8 (288.8–288.8) |
| Dragonfly 2.0.0 | 48.9 (48.4–48.9) | 61.2 (60.7–61.2) | 146.4 (144.4–146.4) | 230.4 (230.4–232.4) | 391.2 (391.2–391.2) |
| Garnet 2.1.8 | 25.7 (25.7–26.0) | 56.6 (55.5–57.6) | 138.2 (138.2–142.3) | 162.8 (158.7–162.8) | 232.4 (232.4–234.5) |
| Memcached 1.6.45 | 25.0 (24.7–25.0) | 42.2 (41.7–42.8) | 105.0 (105.0–106.0) | 142.3 (140.3–148.5) | 250.9 (248.8–250.9) |
| *KvAsm ÷ Redis* | 0.51× [0.51–0.53] sign p=0.250 ≈ | 0.54× [0.54–0.56] sign p=0.250 ≈ | 0.37× [0.34–0.38] sign p=0.250 ≈ | 0.33× [0.32–0.35] sign p=0.250 ≈ | 0.26× [0.23–0.29] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 0.95× [0.90–0.99] sign p=0.250 ≈ | 0.83× [0.83–0.85] sign p=0.250 ≈ | 0.53× [0.53–0.54] sign p=0.250 ≈ | 0.49× [0.48–0.52] sign p=0.250 ≈ | 0.31× [0.29–0.34] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 0.51× [0.51–0.52] sign p=0.250 ≈ | 0.76× [0.74–0.79] sign p=0.250 ≈ | 0.53× [0.53–0.54] sign p=0.250 ≈ | 0.34× [0.33–0.36] sign p=0.250 ≈ | 0.23× [0.21–0.25] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.96× [0.95–0.98] sign p=0.250 ≈ | 0.82× [0.79–0.86] sign p=0.250 ≈ | 0.57× [0.55–0.57] sign p=0.250 ≈ | 0.47× [0.47–0.51] sign p=0.250 ≈ | 0.38× [0.35–0.43] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.99× [0.99–1.02] sign p=1.000 ≈ | 1.10× [1.09–1.12] sign p=0.250 ≈ | 0.75× [0.74–0.76] sign p=0.250 ≈ | 0.54× [0.52–0.58] sign p=0.250 ≈ | 0.35× [0.33–0.39] sign p=0.250 ≈ |

![e7_concurrency](charts/e7_concurrency.svg)

## E8 — Client validation (kvbench vs each ecosystem's standard tool, 4 cores)

If kvbench reaches at least the throughput of the standard tool on the same server and workload, it does not handicap that system.

| Server | Tool / keys | Throughput (ops/s) |
|---|---|---|
| KvAsm 0.1.0 | h2load-onekey | 5.89 M (5.76–5.91 M) |
| KvAsm 0.1.0 | kvbench-onekey | 9.30 M (8.82–9.39 M) |
| Redis 8.10.2 | kvbench-onekey | 3.43 M (2.87–3.44 M) |
| Redis 8.10.2 | kvbench-random | 2.80 M (2.78–2.92 M) |
| Redis 8.10.2 | memtier-random | 2.67 M (2.33–2.81 M) |
| Redis 8.10.2 | redis-benchmark-onekey | 3.24 M (3.00–3.33 M) |
| Redis 8.10.2 | redis-benchmark-random | 2.73 M (2.33–2.79 M) |

- Redis 8.10.2: kvbench ÷ redis = 1.03× [1.00–1.19] sign p=0.250 ≈
- Redis 8.10.2: kvbench ÷ memtier = 1.05× [0.99–1.25] sign p=1.000 ≈
- Redis 8.10.2: kvbench ÷ redis = 1.06× [0.96–1.06] sign p=1.000 ≈
- KvAsm 0.1.0: kvbench ÷ h2load = 1.58× [1.53–1.59] sign p=0.250 ≈

## N — one core: KvAsm vs Redis and Valkey

E1's and E5's 1-core cells for the single-threaded-execution servers, run on their own so the headline is re-checked with full pairing. Ratios are KvAsm ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvAsm 0.1.0 | 1.76 M (1.71–1.81 M) | 1.74 M (1.68–1.79 M) | 576 (559–595) | 532 (528–545) | 1171 (1122–1204) |
| Redis 8.10.2 | 1.40 M (1.31–1.48 M) | 1.38 M (1.29–1.45 M) | 722 (690–774) | 705 (676–725) | 1278 (1155–1516) |
| Valkey 9.1.2 | 1.42 M (1.36–1.45 M) | 1.41 M (1.34–1.43 M) | 710 (698–744) | 696 (684–725) | 1212 (1106–1270) |
| *KvAsm ÷ Redis* | 1.26× [1.22–1.38] sign p=0.031 ▲ | 1.25× [1.22–1.38] sign p=0.031 ▲ | 0.80× [0.72–0.82] sign p=0.031 ▲ | 0.76× [0.73–0.78] sign p=0.031 ▲ | 0.92× [0.74–0.99] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.24× [1.20–1.28] sign p=0.031 ▲ | 1.23× [1.19–1.28] sign p=0.031 ▲ | 0.81× [0.78–0.84] sign p=0.031 ▲ | 0.76× [0.74–0.79] sign p=0.031 ▲ | 0.97× [0.92–1.03] sign p=0.219 ≈ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvAsm 0.1.0 | 2.13 M (1.99–2.25 M) | 475 (452–507) |
| Redis 8.10.2 | 2.06 M (2.04–2.09 M) | 491 (484–500) |
| Valkey 9.1.2 | 1.93 M (1.90–1.98 M) | 526 (514–534) |
| *KvAsm ÷ Redis* | 1.03× [0.97–1.10] sign p=0.688 ≈ | 0.97× [0.91–1.02] sign p=0.688 ≈ |
| *KvAsm ÷ Valkey* | 1.10× [1.00–1.17] sign p=0.031 ▲ | 0.90× [0.85–0.99] sign p=0.031 ▲ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 726 timed runs): median coefficient of variation 2.4%, 90th percentile 5.8%, worst 37.8%.

**Between runs** (fresh server each, E1 and S1 cells with ≥ 3 runs): median CV 14.4%, worst 29.4% (redis, 4 cores).

| System | runs | median ops/s | between-run CV | median within-run CV | worst 1-s sample ÷ mean | median trend per 10 s | S1 ÷ E1 (same cell, hours apart) |
|---|---|---|---|---|---|---|---|
| KvAsm 0.1.0 | 10 | 8.84 M | 4.2% | 2.2% | 0.91 | -1.4% | 1.78× [0.97–1.92] MW p<0.001 ≈ |
| Redis 8.10.2 | 10 | 2.88 M | 5.3% | 2.4% | 0.59 | -0.9% | 1.72× [0.84–2.18] MW p=0.007 ≈ |
| Valkey 9.1.2 | 10 | 3.20 M | 2.2% | 2.2% | 0.95 | -1.6% | 1.30× [0.92–1.64] MW p=0.220 ≈ |
| Dragonfly 2.0.0 | 10 | 2.40 M | 2.9% | 1.3% | 0.85 | +0.4% | 1.43× [0.91–1.72] MW p=0.016 ≈ |
| Garnet 2.1.8 | 10 | 3.86 M | 1.6% | 3.5% | 0.91 | +2.0% | 1.16× [0.94–2.06] MW p=0.181 ≈ |
| Memcached 1.6.45 | 10 | 3.01 M | 2.7% | 1.7% | 0.83 | -2.9% | 1.22× [0.91–1.43] MW p=0.022 ≈ |

Reading guide: a between-run CV of a few percent means a single run is representative to within that margin; differences between systems smaller than the CVs involved are noise. The S1 ÷ E1 column should be ≈ 1.00 (≈): the same cell measured at the start and the end of the session.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvAsm ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvAsm clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
