# Operating KvNode

## Configuration

Every setting is an environment variable. Numeric values are strict decimal
integers: no sign, no spaces, no units. Named choices (`KVNODE_SCAN`,
`KVNODE_CRC32C`) must be one of the listed lowercase words exactly.
Out-of-range, malformed or unknown values stop startup with exit code **2**
and a log line naming the variable, the value and what is allowed. A typo
never silently falls back to a default.

| Variable | Range | Default | Meaning |
|---|---|---|---|
| `KVNODE_PORT` | 1–65535 | 50051 | h2c listen port (all interfaces) |
| `KVNODE_WORKERS` | 0–256 | 0 | event-loop threads; 0 = CPUs in the affinity mask. This honors `--cpuset-cpus` but not a `--cpus` quota, so set it explicitly under quotas |
| `KVNODE_CAPACITY` | 1–2³⁶ | 1000000 | maximum number of keys; sizes the table once, resident from startup (see [Memory](#memory)) |
| `KVNODE_MAX_KEY_BYTES` | 1–65536 | 1024 | longest accepted key |
| `KVNODE_MAX_VALUE_BYTES` | 0–67108864 | 1048576 | largest value Put accepts and Get returns (larger ones only through Upload/Download) |
| `KVNODE_MAX_UPLOAD_BYTES` | 0–2147483647 | 67108864 | largest value Upload stores; must not exceed `KVNODE_CONN_BUFFER_MB` (checked: exit 2 — such an upload could never fit one connection's budget) |
| `KVNODE_MAX_MEMORY_MB` | 0–16777216 | 0 | cap on record memory, uploads in progress and records pinned by downloads included; 0 = unlimited |
| `KVNODE_MAX_STREAMS` | 1–1000 | 100 | concurrent requests per connection (HTTP/2 SETTINGS) |
| `KVNODE_MAX_CONNECTIONS` | 1–1000000 | 10000 | connections per worker; beyond it, accepting pauses |
| `KVNODE_IDLE_TIMEOUT_SEC` | 0–86400 | 300 | connections with no byte received or sent get GOAWAY and are closed; one whose peer does not even read that is dropped at the next expiry; 0 = never |
| `KVNODE_SHUTDOWN_GRACE_SEC` | 0–3600 | 10 | time in-flight requests get after SIGTERM |
| `KVNODE_CONN_BUFFER_MB` | 1–4096 | 64 | request bytes one connection may buffer (the excess stream is reset with `ENHANCE_YOUR_CALM`), and the total declared size of one connection's uploads in progress (the excess upload is refused, `RESOURCE_EXHAUSTED`) |
| `KVNODE_CRC32C` | `auto` \| `vpclmul` \| `pclmul` \| `sse42` \| `table` | `auto` | CRC-32C of Upload/Download: `auto` takes the fastest path the CPU has — `vpclmul` (256-bit carry-less folding, AVX2 + VPCLMULQDQ: Ice Lake, Zen 3 and later; ~88 GB/s fused with a 16 KiB copy in cache), `pclmul` (128-bit folding, any x86-64 since 2010; ~45 GB/s), `sse42` (the `crc32` instruction, one dependency chain; ~15 GB/s), `table` (slicing-by-8; ~3.2 GB/s). Forcing a mode the CPU lacks refuses to start (exit 2). All give identical checksums |
| `KVNODE_SNAPSHOT` | path | unset | snapshot to load at startup (see Snapshots) |
| `KVNODE_SCAN` | `swar` \| `sse2` | `sse2` | how lookups match tags. `sse2` matches both candidate buckets in one compare: 2–6% faster per batched lookup, and SSE2 is part of every x86-64 CPU. `swar` is the scalar path. Results are identical ([ADR 0006](adr/0006-store-memory-levels-simd-and-private-caches.md)) |
| `KVNODE_CACHE_L1_BITS` | 0–24 | 0 | per-worker private L1 record cache of 2^bits entries (32 bytes each); 0 = off. Measured slower than the CPU's own caches except for tiny hot sets, so off by default (ADR 0006) |
| `KVNODE_CACHE_L2_BITS` | 0–24 | 0 | the same for a second, larger private level |

`KVNODE_H2_ENGINE` no longer exists. HTTP/2 is always KvNode's own engine
(`h2n.asm`, h2spec 146/146), and nghttp2 was removed from the server
([ADR 0008](adr/0008-remove-nghttp2-from-the-server.md)). Like any variable
KvNode does not read, a leftover `KVNODE_H2_ENGINE` is ignored.

## Exit codes and signals

| Exit | Meaning |
|---|---|
| 0 | clean shutdown after SIGTERM/SIGINT |
| 1 | runtime or startup failure: port already in use, snapshot rejected, out of memory, cannot bind |
| 2 | invalid configuration |

On **SIGTERM** or **SIGINT**, listeners close and every connection gets
GOAWAY. In-flight requests finish — an Upload still sending, a Download
still streaming — and health checks answer `NOT_SERVING`. The process exits
0 within `KVNODE_SHUTDOWN_GRACE_SEC`: size the grace period for your largest
transfers, since what is still open then is cut. Signals are consumed
with `sigwaitinfo`, so this works when kvnode runs as PID 1 (the normal case
in a container). `docker stop` needs no `--init`.

**Port ownership.** Workers bind with `SO_REUSEPORT`. Before that, kvnode
probes the port *without* it and exits 1 with
`msg="port already in use, refusing to share it"` if anything holds the port.
Without the probe, a second instance on the same host network would silently
take a share of the connections and serve them from its own, different store.

## Logs

One line per event on stderr, written with a single `write(2)`, so lines from
different threads never interleave:

```
ts=1790373017.207 level=INFO msg="starting" version=0.1.0 port=50051 workers=2 capacity=1000000 ... max_upload_bytes=67108864 crc32c=vpclmul snapshot=none
ts=1790373017.208 level=INFO msg="listening" transport=h2c port=50051 workers=2
ts=1790373019.575 level=INFO msg="shutdown requested" signal=15
ts=1790373019.575 level=INFO msg="stopped" connections=2 requests=1 errors=0 keys=0 pinned=0 deferred=0
```

`ts` is Unix time with milliseconds. Errors carry a `reason`, and `record`
or `errno` where one applies. Failure lines use `level=ERROR`. The starting
line reports the CRC-32C implementation in force
(`crc32c=vpclmul|pclmul|sse42|table`).
`pinned` and `deferred` in the stopped line count Download pins still held,
and replaced records still waiting for one, after every connection closed:
both are always 0 — anything else is a leak worth a bug report.

## Health checks

The server implements the standard `grpc.health.v1.Health/Check`, so
orchestrators probe it natively. The image is `FROM scratch` and has no probe
binary to run as a Docker `HEALTHCHECK`. Kubernetes:

```yaml
readinessProbe:
  grpc: { port: 50051 }          # service "" => SERVING, NOT_SERVING while draining
  periodSeconds: 5
livenessProbe:
  grpc: { port: 50051 }
  periodSeconds: 10
  failureThreshold: 3
terminationGracePeriodSeconds: 15   # > KVNODE_SHUTDOWN_GRACE_SEC
```

## Snapshots

A snapshot is the bulk data source: `KVNSNAP1` format (docs/PROTOCOL.md),
written for example by the .NET `SnapshotFile.WriteAsync`, which replaces
the file atomically. Mount it read-only:

```sh
docker run -p 50051:50051 -v "$PWD/data:/data:ro" -e KVNODE_SNAPSHOT=/data/seed.kvs kvnode:0.1.0
```

The whole file is verified before any record is stored: size, magic,
version, trailer, and checksum. Then every record is checked against the
data bounds and the key/value limits. Any problem exits 1 with the reason,
for example `reason="checksum mismatch"` or
`reason="value too large" record=42`. Serving a silently truncated dataset
would be worse than not starting. Duplicate keys are fine; the last one
wins. `KVNODE_CAPACITY` must cover the number of distinct keys.

The loaded data is served exactly as if it had been Put: it can then be
modified through gRPC. The server never writes the file back. After a
restart it serves the snapshot again, not the later changes.

## Memory

Measured resident memory (`/proc/PID/smaps_rollup`; `VmRSS` in
`/proc/PID/status` can be stale). The method and raw output are in
`benchmarks/results/2026-09-26-profile-memory`.

| Part | Resident | Grows with |
|---|---|---|
| Process baseline: code, glibc, stacks, heap | ~0.7 MiB (a server with capacity 1,000 is 740 KiB in total, 536 KiB of it the binary's code pages, which the page cache shares) | nothing |
| Store table | 64 bytes × the power of two ≥ ⌈capacity / 5⌉: **16 MiB for the default 1,000,000 keys**, 128 MiB for 10M, 2 GiB for 100M (12.8–25.6 bytes per key of capacity) | `KVNODE_CAPACITY`, resident from startup |
| Per worker, idle | ~14 KiB (32 workers add 444 KiB) | `KVNODE_WORKERS` |
| Per key | `24 + key rounded up to 8 + value` bytes, in malloc chunks (16-byte steps, 8-byte header): **162 bytes** for a 16-byte key and a 100-byte value, 66 for a 16-byte value, 1,083 for a 1 KiB value | keys × value size |
| Per idle connection | ~9.1 KiB: the 848-byte connection record (HTTP/2 state embedded), the first touched page of its 64 KiB output buffer, a 4 KiB HPACK entry ring and a 16-slot stream map from the worker's pool | connections |
| Per busy connection | plus the output buffer pages a response batch touches (at most ~80 KiB: production stops at 64 KiB) and, per request in flight, a 256-byte stream block and its request buffer (none when the body arrives in one frame) | load |

- **Worked example:** the default capacity with 1,000,000 keys of 100 bytes
  under the 64-connection benchmark load: 16 MiB table + 155 MiB records +
  ~1 MiB baseline and connections = **174 MiB** (measured 174,260 KiB at
  peak, 178 bytes per key all included). The published comparison (E1):
  Redis 174, Valkey 173, Dragonfly 159, Memcached 196, Garnet 284 bytes
  per key for the same data.
- **The table is resident from startup.** With transparent huge pages
  enabled (`always` or `madvise`), every whole 2 MiB region is faulted in
  at startup as one huge page, so every lookup hits the TLB; without THP it
  is touched lazily as keys arrive. Size `KVNODE_CAPACITY` to the keys you
  expect: an empty 100M-key table costs 2 GiB.
- **Records:** `KVNODE_MAX_MEMORY_MB` caps the sum of their exact sizes
  (the malloc overhead above is not counted). Replaced and deleted records
  stay counted until reclamation frees them, typically within milliseconds;
  glibc keeps freed record memory for later Puts.
- **Uploads** reserve their whole declared size when the first message
  arrives — counted against `KVNODE_MAX_MEMORY_MB` and against the
  connection's `KVNODE_CONN_BUFFER_MB` — and fill it as data comes; the
  record *is* the buffer, so an upload costs its value once, never twice.
  A cancelled, refused, reset or timed-out upload gives it all back at once.
- **Downloads** cost no buffer: the response is generated frame by frame
  from the stored record as the client's window opens (a slow client slows
  its download, it does not grow the server). But a download **pins** its
  record: replaced or deleted meanwhile, it stays in memory (and counted)
  until the download ends. Long downloads of hot, often-replaced large
  values therefore hold one extra version each; budget
  `KVNODE_MAX_MEMORY_MB` for the concurrent downloads you allow.
- **Stalled transfers** end with the idle timeout: a peer that sends and
  receives nothing for `KVNODE_IDLE_TIMEOUT_SEC` (default 300 s) gets
  GOAWAY and loses its connection, and with it its reservations and pins.
  A peer that stopped reading its socket altogether cannot even receive the
  GOAWAY; it is dropped at the next expiry (about 2 s later).
- **Connections:** buffered request bytes are bounded per connection by
  `KVNODE_CONN_BUFFER_MB`; the peer's HPACK entries add at most 4 KiB. The
  worker's pool keeps its chunks until the worker exits, so its size
  follows the peak of concurrent connections and streams, not the traffic:
  after 1,000 connections closed, 4.2 MiB of pool stayed for reuse.
- **Kernel memory** is outside the process: an idle loopback TCP socket
  held about 2 KiB (`/proc/net/sockstat`); a busy one holds its queued data.
- A Put that finds the table full (capacity, or a failed cuckoo
  displacement near the load limit) or the memory cap reached gets
  `RESOURCE_EXHAUSTED`. Existing data is unaffected.

## Security notes

- **Plaintext.** KvNode speaks h2c. Put it behind a TLS-terminating proxy
  or a service mesh whenever traffic leaves a trusted network. It has no
  authentication: restrict who can reach the port.
- **Hash flooding** is prevented: bucket placement uses SipHash under a
  per-process secret from `getrandom`, and clients cannot influence it
  (ADR 0004).
- **Resource limits:** key and value sizes, request size, upload size,
  per-connection buffering and upload budget (a client cannot pin more
  memory than one connection's budget by declaring uploads it never sends),
  streams per connection, connections per worker, idle timeout, and the
  HTTP/2 engine's flood defenses. The engine limits rapid stream
  resets to a burst of 1,000 then 33 per second (CVE-2023-44487), caps a
  header block at 64 KiB and 64 CONTINUATION frames, and never buffers an
  oversized frame (it is rejected from its header). Every build fuzzes it
  and runs h2spec against it.
- **Container:** the image contains only the static binary. It runs as
  uid/gid 10001 and needs no capabilities and no writable file system:
  `--read-only --cap-drop ALL --security-opt no-new-privileges` all work.
