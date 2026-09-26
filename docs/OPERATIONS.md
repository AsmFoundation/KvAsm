# Operating KvNode

## Configuration

Every setting is an environment variable. Numeric values are strict decimal
integers: no sign, no spaces, no units. Named choices (`KVNODE_SCAN`,
`KVNODE_H2_ENGINE`) must be one of the listed lowercase words exactly.
Out-of-range, malformed or unknown values stop startup with exit code **2**
and a log line naming the variable, the value and what is allowed. A typo
never silently falls back to a default.

| Variable | Range | Default | Meaning |
|---|---|---|---|
| `KVNODE_PORT` | 1–65535 | 50051 | h2c listen port (all interfaces) |
| `KVNODE_WORKERS` | 0–256 | 0 | event-loop threads; 0 = CPUs in the affinity mask. This honors `--cpuset-cpus` but not a `--cpus` quota, so set it explicitly under quotas |
| `KVNODE_CAPACITY` | 1–2³⁶ | 1000000 | maximum number of keys; sizes the table once (see Sizing) |
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
| `KVNODE_H2_ENGINE` | `native` \| `nghttp2` | `native` | which HTTP/2 implementation serves every connection. `native` is KvNode's own engine (`h2n.asm`): h2spec 146/146, and 2–3× the throughput of nghttp2 on the same hardware. `nghttp2` is nghttp2 1.70 (h2spec 138/146), kept as a fallback. Both give the same gRPC behavior, limits and statuses; the starting log line shows the engine in use ([ADR 0007](adr/0007-native-http2-engine.md)) |

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

## Sizing

- **Table (fixed, allocated at startup):** 64 bytes × the power of two ≥
  ⌈capacity / 5⌉. The default 1,000,000 keys → 262,144 buckets → 16 MiB.
  With transparent huge pages enabled (`always` or `madvise`), every whole
  2 MiB region of it is faulted in at startup as one huge page, so the
  table is resident from the start and every lookup hits the TLB; without
  THP it is touched lazily as keys arrive.
- **Records:** `24 + key rounded up to 8 + value` bytes each, plus the
  allocator's overhead (about 16 bytes). `KVNODE_MAX_MEMORY_MB` caps the sum
  of the first part. Replaced and deleted records stay counted until
  reclamation frees them, typically within milliseconds.
- **Per connection:** HTTP/2 state plus buffered request bytes, bounded by
  `KVNODE_CONN_BUFFER_MB`. With the native engine that is about 4 KiB per
  connection: ~0.6 KiB in the connection record, a 3 KiB HPACK entry ring,
  and a stream map of 8 bytes per slot (16 slots, doubling with concurrent
  requests); the peer's HPACK entries add at most 4 KiB. nghttp2's session
  is larger and allocates per stream and per header.
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
  and the HTTP/2 engine's flood defenses. The native engine limits rapid
  stream resets to a burst of 1,000 then 33 per second (CVE-2023-44487),
  caps a header block at 64 KiB and 64 CONTINUATION frames, and never
  buffers an oversized frame (it is rejected from its header). nghttp2
  applies its own equivalents.
- **Container:** the image contains only the static binary. It runs as
  uid/gid 10001 and needs no capabilities and no writable file system:
  `--read-only --cap-drop ALL --security-opt no-new-privileges` all work.
