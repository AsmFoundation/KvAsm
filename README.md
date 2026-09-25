# KvNode

A gRPC key-value server written in **x86-64 assembly**, shipped as a
**1.7 MB `FROM scratch` container**.

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
- **Real HTTP/2.** nghttp2 1.70.0, statically linked; h2spec 138/146, with
  every remaining failure traced to the library and pinned as a regression
  baseline ([ADR 0002](docs/adr/0002-http2-library-nghttp2.md)).
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

Indicative numbers: Docker Desktop on WSL2, loopback, 4 workers, `h2load`
with 16 connections × 64 concurrent streams, 1 M requests each.

| Operation | Throughput | Mean latency at 1024 in flight |
|---|---|---|
| Get (hit) | ~2.8 M req/s | 231 µs |
| Put (same key, every write through the writer lock) | ~1.95 M req/s | 377 µs |

Store alone, single thread, 4M keys: 279 ns per dependent lookup, p99.9
714 ns, including the SipHash of the key (ADR 0001).

## Repository

| Path | Contents |
|---|---|
| [`proto/kvnode/v1/kvnode.proto`](proto/kvnode/v1/kvnode.proto) | the gRPC contract |
| [`server/`](server) | NASM sources, native unit tests, bake-off, end-to-end suite (`make all test itest bench`) |
| [`client/dotnet/`](client/dotnet) | .NET 8/10 client and snapshot writer, with unit and container tests |
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [protocol and client contract](docs/PROTOCOL.md), [operations](docs/OPERATIONS.md), [decision records](docs/adr) |
| [`Dockerfile`](Dockerfile) | toolchain (pinned and verified nghttp2 + h2spec) → build + all tests → scratch runtime |

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
