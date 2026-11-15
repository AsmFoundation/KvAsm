# ADR 0007: A native HTTP/2 engine in assembly (h2n.asm), nghttp2 kept as the alternative

- **Status:** accepted; amended by
  [ADR 0008](0008-remove-nghttp2-from-the-server.md). nghttp2 is no longer
  "kept as the alternative": the server links no HTTP/2 library, and
  `KVNODE_H2_ENGINE` is gone. So is everything below that names the
  nghttp2 engine as selectable, the engine-neutral `session` parameter, or
  the n-phases (`make itest` phases 1-7 now all run this engine; the h2spec
  baseline is `h2spec-known-failures.txt`, empty). The engine itself, its
  verification and the measurements are unchanged. Amended again on
  2026-11-15 (see [Amendments](#amendments)): replies of any length are
  framed in place, and the steady-state response comes from a template.
- **Date:** 2026-09-26
- **Amends:** [ADR 0002](0002-http2-library-nghttp2.md) (nghttp2 becomes one of two engines)

## Context

The request: "exceed performance on 1 core — this is why we are on
assembly." On one core KvNode served 813k GET/s against Redis's 1.45M and
Valkey's 1.44M (E1, `benchmarks/results/2026-09-26-standard-monitored`).

The profile says where the time goes. With `perf` (cpu-clock) and the
`/proc` user/kernel split, KvNode with 1 worker under E1's load spends
1,427 ns of server time per request:

| Part | ns per request |
|---|---|
| kernel (read, sendto, epoll, loopback TCP) | ~485 |
| **nghttp2** | **~672** |
| KvNode assembly (parse, store, respond; incl. ~75-115 ns of prefetch stalls) | ~222 |
| libc (memcpy, malloc remnants) | ~47 |

Redis, profiled the same way: 720 ns per request, about half user and half
kernel. strace shows that both servers make the same calls: one read and
one write per connection event, 16 requests per read. So the gap is almost
entirely user space, and mostly nghttp2. Its hottest symbols were
`nghttp2_hd_inflate_hd_nv` (7.0%), `nghttp2_hd_huff_decode` (6.4%),
`nghttp2_session_mem_recv2` (4.8%), `nghttp2_map_find` (4.7%) and
`hd_deflate` (4.0%). This is generic machinery that a unary gRPC server
does not need: priority trees and outbound queues, a hash map for stream
ids, reference-counted buffers per header, data-provider callbacks, and a
full encoder for response headers that never change.

The store and the SIMD work of ADR 0006 cannot close this gap; they act
on the ~222 ns share. Only the HTTP/2 layer can.

ADR 0002 rejected writing HTTP/2 in assembly as putting "the riskiest,
most attack-exposed code in the least reviewable language". That risk is
real. This ADR accepts it for a measured gain, and pays for it with the
verification listed below. nghttp2 stays available: one environment
variable switches back.

## Decision

Add `server/src/h2n.asm`, a server-side HTTP/2 engine built for exactly
what KvNode serves. Select the engine per process with
`KVNODE_H2_ENGINE=nghttp2|native`. Both engines share everything above the
transport in `grpc.asm`:

- request-body buffering and its budgets (`kv_grpc_stream_append`);
- validation, routing and every gRPC status;
- store operations, and the prefetched GET batch.

`dispatch` sees `session = 0` on native connections and answers through
h2n's reply functions instead of `nghttp2_submit_*`. The rules exist once;
only the framing differs.

### Input

- **In place.** Frames are parsed directly in the worker's read buffer.
  A partial frame (at most 16,393 bytes, because larger frames are
  rejected on their header) is carried to the front of the next read.
- **HPACK** is decoded by `hpack.asm` (commit ec83e0e). Each field arrives
  with a *meaning* computed once per distinct field: at startup for the
  static table, and on insertion for dynamic entries. So an indexed field
  costs a table lookup, not a string comparison. Huffman strings are
  decoded 4 bits per step through a generated FSM into a per-worker
  scratch buffer.
- **A header-block cache** makes repeated calls skip HPACK entirely.
  gRPC clients send byte-identical blocks for identical calls once their
  table is warm. `HpackDec.gen` changes whenever the dynamic table may
  change, so a request block equal to the connection's cached block under
  the same generation replays the recorded effects of its decode:
  - stream flags, route, HEAD bit, content-length;
  - accepted or malformed.
  Only blocks whose own decode left the table unchanged are recorded.
- **Streams** live in an open-addressing map keyed by (id >> 1) & mask.
  Client ids are odd and increasing, so concurrent streams fill
  consecutive slots and a lookup is one load. Deletion uses backward
  shifting, never tombstones. The map starts at 16 slots and doubles at
  load 1/2.
- **Stream states** (RFC 9113 §5.1) are enforced exactly: idle, open,
  half-closed (remote), closed.
  - Ids above `last_id` are idle.
  - Ids at or below it that are not in the map are closed. Two 32-entry
    rings tell the cases apart: streams we reset (their late frames are
    ignored, as §5.1 requires) and streams closed normally or by the peer
    (STREAM_CLOSED).
  - An id that was never opened gets PROTOCOL_ERROR (§5.1.1).

### Output

Responses are framed straight into the connection's output buffer. The
response header blocks are precomputed:

- The first response on a connection inserts `content-type:
  application/grpc`, and its trailers insert `grpc-status: 0`, into the
  peer's HPACK table.
- Every later response is `88 BF` (2 bytes) for HEADERS and `BE` (1 byte)
  for the trailers.
- If the peer shrinks SETTINGS_HEADER_TABLE_SIZE below 4,096, the next
  block starts with a size update to 0, and literals are used from then
  on (RFC 7541 §4.2).

A GET hit whose message fits the windows and one frame is written in a
single pass: HEADERS, DATA with the value copied straight from the store
record, and trailers. There is no response buffer, no allocation and no
second copy. On the input side, a request body that arrives whole in one
DATA frame with END_STREAM is used in place in the read buffer
(`ST_BORROWED`): dispatch and the GET batch both finish before the next
read reuses it. Anything else is copied into the stream's buffer and
framed as flow control allows:

- every DATA frame is bounded by the connection window, the stream window
  (both signed), the peer's maximum frame size, and the output high-water
  mark;
- blocked streams wait in a FIFO list that the worker's flush pumps after
  WINDOW_UPDATE, SETTINGS or drained output.

### Protocol coverage and hardening

- **Preface and SETTINGS:** the connection preface and the first-SETTINGS
  rule.
- **Frame rules:** frame-size limits; every per-frame-type rule (stream 0
  vs non-0, lengths, padding, ACK payloads); CONTINUATION sequencing
  (nothing may interleave).
- **Streams:** stream-id ordering; MAX_CONCURRENT_STREAMS (REFUSED_STREAM,
  retryable).
- **Flow control:** both directions, including SETTINGS_INITIAL_WINDOW_SIZE
  deltas and 2^31-1 overflow checks.
- **Priority:** PRIORITY validation (self-dependency, length).
- **Malformed requests (§8):** uppercase or invalid names;
  connection-specific fields; `te` other than `trailers`; bad value bytes;
  pseudo-header order, duplicates, unknown and response pseudo-headers;
  missing or empty `:method`, `:scheme` or `:path`; content-length that
  does not parse or does not match.
- **Hardening:**
  - rapid reset: a 1,000-token budget refilled at 33 per second
    (CVE-2023-44487);
  - header blocks are capped at 64 KiB and 64 CONTINUATION frames
    (CVE-2024-27316 class);
  - output stays bounded by the high-water mark and the worker's
    half-duplex back-pressure.
- **Error scope:** stream errors reset one stream. Connection errors queue
  GOAWAY and close once it is sent. GOAWAY is also used for drain and for
  the idle timeout, exactly as with nghttp2.

## Verification

- **h2spec 2.6.0: 146/146**, with an empty baseline
  (`server/tests/integration/h2spec-known-failures-native.txt`).
  nghttp2 1.70 passes 138. Its eight failures are closed-stream tracking
  and PRIORITY validation, which nghttp2 removed in 1.65; they stay
  documented in its own baseline.
- **HPACK** (`test_hpack`): RFC 7541 C.2-C.6 (decoded by nghttp2 too),
  every Huffman padding rule, malformed blocks, an eviction
  use-after-free regression, and 30,000 random header lists encoded by
  nghttp2's deflater (194,660 fields) that must decode identically.
- **End to end** (`make itest`, phases 1n-7n): the full gRPC suite on the
  native engine. It covers:
  - CRUD and 1 MiB values across flow-control windows;
  - every HTTP and gRPC error path;
  - 300 pipelined calls against a 100-stream limit;
  - cancellation mid-body;
  - resets inside a GET batch;
  - health checks;
  - 8-connection torn-value detection;
  - graceful drain with an in-flight request, and the idle timeout.
- **Fuzzing** (`fuzz_h2`, phase 7n): garbage, mutated requests, random
  frame soup, random HPACK blocks, truncation, and requests split into
  random writes. Liveness is checked every 50 connections, memory growth is
  bounded, and then the full suite must pass on the same server, which must
  still exit 0.
- **An independent client:** the .NET client's suite (Grpc.Net.Client,
  whose HTTP/2 stack shares nothing with nghttp2) passes 190/190 on net8.0
  and net10.0 against the image with the native engine as default
  (`KVNODE_REQUIRE_DOCKER=1`, so nothing can skip).
- **A soak:** 20,000 fuzz connections (seed 7) left the server alive, with
  resident memory 988 → 1,080 KiB, and the full suite passing afterwards.
- The Docker build runs all of it except the .NET suite. No image exists
  unless the native engine passes.

## Measurements

The benchmark suite gained three experiments. KvNode appears as two
systems of the same image, `kvnode-native` and `kvnode-nghttp2`, with the
engine named explicitly:

- **N1:** 1 core, E1's workload, against Redis and Valkey;
- **N2:** both engines at 2 and 4 cores;
- **N3:** E5's no-op round trip, 1 core.

Method as always: fresh server per run, 6 blocks, rotated order, paired
ratios with an exact sign test (6/6 blocks agreeing gives p = 0.031),
outside load recorded per run.

**Final session** (`benchmarks/results/2026-09-26-native-final`, this
decision's revision, outside load a median of 7-8 CPUs, 72/72 runs valid):

| | native | ÷ Redis | ÷ Valkey | ÷ nghttp2 |
|---|---|---|---|---|
| N1 GET/s, 1 core | **1.51M** | **1.19×** [1.10–1.35] | **1.22×** [1.14–1.34] | 2.03× |
| N1 server CPU per GET | 665 ns | 0.84× | 0.82× | 0.49× |
| N1 p50 latency | 586 µs | 0.79× | 0.77× | 0.46× |
| N2 GET/s, 2 / 4 cores | 2.56M / 5.45M | — | — | 1.96× / 2.07× |
| N3 no-op round trips/s, 1 core | 1.93M | 1.00× (tie) | 1.07× | 2.40× |

**How the engine got there** (three sessions, each with a README):

- **v1**, the first complete engine: 1.79× nghttp2 on 1 core, but only
  0.82× Redis on the no-op floor. Its N1 ran under 27 CPUs of outside
  load, which inflates the GET ratios against Redis and Valkey (their
  sequential misses suffer more), so it is not quoted.
- **Profile of v1** (perf, 1 core): user ~484 ns per request.
  - Huffman decoding of `:path` was ~23% of it, ~113 ns. nghttp2-based
    clients never index `:path`, so it arrives as a Huffman literal on
    every request.
  - HPACK field handling added ~12%.
- **v2** added the header-block cache and O(1) stream-map removal. The
  HTTP/2 layer fell to ~18 ns per request. On the quiet host:
  - N1 1.61M GET/s: 1.14× Redis, 1.18× Valkey, 2.05× nghttp2 (weakest
    block against Redis 1.00×);
  - 4 cores 7.80M (3.06× nghttp2);
  - the no-op floor level with Redis.
- **Final** added zero-copy request bodies, an inline head copy and
  huge-page backing for the store's table. The weakest N1 block against
  Redis is now 1.10×.

**Where a request's time goes now** (1 core, ~620-650 ns): ~75-79% kernel.
strace shows the minimum possible pattern (one `read` and one `sendto` per
16 requests, ~64 connections per `epoll_wait`), and that work is loopback
TCP, which every server pays. User time is ~140 ns, dominated by the
store's DRAM latency, already overlapped by batched prefetching. The next
levers are listed in the cookbook (§0, §1): a huge-page record slab (glibc
arenas got no huge pages here), fewer bytes per response, and io_uring
where seccomp allows it.

**Correctness cost of the speed:** none observed.

- The header-block cache is exact by construction: equal bytes over an
  equal table (HpackDec.gen) decode to equal fields, and it only records
  blocks whose own decode left the table unchanged.
- Borrowed request bodies never outlive the read buffer: dispatch and the
  GET batch both finish within the read that carried them.
- All of it runs under h2spec, the fuzzer and the .NET suite.

## Consequences

- KvNode owns its HTTP/2 now: protocol bugs are its bugs. The mitigations
  are the verification above, running in every build. nghttp2 remains one
  variable away (`KVNODE_H2_ENGINE=nghttp2`) as a fallback and as a
  conformance and performance reference.
- Scope stays deliberately narrow:
  - server side only, h2c only (TLS is terminated in front of KvNode, as
    before);
  - no server push and no priority scheduling (both deprecated by
    RFC 9113);
  - a fixed 4,096-byte HPACK table;
  - responses are always unary.
  A feature outside that scope means extending h2n.asm, or selecting
  nghttp2.
- `grpc.asm` is engine-neutral: one validation and dispatch path, so a gRPC
  rule change applies to both engines at once.
- A connection's engine state is embedded in `Conn` (no extra allocation
  or pointer chase). nghttp2 connections carry those ~500 bytes unused.

## Alternatives rejected

- **Tune nghttp2** (options, custom memory, no automatic window updates):
  the allocator was already replaced (ADR 0005). The remaining cost is in
  its structure (queues, maps, rcbufs, callbacks), which options do not
  remove.
- **A different C library** (e.g. h2o's HTTP/2 core): same class of
  generic machinery, a new dependency, and still C behind callbacks.
- **Precomputed responses on top of nghttp2** (`nghttp2_submit_*` with
  cached nv arrays): the encoder and the outbound queue still run for
  every response. Only replacing the engine removes them.

## Amendments

### Replies longer than a frame, an early send, and the response template (2026-11-15)

Three changes to how `reply_fast` frames a unary response. Evidence:
`benchmarks/results/2026-11-15-asm-audit`.

**1. A message longer than one frame is still framed in place.** The text
above says a GET hit is written in a single pass when its message "fits the
windows and one frame". The one-frame condition turned out to exclude a
common size: a 16,384-byte value makes a 16,395-byte gRPC message, 11 bytes
over the default frame, so every such GET was buffered — a `malloc` of the
message, a copy into it, the pend list, a second copy into the output
buffer and a `free`. Measured on 0.2.0: `reply_slow` ran once per request,
the value was copied twice, and glibc trimmed and regrew its heap around
the 16 KiB blocks (1.9 minor page faults per GET).

- A message of any length is now split over as many DATA frames as the
  peer's frame size requires and copied once, straight from the store
  record, as long as both flow-control windows admit it whole.
- A message whose head (the gRPC prefix and protobuf field headers) is not
  small keeps the buffered path. No caller produces one.

**2. Queued output is sent before a reply that would pass the high-water
mark.** The second condition was "the output buffer is below 64 KiB". With
16 requests of 16,000 bytes in flight a read produces 262 KiB of replies,
so every reply after the first four was buffered the same way.

- When the queued output plus the next reply would exceed `OUT_HIGH_WATER`,
  `out_try_drain` hands the queue to the socket at once and the reply is
  framed into the emptied buffer.
- A reply longer than the mark is sent as it is framed: each time the
  queue reaches the mark, between two of its DATA frames, it goes to the
  socket.
- If the socket takes less than everything, the reply is buffered as
  before — or, when that happens in the middle of a long reply, only what
  is left of its body is copied and queued. No further attempt is made
  during that read: a slow reader costs one refused `sendto` per read, not
  one per reply.
- Replies already waiting on the pend list keep their turn: with any
  queued, nothing is sent early.
- **The bound on the output buffer is unchanged:** the mark plus one
  frame. Memory per connection is as documented in `docs/OPERATIONS.md`.
- **A long reply holds its worker** for as long as copying it to the
  socket takes, about 0.2 ms per MiB: before, the worker spent a similar
  time copying the value into a buffer. Values of tens of MiB belong to
  Download, which yields every 256 KiB.
- This is an exception to ADR 0009 ("nothing is sent until the whole batch
  has been read"); that record's amendment explains why it costs nothing
  there.

**3. The steady-state response comes from one template.** After a
connection's first response both response fields are in the peer's table,
and every unary response is the same 30 bytes around the message:
`HEADERS(88 BF)`, a DATA header, `HEADERS(BE)`. They are written with
constant stores instead of through `put_response_headers`, `put_tsu`,
`put_hdr` (three times), `put_trailers` and `out_reserve`. Any other
encoder state, a pending table size update, or a buffer that must grow
takes the general path.

**Measured:**

- One worker with its clients saturating it, paired A/B against the
  version before (quads of A B B A, 3 of 3 quads each; the host was
  shared with another benchmark):
  - 16,384-byte values: 1.84× the GETs per second at 0.54× the server
    CPU per GET (5.7–6.8 µs against 10.8–12.4 µs);
  - 100,000-byte values: 1.62× at 0.62×;
  - 1,048,576-byte values: 1.58× at 0.63× (about 250 µs per GET against
    395 µs).
  - On a quieter host, the step before "sent as it is framed" measured
    2.02× at 0.50× with 16,384-byte values (3 of 3 quads).
- Instructions per request (callgrind): a GET of a 16,384-byte value
  35,061 → 17,924; response framing of a small GET 187 → 82.
- The request path in process (no kernel): the template takes 6.2% off a
  GET and 16.1% off a health check. The bytes on the wire are identical
  (equal digests of 32,000 responses).

**Verified:** the whole suite, h2spec 146/146 and the fuzzer; and
`test_get_bursts` (`itest_grpc.c`): bursts of Gets at every frame-count
boundary (16,374 / 16,375, 32,757, 49,141, 65,525 / 65,526, 262,133 /
262,134 bytes) up to the largest value, read at full speed; the same
bursts against a reader that stops reading and then drains slowly, with
16 KiB values (refused between two replies) and 300,000-byte values
(refused in the middle of one); and against the default 65,535-byte
windows.
