# Benchmark methodology

The goal is a comparison a skeptic can check. Every rule below is enforced
by code: [`suite.py`](suite.py) (what runs), [`run.py`](run.py) (how it runs),
[`kvbench/`](kvbench) (the client), and [`analyze.py`](analyze.py) (the
statistics).

## 1. Comparison class

Included are **single-node, in-memory key-value servers** that answer
exact-key GET/SET over the network, run without persistence. That is
KvNode's class.

| System | Why it is included |
|---|---|
| Redis 8.10.2 | The reference in-memory KV server. |
| Valkey 9.1.2 | The Linux Foundation fork of Redis, with a reworked I/O threading model. |
| Dragonfly 2.0.0 | Multi-threaded shared-nothing design, Redis-compatible (closest to KvNode's thread-per-core model). |
| Garnet 2.1.8 | Microsoft Research's multi-threaded Redis-compatible server (.NET). |
| Memcached 1.6.45 | The classic multi-threaded cache, with the simplest protocol. |

Excluded:

- **KeyDB**: its last maintained release is from 2023.
- **etcd, TiKV, FoundationDB**: replicated, durable consensus stores; every
  write pays for Raft and fsync. Putting them beside in-memory servers would
  measure durability, not engineering.
- **Embedded engines** (RocksDB, LMDB): no network server.

**Version rule:** use the latest stable release on the test date, pinned
exactly. No release candidates: Valkey 9.2 was only 9.2.0-rc1, so 9.1.2 is
used. No nightlies: Garnet's `latest` image is an unreleased `main` build.
Garnet's newest published *image* (2.1.0) lagged its newest release (2.1.8),
so 2.1.8 is built from **Microsoft's own Dockerfile at the release tag**.
Every result records the image ID and digest that produced it.

## 2. Testbed and placement

- The testbed hardware is recorded in each run's `environment.json`. The
  published run used an Intel i9-13900K (8 P-cores with SMT + 16 E-cores)
  under Docker Desktop 27.3.1 on WSL2 (kernel 5.15), cgroup v1.
- **Server:** `--cpuset-cpus 0..N-1` for a budget of N cores. **Client:**
  vCPUs 16–31 with 16 threads. The two never share a vCPU.
- **Network:** the client joins the server container's network namespace
  (`--network container:`) and connects to 127.0.0.1. That is the same
  loopback path for every system, with no bridge or NAT in the way.
- **A fresh server for every repetition.** No state (memory layout,
  fragmentation, JIT profile) carries from one run into the next.

## 3. One client for everyone: kvbench

A benchmark measures client and server together. If KvNode were measured
with h2load and Redis with redis-benchmark, part of every difference would
be tool quality. [`kvbench`](kvbench) is a single C program that drives
every system with:

- the same threads (one epoll loop each), connections and in-flight depth.
  Pipelining for RESP and memcached and concurrent HTTP/2 streams for gRPC
  are the same thing seen from the load: *depth* requests outstanding per
  connection;
- the same batching: each loop iteration sends one `send()` per connection;
- the same workload generator (uniform, or YCSB's scrambled Zipfian);
- the same timing: a monotonic clock, and HDR-style log-linear histograms
  with ≤ 1.6% relative error, merged across threads;
- **full response validation.** Every GET value is compared byte for byte
  (length, the key id in its first 8 bytes, the rest against a template).
  Any error, miss or wrong value marks the run invalid.

Only the wire encoding differs, because it is part of each system: RESP2
for Redis, Valkey, Dragonfly and Garnet; the memcached **meta** protocol
(its current recommended protocol, with the most compact replies); gRPC
over HTTP/2 for KvNode, via nghttp2's client with large flow-control
windows, as an efficient production client would use.

**Validation (E8).** kvbench must be at least as efficient as each
ecosystem's standard tool on the same server and workload: redis-benchmark
and memtier_benchmark for Redis, h2load for KvNode. If it is slower than a
standard tool for one system only, the comparison is biased and the report
says so.

## 4. Workloads

- **Keys:** 16 bytes, `key:%012d`, exactly redis-benchmark's `__rand_int__`
  format. The key space is 1,000,000 unless stated. **Every key is
  preloaded** before measuring, so a miss is an error, never a cheaper
  answer.
- **Values:** 100 bytes by default; 1 KiB and 16 KiB in E6. In E6 the key
  count shrinks so that each dataset is about 1 GiB.
- **Distribution:** uniform, or scrambled Zipfian θ = 0.99 (YCSB's default
  skew).
- **Load disciplines:**
  - *Closed loop* (throughput): every connection keeps exactly `depth`
    requests in flight. Latency then reflects the queue depth, by
    Little's law, so it is reported but not compared.
  - *Open loop* (E4): Poisson arrivals at a fixed total rate. Latency is
    measured from each request's **intended** send time, so a server
    stall shows up as latency instead of being hidden by a waiting client
    (coordinated omission). Offered loads are 25%, 50% and 75% of the
    *slowest* system's E1 peak, so every system is compared at identical,
    sustainable load.

## 5. Fair server configuration

| Rule | Implementation |
|---|---|
| Same core budget | `--cpuset-cpus` with N vCPUs for every server |
| Use the cores given | Thread count tuned per system and N in **E0**: N vs N−1 (Redis and Valkey document leaving a core spare; others recommend one thread per core). The median winner is used by every later experiment. Garnet sizes its .NET thread pool from the cpuset; it has no equivalent knob. |
| No persistence | Redis/Valkey `--save "" --appendonly no`, Dragonfly `--dbfilename=`, Garnet AOF off (default), KvNode has none |
| No eviction | Memcached `-m` sized with 4× headroom; other servers unlimited |
| No TLS, no auth | Everywhere (KvNode is h2c) |
| Best I/O engine | Dragonfly uses io_uring: this host's Docker runs seccomp-unconfined. KvNode uses epoll by design. |

## 6. Measurement protocol

- Each run: start the server → kvbench waits for the port → preload all
  keys → **warm-up** (5 s; 4 s in E0) → **measure** (10 s; 8 s in E0) →
  remove the server. The warm-up also covers JIT tiering, which matters
  for Garnet.
- **Repetitions:** 5 per cell (3 in E0, E4, E6 and E8), each with a fresh
  server.
- **Order:** the runs of each experiment execute in a seeded random order
  (seed recorded), so drift over time (thermals, background activity) hits
  every system alike.
- **Resource accounting** is done from the outside:
  - *Server CPU* is read from the server container's cgroup exactly at the
    measurement window's boundaries.
  - *Memory* is the cgroup's anonymous memory (`rss` in v1, `anon` in v2):
    what an in-memory store actually holds. No self-reported statistics
    are used, because each system counts differently.
- **Instrument calibration.** On this virtualized host the cgroup CPU clock
  runs about 6–7% fast: a busy loop on 4 vCPUs reads 4.25 CPU-seconds per
  second. `run.py` measures the factor at the start of every session, and
  server CPU figures are divided by it.
- **Client-bound check.** A run where the client was more than 85% busy
  while the server used less than 90% of its budget is flagged in the
  report.

## 7. Statistics

With 3–5 repetitions, normal-theory confidence intervals would pretend to
precision the data does not have. The analysis therefore reports:

- per cell, the **median** and the **min–max range**;
- KvNode against each system:
  - the **ratio of medians**;
  - a **conservative ratio range**, [min(KvNode)/max(other),
    max(KvNode)/min(other)], which contains every pairwise run ratio;
  - the **exact two-sided Mann–Whitney U p-value**, by full enumeration.
    The smallest possible p is 0.0079 at 5 vs 5 and 0.1 at 3 vs 3.
- A difference counts as **clear** only if the ratio range excludes 1 and
  p < 0.05; otherwise the report says "no clear difference". With n = 3,
  nothing can be called clear by this rule, and E0, E4, E6 and E8 are read
  as estimates.
- Invalid and failed runs are excluded from statistics but listed, with
  their reasons, in every report.

## 8. Threats to validity

- **Virtualization.** WSL2's vCPUs are scheduled by Hyper-V onto hybrid P-
  and E-cores. Pinning fixes vCPUs, not physical cores, so absolute numbers
  carry noise that bare metal would not. Repetition, randomized order and
  the ranges reported make the noise visible. Treat absolute numbers as
  indicative and ratios as the result.
- **One machine, loopback.** There is no NIC, so no interrupt or packet
  cost of real networks. Protocol overhead weighs relatively more than it
  would across a network, which penalizes heavier protocols (gRPC over
  HTTP/2) the most. E5 measures that overhead directly.
- **Protocol is part of the system.** KvNode's gRPC framing costs more
  bytes and CPU per request than RESP, on both server and client. That
  cost is real for users and is not factored out.
- **Configuration expertise.** Each system runs its documented defaults,
  plus the fairness rules above and the E0 thread tuning. Deeper tuning
  (kernel, NUMA, allocator) is out of scope and would apply to every
  system alike.
- **Workload coverage.** The workloads are small-object GET/SET, the
  operations KvNode offers. Richer data types, where Redis-compatible
  servers do much more, are out of scope by construction.

## 9. Reproducing

```sh
docker build --target runtime -t kvnode:0.1.0 .
docker build --target kvbench -t kvnode-kvbench:dev .
docker build --target toolchain -t kvnode-toolchain .
docker build -t kvbench/garnet:2.1.8 https://github.com/microsoft/garnet.git#v2.1.8
python benchmarks/run.py --profile standard     # ≈ 3 hours; resumable with --out
python benchmarks/analyze.py benchmarks/results/<directory>
```

`--profile quick` runs the same pipeline in about 15 minutes as a smoke
test. Its numbers are not results.
