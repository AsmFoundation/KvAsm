# Operating KvNode

## Configuration

Every setting is an environment variable. Numeric values are strict decimal
integers: no sign, no spaces, no units. Named choices (`KVNODE_SCAN`) must
be one of the listed lowercase words exactly.
Out-of-range, malformed or unknown values stop startup with exit code **2**
and a log line naming the variable, the value and what is allowed. A typo
never silently falls back to a default.

| Variable | Range | Default | Meaning |
|---|---|---|---|
| `KVNODE_PORT` | 1–65535 | 50051 | h2c listen port (all interfaces) |
| `KVNODE_WORKERS` | 0–256 | 0 | event-loop threads; 0 = CPUs in the affinity mask. This honors `--cpuset-cpus` but not a `--cpus` quota, so set it explicitly under quotas |
| `KVNODE_CAPACITY` | 1–2³⁶ | 1000000 | maximum number of keys; sizes the table once, resident from startup (see [Memory](#memory)) |
| `KVNODE_MAX_KEY_BYTES` | 1–65536 | 1024 | longest accepted key |
| `KVNODE_MAX_VALUE_BYTES` | 0–67108864 | 1048576 | largest accepted value |
| `KVNODE_MAX_MEMORY_MB` | 0–16777216 | 0 | cap on record memory; 0 = unlimited |
| `KVNODE_MAX_STREAMS` | 1–1000 | 100 | concurrent requests per connection (HTTP/2 SETTINGS) |
| `KVNODE_MAX_CONNECTIONS` | 1–1000000 | 10000 | connections per worker; beyond it, accepting pauses |
| `KVNODE_IDLE_TIMEOUT_SEC` | 0–86400 | 300 | idle connections get GOAWAY and are closed; 0 = never |
| `KVNODE_SHUTDOWN_GRACE_SEC` | 0–3600 | 10 | time in-flight requests get after SIGTERM |
| `KVNODE_CONN_BUFFER_MB` | 1–4096 | 64 | request bytes one connection may buffer; the excess stream is reset with `ENHANCE_YOUR_CALM` |
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
GOAWAY. In-flight requests finish, and health checks answer `NOT_SERVING`.
The process exits 0 within `KVNODE_SHUTDOWN_GRACE_SEC`. Signals are consumed
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
ts=1790373017.207 level=INFO msg="starting" version=0.1.0 port=50051 workers=2 capacity=1000000 ...
ts=1790373017.208 level=INFO msg="listening" transport=h2c port=50051 workers=2
ts=1790373019.575 level=INFO msg="shutdown requested" signal=15
ts=1790373019.575 level=INFO msg="stopped" connections=2 requests=1 errors=0 keys=0
```

`ts` is Unix time with milliseconds. Errors carry a `reason`, and `record`
or `errno` where one applies. Failure lines use `level=ERROR`.

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
- **Resource limits:** key and value sizes, request size, per-connection
  buffering, streams per connection, connections per worker, idle timeout,
  and the HTTP/2 engine's flood defenses. The engine limits rapid stream
  resets to a burst of 1,000 then 33 per second (CVE-2023-44487), caps a
  header block at 64 KiB and 64 CONTINUATION frames, and never buffers an
  oversized frame (it is rejected from its header). Every build fuzzes it
  and runs h2spec against it.
- **Container:** the image contains only the static binary. It runs as
  uid/gid 10001 and needs no capabilities and no writable file system:
  `--read-only --cap-drop ALL --security-opt no-new-privileges` all work.
