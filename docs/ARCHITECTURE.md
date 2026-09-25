# KvNode architecture

KvNode is a gRPC key-value server written in x86-64 assembly (NASM). It
links two C libraries: glibc and nghttp2 (ADR 0002). This page shows how the
parts fit together. The ADRs record why each choice was made.

```mermaid
flowchart LR
    C[gRPC clients<br/>.NET, others] -- "h2c :50051" --> K{{"kernel<br/>SO_REUSEPORT"}}
    K --> W1 & W2 & Wn
    subgraph P["kvnode process (static-PIE, FROM scratch)"]
        M["main thread<br/>config → port probe → store → snapshot<br/>→ workers → sigwaitinfo"]
        subgraph W1["worker 0"]
            E1[epoll loop] --> N1[nghttp2 sessions] --> G1[gRPC dispatch]
        end
        subgraph W2["worker 1"]
            E2[epoll loop] --> N2[nghttp2 sessions] --> G2[gRPC dispatch]
        end
        subgraph Wn["worker n"]
            En[epoll loop] --> Nn[nghttp2 sessions] --> Gn[gRPC dispatch]
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
2. **Read.** The worker's `epoll` loop reads up to 64 KiB and hands it to
   `nghttp2_session_mem_recv2`. nghttp2 parses frames, applies HPACK and
   flow control, enforces stream limits, and invokes callbacks
   (`grpc.asm`).
3. **Route.** Headers decide the route: `:path` picks
   `/kvnode.v1.KvNode/{Get,Put,Delete}` or `/grpc.health.v1.Health/Check`,
   and `:method`, `content-type` and `grpc-encoding` are checked. The body
   is buffered per stream within two budgets: the per-request
   `max_request` and the per-connection `KVNODE_CONN_BUFFER_MB`.
4. **Execute.** At END_STREAM the worker validates the 5-byte gRPC framing,
   decodes the protobuf (zero-copy, `proto.asm`), checks limits, hashes the
   key (SipHash-2-4-128, secret key) and runs the store operation. Store
   operations are sub-microsecond, so the request finishes inside the
   callback and there is no async state.
5. **Respond.** HEADERS, then DATA (the message, via a data provider that
   honors the peer's flow-control window), then trailer HEADERS
   (`grpc-status: 0`). Errors are a single Trailers-Only HEADERS frame.
   All output produced by one input batch is coalesced into one `sendto`.

## Store (ADR 0001, ADR 0003)

- **Table:** 2^k buckets × 64 bytes. Each bucket holds a version word, six
  8-bit tags and six record pointers. It is sized once from
  `KVNODE_CAPACITY` at up to about 83% load and backed by `mmap`
  (`MAP_NORESERVE`, transparent huge pages when available).
- **Lookup:** tag = the top byte of h2; bucket 1 comes from h1; bucket 2 =
  bucket 1 XOR a hash of the tag. The six tags are compared in one SWAR
  step, and the full key is compared exactly on a tag hit. At most two
  cache lines are touched.
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
| `server/src/grpc.asm` | nghttp2 callbacks, routing, validation, responses, health service |
| `server/src/proto.asm` | protobuf decode (zero-copy, skips unknown fields) and varint encode |
| `server/src/store.asm` | table lifecycle, Put/Delete, memory accounting, QSBR |
| `server/src/cuckoo.asm` | lock-free lookup, insert with BFS displacement, remove |
| `server/src/siphash.asm`, `util.asm` | SipHash-2-4-128; exact key compare and SWAR tag match |
| `server/src/wlock.asm` | futex writer mutex |
| `server/src/snapshot.asm` | snapshot verification and loading |
| `server/src/log.asm` | one-`write` structured log lines |
| `server/src/abi.asm` | struct layouts exported for the ABI tests |
| `client/dotnet` | .NET client, snapshot writer, tests (Testcontainers) |

## Tests

| Layer | What | Where |
|---|---|---|
| unit (C over assembly) | ABI layouts, the nghttp2 mirror, SipHash vectors, key compare with guard pages, protobuf, store model and torture, snapshot parsing and every rejection reason | `make -C server test` |
| bake-off | cuckoo vs chaining vs trie, every answer verified | `make -C server bench` |
| end to end | real server over loopback: protocol and limits, pipelining, cancel, torn-value detection, drain with an in-flight request, idle timeout, snapshot source, startup refusals, h2spec baseline | `make -C server itest` |
| client | 64 SipHash vectors, compaction, byte-exact snapshot vs the server tool, and the real container via Testcontainers | `dotnet test --solution client/dotnet/KvNode.Client.slnx` |

The Docker build runs the unit, bake-off and end-to-end layers. An image
exists only if all of them pass.
