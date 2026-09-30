# ADR 0003: Thread-per-core workers over one shared lock-free-read store

- **Status:** accepted. The "nghttp2 session" below became the native
  engine's per-connection state (`h2n.asm`, embedded in `Conn`; ADR 0007,
  ADR 0008). It has the same single-thread ownership, so nothing else
  changes. Amended by [ADR 0009](0009-flush-once-per-event-batch.md):
  output is now flushed once per epoll batch, after all of the batch's
  input, rather than per connection. That cuts co-located clients'
  wakeups: +4% to +26% GET/s on one core, depending on how many
  connections each client thread owns.
- **Date:** 2026-09-26

## Context

Requirements: atomic reads and writes; readers must never see a torn value;
no SIMD requirement; must run in a default Docker container.

Two constraints come from the platform:

- An nghttp2 session is single-threaded. Its callbacks must run on the
  thread that feeds it.
- Docker ≥ 25's default seccomp profile blocks `io_uring_setup`, so an
  io_uring server would need a custom profile to run at all.

## Decision

**Networking: thread-per-core, share-nothing.** `KVNODE_WORKERS` threads
(default: the CPUs in the affinity mask). Each worker has its own
`SO_REUSEPORT` listener, `epoll` instance, connections and nghttp2 sessions,
so a connection never changes threads and needs no lock. The kernel spreads
new connections across the listeners. A worker applies backpressure per
connection: it stops reading while output is pending, coalesces output into
one `sendto` per batch, and closes gracefully with a lingering close after
GOAWAY.

**Store: one table shared by all workers.**

- *Readers are lock-free.* Each 64-byte bucket carries a version word used
  as a seqlock (odd means a write is in progress). A Get reads without
  writing shared memory. It validates bucket versions only on a miss, to
  rule out a key that was moving between its two buckets at that instant.
- *Writers are serialized* by one futex mutex (spin, then `FUTEX_WAIT`).
  Records are immutable once published: a Put builds a new record *outside*
  the lock and swaps one pointer inside it. That is why a reader sees the
  old or the new value, never a mix.
- *Memory reclamation uses QSBR* (quiescent-state-based reclamation). Every
  worker announces a quiescent state between event batches and goes
  offline around blocking `epoll_wait`. A replaced or deleted record is
  freed only after every online reader has passed a quiescent state since
  its retirement. Frees are rate-limited and bounded per operation, so
  reclamation never adds a latency spike.

**Hashing:** SipHash-2-4-128 with a 16-byte key drawn from `getrandom` at
startup (h1 selects the bucket, h2 the 8-bit tag). Bucket placement is
unpredictable to clients, which defeats hash-flooding attacks.

## Alternatives rejected

- **Per-worker store shards.** No shared memory at all, but a key would live
  on the shard of whichever worker accepted the connection. Every client
  would then need to reach the right worker, which SO_REUSEPORT does not
  allow, or cross-thread forwarding would be required.
- **Reader-writer lock.** Every read would write the lock word. The resulting
  cache-line ping-pong across cores is the exact cost the seqlock avoids.
- **Fine-grained bucket locks for writers.** Cuckoo displacement touches
  many buckets. Lock ordering over a BFS path is complex, and measured
  write throughput is already about 2 M Puts/s on one hot key
  (README, Performance).
- **io_uring.** Unavailable under Docker's default seccomp profile, and epoll
  is not the bottleneck here.

## Verification

- `test_store_mt`: 8 readers and 4 writers for 4 s, with near-full-table
  relocations and constant reclamation, and zero errors.
- `itest_grpc` concurrency phase: 8 connections through the real server,
  about 180k RPC/s, **0 torn values**. Readers check self-describing values
  written concurrently by other connections.
- .NET `ConcurrentCallsOverOneClientStayConsistent`: 64 tasks multiplexed on
  one HTTP/2 connection, flipping a shared key between two uniform values;
  any torn read would mix letters. Since the client spreads calls over 4
  connections by default (2026-09-30, ADR 0010), it also runs over four,
  where a Put and the Get after it usually travel on different connections.
