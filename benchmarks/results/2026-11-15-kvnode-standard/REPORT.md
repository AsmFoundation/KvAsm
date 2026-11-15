# KvNode benchmark report

Profile **standard**, recorded 2026-11-15T13:55:48 UTC, repository revision `0e48073b64ce`. Method: [METHODOLOGY.md](../../METHODOLOGY.md).

## Environment

- CPU: 13th Gen Intel(R) Core(TM) i9-13900K, 32 vCPUs seen by the container VM; Docker Desktop, kernel 5.15.153.1-microsoft-standard-WSL2
- Docker 27.3.1, cgroup v1 (cgroupfs), security options ['name=seccomp,profile=unconfined']
- Placement: server on vCPUs 0..N-1, kvbench on 16-31 with 16 threads, loopback in the server's network namespace
- CPU clock factor 1.0354 (cgroup CPU time per wall second on a known load; server CPU figures are divided by it)

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
| E0 | 36 | 0 | 0 |
| E1 | 48 | 0 | 0 |
| E2 | 48 | 0 | 0 |
| E3 | 12 | 0 | 0 |
| E4 | 18 | 0 | 0 |
| E5 | 12 | 0 | 0 |
| E6 | 12 | 0 | 0 |
| E7 | 30 | 0 | 0 |
| E8 | 12 | 0 | 0 |
| N1 | 12 | 0 | 0 |
| N3 | 12 | 0 | 0 |
| S1 | 20 | 0 | 0 |

Client-bound runs (client > 85% busy, server < 90%): 24
- `E0/kvnode/c8/t8/get100-v100-k1000000-uni-c64d16/r0`
- `E0/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r0`
- `E0/kvnode-control/c8/t7/get100-v100-k1000000-uni-c64d16/r0`
- `E0/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r0`
- `E0/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r1`
- `E0/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r1`
- `E0/kvnode-control/c8/t7/get100-v100-k1000000-uni-c64d16/r1`
- `E0/kvnode/c8/t8/get100-v100-k1000000-uni-c64d16/r1`
- `E0/kvnode/c8/t8/get100-v100-k1000000-uni-c64d16/r2`
- `E0/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r2`
- `E0/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r2`
- `E0/kvnode-control/c8/t7/get100-v100-k1000000-uni-c64d16/r2`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r0`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r0`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r1`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r1`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r2`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r2`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r3`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r3`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r4`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r4`
- `E1/kvnode-control/c8/t8/get100-v100-k1000000-uni-c64d16/r5`
- `E1/kvnode/c8/t7/get100-v100-k1000000-uni-c64d16/r5`

## Host interference

Before the first run, programs outside the container VM used **5.2 of 32 logical CPUs** (peak 7.0).

| Experiment | runs | median outside load (CPUs) | 90th percentile | max | runs above 2 |
|---|---|---|---|---|---|
| E0 | 36 | 5.1 | 7.3 | 11.5 | 36 |
| E1 | 48 | 5.8 | 8.1 | 12.9 | 48 |
| E2 | 48 | 6.7 | 10.4 | 12.4 | 48 |
| E3 | 12 | 7.7 | 12.6 | 13.7 | 12 |
| E4 | 18 | 5.9 | 8.6 | 10.7 | 18 |
| E5 | 12 | 4.7 | 6.8 | 7.3 | 12 |
| E6 | 12 | 4.6 | 7.2 | 7.8 | 12 |
| E7 | 30 | 6.1 | 9.6 | 11.8 | 30 |
| N1 | 12 | 5.1 | 7.5 | 8.3 | 12 |
| N3 | 12 | 4.4 | 6.4 | 6.5 | 12 |
| S1 | 20 | 4.4 | 6.8 | 7.7 | 20 |

**Effect:** Spearman ρ = -0.28 between outside load and each run's throughput relative to its cell's median (242 runs). Negative means outside load cost throughput; near zero means it did not measurably matter.

**Quiet-only check** (declared in advance): 0 of 81 KvNode-vs-other pairs had at most 2 CPUs of outside load on both runs, too few to recompute anything.

**Adjusted to equal outside load.** Each experiment's pairs are corrected with one common slope of log(KvNode ÷ X) on the difference in outside load between the two runs of a block, estimated within comparisons (analysis of covariance). The slope column gives the change of the ratio per logical CPU of extra outside load on KvNode's run. Raw, unadjusted results are the tables in each section below. Where the two disagree, the difference is load, not the servers.

| Experiment · cell | slope |  |
|---|---|
| E1 · 1 core | -2.4% |  |
| E1 · 2 cores | -2.4% |  |
| E1 · 4 cores | -2.4% |  |
| E1 · 8 cores | -2.4% |  |
| E2 · 90% GET | -3.4% |  |
| E2 · 90% GET, Zipf | -3.4% |  |
| E2 · 50% GET | -3.4% |  |
| E2 · 100% SET | -3.4% |  |
| E3 · 256×1 | -2.1% |  |
| E5 · no-op | -1.3% |  |
| E6 · 1024 B values | -0.6% |  |
| E6 · 16384 B values | -0.6% |  |
| E7 · 1×1 | -0.5% |  |
| E7 · 16×1 | -0.5% |  |
| E7 · 64×1 | -0.5% |  |
| E7 · 64×4 | -0.5% |  |
| E7 · 64×16 | -0.5% |  |

## E0 — Thread-count tuning

Median GET throughput for each thread setting; the better one (bold) is used by every later experiment.

| System | Cores | threads = N−1 | threads = N |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 2 | 955 k (753–996 k) | **2.35 M (2.23–2.37 M)** |
| KvNode (kvnode:adr0011) | 4 | 3.87 M (3.24–4.01 M) | **4.76 M (3.66–4.98 M)** |
| KvNode (kvnode:adr0011) | 8 | **5.30 M (5.08–5.52 M)** | 5.29 M (5.12–5.69 M) |
| KvNode control (kvnode:0.1.0) | 2 | 931 k (886–982 k) | **2.18 M (1.98–2.50 M)** |
| KvNode control (kvnode:0.1.0) | 4 | 3.86 M (3.27–3.96 M) | **4.41 M (4.34–4.76 M)** |
| KvNode control (kvnode:0.1.0) | 8 | 5.13 M (5.05–5.23 M) | **5.58 M (5.56–5.68 M)** |

## E1 — Throughput scaling (100% GET, 100-byte values, 1M keys, uniform)

| Throughput (ops/s) | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 919 k (827–977 k) | 2.30 M (1.91–2.39 M) | 4.47 M (3.55–4.81 M) | 5.27 M (4.35–5.44 M) |
| KvNode control (kvnode:0.1.0) | 941 k (801–984 k) | 2.22 M (2.00–2.32 M) | 4.58 M (3.93–4.95 M) | 5.33 M (5.00–5.52 M) |
| *KvNode ÷ KvNode* | 0.98× [0.89–1.22] sign p=0.688 ≈ | 1.04× [0.88–1.16] sign p=0.688 ≈ | 0.98× [0.87–1.03] sign p=1.000 ≈ | 0.99× [0.83–1.05] sign p=0.219 ≈ |

![e1_throughput](charts/e1_throughput.svg)

**CPU efficiency** — operations per server CPU-second (higher is better):

| ops per CPU-second | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 921 k (825–974 k) | 1.15 M (955–1.20 M) | 1.16 M (926–1.25 M) | 944 k (910–964 k) |
| KvNode control (kvnode:0.1.0) | 940 k (803–981 k) | 1.11 M (1.00–1.16 M) | 1.19 M (1.02–1.29 M) | 956 k (904–1.01 M) |
| *KvNode ÷ KvNode* | 0.98× [0.90–1.21] sign p=0.688 ≈ | 1.03× [0.88–1.15] sign p=0.688 ≈ | 0.97× [0.86–1.08] sign p=0.688 ≈ | 0.99× [0.93–1.03] sign p=0.688 ≈ |

**Server CPU utilization** of its core budget (1.0 = all given cores busy; ▲ only means KvNode kept more of its cores busy, which is neither good nor bad by itself):

| utilization | 1 core | 2 cores | 4 cores | 8 cores |
|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 1.00 (1.00–1.00) | 1.00 (1.00–1.00) | 0.96 (0.92–0.97) | 0.70 (0.60–0.71) |
| KvNode control (kvnode:0.1.0) | 1.00 (1.00–1.00) | 1.00 (1.00–1.00) | 0.96 (0.96–0.97) | 0.71 (0.65–0.72) |
| *KvNode ÷ KvNode* | 1.00× [0.99–1.01] sign p=0.688 ≈ | 1.00× [1.00–1.00] sign p=0.688 ≈ | 1.00× [0.95–1.01] sign p=1.000 ≈ | 0.98× [0.83–1.07] sign p=1.000 ≈ |

**Memory per key** after preloading 1M keys (anonymous memory delta ÷ keys; 16-byte key + 100-byte value = 116 raw bytes; lower is better):

| bytes / key | all E1 runs |
|---|---|
| KvNode (kvnode:adr0011) | 163 B (162–163 B) |
| KvNode control (kvnode:0.1.0) | 163 B (162–163 B) |
| *KvNode ÷ KvNode* | 1.00× [1.00–1.00] sign p=0.210 ≈ |

## E2 — Read/write mixes (4 cores)

100-byte values, 1M keys; Zipf uses YCSB's scrambled Zipfian with θ = 0.99.

| Throughput (ops/s) | 90% GET | 50% GET | 100% SET | 90% GET, Zipf 0.99 |
|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 4.02 M (3.70–4.27 M) | 3.73 M (3.07–3.83 M) | 4.00 M (3.11–4.19 M) | 4.18 M (3.57–4.61 M) |
| KvNode control (kvnode:0.1.0) | 3.85 M (3.10–4.31 M) | 2.18 M (2.09–2.26 M) | 1.34 M (1.18–1.37 M) | 4.08 M (3.76–4.43 M) |
| *KvNode ÷ KvNode* | 1.05× [0.93–1.38] sign p=0.688 ≈ | 1.71× [1.41–1.79] sign p=0.031 ▲ | 2.99× [2.32–3.41] sign p=0.031 ▲ | 1.02× [0.95–1.11] sign p=0.688 ≈ |

## E3 — No pipelining (256 connections × 1 request in flight, 4 cores)

| metric | ops/s | p50 µs | p99 µs |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 446,642 (424,679–481,310) | 518.1 (477.2–544.8) | 1,409 (1,335–1,450) |
| KvNode control (kvnode:0.1.0) | 451,830 (382,717–465,740) | 514.0 (493.6–561.1) | 1,376 (1,352–2,572) |
| *KvNode ÷ KvNode* | 0.99× [0.97–1.11] sign p=1.000 ≈ | 1.01× [0.93–1.04] sign p=1.000 ≈ | 1.02× [0.56–1.05] sign p=1.000 ≈ |

## E4 — Latency at identical offered load (open loop, Poisson, 4 cores)

Latency is measured from each request's intended send time, so queueing behind a slow response counts (no coordinated omission). Lower is better.

**p50 latency (µs)**

| p50 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 189.4 (185.3–191.5) | 209.9 (193.5–218.1) | 242.7 (226.3–244.7) |
| KvNode control (kvnode:0.1.0) | 201.7 (160.8–203.8) | 214.0 (203.8–230.4) | 232.4 (214.0–240.6) |
| *KvNode ÷ KvNode* | 0.94× [0.91–1.19] sign p=1.000 ≈ | 0.98× [0.90–1.03] sign p=1.000 ≈ | 1.04× [0.97–1.14] sign p=1.000 ≈ |

**p99 latency (µs)**

| p99 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 766.0 (692.2–798.7) | 700.4 (626.7–716.8) | 708.6 (643.1–1335.3) |
| KvNode control (kvnode:0.1.0) | 610.3 (602.1–806.9) | 725.0 (634.9–741.4) | 675.8 (634.9–774.1) |
| *KvNode ÷ KvNode* | 1.26× [0.99–1.26] sign p=1.000 ≈ | 0.97× [0.86–1.13] sign p=1.000 ≈ | 1.05× [1.01–1.72] sign p=0.250 ≈ |

**p999 latency (µs)**

| p999 | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 1155.1 (1073.2–1368.1) | 978.9 (954.4–987.1) | 1237.0 (1044.5–3031.0) |
| KvNode control (kvnode:0.1.0) | 1073.2 (913.4–1548.3) | 1286.1 (921.6–1581.1) | 1105.9 (978.9–1482.8) |
| *KvNode ÷ KvNode* | 1.08× [0.88–1.17] sign p=1.000 ≈ | 0.76× [0.60–1.06] sign p=1.000 ≈ | 1.12× [1.07–2.04] sign p=0.250 ≈ |

![e4_p99](charts/e4_p99.svg)

Achieved throughput (should equal the offered load when the system keeps up):

| achieved | 418 k/s | 836 k/s | 1.25 M/s |
|---|---|---|---|
| KvNode (kvnode:adr0011) | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| KvNode control (kvnode:0.1.0) | 418 k (418–418 k) | 836 k (836–836 k) | 1.25 M (1.25–1.25 M) |
| *KvNode ÷ KvNode* | 1.00× [1.00–1.00] sign p=1.000 ≈ | 1.00× [1.00–1.00] sign p=0.500 ≈ | 1.00× [1.00–1.00] sign p=0.250 ≈ |

## E5 — Per-request floor (no-op round trip, 1 core)

RESP `PING`, memcached meta `mn`, gRPC `grpc.health.v1.Health/Check`: the cost of protocol, framing and event loop without any data access.

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvNode (kvnode:adr0011) | 1.12 M (1.09–1.18 M) | 894 (849–923) |
| KvNode control (kvnode:0.1.0) | 1.15 M (1.08–1.19 M) | 868 (846–928) |
| *KvNode ÷ KvNode* | 0.97× [0.92–1.08] sign p=0.688 ≈ | 1.03× [0.93–1.08] sign p=0.688 ≈ |

## E6 — Value size (4 cores, 100% GET, about 1 GiB of data)

| Throughput (ops/s) | 1024 B values | 16384 B values |
|---|---|---|
| KvNode (kvnode:adr0011) | 3.23 M (2.92–3.69 M) | 317 k (287–325 k) |
| KvNode control (kvnode:0.1.0) | 3.42 M (2.95–3.62 M) | 325 k (306–358 k) |
| *KvNode ÷ KvNode* | 0.95× [0.95–1.02] sign p=1.000 ≈ | 0.98× [0.89–1.00] sign p=1.000 ≈ |

Memory per key (lower is better):

| bytes / key | 1024 B | 16384 B |
|---|---|---|
| KvNode (kvnode:adr0011) | 1,075 B (1,075–1,075 B) | 18,438 B (18,434–18,438 B) |
| KvNode control (kvnode:0.1.0) | 1,075 B (1,075–1,075 B) | 16,515 B (16,515–16,515 B) |
| *KvNode ÷ KvNode* | 1.00× [1.00–1.00] sign p=0.500 ≈ | 1.12× [1.12–1.12] sign p=0.250 ≈ |

## E7 — Concurrency sweep (4 cores, 100% GET)

Requests in flight = connections × depth. At 1 in flight the throughput is exactly 1 / round-trip time; beyond that, each system shows how far concurrency carries it.

| Throughput (ops/s) | 1×1 = 1 | 16×1 = 16 | 64×1 = 64 | 64×4 = 256 | 64×16 = 1024 |
|---|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 15 k (12–17 k) | 149 k (141–159 k) | 370 k (323–390 k) | 1.39 M (1.26–1.53 M) | 4.17 M (4.06–4.46 M) |
| KvNode control (kvnode:0.1.0) | 17 k (16–18 k) | 121 k (116–128 k) | 369 k (339–385 k) | 1.43 M (1.41–1.45 M) | 3.83 M (3.74–4.01 M) |
| *KvNode ÷ KvNode* | 0.85× [0.68–0.93] sign p=0.250 ≈ | 1.24× [1.10–1.37] sign p=0.250 ≈ | 1.00× [0.95–1.01] sign p=1.000 ≈ | 0.97× [0.90–1.07] sign p=1.000 ≈ | 1.09× [1.01–1.16] sign p=0.250 ≈ |

Median latency (µs; closed loop, so it grows with the queue by Little's law):

| p50 µs | 1 in flight | 16 in flight | 64 in flight | 256 in flight | 1024 in flight |
|---|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 56.6 (50.4–60.2) | 91.7 (86.5–96.8) | 150.5 (146.4–164.9) | 158.7 (148.5–166.9) | 187.4 (183.3–212.0) |
| KvNode control (kvnode:0.1.0) | 51.5 (48.9–57.1) | 116.2 (103.9–119.3) | 150.5 (148.5–160.8) | 154.6 (152.6–154.6) | 207.9 (193.5–220.2) |
| *KvNode ÷ KvNode* | 1.10× [0.99–1.17] sign p=1.000 ≈ | 0.79× [0.73–0.93] sign p=0.250 ≈ | 1.00× [0.99–1.03] sign p=1.000 ≈ | 1.03× [0.96–1.08] sign p=1.000 ≈ | 0.90× [0.90–0.96] sign p=0.250 ≈ |

![e7_concurrency](charts/e7_concurrency.svg)

## E8 — Client validation (kvbench vs each ecosystem's standard tool, 4 cores)

If kvbench reaches at least the throughput of the standard tool on the same server and workload, it does not handicap that system.

| Server | Tool / keys | Throughput (ops/s) |
|---|---|---|
| KvNode (kvnode:adr0011) | h2load-onekey | 3.20 M (3.13–3.21 M) |
| KvNode (kvnode:adr0011) | kvbench-onekey | 4.92 M (4.76–4.96 M) |
| KvNode control (kvnode:0.1.0) | h2load-onekey | 3.22 M (3.19–3.23 M) |
| KvNode control (kvnode:0.1.0) | kvbench-onekey | 4.77 M (4.70–4.97 M) |

- KvNode (kvnode:adr0011): kvbench ÷ h2load = 1.54× [1.49–1.58] sign p=0.250 ≈

## N — one core: KvNode vs Redis and Valkey

E1's and E5's 1-core cells for the single-threaded-execution servers, run on their own so the headline is re-checked with full pairing. Ratios are KvNode ÷ X, paired by block.

### N1 — 1 core, E1's workload (100% GET, 100-byte values, 1M keys, 64×16)

| metric | ops/s | ops per CPU-second | server CPU ns/op | p50 µs | p99 µs |
|---|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 909 k (826–979 k) | 904 k (822–970 k) | 1106 (1031–1217) | 1034 (987–1073) | 2195 (2114–2933) |
| KvNode control (kvnode:0.1.0) | 901 k (802–949 k) | 895 k (801–943 k) | 1117 (1060–1248) | 1042 (1012–1155) | 2261 (2114–2966) |
| *KvNode ÷ KvNode* | 1.01× [0.96–1.05] sign p=0.219 ≈ | 1.01× [0.96–1.04] sign p=0.219 ≈ | 0.99× [0.96–1.04] sign p=0.219 ≈ | 0.99× [0.92–1.04] sign p=0.375 ≈ | 0.97× [0.90–1.22] sign p=1.000 ≈ |

### N3 — per-request floor (no-op round trip, 1 core)

| metric | ops/s | server CPU ns/op |
|---|---|---|
| KvNode (kvnode:adr0011) | 1.10 M (1.04–1.20 M) | 913 (843–963) |
| KvNode control (kvnode:0.1.0) | 1.14 M (1.07–1.15 M) | 886 (876–940) |
| *KvNode ÷ KvNode* | 0.97× [0.93–1.07] sign p=0.688 ≈ | 1.03× [0.94–1.08] sign p=0.688 ≈ |

## S1 — Measurement stability

**Within runs** (per-second throughput samples, all 260 timed runs): median coefficient of variation 4.0%, 90th percentile 9.0%, worst 21.6%.

**Between runs** (fresh server each, E1 and S1 cells with ≥ 3 runs): median CV 5.9%, worst 10.0% (kvnode, 4 cores).

| System | runs | median ops/s | between-run CV | median within-run CV | worst 1-s sample ÷ mean | median trend per 10 s | S1 ÷ E1 (same cell, hours apart) |
|---|---|---|---|---|---|---|---|
| KvNode (kvnode:adr0011) | 10 | 4.76 M | 5.3% | 2.5% | 0.39 | +1.3% | 1.07× [0.88–1.42] MW p=0.118 ≈ |
| KvNode control (kvnode:0.1.0) | 10 | 4.80 M | 3.3% | 3.6% | 0.88 | +1.0% | 1.05× [0.90–1.26] MW p=0.118 ≈ |

Reading guide: a between-run CV of a few percent means a single run is representative to within that margin; differences between systems smaller than the CVs involved are noise. The S1 ÷ E1 column should be ≈ 1.00 (≈): the same cell measured at the start and the end of the session.

---

Legend: `median (min–max)` over repetitions, each with a fresh server. KvNode ÷ X: ratio of medians, then either **sign** — [min–max of the per-block ratios] and the exact two-sided sign test over blocks (paired: both systems ran in the same block, so shared drift cancels) — or **MW** — [conservative range containing every pairwise run ratio] and the exact two-sided Mann–Whitney p (unpaired). ▲ KvNode clearly better, ▼ clearly worse (p < 0.05; for MW also the range excludes 1), ≈ no clear difference. With 6 blocks, ▲/▼ means all six agree; cells with 3 repetitions are descriptive only — no test can reach p < 0.05 with them.
