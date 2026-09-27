# The full suite on the current server (2026-09-27)

The first full standard session since the server changed:

- its own HTTP/2 engine ([ADR 0007](../../../docs/adr/0007-native-http2-engine.md),
  nghttp2 removed by [ADR 0008](../../../docs/adr/0008-remove-nghttp2-from-the-server.md));
- the two-phase event loop ([ADR 0009](../../../docs/adr/0009-flush-once-per-event-batch.md));
- replies without allocation for PUT, DELETE and health checks (`a0264f7`).

The previous full session
([2026-09-26-standard-monitored](../2026-09-26-standard-monitored/REPORT.md),
`8cb0fef`) still ran the nghttp2 build.

- **Revision:** `a0264f7`, clean tree. Both images were rebuilt from it
  just before the run, and every build step was a cache hit.
- **Runs:** 747 of 747 valid. Every table is in [REPORT.md](REPORT.md),
  and the method is in [METHODOLOGY.md](../../METHODOLOGY.md).
- **Host:** i9-13900K, Docker Desktop on WSL2, 17:23–21:31 UTC.

## Host conditions: two phases

A check about five minutes before the start found 4.0 of 32 logical CPUs
busy outside the container VM (`hostload.py`). By the time the suite
started, run.py's own baseline read 8.1. A batch of backtests from another
project had begun: ten `nautilus_backtesting` Python workers at one CPU
each, replaced every minute or two. Offered the choice between waiting for
a quiet host and running now, the user chose to run now. METHODOLOGY.md §7
measures outside load and never excludes it, so no run was discarded.

| Experiments | Time (UTC) | Median outside load (CPUs) |
|---|---|---|
| E0, E1 blocks 0–3 | 17:23–18:19 | 16–18 |
| E1 block 4 | 18:20–18:27 | 11.2, falling to ~6 by its end |
| E1 block 5 | 18:27–18:35 | 5.0 |
| E2–E8, N1, N3, S1 | 18:35–21:31 | 3.6–6.4 per experiment |

So **E0 and E1 ran on a heavily loaded host, and everything after them ran
at the level of the published 2026-09-26 session** (3–6 CPUs). The effect
of the load shows in S1, which repeats E1's 4-core cell at the end. The
same cell ran 1.16–1.78× faster there, depending on the system: KvNode
1.78×, Redis 1.72×, Dragonfly 1.43×, Valkey 1.30×, Memcached 1.22× and
Garnet 1.16×.

The load cost KvNode more than the multi-threaded servers. Its 4-core lead
over Garnet was 1.50× in E1 but 2.29× in S1, over Memcached 2.02× against
2.94×, and over Valkey 2.02× against 2.76×. Over Redis it barely moved
(2.98× against 3.07×). E1's ratios therefore *understate* KvNode's lead
where they differ from S1.

A plausible mechanism, not verified: KvNode runs closest to the load
generator's limit (see *The client is the ceiling*), and the client's 16
vCPUs are what the outside load contends with.

**No interference inside the VM.** Another Claude session building or
benchmarking in the shared Docker VM would be invisible to the outside-load
monitor, so this session ran an in-VM watcher (see *In-VM interference
watcher* below).

- In no run window did the dev container or BuildKit use CPU, and there
  were never more containers than the suite's own two (server and client).
- 745 of 747 run windows are clean.
- The other two are single 5-s blips of 0.3–0.5 vCPU on otherwise unused
  vCPUs, both in block 0 of E0/E1:
  - one was the observer's own first `docker logs` call (17:27:06–11 UTC);
  - one is unexplained (17:49:07–12 UTC, `E1/kvnode/c1/r0`).

## The headline: 4 cores on the quiet host (S1)

S1 is E1's 4-core cell run ten more times at the end of the session: 100%
GET, 1M keys, 100-byte values, 64 connections × 16 in flight. Outside load
had a median of 5.2 CPUs. The paired statistics come from `pairs.py` →
`pairs.out`, with the method of METHODOLOGY.md §7.

| 4 cores, S1 | KvNode | Redis | Valkey | Dragonfly | Garnet | Memcached |
|---|---|---|---|---|---|---|
| GET/s, median of 10 | **8.84 M** | 2.88 M | 3.20 M | 2.40 M | 3.86 M | 3.01 M |
| KvNode ÷ X | — | **3.07×** [2.76–3.84] | **2.76×** [2.52–2.97] | **3.68×** [3.40–4.25] | **2.29×** [2.09–2.47] | **2.94×** [2.73–3.21] |
| between-run CV | 4.2% | 5.3% | 2.2% | 2.9% | 1.6% | 2.7% |

KvNode was ahead of every system in **all ten blocks** (sign test p =
0.002 each).

## One core (N1, N3, E5; quiet host)

| 1 core, N1 | KvNode | Redis 8.10.2 | Valkey 9.1.2 |
|---|---|---|---|
| GET/s | **1.76 M** | 1.40 M | 1.42 M |
| KvNode ÷ X, 6 blocks | — | **1.26×** [1.22–1.38], 6/6 | **1.24×** [1.20–1.28], 6/6 |
| server CPU per GET | **576 ns** | 722 ns | 710 ns |
| p50 | **532 µs** | 705 µs | 696 µs |

KvNode won every block against both (p = 0.031). So did the 2026-09-26
native-final session, by less (1.19× and 1.22×); the noisier two-phase
session won 5 of 6. Absolute numbers are not comparable across sessions on
this host, because the hypervisor places vCPUs on P- or E-cores
differently each session.

**Per-request floor (no-op round trip).** A gRPC health check costs **472
ns** of server CPU against 479 ns for a Redis `PING` (E5: 1.01×, a tie;
N3: 1.03×). It is clearly cheaper than Valkey (1.11× the throughput),
Dragonfly (1.15×), Garnet (1.64×) and Memcached (3.88×). On 2026-09-26,
with nghttp2, it was 1,086 ns against Redis's 422.

## Scaling, 1 to 8 cores (E1; loaded host)

Blocks 0–4 ran with 11–18 CPUs busy outside the VM, so the absolute numbers
are depressed and S1 above is the quiet-host reference.

| E1 | KvNode | Redis | Valkey | Dragonfly | Garnet | Memcached |
|---|---|---|---|---|---|---|
| 1 core | **1.39 M** | 1.01 M | 1.04 M | 629 k | 642 k | 587 k |
| 2 cores | **3.06 M** | 1.29 M | 1.68 M | 1.08 M | 1.51 M | 1.23 M |
| 4 cores | **4.98 M** | 1.67 M | 2.46 M | 1.68 M | 3.32 M | 2.47 M |
| 8 cores | **5.99 M** | 1.86 M | 1.82 M | 2.18 M | 4.96 M | 4.34 M |

KvNode was ahead of every system in all six blocks at every budget. The one
exception is Garnet at 8 cores: 1.21× [0.88–1.29], 5/6. After the
adjustment to equal outside load, the conclusions are the same (REPORT.md,
*Host interference*).

Once the load had dropped, in blocks 4 and 5 (5.6–8.9 CPUs during KvNode's
runs), KvNode served 7.78M and 8.39M at 4 cores, and 9.33M and 9.02M at 8.

**At 8 cores the limit is not KvNode.**

- Under the load (blocks 0–3), KvNode used only 0.52–0.56 of its 8 cores
  and the client 0.74–0.81 of its 16 vCPUs: the host was taking vCPUs away
  from both.
- In blocks 4 and 5 the client was 90–92% busy while KvNode's cores were
  0.67–0.70 busy.

Both of those runs, and two 8-core E0 runs, are flagged client-bound (see
*The client is the ceiling*).

E0 ran under the heaviest load, and at 8 cores it chose N−1 threads for
KvNode, Redis, Valkey and Dragonfly (N for Memcached). With 8 server vCPUs
plus 16 busy client vCPUs against ~15 free logical CPUs, the 8-core cells
were oversubscribed. On 2026-09-26, N won for every system except Valkey.
Only E1's 8-core cell uses these choices.

## Read/write mixes (E2; 4 cores, quiet host)

| 4 cores | 90% GET | 50% GET | 100% SET | 90% GET, Zipf 0.99 |
|---|---|---|---|---|
| KvNode | **7.66 M** | 3.79 M | 2.23 M | **7.87 M** |
| Redis | 2.88 M | 2.65 M | 2.58 M | 2.98 M |
| Valkey | 3.01 M | 2.24 M | 1.78 M | 3.09 M |
| Dragonfly | 2.43 M | 2.33 M | 2.48 M | 2.57 M |
| Garnet | 3.96 M | **3.84 M** | **3.75 M** | 4.19 M |
| Memcached | 2.95 M | 2.36 M | 1.54 M | 4.13 M |

- **Read-heavy:** KvNode is ahead of every system in every block, 1.88–3.16×
  (Garnet is the closest).
- **50/50:** a tie with Garnet (0.99×). KvNode is ahead of the others,
  1.43–1.69×.
- **Writes are still the weak spot:**
  - 2.23M SET/s is behind Redis (0.87×: 5/6 blocks raw, clear after the
    load adjustment), Dragonfly (0.90×) and Garnet (0.60×);
  - it is ahead of Valkey (1.26×) and Memcached (1.45×);
  - all writes still pass one writer lock (ADR 0003).

## No pipelining, latency, concurrency (E3, E4, E7; quiet host)

- **No pipelining** (E3, 256 connections × 1 request, 4 cores):
  - **815k requests/s**, ahead of every system: 1.34× Memcached, 1.39×
    Valkey, 1.85–2.07× Dragonfly, Garnet and Redis;
  - the lowest p50, 274 µs;
  - a p99 of 791 µs, lower than every system but Valkey (721 µs).
- **Latency at identical offered load** (E4, open loop):
  - At 418k, 836k and 1.25M requests/s, KvNode's p50 is 101, 117 and 111
    µs, and its p99 375, 444 and 428 µs.
  - Only Valkey has a lower p99 (253–395 µs). Redis reaches 0.65–0.91 ms.
    Garnet's p99 grows to 9.0 ms and then 93 ms.
  - Every system kept up with the offered load.
  - These loads are 25/50/75% of the slowest system's E1 median at 4
    cores. That median was measured under the heavy load, which is why the
    loads are lower than 2026-09-26's 574k, 1.15M and 1.72M.
  - 3 runs per cell, so these are estimates.
- **Concurrency sweep** (E7, 4 cores; the tables are in REPORT.md):
  - At 1 request in flight, the round trip is 24.7 µs (35k/s), level with
    Valkey, Garnet and Memcached. Redis and Dragonfly take ~48 µs.
  - At 16 in flight, Memcached is ahead (0.90×).
  - From 64 in flight on, KvNode leads every system. At 1,024 in flight it
    does **9.11M GET/s**: 2.23× Garnet and 2.67–3.64× the others.

## Value size (E6; 4 cores, quiet host)

| 4 cores | 1 KiB values | 16 KiB values |
|---|---|---|
| KvNode | **5.59 M** | 481 k |
| Redis | 1.91 M | 461 k |
| Valkey | 1.85 M | 273 k |
| Dragonfly | 1.53 M | 444 k |
| Garnet | 3.16 M | 972 k |
| Memcached | 2.47 M | **995 k** |

- **1 KiB:** 1.77× Garnet and 2.3–3.7× the others.
- **16 KiB:** 1.04× Redis, but **0.50× Garnet and 0.48× Memcached**, and
  **30% below the old nghttp2 build** (see *A 16 KiB regression*).
- **Memory per key** is the lowest or nearly so:
  - 1,075 B at 1 KiB (Dragonfly 1,065, Redis 1,351);
  - 16,515 B at 16 KiB (the lowest; Redis 20,727).

## Memory per key (E1)

KvNode's 163 bytes per 100-byte-value key is **not** a gain over
2026-09-26's 178. Since `9885e64` the store table (16 MiB for the default
capacity) is written at startup, so it is resident before kvbench's
pre-load baseline and no longer counted in the delta. With it, the figure
is ≈ 180 bytes. Redis uses 174, Valkey 173, Dragonfly 157, Memcached 196
and Garnet 285.

## Against the previous full session

KvNode then was the nghttp2 build `8cb0fef`. Both sessions' quiet cells
are compared here. The other systems barely moved between the two sessions
(Redis's S1: 2.89M then, 2.88M now), so KvNode's column is its own change.

| Cell | KvNode 2026-09-26 | KvNode now | Change | The other systems' change |
|---|---|---|---|---|
| 4 cores, 100% GET (S1) | 2.93 M | **8.84 M** | **3.02×** | −6% to 0% |
| 90% GET (E2) | 2.75 M | 7.66 M | 2.79× | 0% to +10% |
| 50% GET (E2) | 2.30 M | 3.79 M | 1.65× | −1% to +8% |
| 100% SET (E2) | 1.72 M | 2.23 M | 1.30× | 0% to +8% |
| no pipelining (E3) | 618 k | 815 k | 1.32× | −15% to −6% |
| no-op, 1 core (E5) | 923 k (1,086 ns) | 2.13 M (472 ns) | 2.31× | −12% to −5% |
| 1 KiB values (E6) | 2.63 M | 5.59 M | 2.13× | −10% to −5% |
| **16 KiB values (E6)** | **691 k** | **481 k** | **0.70×** | −8% to −5% |
| 1,024 in flight (E7) | 2.77 M | 9.11 M | 3.29× | +3% to +8% |

The other systems moved by up to about ±10% between sessions, cell by cell:
the host itself was not identical. KvNode's changes are several times
larger, except at 16 KiB, where it moved the other way.

## A 16 KiB regression

This is the only cell where the current server is slower than the old one:
−30%, while every other system lost 5–8%. With 3 runs per build it is an
estimate, but the ranges are far apart: 654–730k then, 473–492k now.

Nothing yet shows which change caused it. Hypotheses to test with a bisect
and a profile:

- **Framing:** a 16 KiB value makes a gRPC message just over the default
  16,384-byte maximum frame, so every reply takes a full DATA frame plus a
  tiny second one.
- **Flow control:** with 16 × 16 KiB in flight per connection, the default
  65,535-byte windows force WINDOW_UPDATE round trips. How the native
  engine resumes blocked streams may differ from nghttp2.
- **Buffering:** the two-phase loop holds a whole batch's replies (up to
  ~260 KiB per connection) before flushing them.

## The client is the ceiling

kvbench's gRPC client reaches about **9.3M requests/s** on its 16 vCPUs:

- E8, one key: 9.30M against KvNode, with h2load at only 5.89M;
- E1 at 8 cores once the load had dropped: 9.02–9.33M, with the client
  90–92% busy while KvNode used 0.67–0.70 of its cores;
- S1, 4 cores: the client was 80% busy with KvNode's server at 0.96.

KvNode's 8-core cell (and E7's 1,024-in-flight cell) therefore measures the
load generator. Its 4-core S1 is a floor, not a ceiling. E8 also shows that
kvbench never handicaps a system: it matches or beats redis-benchmark
(1.03–1.06×) and memtier_benchmark (1.05×) against Redis.

Measuring KvNode at 8 cores needs clients on another machine. This host
cannot supply more client CPU, as a follow-up A/B measured
([2026-09-28-client-ceiling](../2026-09-28-client-ceiling/README.md)):

- **The client is saturated:** with the standard client (16 vCPUs), 8
  workers did 9.72M GET/s, with the client 97% busy and KvNode's cores 69%
  busy.
- **A bigger client is slower:** on 24 vCPUs (8–31) it was 12% slower in
  6 of 6 blocks. KvNode's CPU per GET rose 43% and the client's 18%.
- **Why:** the i9-13900K's 24 physical cores are already taken by 8 server
  and 16 client vCPUs. The spare vCPUs are hyperthread siblings.

## In-VM interference watcher

`vmwatch.sh` ran in a container pinned to vCPU 15, which neither the
servers (0–7) nor the client (16–31) use. Every 5 s it logged cumulative
per-vCPU busy time from `/proc/stat` (the whole VM, not namespaced) and
every container cgroup's CPU (`cpuacct.usage`), keyed by container id.

`vmwatch_report.py --runs` differences the samples and judges each run's
measured window, [end − duration − 3 s, end]:

- CPU used by the dev container (`kvnode-dev`, idle at exactly 0) or by
  BuildKit;
- more than the suite's own two containers active inside the window;
- the mean load on the side vCPUs 8–14 over the window's interior.

The side-vCPU level is judged sustained, not per interval, because at run
boundaries the suite's own container start and removal, and the
observer's `docker` calls, reach 0.3–0.5 vCPU for a single interval. This
was measured this session by aligning intervals with run boundaries and
poll times (`polls.log`).

Median side load per run window was 0.01–0.05 vCPU in every experiment.

The VM clock ran 0.45 s behind Windows at the start and 0.45 s ahead at
the end. The verdict is identical for any assumed offset from −1 to +1 s
(at +1 s one flag fewer).

Limit: a 13-s window holds only one or two whole 5-s intervals, so a
single blip can flag a run, which is what happened twice. Nothing
sustained occurred.

Reproduce the verdict:

```sh
python vmwatch_report.py --log vmwatch.log --runs raw.jsonl \
    --watch 3719a2e3beb5 --dev bae3fd5a0c43 --offset -0.45
```

## Files

| File | What |
|---|---|
| `raw.jsonl`, `environment.json`, `REPORT.md`, `charts/` | the session: every run, the environment, and analyze.py's report |
| `run.log` | run.py's console output |
| `pairs.py` → `pairs.out` | S1 and E1 paired KvNode ÷ X with sign tests, plus client and server saturation |
| `hostload.py` | the pre-start probe: outside-VM load and the heaviest host processes |
| `vmwatch.sh`, `vmwatch.log` | the in-VM watcher and its raw samples |
| `vmwatch_report.py` → `vmwatch-verdict.txt` | the per-run interference verdict |
| `polls.log` | when the observer's live polls called `docker logs` (epoch seconds), to attribute blips |
