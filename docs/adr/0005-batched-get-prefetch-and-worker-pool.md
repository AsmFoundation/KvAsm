# ADR 0005: Batched GETs with two-stage prefetch, and a per-worker allocator

- **Status:** accepted; amended twice (see [Amendments](#amendments)):
  - pass 1 now prefetches three record lines;
  - the stream-lookup cache (§3) left with nghttp2 (ADR 0008).
  
  The nghttp2 context below is history.
- **Date:** 2026-09-26

## Context

The goal was **2,000,000 GET/s at 4 cores** on the standard benchmark
workload: 100% GET, 1,000,000 keys, 100-byte values, 64 connections × 16
requests in flight. One core cannot reach it with nghttp2. For 0.1.0, the
benchmark's no-op floor (E5, a gRPC health check) was about 1.5 µs of
server CPU per round trip, so a core peaked around 650k requests/s before
touching any data.

KvNode 0.1.0 reached about 1.6–1.8M GET/s at 4 cores. A `perf` profile
(cpu-clock, 1 core, the workload above) showed where the time went:

- **Kernel: 31% of all CPU** (socket reads and writes, epoll).
- **User time:**
  - nghttp2 about 35% (framing, HPACK, stream bookkeeping);
  - cuckoo lookups about 25%, almost all of it cache misses: the
    instruction loading a bucket's tag word was the hottest in the
    process;
  - `malloc`/`free` about 12%, from nghttp2's per-stream allocations and
    KvNode's own Stream objects.

With 1M keys the table and records far exceed the last-level cache, so
nearly every GET takes two dependent misses: the bucket, then the record.
Each request ran to completion before the next, so those misses stalled
the core one after another. Meanwhile one 64 KiB read typically carries
dozens of independent GETs.

## Decision

### 1. Batch GETs per read and prefetch in two stages

- **Parse time:** when a GET's request is complete (END_STREAM), the
  worker validates it, decodes the key and hashes it, exactly as before.
  It then prefetches the key's two candidate buckets
  (`kv_store_prefetch`) and **appends the stream to the connection's
  batch** instead of executing it.
- **After the read:** once `nghttp2_session_mem_recv2` has consumed the
  whole read, `kv_grpc_run_batch` makes two passes:
  1. For every batched GET, match the tag in both buckets, which have
     arrived by now, and prefetch the candidate record's first two lines
     (`kv_store_prefetch_records`).
  2. Detach the batch, then look up each GET and submit its response.
     Bucket and record lines are warm.
- **Effect:** a read's cache misses overlap in the core's fill buffers
  instead of serializing, i.e. memory-level parallelism.
- **Unchanged:** Puts, Deletes and health checks still execute at END_STREAM.
- **Reset in the same read:** if the peer resets a batched stream before
  the batch runs, `on_stream_close` marks it `ST_CLOSED` and the batch
  frees it without answering. The batch owns its streams until then.

### 2. Give each worker its own allocator

- **`pool.asm`**: seven size classes from 64 to 4096 bytes. Each class has
  a LIFO free list, so a freed block is reused while still in cache.
  Blocks are bump-carved from 256 KiB chunks; larger requests fall through
  to `malloc`.
- nghttp2 uses the pool through its `nghttp2_mem` hook
  (`nghttp2_session_server_new3`), and KvNode's Streams and buffers use it
  as well.
- A session never leaves its worker (ADR 0003), so the pool needs no
  locks. All its chunks are freed when the worker exits.

### 3. Cache the last stream lookup

Header, DATA-chunk, frame and close callbacks each name a stream by id, and
nghttp2 resolves ids through a hash map. A one-entry cache per connection
(`stream_lookup`) answers repeated lookups for the same stream. It is safe
because stream ids are never reused on a connection, and the entry is
cleared whenever its Stream is freed.

## Measurements

Ablation with the four builds interleaved within each repetition, 8
repetitions with Latin-square rotation of their order. Evidence, scripts
and a second fixed-order run:
[benchmarks/results/2026-09-26-kvnode-ablation](../../benchmarks/results/2026-09-26-kvnode-ablation/README.md).

| Cores | 0.1.0 | + stream cache | + pool | + batch & prefetch |
|---|---|---|---|---|
| 1 | 361k | 348k, paired 0.99 | 384k, paired 1.16 | **459k, paired 1.35, faster 8/8** |
| 4 | 1.84M | 1.43M, paired 0.98 | 1.97M, paired 1.10 | **2.13M, paired 1.28, faster 8/8** |

Medians of 8 runs. "Paired" is the median over repetitions of each build
÷ the 0.1.0 build of the same repetition.

- **Batch & prefetch is the decisive change:** faster than the baseline
  in every repetition (sign test p = 0.008), +28% at 4 cores. It reaches
  the 2M target: 2.13M median.
- **The pool is probably positive but not conclusive** on this host:
  faster in 6 of 8 repetitions.
- **The stream cache is neutral:** nghttp2's own internal lookups
  dominate.
- **The standard benchmark confirms the target**
  ([benchmarks/](../../benchmarks/README.md), 2026-09-26, 6 fresh-server
  runs per cell, outside host load measured):
  - 3.02M GET/s at 4 cores (range 2.80–3.23M) and 5.18M at 8 cores;
  - S1, ten more runs at 4 cores at the end of the session: 2.93M.
- **The no-op floor fell too:** from 1,516 ns (0.1.0, a single
  quick-profile run) to 1,086 ns of server CPU per health check (median of
  6), about 920k requests/s per core. A health check touches no store, so
  batching cannot explain it. The pool is the likely cause, but this was
  not isolated by a controlled test.
- These numbers come from a shared, noisy host: WSL2 on a hybrid P/E-core
  CPU, with other heavy programs running at the time. The benchmark
  session right after measured 14–23 of its 32 logical CPUs busy outside
  the container VM. The between-run CV was 12–29%, which is why only
  paired ratios are quoted, and why the absolute numbers are lower bounds
  for an idle machine. The comparison with other stores is in
  [benchmarks/](../../benchmarks/).

## Consequences

- **Order of concurrent requests within one read.** A Put that arrives
  after a GET in the same read now executes before that GET is answered.
  This is allowed: both requests were in flight at once, so they are
  concurrent. Each one still takes effect atomically at a single point
  between its arrival and its response, so the store stays linearizable.
  gRPC promises no order between concurrent calls. A client that needs
  read-then-write order must wait for the read's response, as it already
  had to. GETs among themselves are answered in arrival order.
- **Latency.** A GET is answered once the rest of its read has been
  parsed, not immediately. That delay is bounded by one 64 KiB read (256
  KiB since 2026-09-30: `READ_CHUNK` was raised for streaming uploads,
  GET throughput and latency measured unchanged — ADR 0010), and
  a read that carries a single request sees no change. E3 (one request in
  flight per connection) and E4 (open-loop latency) measure the effect.
- **Memory.** Batched Streams live until the end of their read, which
  `KVNODE_MAX_STREAMS` already bounds per connection.
- **Safe reclamation.** The batch runs inside the same event-loop pass,
  before the worker announces a quiescent state. Record pointers read in
  pass 2 are therefore covered by QSBR (ADR 0003). Pass 1 only prefetches,
  and a prefetch never faults, even on a stale address.

## Alternatives deferred or rejected

- **Prefetch and execute immediately.** A bucket needs roughly 100 ns to
  arrive, far longer than the gap before its own lookup. Without deferral
  there is nothing to overlap with.
- **Batch across all connections of an epoll wake-up.** More
  memory-level parallelism, but one connection's responses would wait on
  other connections' parsing. Deferred until a per-connection batch is
  shown to be too small.
- **Replace nghttp2 with a purpose-built HTTP/2 engine.** It is the only
  path well past about 920k requests/s per core (the floor after this
  change), because the protocol layer is the floor. But it is a security-critical rewrite that needs
  fuzzing and conformance work, and it would revise ADR 0002. Deferred.
- **Slab records on huge pages, SipHash-1-3, SIMD tag matching.** Smaller
  expected gains; listed with their status in the performance cookbook
  (`.claude/skills/asm-perf-cookbook`).

## Verification

- `test_pool`: a 60,000-step randomized model check against a reference
  allocator, plus alignment, LIFO reuse, calloc overflow, realloc edge cases
  and sizes near 2^64.
- `itest_grpc` `test_reset_in_batch`: 32 GETs arrive in one read and the
  odd 16 are reset before the server answers. It expects 16 correct
  values, 16 CANCEL closes and a healthy connection afterwards.
- All existing layers still pass: unit, bake-off, end to end including
  pipelining, cancellation, torn-value detection and h2spec at its
  baseline. The Docker build runs all of them.

## Amendments

### Three record lines in pass 1 (2026-09-26)

A kernel-inclusive profile of one worker explained why glibc `memmove` still
took 4.2% of the core with a busy-polling client
(`benchmarks/results/2026-09-26-profile-memory`).

- **The cause:** a record is a 24-byte header, the key padded to 8, then
  the value, and it starts on any 16-byte boundary. With a 16-byte key and
  the benchmark's 100-byte value it is 140 bytes, so it always spans three
  cache lines. Pass 1 prefetched two, and the value copy then waited on
  DRAM for the third.
- **The change:** `kv_cuckoo_prefetch_records` now prefetches
  `[rec + 128]` as well. That covers a whole record of up to 144 bytes at
  every alignment. Result: `memmove` fell to 1.05% of the core.
- **Throughput, measured concurrently:** A and B at the same time on two
  pinned vCPUs, swapped every block, 6 blocks. A sequential design showed a
  position effect larger than the effect itself; `ab-sequential-aborted.out`
  records it. Results, B/A:
  - blocking client, 100-byte values: 1.001× [0.992–1.010];
  - busy-polling client: 1.025× [0.958–1.071], 4 of 6 blocks;
  - 16-byte values, where the third line is outside the record: 0.993×
    [0.968–1.015].
- **Verdict:** within noise everywhere. It is kept for the stall it
  removes (🧪 in the cookbook), not for a proven gain.

### The stream-lookup cache is gone (ADR 0008)

§3's one-entry cache served nghttp2's id-based callbacks. It was
measured neutral, and it left the server with nghttp2. The native engine
resolves ids through its own stream map, in one load for consecutive
client ids.
