# KvNode benchmarks

This folder compares KvNode with other in-memory key-value servers (Redis,
Valkey, Dragonfly, Garnet and Memcached), using one client, one placement
and one statistical method for all of them. The rules, and the reasons
behind each, are in [METHODOLOGY.md](METHODOLOGY.md).

## Native HTTP/2 engine: 1 core now beats Redis and Valkey (2026-09-26)

KvNode's own HTTP/2 engine (`server/src/h2n.asm`,
[ADR 0007](../docs/adr/0007-native-http2-engine.md)) replaced nghttp2 as
the default. N1–N3 compare it with nghttp2 in the same image, and with Redis
and Valkey, with the same method as below. Final session:
[REPORT.md](results/2026-09-26-native-final/REPORT.md)
([README](results/2026-09-26-native-final/README.md)); the two earlier
sessions record the engine's first version and the header-block cache.

| 1 core (N1) | KvNode native | Redis | Valkey | KvNode nghttp2 |
|---|---|---|---|---|
| GET/s, median of 6 | **1.51 M** | 1.27 M | 1.24 M | 744 k |
| native ÷ X (paired, 6/6 blocks) | — | **1.19×** [1.10–1.35] | **1.22×** [1.14–1.34] | 2.03× |
| server CPU per GET | 665 ns | 788 ns | 813 ns | 1,348 ns |

- **Scaling (N2):** native vs nghttp2 is 1.96× at 2 cores and 2.07× at 4
  cores (5.45M GET/s). In the quieter preceding session it was 2.29× and
  3.06×, with 7.80M GET/s at 4 cores.
- **No-op floor (N3):** a native gRPC health check round trip runs at
  1.93M/s, level with a Redis `PING` (1.00×). That is 1.07× Valkey and
  2.40× nghttp2.
- **Host:** outside load was a median of 7–8 logical CPUs; 72 of 72 runs
  were valid.

- **The 1-core goal is met.** The engine removed ~700 ns per request.
  Server CPU per GET is 665 ns against Redis's 788 and Valkey's 813, and
  the median latency is lower too (p50 0.79× Redis's).
- **Where the rest goes:** about three quarters of a request is now kernel
  time (one `read` and one `sendto` per 16 requests over loopback TCP),
  shared by every server here. The remaining user time is dominated by the
  store's DRAM latency, already overlapped by batched prefetching
  ([cookbook](../.claude/skills/asm-perf-cookbook/SKILL.md) §0).
- **Scaling caveat.** On this virtualized hybrid CPU, 1-core runs share
  vCPU 0 with the VM's interrupt work, and throughput grows faster than
  linearly from 1 to 4 cores for every server. Compare servers within one
  core budget, not across budgets.

The standard session below predates the native engine: its KvNode is the
nghttp2 build (at the time, the only engine).

## Results (2026-09-26)

**Setup.** Intel i9-13900K under Docker Desktop / WSL2, revision `8cb0fef`,
711 runs, all valid. Full tables, charts and statistics:
[REPORT.md](results/2026-09-26-standard-monitored/REPORT.md). The machine
was shared: other programs kept 2–7 of its 32 logical CPUs busy
(per-experiment medians, mostly 4–6). Every run records that outside load,
and the report adjusts for it
([Host interference](results/2026-09-26-standard-monitored/REPORT.md#host-interference)).

**GET throughput** (E1): 100% GET, 1M preloaded keys, 100-byte values,
1,024 requests in flight. Medians of 6 fresh-server runs, ops/s:

| Server cores | KvNode | Redis | Valkey | Dragonfly | Garnet | Memcached |
|---|---|---|---|---|---|---|
| 1 | 813 k | 1.45 M | 1.44 M | 790 k | 872 k | 771 k |
| 2 | 1.50 M | 1.57 M | 2.12 M | 1.39 M | 1.97 M | 1.57 M |
| 4 | **3.02 M** | 2.89 M | 3.29 M | 2.29 M | 3.80 M | 3.12 M |
| 8 | **5.18 M** | 2.82 M | 2.40 M | 2.92 M | 7.91 M | 6.22 M |

What the comparison shows. Ratios are KvNode ÷ other, raw medians.
"Clear" means KvNode was ahead (or behind) in all six paired blocks, both
raw and after adjusting to equal outside load. Everything else is an
estimate.

- **Scaling is KvNode's strength.** From 1 to 8 cores it grows 6.4×,
  thanks to thread-per-core workers over a lock-free store. Redis grows
  1.9×: commands run on one thread. Valkey peaks at 4 cores. At 8 cores
  KvNode is clearly ahead of Redis (1.83×), Valkey (2.15×) and Dragonfly
  (1.77×), and clearly behind Garnet (0.66×). Behind Memcached (0.83×) is
  clear only after the load adjustment.
- **Per request, the protocol costs KvNode.** At 1 core it clearly trails
  Redis (0.56×) and Valkey (0.57×) and ties Dragonfly, Garnet and
  Memcached. E5 isolates the cause: a no-op gRPC health check costs 1,086
  ns of server CPU, against 422 ns for a Redis `PING` (2.6×). That is
  HTTP/2 framing, HPACK and gRPC trailers. It caps KvNode at about 920k
  requests/s per core.
- **Without pipelining** (E3, 256 connections × 1 request): 618k/s, clearly
  ahead of Redis (1.40×), Dragonfly (1.32×) and Garnet (1.25×), tied with
  Valkey and Memcached.
- **Latency at identical offered load** (E4, open loop, 3 runs each, so
  estimates). At 574k, 1.15M and 1.72M requests/s:
  - KvNode's p50 is 112, 142 and 163 µs; its p99 is 0.61, 0.68 and 1.17 ms.
  - Only Valkey has consistently lower tails (p99 0.36–0.93 ms).
    Memcached's are lower only at the lightest load.
  - Redis reaches a p99 of 50 ms at the highest load, Dragonfly 3.0 ms, and
    Garnet 97 ms already at 1.15M/s. The highest closed-loop throughput
    does not mean the best latency.
  - Two runs were flagged client-bound: Redis and Dragonfly at 1.72M/s.
- **Round trip at one request in flight** (E7): KvNode 25.5 µs (p50), level
  with Valkey, Garnet and Memcached (24–27 µs). Redis and Dragonfly take
  47–48 µs.
- **Writes are the weak spot** (E2, 100% SET, 4 cores): 1.72M/s, clearly
  behind Redis (0.72×), Dragonfly (0.70×), Valkey (0.96×) and Garnet
  (0.47×), and clearly ahead of Memcached (1.13×). All writes pass one
  mutex (ADR 0003).
- **Mixed reads and writes** (E2, 90% GET): tied with Redis and Valkey,
  clearly ahead of Dragonfly (1.16×), clearly behind Garnet (0.71×) and
  Memcached (0.93×).
- **Large values** (E6, 3 runs each):
  - 1 KiB values: 2.63M GET/s. That is 1.31–1.55× Redis, Valkey and
    Dragonfly, tied with Memcached, 0.78× Garnet.
  - 16 KiB values: 691k GET/s, about 11 GB/s of values. That is 1.38×
    Redis and 2.40× Valkey, but 0.67× Garnet and 0.66× Memcached.
- **Memory** (E1): 178 bytes per key for 116 raw bytes. Redis uses 174,
  Valkey 173, Dragonfly 159, Memcached 196, Garnet 284.
- **Verdict:** KvNode is a strong multi-core GET server with low, stable
  latency. Garnet and Memcached are faster in raw multi-core throughput,
  and Redis and Valkey are cheaper per request on one core. Writes are
  KvNode's weakest area.

**Client validation** (E8): kvbench was at least as fast as each
ecosystem's own tool on the same server:

| Server | Keys | kvbench | Standard tools |
|---|---|---|---|
| Redis | random | 2.91M | redis-benchmark 2.73M, memtier_benchmark 2.73M |
| Redis | one key | 3.45M | redis-benchmark 3.24M |
| KvNode | one key | 3.12M | h2load 2.41M |

It handicaps no system.

## Answers

### Are the measurements stable?

On a quiet host, yes, and this session shows how to tell:

- **Between runs:** S1 ran E1's 4-core cell ten more times per system at
  the end of the session. Between-run CV was **3.2–4.9%**, and S1 ÷ E1
  was 0.97–1.05 for every system: no drift over the five hours.
- **Within runs:** per-second samples vary by a median of 2.4%.
- **Outside load is the main noise source, and it is measured:**
  - Other programs on the PC cost throughput visibly: Spearman ρ = −0.57
    between outside load and run throughput.
  - Each logical CPU of extra outside load on one side shifted a
    KvNode ÷ X ratio by about 2–4%, depending on the experiment. The
    report corrects every closed-loop comparison for it.
  - An earlier session with 14–23 busy CPUs outside the VM showed 3×
    swings ([its README](results/2026-09-26-standard/README.md)). Its
    final, quieter experiment varied by only 1.1–1.7%.
- **Design protections:** blocked randomization, paired sign tests, S1
  and the outside-load monitor are in [METHODOLOGY.md](METHODOLOGY.md)
  §6–§8.

### Is concurrency enabled, how much, and can it be other than 1?

Yes, on both sides, and it is configurable:

- **Server:** KvNode runs one worker thread per core (`KVNODE_WORKERS` =
  N). E0 tested N against N−1 and N won at every budget. Every other
  system also runs its best E0 setting.
- **Client:** kvbench runs 16 threads with 64 connections and 16 requests
  in flight per connection. That is **1,024 concurrent requests** by
  default: HTTP/2 streams for gRPC, pipelined commands for RESP and
  memcached.
- **Other levels:** `--threads`, `--connections` and `--depth` set any
  level from 1 up. E3 runs 256 × 1. E7 sweeps 1, 16, 64, 256 and 1,024 in
  flight. KvNode then goes from 33k to 2.77M GET/s at 4 cores.

### Did KvNode reach 2,000,000 GET/s?

Yes, reading the goal as 2M GET/s at 4 cores on the standard workload:

- **4 cores:** **3.02M GET/s** (6 runs, 2.80–3.23M), and 2.93M in S1's
  ten runs.
- **8 cores:** 5.18M.
- **1 core:** 813k with nghttp2. The native HTTP/2 engine lowered the
  protocol floor from 1,086 to about 520 ns of server CPU (N3) and serves
  1.51M GET/s on one core, ahead of Redis and Valkey (N1, section above).
  A full 2M per core would need ~500 ns per GET including the lookup; three
  quarters of today's ~650 ns is kernel time.
- **4 cores with the native engine:** 5.45M GET/s in the final N2 session,
  7.80M in the quieter one before it.

How KvNode got there (+28% from batched GET prefetching, ablation-tested):
[ADR 0005](../docs/adr/0005-batched-get-prefetch-and-worker-pool.md) and
[the ablation](results/2026-09-26-kvnode-ablation/README.md).

## What is here

| Path | Role |
|---|---|
| [`kvbench/`](kvbench) | The load generator: one C program for RESP, memcached meta and gRPC, with identical threading, batching, timing and byte-exact validation for every system |
| [`suite.py`](suite.py) | What runs: systems (pinned versions, fairness settings), experiments, workloads |
| [`run.py`](run.py) | How it runs: placement, fresh server per run, cgroup accounting, blocked randomized order, resumable raw results |
| [`analyze.py`](analyze.py) | Statistics and `REPORT.md` with charts |
| [`METHODOLOGY.md`](METHODOLOGY.md) | The method, and threats to validity |
| [`results/`](results) | Committed evidence: raw records, environment, report per session |

## Running it

```sh
# images (from the repository root)
docker build --target runtime -t kvnode:0.1.0 .
docker build --target kvbench -t kvnode-kvbench:dev .
docker build --target toolchain -t kvnode-toolchain .            # h2load for E8
docker build -t kvbench/garnet:2.1.8 https://github.com/microsoft/garnet.git#v2.1.8

python benchmarks/run.py --profile standard --out benchmarks/results/<date>-standard
python benchmarks/analyze.py benchmarks/results/<date>-standard   # writes REPORT.md + charts/
```

- `--only E1,S1` runs a subset. `--out` pointing at an existing directory
  resumes: finished runs are skipped.
- `--profile quick` checks the pipeline in minutes. Its numbers are not
  results.
- Keep the machine otherwise idle during a run. Builds and other heavy
  jobs share the CPUs and show up as noise.

kvbench also works on its own, against any server:

```sh
kvbench --protocol resp|memcache|grpc --port P [--threads 16] [--connections 64]
        [--depth 16] [--keys 1000000] [--value-size 100] [--read-ratio 1.0]
        [--zipf 0.99] [--rate OPS] [--load] [--warmup 5] [--duration 10]
```

It prints one JSON document and exits with 0 when every response was
correct, 1 when any was wrong, missing or an error, and 2 on usage errors.

## Experiments

| Id | Question | Cells × repetitions |
|---|---|---|
| E0 | Best thread count per system and core budget: N or N−1? | 5 systems × {2, 4, 8} cores × 2 settings × 3 |
| E1 | Peak GET throughput at 1, 2, 4 and 8 cores; CPU efficiency; memory per key | 6 × 4 × 6 |
| E2 | Mixes at 4 cores: 90/50/0% GET, and 90% GET with Zipf 0.99 | 6 × 4 × 6 |
| E3 | No pipelining: 256 connections × 1 request, 4 cores | 6 × 1 × 6 |
| E4 | Latency at identical offered load (open loop, Poisson), 25/50/75% of the slowest system's peak | 6 × 3 × 3 |
| E5 | Per-request floor: PING / meta no-op / gRPC health check, 1 core | 6 × 1 × 6 |
| E6 | Value size: 1 KiB and 16 KiB values, about 1 GiB of data, 4 cores | 6 × 2 × 3 |
| E7 | Concurrency sweep: 1, 16, 64, 256 and 1,024 requests in flight, 4 cores | 6 × 5 × 3 |
| E8 | Is kvbench as good a client as redis-benchmark, memtier_benchmark and h2load? | 7 tool configurations × 3 |
| N1 | KvNode's native HTTP/2 engine vs nghttp2, Redis and Valkey on 1 core (E1's workload) | 4 × 1 × 6 |
| N2 | The two KvNode engines at 2 and 4 cores | 2 × 2 × 6 |
| N3 | Per-request floor on 1 core: native vs nghttp2 health check, Redis/Valkey `PING` | 4 × 1 × 6 |
| S1 | Stability: E1's 4-core cell ten more times, at the end of the session | 6 × 1 × 10 |

Default workload: 100% GET, 1,000,000 keys (all preloaded), 100-byte
values, uniform keys, 64 connections × 16 requests in flight, kvbench with
16 threads on vCPUs 16–31, servers on vCPUs 0..N−1.
