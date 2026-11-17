# KvAsm benchmark report

Profile **standard**, recorded 2026-09-26T06:45:31 UTC, repository revision `8cb0fef878bb`. Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.1079 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

| System | Image | Configuration |
|---|---|---|
| KvAsm 0.1.0 | `kvasm:0.1.0`<br/>`d99969812c82bf8d577` | gRPC over h2c; one worker thread per core (KVASM_WORKERS). |
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
| S1 | 60 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 2
- `E4/redis/c4/t4/get100-v100-k1000000-uni-c64d64-r1721000/r0`
- `E4/dragonfly/c4/t4/get100-v100-k1000000-uni-c64d64-r1721000/r0`

## Host interference

Before the first run, programs outside the container VM used **3.9 of 32 logical CPUs** (peak 5.2).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| E0 | 90 | 4.4 | 6.8 | 14.0 | 90 |
| E1 | 144 | 5.7 | 9.3 | 16.2 | 144 |
| E2 | 144 | 5.8 | 7.2 | 10.6 | 144 |
| E3 | 36 | 5.4 | 10.3 | 11.2 | 36 |
| E4 | 54 | 6.5 | 10.6 | 15.9 | 54 |
| E5 | 36 | 2.3 | 3.0 | 3.8 | 33 |
| E6 | 36 | 3.8 | 7.1 | 8.1 | 36 |
| E7 | 90 | 6.0 | 12.8 | 15.1 | 90 |
| S1 | 60 | 4.5 | 5.9 | 9.8 | 60 |

**Effect:** Spearman ρ = -0.57 between outside load and each run's throughput relative to its cell's median (636 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

**Quiet-only check** (declared in advance): 0 of 405 KvAsm-vs-other pairs had at most 2 CPUs of outside load on both runs, too few to recompute anything.

**Adjusted to equal outside load.** Each experiment's pairs are corrected with one common slope of log(KvAsm ÷ X) on the difference in outside load between the two runs of a block, estimated within comparisons (analysis of covariance). The slope column gives the change of the ratio per logical CPU of extra outside load on KvAsm's run. Raw, unadjusted results are the tables in each section below. Where the two disagree, the difference is load, not the servers.

| Experiment · cell | slope | KvAsm ÷ Redis | KvAsm ÷ Valkey | KvAsm ÷ Dragonfly | KvAsm ÷ Garnet | KvAsm ÷ Memcached |
|---|---|---|---|---|---|---|
| E1 · 1 core | -3.1% | 0.55× [0.51–0.59] ▼ | 0.55× [0.53–0.57] ▼ | 1.00× [0.95–1.08] ≈ | 0.92× [0.88–1.08] ≈ | 1.06× [0.99–1.12] ≈ |
| E1 · 2 cores | -3.1% | 0.99× [0.86–1.03] ≈ | 0.74× [0.72–0.79] ▼ | 1.14× [1.11–1.21] ▲ | 0.79× [0.74–0.86] ▼ | 1.02× [0.99–1.08] ≈ |
| E1 · 4 cores | -3.1% | 1.04× [1.02–1.15] ▲ | 0.91× [0.88–0.93] ▼ | 1.29× [1.20–1.32] ▲ | 0.74× [0.69–0.76] ▼ | 0.96× [0.91–0.98] ▼ |
| E1 · 8 cores | -3.1% | 1.83× [1.76–1.99] ▲ | 2.24× [2.19–2.34] ▲ | 1.75× [1.66–1.78] ▲ | 0.66× [0.64–0.70] ▼ | 0.82× [0.78–0.88] ▼ |
| E2 · 90% GET | -2.7% | 1.08× [0.99–1.15] ≈ | 0.94× [0.91–1.01] ≈ | 1.22× [1.16–1.32] ▲ | 0.72× [0.70–0.76] ▼ | 0.94× [0.91–0.98] ▼ |
| E2 · 90% GET, Zipf | -2.7% | 0.98× [0.95–1.08] ≈ | 0.94× [0.90–1.01] ≈ | 1.18× [1.12–1.23] ▲ | 0.67× [0.64–0.69] ▼ | 0.68× [0.67–0.69] ▼ |
| E2 · 50% GET | -2.7% | 0.94× [0.84–1.02] ≈ | 1.05× [1.02–1.10] ▲ | 1.05× [1.00–1.08] ≈ | 0.62× [0.61–0.65] ▼ | 0.98× [0.96–1.02] ≈ |
| E2 · 100% SET | -2.7% | 0.71× [0.68–0.76] ▼ | 0.96× [0.95–0.97] ▼ | 0.71× [0.70–0.76] ▼ | 0.46× [0.44–0.48] ▼ | 1.12× [1.10–1.13] ▲ |
| E3 · 256×1 | -2.8% | 1.45× [1.43–1.52] ▲ | 0.99× [0.98–1.04] ≈ | 1.36× [1.30–1.45] ▲ | 1.32× [1.15–1.40] ▲ | 0.99× [0.93–1.05] ≈ |
| E5 · no-op | +0.5% | 0.39× [0.35–0.41] ▼ | 0.42× [0.39–0.45] ▼ | 0.45× [0.43–0.48] ▼ | 0.67× [0.64–0.70] ▼ | 1.57× [1.43–1.63] ▲ |
| E6 · 1024 B values | -4.0% | 1.31× [1.25–1.34] ≈ | 1.33× [1.33–1.36] ≈ | 1.44× [1.40–1.46] ≈ | 0.78× [0.78–0.81] ≈ | 1.00× [0.97–1.01] ≈ |
| E6 · 16384 B values | -4.0% | 1.48× [1.46–1.65] ≈ | 2.57× [2.39–2.60] ≈ | 1.54× [1.47–1.55] ≈ | 0.71× [0.70–0.72] ≈ | 0.65× [0.65–0.68] ≈ |
| E7 · 1×1 | -1.9% | 1.80× [1.58–1.87] ≈ | 0.94× [0.94–1.25] ≈ | 1.61× [1.61–1.76] ≈ | 1.00× [0.88–1.30] ≈ | 0.88× [0.88–0.96] ≈ |
| E7 · 16×1 | -1.9% | 1.61× [1.61–1.70] ≈ | 1.00× [0.95–1.05] ≈ | 1.30× [1.18–1.40] ≈ | 1.26× [1.12–1.26] ≈ | 0.81× [0.77–0.87] ≈ |
| E7 · 64×1 | -1.9% | 2.01× [1.74–2.16] ≈ | 1.25× [1.13–1.26] ≈ | 1.29× [1.17–1.31] ≈ | 1.27× [1.09–1.28] ≈ | 1.02× [1.00–1.02] ≈ |
| E7 · 64×4 | -1.9% | 1.52× [1.46–1.52] ≈ | 0.85× [0.85–0.93] ≈ | 1.32× [1.31–1.67] ≈ | 1.02× [0.97–1.02] ≈ | 0.87× [0.87–1.02] ≈ |
| E7 · 64×16 | -1.9% | 1.13× [0.95–1.30] ≈ | 0.87× [0.87–0.89] ≈ | 1.21× [1.16–1.26] ≈ | 0.77× [0.71–0.79] ≈ | 0.84× [0.84–1.01] ≈ |

## E0 — Thread-count tuning

Median GET throughput for each thread setting; the better one (bold) is used by every later experiment.

| System | Cores | threads = N−1 | threads = N |
|---|---|---|---|
| KvAsm 0.1.0 | 2 | 834 k (798–946 k) | **1.65 M (1.58–1.66 M)** |
| KvAsm 0.1.0 | 4 | 2.31 M (2.19–2.41 M) | **3.13 M (3.09–3.27 M)** |
| KvAsm 0.1.0 | 8 | 5.23 M (4.92–5.23 M) | **5.64 M (5.58–5.70 M)** |
| Redis 8.10.2 | 2 | 1.54 M (1.51–1.62 M) | **1.66 M (1.64–1.66 M)** |
| Redis 8.10.2 | 4 | 2.55 M (2.31–2.60 M) | **3.05 M (2.68–3.15 M)** |
| Redis 8.10.2 | 8 | 3.05 M (2.98–3.12 M) | **3.08 M (3.01–3.16 M)** |
| Valkey 9.1.2 | 2 | 1.56 M (1.53–1.58 M) | **2.23 M (2.18–2.27 M)** |
| Valkey 9.1.2 | 4 | 3.20 M (3.04–3.34 M) | **3.25 M (2.90–3.68 M)** |
| Valkey 9.1.2 | 8 | **2.45 M (2.05–2.47 M)** | 2.32 M (2.28–2.52 M) |
| Dragonfly 2.0.0 | 2 | 855 k (842–884 k) | **1.46 M (1.43–1.48 M)** |
| Dragonfly 2.0.0 | 4 | 1.98 M (1.73–2.09 M) | **2.48 M (2.40–2.61 M)** |
| Dragonfly 2.0.0 | 8 | 3.22 M (3.00–3.49 M) | **3.48 M (3.18–3.55 M)** |
| Memcached 1.6.45 | 2 | 983 k (949–1.01 M) | **1.60 M (1.58–1.66 M)** |
| Memcached 1.6.45 | 4 | 2.76 M (2.76–2.76 M) | **3.29 M (3.25–3.41 M)** |
| Memcached 1.6.45 | 8 | 6.66 M (6.56–6.79 M) | **6.80 M (6.68–6.84 M)** |

## E1 — Throughput scaling (100% GET, 100-byte values, 1M keys, uniform)

| Throughput (ops/s) | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 813 k (695–833 k) | 1.50 M (1.39–1.58 M) | 3.02 M (2.80–3.23 M) | 5.18 M (4.68–5.37 M) |
| Redis 8.10.2 | 1.45 M (1.02–1.63 M) | 1.57 M (1.51–1.65 M) | 2.89 M (2.55–3.07 M) | 2.82 M (2.52–2.94 M) |
| Valkey 9.1.2 | 1.44 M (1.17–1.50 M) | 2.12 M (2.06–2.22 M) | 3.29 M (2.81–3.38 M) | 2.40 M (2.26–2.55 M) |
| Dragonfly 2.0.0 | 790 k (634–878 k) | 1.39 M (1.29–1.49 M) | 2.29 M (2.21–2.61 M) | 2.92 M (2.38–3.15 M) |
| Garnet 2.1.8 | 872 k (686–915 k) | 1.97 M (1.76–2.08 M) | 3.80 M (3.28–4.38 M) | 7.91 M (7.49–7.98 M) |
| Memcached 1.6.45 | 771 k (757–824 k) | 1.57 M (1.47–1.67 M) | 3.12 M (3.00–3.36 M) | 6.22 M (4.56–6.74 M) |
| *KvAsm ÷ Redis* | 0.56× [0.50–0.75] sign p=0.031 ▼ | 0.96× [0.92–0.98] sign p=0.031 ▼ | 1.04× [0.98–1.17] sign p=0.219 ≈ | 1.83× [1.75–2.12] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 0.57× [0.48–0.69] sign p=0.031 ▼ | 0.71× [0.65–0.74] sign p=0.031 ▼ | 0.92× [0.86–1.01] sign p=0.219 ≈ | 2.15× [2.04–2.28] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 1.03× [0.84–1.28] sign p=1.000 ≈ | 1.08× [0.97–1.17] sign p=0.219 ≈ | 1.31× [1.24–1.35] sign p=0.031 ▲ | 1.77× [1.65–1.96] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 0.93× [0.79–1.19] sign p=0.219 ≈ | 0.76× [0.74–0.81] sign p=0.031 ▼ | 0.79× [0.71–0.88] sign p=0.031 ▼ | 0.66× [0.59–0.71] sign p=0.031 ▼ |
| *KvAsm ÷ Memcached* | 1.06× [0.91–1.09] sign p=0.688 ≈ | 0.96× [0.89–1.03] sign p=0.688 ≈ | 0.97× [0.91–1.00] sign p=0.219 ≈ | 0.83× [0.77–1.03] sign p=0.219 ≈ |

![e1_throughput](charts/e1_throughput.svg)

**CPU efficiency** — operations per server CPU-second (higher is better):

| ops per CPU-second | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 814 k (694–841 k) | 755 k (697–787 k) | 752 k (698–811 k) | 656 k (593–721 k) |
| Redis 8.10.2 | 1.46 M (1.03–1.63 M) | 1.09 M (1.01–1.18 M) | 1.01 M (899–1.05 M) | 639 k (590–692 k) |
| Valkey 9.1.2 | 1.44 M (1.17–1.50 M) | 1.06 M (1.04–1.11 M) | 830 k (709–843 k) | 350 k (327–380 k) |
| Dragonfly 2.0.0 | 794 k (636–877 k) | 698 k (650–745 k) | 592 k (572–658 k) | 399 k (324–425 k) |
| Garnet 2.1.8 | 874 k (692–913 k) | 997 k (897–1.06 M) | 1.07 M (902–1.18 M) | 1.13 M (1.09–1.17 M) |
| Memcached 1.6.45 | 777 k (759–827 k) | 804 k (757–854 k) | 797 k (763–849 k) | 790 k (575–862 k) |
| *KvAsm ÷ Redis* | 0.56× [0.50–0.74] sign p=0.031 ▼ | 0.69× [0.66–0.72] sign p=0.031 ▼ | 0.75× [0.70–0.82] sign p=0.031 ▼ | 1.03× [0.94–1.14] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 0.57× [0.48–0.70] sign p=0.031 ▼ | 0.71× [0.65–0.74] sign p=0.031 ▼ | 0.91× [0.86–1.00] sign p=0.219 ≈ | 1.87× [1.72–2.01] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 1.03× [0.84–1.28] sign p=1.000 ≈ | 1.08× [0.97–1.17] sign p=0.219 ≈ | 1.27× [1.22–1.30] sign p=0.031 ▲ | 1.65× [1.54–1.83] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 0.93× [0.79–1.18] sign p=0.219 ≈ | 0.76× [0.73–0.80] sign p=0.031 ▼ | 0.71× [0.65–0.81] sign p=0.031 ▼ | 0.58× [0.51–0.66] sign p=0.031 ▼ |
| *KvAsm ÷ Memcached* | 1.05× [0.91–1.09] sign p=0.688 ≈ | 0.94× [0.88–1.01] sign p=0.219 ≈ | 0.94× [0.90–0.98] sign p=0.031 ▼ | 0.83× [0.77–1.03] sign p=0.219 ≈ |

**Server CPU utilization** of its core budget (1.0 = all given cores busy; ▲ only means KvAsm kept more of its cores busy, which is neither good nor bad by itself):

| utilization | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvAsm 0.1.0 | 1.00 (0.99–1.00) | 1.00 (0.99–1.00) | 1.00 (0.99–1.00) | 0.99 (0.93–1.00) |
| Redis 8.10.2 | 0.99 (0.99–1.00) | 0.73 (0.70–0.75) | 0.72 (0.70–0.73) | 0.53 (0.52–0.56) |
| Valkey 9.1.2 | 1.00 (0.99–1.00) | 1.00 (0.98–1.00) | 0.99 (0.99–1.00) | 0.85 (0.84–0.87) |
| Dragonfly 2.0.0 | 1.00 (0.99–1.00) | 1.00 (0.99–1.00) | 0.97 (0.96–0.99) | 0.92 (0.90–0.93) |
| Garnet 2.1.8 | 1.00 (0.99–1.00) | 0.98 (0.98–0.99) | 0.91 (0.88–0.93) | 0.87 (0.86–0.89) |
| Memcached 1.6.45 | 1.00 (0.98–1.00) | 0.98 (0.97–0.98) | 0.98 (0.98–0.99) | 0.98 (0.98–0.99) |
| *KvAsm ÷ Redis* | 1.01× [0.99–1.01] sign p=0.219 ≈ | 1.37× [1.34–1.43] sign p=0.031 ▲ | 1.40× [1.36–1.43] sign p=0.031 ▲ | 1.85× [1.74–1.88] sign p=0.031 ▲ |
| *KvAsm ÷ Valkey* | 1.00× [0.99–1.01] sign p=1.000 ≈ | 1.00× [0.99–1.02] sign p=1.000 ≈ | 1.01× [1.00–1.01] sign p=0.031 ▲ | 1.16× [1.09–1.19] sign p=0.031 ▲ |
| *KvAsm ÷ Dragonfly* | 1.00× [0.99–1.01] sign p=0.688 ≈ | 1.00× [1.00–1.00] sign p=0.219 ≈ | 1.03× [1.00–1.04] sign p=0.031 ▲ | 1.08× [1.00–1.11] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 1.00× [0.99–1.01] sign p=0.219 ≈ | 1.01× [1.00–1.02] sign p=0.031 ▲ | 1.10× [1.08–1.14] sign p=0.031 ▲ | 1.14× [1.08–1.15] sign p=0.031 ▲ |
| *KvAsm ÷ Memcached* | 1.00× [0.99–1.02] sign p=0.688 ≈ | 1.02× [1.01–1.03] sign p=0.031 ▲ | 1.02× [1.01–1.02] sign p=0.031 ▲ | 1.01× [0.94–1.01] sign p=0.688 ≈ |

**Memory per key** after preloading 1M keys (anonymous memory delta ÷ keys; 16-byte key + 100-byte value = 116 raw bytes; lower is better):

| bytes / key | all E1 runs |
|---|---|
| KvAsm 0.1.0 | 178 B (178–178 B) |
| Redis 8.10.2 | 174 B (172–175 B) |
| Valkey 9.1.2 | 173 B (172–173 B) |
| Dragonfly 2.0.0 | 159 B (154–162 B) |
| Garnet 2.1.8 | 284 B (274–287 B) |
| Memcached 1.6.45 | 196 B (195–198 B) |
| *KvAsm ÷ Redis* | 1.02× [1.02–1.03] sign p<0.001 ▼ |
| *KvAsm ÷ Valkey* | 1.03× [1.03–1.03] sign p<0.001 ▼ |
| *KvAsm ÷ Dragonfly* | 1.12× [1.10–1.15] sign p<0.001 ▼ |
| *KvAsm ÷ Garnet* | 0.63× [0.62–0.65] sign p<0.001 ▲ |
| *KvAsm ÷ Memcached* | 0.91× [0.90–0.91] sign p<0.001 ▲ |

## E2 — Read/write mixes (4 cores)

100-byte values, 1M keys; Zipf uses YCSB's scrambled Zipfian with θ = 0.99.

| Throughput (ops/s) | 90% GET | 50% GET | 100% SET | 90% GET, Zipf 0.99 |
|---|---|---|---|---|
| KvAsm 0.1.0 | 2.75 M (2.56–2.94 M) | 2.30 M (2.23–2.32 M) | 1.72 M (1.68–1.80 M) | 2.85 M (2.48–3.10 M) |
| Redis 8.10.2 | 2.61 M (2.44–3.03 M) | 2.46 M (2.17–2.79 M) | 2.39 M (2.25–2.52 M) | 2.86 M (2.47–3.13 M) |
| Valkey 9.1.2 | 2.96 M (2.69–3.20 M) | 2.24 M (2.21–2.31 M) | 1.78 M (1.73–1.85 M) | 3.06 M (2.89–3.29 M) |
| Dragonfly 2.0.0 | 2.37 M (2.23–2.51 M) | 2.21 M (2.08–2.39 M) | 2.44 M (2.23–2.57 M) | 2.38 M (2.32–2.54 M) |
| Garnet 2.1.8 | 3.88 M (3.67–4.16 M) | 3.72 M (3.56–3.91 M) | 3.67 M (3.60–3.78 M) | 4.27 M (3.68–4.67 M) |
| Memcached 1.6.45 | 2.95 M (2.81–3.04 M) | 2.38 M (2.33–2.42 M) | 1.52 M (1.47–1.62 M) | 4.08 M (3.93–4.40 M) |
| *KvAsm ÷ Redis* | 1.05× [0.93–1.10] sign p=0.219 ≈ | 0.93× [0.80–1.06] sign p=0.688 ≈ | 0.72× [0.67–0.76] sign p=0.031 ▼ | 1.00× [0.88–1.14] sign p=0.688 ≈ |
| *KvAsm ÷ Valkey* | 0.93× [0.86–1.09] sign p=0.219 ≈ | 1.02× [0.97–1.04] sign p=0.219 ≈ | 0.96× [0.95–0.99] sign p=0.031 ▼ | 0.93× [0.82–1.00] sign p=0.031 ▼ |
| *KvAsm ÷ Dragonfly* | 1.16× [1.02–1.28] sign p=0.031 ▲ | 1.04× [0.97–1.10] sign p=0.688 ≈ | 0.70× [0.66–0.77] sign p=0.031 ▼ | 1.20× [1.03–1.23] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 0.71× [0.66–0.75] sign p=0.031 ▼ | 0.62× [0.57–0.65] sign p=0.031 ▼ | 0.47× [0.45–0.48] sign p=0.031 ▼ | 0.67× [0.58–0.75] sign p=0.031 ▼ |
| *KvAsm ÷ Memcached* | 0.93× [0.88–0.98] sign p=0.031 ▼ | 0.96× [0.94–0.98] sign p=0.031 ▼ | 1.13× [1.11–1.16] sign p=0.031 ▲ | 0.70× [0.62–0.77] sign p=0.031 ▼ |

## E3 — No pipelining (256 connections × 1 request in flight, 4 cores)

| metric | ops/s | p50 µs | p99 µs |
|---|---|---|---|
| KvAsm 0.1.0 | 618,356 (564,414–708,549) | 350.2 (313.3–395.3) | 1,286 (1,106–1,335) |
| Redis 8.10.2 | 441,664 (429,498–471,249) | 536.6 (522.2–544.8) | 1,565 (1,020–1,679) |
| Valkey 9.1.2 | 644,998 (608,111–667,047) | 370.7 (358.4–387.1) | 716.8 (659.5–839.7) |
| Dragonfly 2.0.0 | 469,886 (410,074–484,177) | 501.8 (485.4–544.8) | 1,434 (1,384–1,712) |
| Garnet 2.1.8 | 494,074 (415,893–543,741) | 483.3 (448.5–528.4) | 1,139 (921.6–1,843) |
| Memcached 1.6.45 | 670,168 (641,724–713,294) | 325.6 (313.3–337.9) | 1,163 (1,028–1,221) |
| *KvAsm ÷ Redis* | 1.40× [1.30–1.54] sign p=0.031 ▲ | 0.65× [0.60–0.73] sign p=0.031 ▲ | 0.82× [0.74–1.26] sign p=0.219 ≈ |
| *KvAsm ÷ Valkey* | 0.96× [0.87–1.06] sign p=1.000 ≈ | 0.94× [0.87–1.08] sign p=0.219 ≈ | 1.79× [1.51–1.86] sign p=0.031 ▼ |
| *KvAsm ÷ Dragonfly* | 1.32× [1.24–1.50] sign p=0.031 ▲ | 0.70× [0.64–0.73] sign p=0.031 ▲ | 0.90× [0.75–0.93] sign p=0.031 ▲ |
| *KvAsm ÷ Garnet* | 1.25× [1.14–1.54] sign p=0.031 ▲ | 0.72× [0.66–0.80] sign p=0.031 ▲ | 1.13× [0.64–1.40] sign p=0.219 ≈ |
| *KvAsm ÷ Memcached* | 0.92× [0.82–1.08] sign p=0.219 ≈ | 1.08× [0.95–1.23] sign p=0.219 ≈ | 1.11× [0.93–1.25] sign p=0.219 ≈ |

## E4 — Latency at identical offered load (open loop, Poisson, 4 cores)

Latency is measured from each request's intended send time, so queueing behind a slow response counts (no coordinated omission). Lower is better.

**p50 latency (µs)**

| p50 | 574 k/s | 1.15 M/s | 1.72 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 112.1 (107.0–113.2) | 142.3 (136.2–156.7) | 162.8 (162.8–179.2) |
| Redis 8.10.2 | 288.8 (268.3–301.1) | 350.2 (350.2–659.5) | 569.3 (460.8–244318.2) |
| Valkey 9.1.2 | 117.2 (114.2–144.4) | 154.6 (144.4–164.9) | 191.5 (185.3–199.7) |
| Dragonfly 2.0.0 | 236.5 (212.0–268.3) | 485.4 (469.0–518.1) | 725.0 (725.0–76021.8) |
| Garnet 2.1.8 | 120.3 (118.3–125.4) | 132.1 (132.1–136.2) | 138.2 (132.1–146.4) |
| Memcached 1.6.45 | 102.9 (98.8–140.3) | 120.3 (119.3–156.7) | 148.5 (148.5–152.6) |
| *KvAsm ÷ Redis* | 0.39× [0.37–0.42] sign p=0.250 ≈ | 0.41× [0.24–0.41] sign p=0.250 ≈ | 0.29× [0.00–0.35] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 0.96× [0.78–0.96] sign p=0.250 ≈ | 0.92× [0.92–0.95] sign p=0.250 ≈ | 0.85× [0.85–0.90] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 0.47× [0.40–0.53] sign p=0.250 ≈ | 0.29× [0.29–0.30] sign p=0.250 ≈ | 0.22× [0.00–0.22] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.93× [0.89–0.94] sign p=0.250 ≈ | 1.08× [1.03–1.15] sign p=0.250 ≈ | 1.18× [1.18–1.23] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 1.09× [0.81–1.13] sign p=1.000 ≈ | 1.18× [1.00–1.19] sign p=0.500 ≈ | 1.10× [1.07–1.21] sign p=0.250 ≈ |

**p99 latency (µs)**

| p99 | 574 k/s | 1.15 M/s | 1.72 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 610.3 (489.5–618.5) | 675.8 (659.5–831.5) | 1171.5 (978.9–1351.7) |
| Redis 8.10.2 | 995.3 (880.6–995.3) | 1187.8 (1105.9–14614.5) | 50069.5 (1876.0–868220.9) |
| Valkey 9.1.2 | 362.5 (329.7–16187.4) | 528.4 (419.8–667.6) | 929.8 (577.5–87556.1) |
| Dragonfly 2.0.0 | 1466.4 (1269.8–1548.3) | 2211.8 (2179.1–3457.0) | 2965.5 (2768.9–3137339.4) |
| Garnet 2.1.8 | 1318.9 (1220.6–1417.2) | 96993.3 (91750.4–104333.3) | 396361.7 (147849.2–1098907.6) |
| Memcached 1.6.45 | 561.1 (553.0–978.9) | 1056.8 (905.2–29491.2) | 1990.7 (1859.6–2342.9) |
| *KvAsm ÷ Redis* | 0.61× [0.49–0.70] sign p=0.250 ≈ | 0.57× [0.06–0.60] sign p=0.250 ≈ | 0.02× [0.00–0.72] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.68× [0.04–1.88] sign p=1.000 ≈ | 1.28× [1.25–1.57] sign p=0.250 ≈ | 1.26× [0.02–1.70] sign p=1.000 ≈ |
| *KvAsm ÷ Dragonfly* | 0.42× [0.32–0.49] sign p=0.250 ≈ | 0.31× [0.24–0.31] sign p=0.250 ≈ | 0.40× [0.00–0.46] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.46× [0.37–0.50] sign p=0.250 ≈ | 0.01× [0.01–0.01] sign p=0.250 ≈ | 0.00× [0.00–0.01] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 1.09× [0.62–1.12] sign p=1.000 ≈ | 0.64× [0.03–0.75] sign p=0.250 ≈ | 0.59× [0.50–0.68] sign p=0.250 ≈ |

**p999 latency (µs)**

| p999 | 574 k/s | 1.15 M/s | 1.72 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 938.0 (839.7–1187.8) | 1105.9 (1036.3–1351.7) | 1794.0 (1663.0–12386.3) |
| Redis 8.10.2 | 1450.0 (1318.9–2506.8) | 5472.3 (2408.4–26345.5) | 58982.4 (8159.2–943718.4) |
| Valkey 9.1.2 | 618.5 (585.7–28966.9) | 970.8 (774.1–5406.7) | 1712.1 (1028.1–187695.1) |
| Dragonfly 2.0.0 | 2408.4 (1941.5–15663.1) | 3424.3 (3260.4–22413.3) | 4227.1 (3555.3–3305111.5) |
| Garnet 2.1.8 | 15269.9 (11206.7–16318.5) | 250609.7 (240123.9–291504.1) | 750780.4 (400556.0–2105540.6) |
| Memcached 1.6.45 | 1073.2 (880.6–2932.7) | 9764.9 (1695.7–46923.8) | 6258.7 (5537.8–8978.4) |
| *KvAsm ÷ Redis* | 0.65× [0.33–0.82] sign p=0.250 ≈ | 0.20× [0.05–0.43] sign p=0.250 ≈ | 0.03× [0.00–1.52] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 1.52× [0.03–1.92] sign p=1.000 ≈ | 1.14× [0.25–1.34] sign p=1.000 ≈ | 1.05× [0.07–1.62] sign p=1.000 ≈ |
| *KvAsm ÷ Dragonfly* | 0.39× [0.06–0.61] sign p=0.250 ≈ | 0.32× [0.06–0.32] sign p=0.250 ≈ | 0.42× [0.00–2.93] sign p=1.000 ≈ |
| *KvAsm ÷ Garnet* | 0.06× [0.06–0.08] sign p=0.250 ≈ | 0.00× [0.00–0.01] sign p=0.250 ≈ | 0.00× [0.00–0.02] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.87× [0.32–1.35] sign p=1.000 ≈ | 0.11× [0.03–0.65] sign p=0.250 ≈ | 0.29× [0.27–1.38] sign p=1.000 ≈ |

![e4_p99](charts/e4_p99.svg)

Achieved throughput (should equal the offered load when the system keeps up):

| achieved | 574 k/s | 1.15 M/s | 1.72 M/s |
|---|---|---|---|
| KvAsm 0.1.0 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.72–1.72 M) |
| Redis 8.10.2 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.67–1.72 M) |
| Valkey 9.1.2 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.71–1.72 M) |
| Dragonfly 2.0.0 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.21–1.72 M) |
| Garnet 2.1.8 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.72–1.75 M) |
| Memcached 1.6.45 | 574 k (574–574 k) | 1.15 M (1.15–1.15 M) | 1.72 M (1.72–1.72 M) |
| *KvAsm ÷ Redis* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.03] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 1.00× [1.00–1.00] sign p=0.250 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.01] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=0.250 ≈ | 1.00× [1.00–1.42] sign p=1.000 ≈ |
| *KvAsm ÷ Garnet* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [0.98–1.00] sign p=1.000 ≈ |
| *KvAsm ÷ Memcached* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=1.000 ≈ |

## E5 — Per-request floor (no-op round trip, 1 core)

RESP `PING`, memcached meta `mn`, gRPC `grpc.health.v1.Health/Check`: the cost of protocol, framing and event loop without any data access.

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvAsm 0.1.0 | 923 k (864–972 k) | 1086 (1032–1160) |
| Redis 8.10.2 | 2.37 M (2.23–2.44 M) | 422 (410–451) |
| Valkey 9.1.2 | 2.18 M (2.17–2.20 M) | 460 (455–462) |
| Dragonfly 2.0.0 | 2.03 M (2.00–2.04 M) | 492 (489–500) |
| Garnet 2.1.8 | 1.37 M (1.35–1.42 M) | 729 (708–740) |
| Memcached 1.6.45 | 589 k (572–606 k) | 1700 (1644–1729) |
| *KvAsm ÷ Redis* | 0.39× [0.35–0.41] sign p=0.031 ▼ | 2.57× [2.43–2.83] sign p=0.031 ▼ |
| *KvAsm ÷ Valkey* | 0.42× [0.39–0.45] sign p=0.031 ▼ | 2.36× [2.23–2.55] sign p=0.031 ▼ |
| *KvAsm ÷ Dragonfly* | 0.45× [0.42–0.48] sign p=0.031 ▼ | 2.21× [2.09–2.36] sign p=0.031 ▼ |
| *KvAsm ÷ Garnet* | 0.67× [0.64–0.70] sign p=0.031 ▼ | 1.49× [1.44–1.57] sign p=0.031 ▼ |
| *KvAsm ÷ Memcached* | 1.57× [1.43–1.63] sign p=0.031 ▲ | 0.64× [0.62–0.71] sign p=0.031 ▲ |

## E6 — Value size (4 cores, 100% GET, about 1 GiB of data)

| Throughput (ops/s) | 1024 B values | 16384 B values |
|---|---|---|
| KvAsm 0.1.0 | 2.63 M (2.39–2.64 M) | 691 k (654–730 k) |
| Redis 8.10.2 | 2.01 M (1.97–2.08 M) | 501 k (396–506 k) |
| Valkey 9.1.2 | 1.98 M (1.79–1.98 M) | 288 k (281–314 k) |
| Dragonfly 2.0.0 | 1.69 M (1.54–1.72 M) | 472 k (471–496 k) |
| Garnet 2.1.8 | 3.37 M (2.92–3.38 M) | 1.02 M (991–1.04 M) |
| Memcached 1.6.45 | 2.64 M (2.54–2.65 M) | 1.05 M (935–1.05 M) |
| *KvAsm ÷ Redis* | 1.31× [1.21–1.31] sign p=0.250 ≈ | 1.38× [1.36–1.65] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.33× [1.33–1.33] sign p=0.250 ≈ | 2.40× [2.20–2.53] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.55× [1.39–1.71] sign p=0.250 ≈ | 1.46× [1.39–1.55] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.78× [0.78–0.82] sign p=0.250 ≈ | 0.67× [0.66–0.71] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 1.00× [0.94–1.00] sign p=0.250 ≈ | 0.66× [0.66–0.70] sign p=0.250 ≈ |

Memory per key (lower is better):

| bytes / key | 1024 B | 16384 B |
|---|---|---|
| KvAsm 0.1.0 | 1,090 B (1,090–1,090 B) | 19,520 B (19,518–19,570 B) |
| Redis 8.10.2 | 1,352 B (1,351–1,352 B) | 20,739 B (20,716–20,748 B) |
| Valkey 9.1.2 | 1,350 B (1,349–1,350 B) | 20,745 B (20,744–20,755 B) |
| Dragonfly 2.0.0 | 1,064 B (1,064–1,066 B) | 16,650 B (16,591–16,690 B) |
| Garnet 2.1.8 | 1,209 B (1,207–1,209 B) | 18,431 B (18,398–18,538 B) |
| Memcached 1.6.45 | 1,192 B (1,192–1,192 B) | 17,567 B (17,567–17,567 B) |
| *KvAsm ÷ Redis* | 0.81× [0.81–0.81] sign p=0.250 ≈ | 0.94× [0.94–0.94] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 0.81× [0.81–0.81] sign p=0.250 ≈ | 0.94× [0.94–0.94] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.02× [1.02–1.02] sign p=0.250 ≈ | 1.17× [1.17–1.18] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.90× [0.90–0.90] sign p=0.250 ≈ | 1.06× [1.05–1.06] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.91× [0.91–0.91] sign p=0.250 ≈ | 1.11× [1.11–1.11] sign p=0.250 ≈ |

## E7 — Concurrency sweep (4 cores, 100% GET)

Requests in flight = connections × depth. At 1 in flight the throughput is exactly 1 / round-trip time; beyond that, each system shows how far concurrency carries it.

| Throughput (ops/s) | 1×1 = 1 | 16×1 = 16 | 64×1 = 64 | 64×4 = 256 | 64×16 = 1024 |
|---|---|---|---|---|---|
| KvAsm 0.1.0 | 33 k (33–36 k) | 282 k (224–293 k) | 533 k (526–548 k) | 1.45 M (1.30–1.46 M) | 2.77 M (2.72–2.95 M) |
| Redis 8.10.2 | 18 k (18–20 k) | 159 k (148–167 k) | 265 k (217–319 k) | 984 k (954–1.01 M) | 2.73 M (1.94–3.23 M) |
| Valkey 9.1.2 | 35 k (23–36 k) | 267 k (224–279 k) | 440 k (413–460 k) | 1.50 M (1.35–1.63 M) | 3.32 M (3.04–3.49 M) |
| Dragonfly 2.0.0 | 20 k (19–21 k) | 214 k (203–229 k) | 413 k (407–476 k) | 968 k (765–1.02 M) | 2.31 M (2.27–2.35 M) |
| Garnet 2.1.8 | 32 k (24–33 k) | 225 k (222–253 k) | 425 k (424–474 k) | 1.42 M (1.42–1.62 M) | 3.98 M (3.83–4.30 M) |
| Memcached 1.6.45 | 38 k (35–39 k) | 330 k (322–332 k) | 515 k (515–546 k) | 1.44 M (1.20–1.50 M) | 3.07 M (2.48–3.09 M) |
| *KvAsm ÷ Redis* | 1.82× [1.81–1.82] sign p=0.250 ≈ | 1.78× [1.41–1.99] sign p=0.250 ≈ | 2.01× [1.72–2.43] sign p=0.250 ≈ | 1.47× [1.32–1.52] sign p=0.250 ≈ | 1.01× [0.91–1.40] sign p=1.000 ≈ |
| *KvAsm ÷ Valkey* | 0.97× [0.95–1.48] sign p=1.000 ≈ | 1.06× [1.00–1.10] sign p=1.000 ≈ | 1.21× [1.19–1.27] sign p=0.250 ≈ | 0.97× [0.90–0.97] sign p=0.250 ≈ | 0.83× [0.82–0.91] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 1.64× [1.61–1.80] sign p=0.250 ≈ | 1.32× [1.04–1.44] sign p=0.250 ≈ | 1.29× [1.15–1.31] sign p=0.250 ≈ | 1.49× [1.43–1.70] sign p=0.250 ≈ | 1.20× [1.17–1.28] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 1.06× [1.00–1.50] sign p=0.250 ≈ | 1.26× [1.01–1.30] sign p=0.250 ≈ | 1.25× [1.16–1.26] sign p=0.250 ≈ | 1.02× [0.90–1.02] sign p=1.000 ≈ | 0.69× [0.69–0.71] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 0.88× [0.86–0.95] sign p=0.250 ≈ | 0.86× [0.68–0.91] sign p=0.250 ≈ | 1.04× [0.98–1.06] sign p=1.000 ≈ | 1.00× [0.97–1.08] sign p=1.000 ≈ | 0.90× [0.89–1.10] sign p=1.000 ≈ |

Median latency (µs; closed loop, so it grows with the queue by Little's law):

| p50 µs | 1 in flight | 16 in flight | 64 in flight | 256 in flight | 1024 in flight |
|---|---|---|---|---|---|
| KvAsm 0.1.0 | 25.5 (25.0–26.0) | 49.9 (45.3–59.1) | 99.8 (98.8–102.9) | 148.5 (148.5–164.9) | 313.3 (280.6–317.4) |
| Redis 8.10.2 | 47.4 (46.3–49.9) | 90.6 (87.5–97.8) | 220.2 (195.6–257.0) | 240.6 (238.6–242.7) | 346.1 (313.3–448.5) |
| Valkey 9.1.2 | 25.2 (24.7–33.5) | 56.6 (54.5–66.0) | 138.2 (138.2–148.5) | 162.8 (146.4–173.1) | 284.7 (272.4–305.1) |
| Dragonfly 2.0.0 | 47.9 (47.4–50.4) | 64.3 (61.2–66.0) | 138.2 (130.6–142.3) | 248.8 (228.3–276.5) | 399.4 (399.4–403.5) |
| Garnet 2.1.8 | 26.5 (25.7–28.5) | 60.7 (53.5–61.2) | 138.2 (127.5–140.3) | 162.8 (150.5–164.9) | 238.6 (220.2–242.7) |
| Memcached 1.6.45 | 23.9 (23.7–24.4) | 42.2 (41.7–42.2) | 100.9 (94.7–101.9) | 146.4 (132.1–158.7) | 252.9 (244.7–284.7) |
| *KvAsm ÷ Redis* | 0.54× [0.51–0.55] sign p=0.250 ≈ | 0.55× [0.46–0.65] sign p=0.250 ≈ | 0.45× [0.40–0.51] sign p=0.250 ≈ | 0.62× [0.61–0.69] sign p=0.250 ≈ | 0.91× [0.70–0.92] sign p=0.250 ≈ |
| *KvAsm ÷ Valkey* | 1.01× [0.77–1.01] sign p=1.000 ≈ | 0.88× [0.80–0.92] sign p=0.250 ≈ | 0.72× [0.69–0.72] sign p=0.250 ≈ | 0.91× [0.91–1.01] sign p=1.000 ≈ | 1.10× [1.03–1.10] sign p=0.250 ≈ |
| *KvAsm ÷ Dragonfly* | 0.53× [0.52–0.53] sign p=0.250 ≈ | 0.78× [0.69–0.92] sign p=0.250 ≈ | 0.72× [0.72–0.76] sign p=0.250 ≈ | 0.60× [0.60–0.65] sign p=0.250 ≈ | 0.78× [0.70–0.79] sign p=0.250 ≈ |
| *KvAsm ÷ Garnet* | 0.96× [0.87–1.01] sign p=1.000 ≈ | 0.82× [0.74–0.97] sign p=0.250 ≈ | 0.72× [0.72–0.78] sign p=0.250 ≈ | 0.91× [0.90–1.01] sign p=1.000 ≈ | 1.31× [1.27–1.33] sign p=0.250 ≈ |
| *KvAsm ÷ Memcached* | 1.06× [1.05–1.06] sign p=0.250 ≈ | 1.18× [1.07–1.42] sign p=0.250 ≈ | 0.99× [0.97–1.05] sign p=1.000 ≈ | 1.01× [1.01–1.12] sign p=0.250 ≈ | 1.24× [1.10–1.30] sign p=0.250 ≈ |

![e7_concurrency](charts/e7_concurrency.svg)

## E8 — Client validation (kvbench vs each ecosystem's standard tool, 4 cores)

If kvbench reaches at least the throughput of the standard tool on the same server and workload, it does not handicap that system.

| Server | Tool / keys | Throughput (ops/s) |
|---|---|---|
| KvAsm 0.1.0 | h2load-onekey | 2.41 M (2.37–2.45 M) |
| KvAsm 0.1.0 | kvbench-onekey | 3.12 M (3.01–3.14 M) |
| Redis 8.10.2 | kvbench-onekey | 3.45 M (3.35–3.46 M) |
| Redis 8.10.2 | kvbench-random | 2.91 M (2.81–3.00 M) |
| Redis 8.10.2 | memtier-random | 2.73 M (1.84–2.88 M) |
| Redis 8.10.2 | redis-benchmark-onekey | 3.24 M (2.93–3.33 M) |
| Redis 8.10.2 | redis-benchmark-random | 2.73 M (2.02–2.79 M) |

- Redis 8.10.2: kvbench ÷ redis = 1.07× [1.07–1.39] sign p=0.250 ≈
- Redis 8.10.2: kvbench ÷ memtier = 1.07× [0.98–1.63] sign p=1.000 ≈
- Redis 8.10.2: kvbench ÷ redis = 1.06× [1.04–1.15] sign p=0.250 ≈
- KvAsm 0.1.0: kvbench ÷ h2load = 1.30× [1.27–1.30] sign p=0.250 ≈

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 690 timed runs): median coefficient of variation 2.4%, 90th percentile 6.9%, worst 37.6%.

**Between runs** (fresh server each, E1 and S1 cells with ≥ 3 runs): median CV 5.0%, worst 14.1% (redis, 1 core).

| System | runs | median ops/s | between-run CV | median within-run CV | worst 1-s sample ÷ mean | median trend per 10 s | S1 ÷ E1 (same cell, hours apart) |
|---|---|---|---|---|---|---|---|
| KvAsm 0.1.0 | 10 | 2.93 M | 4.2% | 2.1% | 0.92 | -0.1% | 0.97× [0.82–1.08] MW p=0.313 ≈ |
| Redis 8.10.2 | 10 | 2.89 M | 4.7% | 2.7% | 0.86 | +2.5% | 1.00× [0.83–1.16] MW p=0.875 ≈ |
| Valkey 9.1.2 | 10 | 3.39 M | 4.6% | 1.7% | 0.81 | +0.2% | 1.03× [0.89–1.23] MW p=0.073 ≈ |
| Dragonfly 2.0.0 | 10 | 2.42 M | 4.9% | 1.8% | 0.94 | +0.1% | 1.05× [0.79–1.13] MW p=0.263 ≈ |
| Garnet 2.1.8 | 10 | 3.97 M | 3.8% | 3.9% | 0.80 | +2.1% | 1.05× [0.81–1.24] MW p=0.562 ≈ |
| Memcached 1.6.45 | 10 | 3.11 M | 3.2% | 2.3% | 0.92 | -1.3% | 1.00× [0.84–1.06] MW p=0.635 ≈ |

Reading guide: a between-run CV of a few percent means a single run is representative to within that margin; differences between systems smaller than the CVs involved are noise. The S1 ÷ E1 column should be ≈ 1.00 (≈): the same cell measured at the start and the end of the session.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvAsm ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvAsm clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
