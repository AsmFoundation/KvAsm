# KvNode

A gRPC key-value server written in **x86-64 assembly**, shipped as a
**1.6 MB `FROM scratch` container** (one 1.1 MB static binary; glibc is the
only library linked in).

- **O(1) everything.** A bucketized cuckoo hash: a lookup touches at most
  two cache lines. Chaining and a branchless binary trie were measured
  against it and lost ([ADR 0001](docs/adr/0001-store-data-structure.md)).
- **Atomic and concurrent.** Lock-free readers (seqlocked buckets), one
  serialized writer path, immutable records, QSBR reclamation. Torture and
  end-to-end tests check for torn values and found none
  ([ADR 0003](docs/adr/0003-concurrency-model.md)).
- **Exact keys, safe hashing.** Keys are compared byte for byte; SipHash
  under a per-process secret prevents hash flooding. Long keys are compacted
  client-side ([ADR 0004](docs/adr/0004-exact-keys-and-client-compaction.md)).
- **Its own HTTP/2, in assembly.** A server-side HTTP/2 engine with an HPACK
  decoder, differential-tested against nghttp2 and fuzzed. It passes h2spec
  **146/146** and doubled the requests a core can serve
  ([ADR 0007](docs/adr/0007-native-http2-engine.md)). nghttp2, which it
  replaced, is out of the server
  ([ADR 0008](docs/adr/0008-remove-nghttp2-from-the-server.md)); it
  survives only as the independent client the tests and benchmarks use.
- **Streaming for large values.** Client-streaming Upload and
  server-streaming Download move values up to 64 MiB (configurable to
  2 GiB) and byte ranges of them in CRC-32C-verified chunks: an upload is
  copied once, straight into the record it will publish atomically; a
  download is generated frame by frame from a pinned record, one version
  per call, with no response buffer. Tested byte by byte, including resets,
  hangs and slow links mid-transfer
  ([ADR 0010](docs/adr/0010-streaming-upload-download.md)).
- **Two data sources:** clients write via gRPC, and a verified snapshot
  file loads at startup.
- **Production behavior:** standard gRPC health service, graceful drain on
  SIGTERM, idle timeouts, strict configuration, structured logs, non-root,
  read-only file system.

## Quick start

```sh
docker build --target runtime -t kvnode:0.1.0 .      # also runs every server test
docker run --rm -p 50051:50051 kvnode:0.1.0
```

```csharp
using KvNode.Client;

using var kv = new KvNodeClient(new Uri("http://localhost:50051"));
bool created = await kv.PutAsync("user:42", "Ada");   // true: new key
string? name = await kv.GetStringAsync("user:42");    // "Ada"
bool deleted = await kv.DeleteAsync("user:42");       // true

// Large values: streamed in chunks, CRC-32C-checked at both ends.
await using FileStream file = File.OpenRead("video.mp4");
UploadResult up = await kv.UploadAsync("media:1"u8.ToArray(), file);
await using FileStream copy = File.Create("copy.mp4");
DownloadResult? got = await kv.DownloadToAsync("media:1"u8.ToArray(), copy);
byte[]? head = await kv.DownloadAsync("media:1"u8.ToArray(), offset: 0, length: 4096);
```

Load data at startup from a snapshot written by
[`SnapshotFile.WriteAsync`](client/dotnet/README.md#snapshots):

```sh
docker run --rm -p 50051:50051 -v "$PWD/data:/data:ro" \
  -e KVNODE_SNAPSHOT=/data/seed.kvs -e KVNODE_CAPACITY=5000000 kvnode:0.1.0
```

## Performance

KvNode was compared with Redis, Valkey, Dragonfly, Garnet and Memcached,
using one client, pinned versions, paired statistics, and host-noise
monitoring ([benchmarks/](benchmarks/README.md)).

- **Latest full session:** 2026-09-27, revision `a0264f7`, 747 runs
  ([results](benchmarks/results/2026-09-27-standard/README.md)).
- **Workload:** GET throughput with 1M keys, 100-byte values and 1,024
  requests in flight.
- **Machine:** i9-13900K, Docker Desktop on WSL2, loopback.

**4 cores** (S1: ten runs per server on a quiet host; KvNode was ahead of
every server in all ten paired blocks):

| 4 cores | KvNode | Redis | Valkey | Dragonfly | Garnet | Memcached |
|---|---|---|---|---|---|---|
| GET/s | **8.84 M** | 2.88 M | 3.20 M | 2.40 M | 3.86 M | 3.01 M |
| KvNode ÷ X | — | **3.07×** | **2.76×** | **3.68×** | **2.29×** | **2.94×** |

**One core** (N1, 6 paired blocks, all agreeing):

| 1 core | KvNode | Redis | Valkey |
|---|---|---|---|
| GET/s | **1.76 M** | 1.40 M | 1.42 M |
| KvNode ÷ X | — | **1.26×** | **1.24×** |
| server CPU per GET | **576 ns** | 722 ns | 710 ns |

- **Reads, latency, concurrency:** KvNode is ahead of every server
  - on 90%-GET mixes (7.66M ops/s at 4 cores, 1.9–3.2×);
  - without pipelining (815k requests/s, 1.3–2.1×);
  - with 1 KiB values (5.59M, 1.8–3.7×);
  - from 64 requests in flight up.

  At fixed offered loads up to 1.25M requests/s its p99 stays at or below
  0.44 ms; only Valkey's is lower.
- **Per-request floor:** a gRPC health check costs 472 ns of server CPU,
  level with a Redis `PING` (479 ns). With nghttp2 it was 1,086 ns.
- **Where it is behind:**
  - writes: 2.23M SETs/s at 4 cores through the single writer lock (0.87×
    Redis, 0.60× Garnet);
  - 16 KiB values: 481k GET/s, half of Garnet's and Memcached's, and 30%
    below the old nghttp2 build (a regression, not yet explained).
- **At 8 cores the benchmark client is the limit:** kvbench's gRPC client
  tops out near 9.3M requests/s, so KvNode's 8-core numbers measure the
  client.
  - A bigger client on this machine was 12% slower, because its physical
    cores are all in use.
  - So 8 cores cannot double 4 cores here: 8.84M → 9.72M GET/s on a quiet
    host. Measuring 8 cores properly needs the client on a second machine
    ([why](benchmarks/results/2026-09-28-client-ceiling/README.md)).
- **Caveat:** the 1-to-8-core scaling experiment ran while another program
  kept half the host busy. KvNode still led every server at every core
  budget there, except that it was not clearly ahead of Garnet at 8 cores.
  The report adjusts every comparison for outside load.

**What changed since the nghttp2 build.** Mostly the native HTTP/2 engine:
against the same image on nghttp2 (selectable then, removed since), it was
2.03× faster on 1 core
([results](benchmarks/results/2026-09-26-native-final/README.md)). The
two-phase event loop and the SSE2 store scan contributed too. The last full
session on nghttp2 (2026-09-26) measured 2.93M GET/s at 4 cores, against
8.84M now. The other servers measured the same in both sessions (Redis
2.89M then, 2.88M now).

**Where one core's time goes** (kernel-inclusive profile,
[results](benchmarks/results/2026-09-26-profile-memory/README.md)):

- **Most of it is the kernel.** Over loopback, about half of a loaded core
  is the server's `sendto` waking the co-located client, a cross-vCPU IPI
  under WSL2.
- **The server now wakes clients less.** It reads a whole epoll batch
  first, then flushes every connection together
  ([ADR 0009](docs/adr/0009-flush-once-per-event-batch.md)). A client
  thread with several connections then wakes once per batch. Measured:
  +12% GET/s with 4 connections per client thread, +26% with 8, with lower
  latency in every case
  ([results](benchmarks/results/2026-09-27-two-phase/README.md)).
- **Event-driven, never spinning:** an idle worker sleeps in `epoll_wait`.
  4 workers holding 1,000 idle connections used 10 ms of CPU in 10 s.
- **With a busy-polling client** that cost disappears, and one core served
  **2.62M GET/s** (382 ns per GET): 177 ns kernel, 205 ns KvNode. The store
  lookup is the biggest part of that, mostly memory stalls. On a quieter
  host the same measurement gave 3.71M GET/s, 269 ns per GET
  ([results](benchmarks/results/2026-09-27-store-lookup/README.md)).
- **SIMD** is already where the data is parallel: SSE2 tag matching,
  vector copies and 16-byte header stores. What remains is memory latency
  and the kernel.

**Memory:** 16 MiB table at the default capacity, 162 bytes per key with
100-byte values (178 with the table, Redis 174), about 9 KiB per idle
connection ([details](docs/OPERATIONS.md#memory)).

Store alone, single thread, 4M keys: 279 ns per dependent lookup, p99.9
714 ns, including the SipHash of the key (ADR 0001).

## Repository

| Path | Contents |
|---|---|
| [`proto/kvnode/v1/kvnode.proto`](proto/kvnode/v1/kvnode.proto) | the gRPC contract |
| [`server/`](server) | NASM sources, native unit tests, bake-off, end-to-end suite (`make all test itest bench`) |
| [`client/dotnet/`](client/dotnet) | .NET 8/10 client and snapshot writer, with unit and container tests |
| [`benchmarks/`](benchmarks/README.md) | comparison with other key-value stores: the kvbench client, the suite, methodology, committed results |
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [protocol and client contract](docs/PROTOCOL.md), [operations](docs/OPERATIONS.md), [decision records](docs/adr) |
| [`Dockerfile`](Dockerfile) | toolchain (pinned and verified nghttp2 for the test clients, and h2spec) → build + all tests → scratch runtime |

## Development

```sh
docker build --target toolchain -t kvnode-toolchain .
docker run --rm -v "$PWD:/src" -w /src/server kvnode-toolchain make all test itest
dotnet test --solution client/dotnet/KvNode.Client.slnx   # needs the kvnode:0.1.0 image
```

## Status

Version 0.1.0. Clients: .NET (done). Two more languages are planned; they
must implement [docs/PROTOCOL.md](docs/PROTOCOL.md), including long-key
compaction and the snapshot conformance vector.
