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

KvNode was compared with Redis, Valkey, Dragonfly, Garnet and Memcached,
using one client, pinned versions, paired statistics, and host-noise
monitoring ([benchmarks/](benchmarks/README.md)). GET throughput with 1M
keys, 100-byte values and 1,024 requests in flight, medians of 6 runs
(i9-13900K, Docker Desktop on WSL2, loopback):

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
- **Costs:** gRPC over HTTP/2 takes 1.09 µs of server CPU per request, and
  a Redis `PING` 0.42 µs. So one core tops out near 0.8–0.9M GETs/s, and
  1-core Redis and Valkey are faster. Writes reach 1.72M SETs/s at 4 cores
  (0.72× Redis) through the single writer lock.

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
