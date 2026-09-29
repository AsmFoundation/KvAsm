# ADR 0010: Streaming Upload and Download (write-once records, pinned reads, CRC-32C)

- **Status:** accepted
- **Date:** 2026-09-29
- **Amends:** [ADR 0001](0001-store-data-structure.md) (records gain a
  reserve → fill → publish life), [ADR 0003](0003-concurrency-model.md)
  (reclamation defers pinned records), [ADR 0007](0007-native-http2-engine.md)
  (streamed request bodies, generated response bodies)

## Context

Every call was unary: one request message, one response message. A value
therefore had to fit one message, which capped values at
`KVNODE_MAX_VALUE_BYTES` (1 MiB by default, 64 MiB at most), and each large
value paid for it twice:

- **Put** buffered the whole request body per stream, then copied the value
  into its record: two copies and a transient second buffer the size of the
  value, per call in flight.
- **Get** answered with the whole value as one message. When the client's
  window was short, the response had to wait in a buffer the size of the
  value; and common gRPC clients refuse messages above 4 MiB by default.

The request was explicit: client streaming, server streaming, and dedicated
APIs for streaming bytes — with tests covering network interruptions.

## Decision

Two streaming methods, specified in
[`kvnode.proto`](../../proto/kvnode/v1/kvnode.proto) and
[PROTOCOL.md](../PROTOCOL.md#streaming-upload-and-download):

```proto
rpc Upload(stream UploadRequest) returns (UploadResponse);          // client streaming
rpc Download(DownloadRequest) returns (stream DownloadResponse);    // server streaming
```

### 1. Upload: decode as it arrives, copy once, publish atomically

- The first message declares `key` and `total_size`. The server then
  **reserves the final record** (`kv_store_rec_alloc`), checking the upload
  limit, the store's memory limit, and a **per-connection budget**: declared
  sizes of a connection's uploads in progress may not exceed
  `KVNODE_CONN_BUFFER_MB`. Without the budget, a client could pin memory by
  declaring uploads it never sends.
- Each DATA payload is fed, as it arrives, to a **resumable message-stream
  decoder** (`kv_pb_stream_feed`): 64 bytes of state per stream, resumable
  at every byte, reporting fields as events and handing `data` payloads over
  as pointers into the frame. Each piece is copied **once**, straight from
  the read buffer into the record, and checksummed in the same pass
  (`kv_crc32c_copy`).
- At END_STREAM the server checks whole messages, the exact size, and the
  client's CRC-32C if sent, then **publishes** (`kv_store_publish`): one
  pointer store under the writer mutex, so readers see the old value or the
  new one. Records stay write-once: nothing reader-visible ever changes.
- **Refusals are immediate**: a Trailers-Only error the moment a rule is
  broken, then `RST_STREAM(NO_ERROR)` (RFC 9113 §8.1) so the client stops
  sending. A cancel, a broken connection or the idle timeout frees the
  unpublished record and returns its reservation.

### 2. Download: pin the record, generate the body

- The record is looked up and **pinned** (`kv_store_pin`) before the
  worker's next quiescent state; it stays valid for the whole call however
  writers replace or delete the key — one version per call.
- The response body is **generated** frame by frame (`kv_grpc_gen_fill`)
  directly from the pinned record, as the client's flow-control windows and
  the output buffer allow: `total_size` in the first message, `data` chunks
  (64 KiB by default, 1 MiB at most), CRC-32C of exactly the range in the
  last. A frame boundary may fall anywhere, even inside a message head or
  the CRC tail. No response buffer exists: a slow reader slows the call,
  it never grows the server.

### 3. Pins in reclamation

Pins live in a small open-addressing table beside the store, updated under
the writer mutex (once per Download at each end). Reclamation consults it
**only while a pin exists** — one compare otherwise — and a pinned record
whose grace period ended is marked deferred; the last `kv_store_unpin`
frees it. Its bytes stay counted against `KVNODE_MAX_MEMORY_MB` until then.
Shutdown logs `pinned=` and `deferred=`, and every test phase requires both
to be 0.

### 4. CRC-32C

Castagnoli CRC (`crc32c.asm`): the SSE4.2 `crc32` instruction where CPUID
has it, else slicing-by-8 tables that NASM generates at assembly time;
`KVNODE_CRC32C=auto|sse42|table`. Fused with the copy: ~16 GB/s (SSE4.2),
~3.3 GB/s (tables) on the development machine.

### 5. What the engine and the worker had to learn

- `h2n.asm`: an Upload's DATA goes to the gRPC layer as it arrives (after
  flow-control accounting) instead of into a request buffer; generated
  bodies (`ST_GEN`); `RST_STREAM(NO_ERROR)` after an early complete answer.
- `worker.asm`: **fairness** — one connection's flush writes at most
  `FLUSH_BUDGET` (256 KiB) per pass and keeps EPOLLIN|EPOLLOUT interest when
  output is pending, so a bulk Download cannot monopolize its worker.
- `worker.asm`: **idle timeout** — bytes sent count as activity (a long
  Download whose client never speaks is not cut). A peer that stops reading
  its socket cannot even receive the idle GOAWAY; such a connection is now
  destroyed at the next expiry (`CONN_IDLE_CLOSING`). Before this, it held
  its streams — a pinned record, a reservation — forever.

## Alternatives considered

| Alternative | Why not |
|---|---|
| Chunked unary calls (PutChunk / GetRange) | No atomicity without server-side sessions (chunks of different uploads interleave; readers see halves); one round trip per chunk; every client reimplements the protocol. Streaming calls give one call = one atomic operation. |
| Buffer the upload, then Put | The value twice in memory and a second copy — exactly the cost this replaces. |
| Copy the value into a response buffer for Download | A slow reader would hold a copy of up to 64 MiB per call; the pin costs nothing extra unless the record is replaced meanwhile. |
| Reference counts on every record | Every Get would pay an atomic increment and decrement on a shared line; pins cost only Downloads, and reclamation checks them only while one exists. |
| Resume a broken Download transparently | Without record versions a resumed range may come from a different value. The client reports the failure; resuming by `offset` is the application's call. |
| xxHash / SHA-256 instead of CRC-32C | CRC-32C is hardware-accelerated on x86-64 and Arm64, is what storage systems and gRPC ecosystems already use for transfer integrity (for example Google Cloud Storage), and detects the errors it is for. It is not a MAC; TLS protects against tampering. |
| A separate buffering limit for uploads | Reusing `KVNODE_CONN_BUFFER_MB` keeps one knob for "what one connection may hold"; `KVNODE_MAX_UPLOAD_BYTES` must fit it (checked at startup). |

## Consequences

- Values up to `KVNODE_MAX_UPLOAD_BYTES` (default 64 MiB, at most 2³¹ − 1).
  Get refuses values above `KVNODE_MAX_VALUE_BYTES` with
  `RESOURCE_EXHAUSTED` "read it with Download" instead of building a giant
  message.
- Memory: an upload costs its value once (the record is the buffer); a
  download costs no buffer, but a pinned record replaced meanwhile stays in
  memory until the download ends — budget `KVNODE_MAX_MEMORY_MB` for the
  concurrent downloads of hot, large, frequently replaced values.
- Measured (`itest_grpc --bench-transfer`, one connection over loopback,
  the nghttp2 client doing as little as it can; medians of 7, two runs):

  | Value | unary Put | unary Get | Upload | Download |
  |---|---|---|---|---|
  | 64 KiB | 485–771 MB/s | 1,184–1,257 | 951–1,458 | 1,063–1,557 |
  | 1 MiB | 1,084–1,237 | 4,286–4,476 | 3,141–3,768 | 4,419–5,387 |
  | 16 MiB | — | — | 2,244–2,735 | 4,959–6,196 |
  | 64 MiB | — | — | 4,250–4,523 | 2,484–2,684 |

  Upload moves the same bytes 2–3× faster than Put (one copy instead of
  buffer-then-copy); Download matches or beats Get. The client is the limit
  at the large sizes (the 64 MiB Download is its own buffer growth), so
  these are lower bounds for the server. Server memory: +65,540 KiB with a
  64 MiB value stored; +4 to +140 KiB during and after downloading it.
- New startup settings `KVNODE_MAX_UPLOAD_BYTES`, `KVNODE_CRC32C`; new log
  fields `max_upload_bytes`, `crc32c`, `pinned`, `deferred`.
- **For Export and Watch** ([proposal](../proposals/export-watch.md), §7
  asks for one shared streaming layer). This one already provides: a
  per-stream release run from `stream_free` whatever ends the stream
  (`xfer_release`: reset, connection loss, drain, idle close); per-connection
  flush budgets with EPOLLIN|EPOLLOUT interest; bytes sent as idle-timeout
  activity and the close of peers that stop reading; the route pattern
  (`M_UPLOAD`, `M_DOWNLOAD`); a resumable event decoder for message streams;
  and record pins that reclamation honors at free time, whatever list the
  record comes from. Not yet: bodies of unknown length (`ST_GEN` needs the
  exact length up front, `download_body_len`) and a producer that can say
  "nothing now" (WAIT); a non-OK status in trailers after a body; a worker
  ready list and a per-worker (not only per-connection) budget; an idle
  exemption for streams that are quiet by design. Export and Watch should
  grow `ST_GEN` into that producer interface rather than add a second
  mechanism.

## Verification

- **Unit:** the stream decoder against golden events and a reference model
  (200,000 random streams, random piece boundaries, guard pages); CRC-32C
  vectors and SSE4.2 / tables / bitwise agreement; write-once records and
  pins; pinned readers re-validated across quiescent states while writers
  churn (hundreds of thousands of pins, 0 corrupted).
- **End to end** (`make itest`, nghttp2's client, its own CRC): every
  refusal with its exact status; a 469-transfer matrix of sizes × chunkings
  × checksum placements; the request split at every byte and in 1–7-byte
  frames; cancels at every stage; ranges parsed message by message; 1-byte
  windows; one version per download while the key is replaced, deleted and
  its memory reused; an 8-connection race with torn-value detection; drain
  mid-transfer; the idle timeout against stalled, slow and socket-frozen
  transfers; memory, budget and capacity limits and their release;
  fairness; fuzzing with Upload and Download soups; conformance vectors
  (PROTOCOL.md) byte for byte.
- **Mutation checks** (each reverted): reclamation ignoring pins → the
  consistency test reads reused memory and `--stream-limits` crashes the
  server; an upload budget never returned → `--stream-limits` fails; the
  stuck-connection path disabled → `--stall` keeps a deleted 8 MiB record
  pinned for the whole 8 s freeze; `FLUSH_BUDGET` removed → worst Get
  latency beside bulk downloads rises from 0.2–0.6 ms to ~3 ms.
- **.NET client** (`UploadAsync`, `DownloadAsync`, `DownloadToAsync`):
  CRC-verified at both ends; tests through a fault-injecting TCP proxy —
  resets mid-upload and mid-download, hung paths, slow links, refused
  connections, deadlines — and a memory-limited server. They found that a
  connection reset mid-upload surfaced as a raw `IOException` from
  grpc-dotnet; the client now reports every transport failure as
  `RpcException`.
