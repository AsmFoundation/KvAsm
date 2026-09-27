# KvNode architecture

KvNode is a gRPC key-value server written in x86-64 assembly (NASM). It
links one C library, glibc. HTTP/2 is KvNode's own engine, `h2n.asm` with
the HPACK decoder `hpack.asm` (ADR 0007). nghttp2, the library it replaced,
was removed from the server (ADR 0008) and survives only as the independent
client the tests and benchmarks check the server with. This page shows how
the parts fit together. The ADRs record why each choice was made.

```mermaid
flowchart LR
    C[gRPC clients<br/>.NET, others] -- "h2c :50051" --> K{{"kernel<br/>SO_REUSEPORT"}}
    K --> W1 & W2 & Wn
    subgraph P["kvnode process (static-PIE, FROM scratch)"]
        M["main thread<br/>config → port probe → store → snapshot<br/>→ workers → sigwaitinfo"]
        subgraph W1["worker 0"]
            E1[epoll loop] --> N1["HTTP/2 engine<br/>h2n.asm + hpack.asm"] --> G1[gRPC dispatch]
        end
        subgraph W2["worker 1"]
            E2[epoll loop] --> N2["HTTP/2 engine<br/>h2n.asm + hpack.asm"] --> G2[gRPC dispatch]
        end
        subgraph Wn["worker n"]
            En[epoll loop] --> Nn["HTTP/2 engine<br/>h2n.asm + hpack.asm"] --> Gn[gRPC dispatch]
        end
        G1 & G2 & Gn --> S[("store<br/>cuckoo buckets + records<br/>lock-free reads · QSBR")]
        M --> S
    end
    F[/"snapshot file<br/>KVNSNAP1"/] -. startup .-> M
```

## Request path

1. **Accept.** The kernel hashes a new connection to one worker's
   `SO_REUSEPORT` listener. The connection stays on that thread for life,
   so connection state needs no locks.
2. **Read.** The worker's `epoll` loop hands the connection to the HTTP/2
   engine (`h2n.asm`), which reads up to 64 KiB, parses frames, applies
   HPACK and flow control and enforces stream limits. Frames are parsed in
   place in the read buffer, and a partial frame is carried to the next
   read. Header blocks are decoded by `hpack.asm`, whose per-field
   *meanings* (computed once per distinct field) replace string
   comparisons; a block byte-identical to the connection's previous one
   under the same HPACK table replays that decode instead. Streams live in
   an open-addressing map, and every RFC 9113 state and error rule is
   applied (h2spec 146/146).
3. **Route.** Headers decide the route: `:path` picks
   `/kvnode.v1.KvNode/{Get,Put,Delete}` or `/grpc.health.v1.Health/Check`,
   and `:method`, `content-type` and `grpc-encoding` are checked. The body
   is buffered per stream within two budgets: the per-request
   `max_request` and the per-connection `KVNODE_CONN_BUFFER_MB`; a body
   that arrives whole in one DATA frame is used in place instead.
4. **Execute.** At END_STREAM the worker validates the 5-byte gRPC framing,
   decodes the protobuf (zero-copy, `proto.asm`), checks limits and hashes
   the key (SipHash-2-4-128, secret key).
   - Puts, Deletes and health checks execute right there.
   - **GETs are deferred:** their two candidate buckets are prefetched
     immediately, and the stream joins the connection's batch.
   - When the whole read has been parsed, `kv_grpc_run_batch` runs two
     passes over the batch: prefetch the first three cache lines of each
     candidate record, then look up and answer. The cache misses of up to a
     whole read's GETs thus overlap instead of stalling one after another
     (ADR 0005: +28% at 4 cores and +35% at 1 core, paired medians; the
     third line covers a whole record of up to 144 bytes, so the value copy
     no longer waits on DRAM).
   - A GET the peer resets within the same read is freed, not answered.
5. **Respond.** HEADERS, then DATA (the message, within the peer's
   flow-control windows), then trailer HEADERS (`grpc-status: 0`). Errors
   are a single Trailers-Only HEADERS frame.
   - Responses are framed straight into the output buffer, with
     precomputed header blocks: after a connection's first response,
     HEADERS carry 2 bytes and the trailers 1 byte. A GET hit is copied
     from the store record into its DATA frame, with no response buffer and
     no allocation.
   - **Nothing is sent until the whole epoll batch has been read**
     (ADR 0009). The worker processes every ready connection first, then
     flushes them all back to back, one `sendto` each. A client thread with
     several connections is woken once per batch instead of once per
     connection. Measured on one core: +26% GET/s with 8 connections per
     client thread, +12% with 4, +4% with 1, and lower p50 and p99 in every
     case.

The loop is event-driven and never spins. A connection is a state machine
resumed by epoll readiness (what C# async/await compiles to, without heap
continuations or thread hand-offs), and an idle worker sleeps in
`epoll_wait`. Measured: 4 workers holding 1,000 idle connections used 10 ms
of CPU in 10 s.

Allocation on this path goes through the worker's own pool (`pool.asm`):
streams, stream maps, HPACK rings and entries, and every request buffer. A
connection never leaves its worker thread, so the pool needs no locks.
Freed blocks are reused LIFO while still cached. Memory use, measured, is
in [OPERATIONS.md](OPERATIONS.md#memory).

## Store (ADR 0001, ADR 0003)

- **Table:** 2^k buckets × 64 bytes. Each bucket holds a version word, six
  8-bit tags and six record pointers. It is sized once from
  `KVNODE_CAPACITY` at up to about 83% load and backed by `mmap`
  (`MAP_NORESERVE`, `MADV_HUGEPAGE`). Each whole 2 MiB region is faulted in
  with a write at startup: left to the lookups, a region's first touch was a
  read that mapped the huge zero page, and the first insert split it into
  4 KiB pages (measured: no huge page at all in a full 16 MiB table).
- **Lookup:** tag = the top byte of h2; bucket 1 comes from h1; bucket 2 =
  bucket 1 XOR a hash of the tag. By default one SSE2 compare matches the
  tags of both buckets (12 slots); `KVNODE_SCAN=swar` compares one bucket's
  six tags per 64-bit SWAR step instead. The full key is compared exactly
  on a tag hit. At most two cache lines are touched (ADR 0006).
- **Private caches (optional, off by default):** each worker can keep a
  two-level, direct-mapped cache of record pointers
  (`KVNODE_CACHE_L1_BITS`/`L2_BITS`). It is validated against per-key
  invalidation generations that writers bump before retiring a record.
  Measured slower than the CPU's own caches except for tiny hot sets
  (ADR 0006).
- **Insert:** use a free slot in either bucket, or else a bounded BFS (at
  most 256 nodes) finds a displacement path. Moves go backwards, copy first
  and clear second, with bucket versions odd, so a concurrent reader either
  sees the key or retries.
- **Records** are immutable: a 24-byte header, the key padded to 8 bytes,
  then the value. Put allocates and fills the new record outside the writer
  lock and publishes it with one pointer store.
- **Reclamation:** old records go to a limbo list stamped with a global
  epoch. They are freed once every online worker has announced a quiescent
  state since then (QSBR). A worker announces between event batches and is
  offline while it blocks in `epoll_wait`.

## Lifecycle

- **Startup** fails fast, in order: settings (exit 2 on any invalid value),
  port probe (refuses to join another process's `SO_REUSEPORT` group),
  SipHash key from `getrandom`, store, snapshot (exit 1 on any integrity
  error), then every listener bound before any worker starts.
- **Shutdown** begins on SIGTERM/SIGINT (`sigwaitinfo` on the main thread,
  which works as PID 1). Workers close their listeners and send GOAWAY with
  the last processed stream id. In-flight requests complete; health checks
  answer `NOT_SERVING` from then on. Idle connections close with a
  lingering close. Anything still open after `KVNODE_SHUTDOWN_GRACE_SEC` is
  closed. The process exits 0.

## Source map

| Path | Role |
|---|---|
| `server/src/main.asm` | process entry: startup order, signals, shutdown, final stats |
| `server/src/config.asm` | strict environment-variable settings |
| `server/src/worker.asm` | epoll event loop, accept, backpressure, idle timeout, drain, port probe |
| `server/src/h2n.asm` | native HTTP/2 engine: frames, stream states and map, flow control, precomputed responses, flood defenses |
| `server/src/hpack.asm` | HPACK decoder (RFC 7541): Huffman FSM, dynamic table, per-field meanings |
| `server/include/huffman.inc` | the Huffman decoding table, generated and self-verified by `server/tools/gen_huffman.py` |
| `server/src/grpc.asm` | request logic: body budgets, validation, dispatch, batched GET execution, health service |
| `server/src/pool.asm` | per-worker size-class allocator (malloc-shaped, no locks) |
| `server/src/proto.asm` | protobuf decode (zero-copy, skips unknown fields) and varint encode |
| `server/src/store.asm` | table lifecycle, Put/Delete, memory accounting, QSBR |
| `server/src/cuckoo.asm` | lock-free lookup (SWAR or SSE2 tag scan), insert with BFS displacement, remove, prefetch stages |
| `server/src/hcache.asm` | optional per-worker private L1/L2 record cache, generation-validated |
| `server/src/siphash.asm`, `util.asm` | SipHash-2-4-128; exact key compare and SWAR tag match |
| `server/src/wlock.asm` | futex writer mutex |
| `server/src/snapshot.asm` | snapshot verification and loading |
| `server/src/log.asm` | one-`write` structured log lines |
| `server/src/abi.asm` | struct layouts exported for the ABI tests |
| `client/dotnet` | .NET client, snapshot writer, tests (Testcontainers) |

## Tests

| Layer | What | Where |
|---|---|---|
| unit (C over assembly) | ABI layouts, SipHash vectors, key compare with guard pages, protobuf, store model and torture, snapshot parsing and every rejection reason, HPACK (RFC vectors and 30,000 blocks differential against nghttp2 as the oracle) | `make -C server test` |
| bake-off | cuckoo vs chaining vs trie, every answer verified | `make -C server bench` |
| store levels | lookup variants per memory level (SWAR/SSE2, private caches), every answer verified | `make -C server bench-store` |
| end to end | real server over loopback, driven by an independent HTTP/2 client (nghttp2's client API): protocol and limits, pipelining, cancel, torn-value detection, drain with an in-flight request, idle timeout, snapshot source, startup refusals, h2spec 146/146 (empty baseline), and fuzzing of the HTTP/2 input path followed by the full suite on the same server | `make -C server itest` |
| client | 64 SipHash vectors, compaction, byte-exact snapshot vs the server tool, and the real container via Testcontainers | `dotnet test --solution client/dotnet/KvNode.Client.slnx` |

The Docker build runs the unit, bake-off and end-to-end layers. An image
exists only if all of them pass.
