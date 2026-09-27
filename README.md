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
monitoring ([benchmarks/](benchmarks/README.md)). GET throughput with 1M
keys, 100-byte values and 1,024 requests in flight, medians of 6 runs
(i9-13900K, Docker Desktop on WSL2, loopback).

**One core, with the native HTTP/2 engine** (N1, 6 paired blocks, all
agreeing):

| 1 core | KvNode | Redis | Valkey |
|---|---|---|---|
| GET/s | **1.51 M** | 1.27 M | 1.24 M |
| KvNode ÷ X | — | **1.19×** | **1.22×** |
| server CPU per GET | **665 ns** | 788 ns | 813 ns |

The same image on nghttp2 (selectable then, removed since) did 744k: the
native engine is 2.03× faster on 1 core and 2–3× at 2 and 4 cores
([results](benchmarks/results/2026-09-26-native-final/README.md)). That
session ran on a busier host than the table below, which is why Redis's
absolute numbers differ between the two.

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
  lookup is the biggest part of that, mostly memory stalls.
- **SIMD** is already where the data is parallel: SSE2 tag matching,
  vector copies and 16-byte header stores. What remains is memory latency
  and the kernel.

**Memory:** 16 MiB table at the default capacity, 162 bytes per key with
100-byte values (178 with the table, Redis 174), about 9 KiB per idle
connection ([details](docs/OPERATIONS.md#memory)).

**Standard session** (all six servers; KvNode was still on nghttp2, before
the native engine):

| Server cores | KvNode | Redis | Valkey | Dragonfly | Garnet | Memcached |
|---|---|---|---|---|---|---|
| 1 | 813 k | 1.45 M | 1.44 M | 790 k | 872 k | 771 k |
| 4 | **3.02 M** | 2.89 M | 3.29 M | 2.29 M | 3.80 M | 3.12 M |
| 8 | **5.18 M** | 2.82 M | 2.40 M | 2.92 M | 7.91 M | 6.22 M |

- **Scales:** 6.4× from 1 to 8 cores. At 8 cores it runs 1.8–2.2× Redis,
  Valkey and Dragonfly, and behind Garnet and Memcached.
- **Low latency under load:** at identical offered loads up to 1.72M
  requests/s, its p99 stays at or below 1.2 ms. Redis reaches 50 ms and
  Garnet 396 ms there.
- **Costs:** with nghttp2, gRPC over HTTP/2 took 1.09 µs of server CPU per
  request against 0.42 µs for a Redis `PING`, which is why 1-core Redis and
  Valkey were faster above. The native engine brings a no-op round trip to
  Redis's level (N3) and a GET below it (665 vs 788 ns). Writes reach
  1.72M SETs/s at 4 cores (0.72× Redis) through the single writer lock.

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
