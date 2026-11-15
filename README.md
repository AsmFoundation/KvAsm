# KvNode

**FOSAS — Free Open Source Assembly Software.**

[![CI](https://github.com/Nucs/KvNode/actions/workflows/ci.yml/badge.svg)](https://github.com/Nucs/KvNode/actions/workflows/ci.yml)
[![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
[![NuGet: KvNode.Client](https://img.shields.io/nuget/v/KvNode.Client.svg?label=KvNode.Client)](https://www.nuget.org/packages/KvNode.Client)

KvNode is a gRPC key-value server written in **x86-64 assembly**. It ships
as a **2 MB `FROM scratch` container** (0.5 MB to download): one 1.1 MB
static binary, with glibc the only library linked in, plus its license
texts. To our knowledge
it is **the first gRPC server written in assembly**: its HTTP/2 engine,
HPACK decoder, gRPC layer and Protocol Buffers codec are all hand-written
NASM.

Every claim below is measured or verifiable in this repository.
[docs/CLAIMS.md](docs/CLAIMS.md) gives the evidence and scope of each one,
and what is deliberately *not* claimed.

- **Outcompetes Redis, Valkey, Dragonfly, Garnet and Memcached on reads
  of small values.**
  - 8.84M GET/s on 4 cores: 2.29–3.68× each of them, ahead of every one
    in all ten paired blocks.
  - 1.26× Redis on one core.
  - It is behind them on writes and on 16 KiB values (see
    [Performance](#performance)).
- **Assembly data structures.** The store is a bucketized cuckoo table of
  64-byte seqlocked buckets. A measured bake-off chose it over chaining and
  a branchless binary trie, both also written in assembly
  ([ADR 0001](docs/adr/0001-store-data-structure.md)).
- **O(1) by architecture.** Every key operation is O(1):
  - a lookup reads at most two cache lines;
  - an insert's displacement search is bounded;
  - the table is sized once at startup, so it never pauses to rehash.
- **SIMD and AVX where they pay.** One SSE2 compare matches all 12
  candidate tags of every lookup
  ([ADR 0006](docs/adr/0006-store-memory-levels-simd-and-private-caches.md)).
  AVX2 + VPCLMULQDQ fold the CRC-32C of streamed values at up to ~88 GB/s.
  CPU features are detected at startup, so any x86-64 CPU runs it.
- **Reads scale across cores.** Readers take no lock and write no shared
  memory, and each worker thread owns its connections. Measured from 1 to
  4 cores: 1.76M → 8.84M GET/s.
  - Writes share one lock.
  - Above 4 cores the benchmark client saturates first.
- **Thread-safe by construction.** Reads are lock-free (seqlocked buckets),
  writes are serialized, records are immutable, and reclamation is QSBR. A
  reader sees the old value or the new one, never a mix. Torture and
  end-to-end tests check for torn values and found none
  ([ADR 0003](docs/adr/0003-concurrency-model.md)).
- **gRPC from scratch, in assembly.** The server-side HTTP/2 engine and
  HPACK decoder are differential-tested against nghttp2 and fuzzed.
  - It passes h2spec **146/146**. nghttp2, which it replaced, passes 138.
  - It doubled the requests a core can serve
    ([ADR 0007](docs/adr/0007-native-http2-engine.md)).
  - No gRPC, HTTP/2 or protobuf library is linked. nghttp2 is out of the
    server ([ADR 0008](docs/adr/0008-remove-nghttp2-from-the-server.md))
    and survives only as the independent client the tests and benchmarks
    use.
- **CPU-accelerated O(n) streaming.** Client-streaming Upload and
  server-streaming Download move values up to 64 MiB (configurable to
  2 GiB), and byte ranges of them, in CRC-32C-verified chunks.
  - An upload is copied once, straight into the record it will publish
    atomically.
  - A download is generated frame by frame from a pinned record: one
    version per call, and no response buffer.
  - Copy and checksum are one pass: one core takes ~11 GB/s of 1 MiB
    uploads.
  - Tested byte by byte, including resets, hangs and slow links
    mid-transfer ([ADR 0010](docs/adr/0010-streaming-upload-download.md)).
- **Exact keys, safe hashing.** Keys are compared byte for byte. SipHash
  under a per-process secret prevents hash flooding. Clients compact long
  keys ([ADR 0004](docs/adr/0004-exact-keys-and-client-compaction.md)).
- **Two data sources:** clients write via gRPC, and a verified snapshot
  file loads at startup.
- **Production behavior:**
  - the standard gRPC health service;
  - graceful drain on SIGTERM, and idle timeouts;
  - strict configuration and structured logs;
  - non-root, on a read-only file system.

## What it is not (yet)

KvNode is young (v0.2.0) and deliberately small. Before you deploy it, know
what it does not do:

- **No persistence.** A snapshot file loads at startup, but the server
  never writes data to disk. A restart serves the snapshot again, without
  the changes made since.
- **No TLS and no authentication.** It speaks cleartext HTTP/2 (h2c). Run
  it on a trusted network, or behind a TLS-terminating proxy or mesh that
  controls who reaches the port ([SECURITY.md](SECURITY.md)).
- **No replication, clustering, key expiry (TTL), data types beyond bytes,
  or IPv6 listener.**
- **Linux on x86-64 only.** The server is x86-64 assembly.

[docs/proposals](docs/proposals/functions-plan.md) designs and prototypes
58 more functions, with measurements, Export/Watch streams and key indexes
among them. None of them is implemented yet. Persistence is out of that
plan's scope and has no design in the repository yet.

## Quick start

Run the published image (linux/amd64):

```sh
docker run --rm -p 50051:50051 ghcr.io/nucs/kvnode:0.2.0
```

Or build it from source. The build runs every server test suite, and the
image exists only if all of them pass:

```sh
docker build --target runtime -t kvnode:0.2.0 .
docker run --rm -p 50051:50051 kvnode:0.2.0
```

Talk to it from .NET 8 or 10 (`dotnet add package KvNode.Client`), or from
any language with gRPC tooling, using
[`proto/kvnode/v1/kvnode.proto`](proto/kvnode/v1/kvnode.proto) and
[docs/PROTOCOL.md](docs/PROTOCOL.md):

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
  -e KVNODE_SNAPSHOT=/data/seed.kvs -e KVNODE_CAPACITY=5000000 ghcr.io/nucs/kvnode:0.2.0
```

Every setting, the exit codes, health probes, memory sizing and security
notes are in [docs/OPERATIONS.md](docs/OPERATIONS.md).

### Requirements

- **Server:** Linux 3.17 or later on x86-64 (it calls `getrandom`). It was
  developed and tested on the 5.15 kernel of WSL2 and runs in any OCI
  runtime.
  - Any x86-64 CPU runs it, because the baseline instruction set includes
    SSE2.
  - SSE4.2, PCLMULQDQ, and AVX2 + VPCLMULQDQ speed up CRC-32C when the CPU
    has them.
- **Client:** .NET 8 or .NET 10.
- **Building from source:** Docker.
  - The toolchain stage installs NASM and GCC from Debian trixie.
  - It pins nghttp2 (the test clients use it) and h2spec by version and
    SHA-256.

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
| [`docs/`](docs) | [architecture](docs/ARCHITECTURE.md), [protocol and client contract](docs/PROTOCOL.md), [operations](docs/OPERATIONS.md), [decision records](docs/adr), [claims and their evidence](docs/CLAIMS.md) |
| [`docs/proposals/`](docs/proposals/functions-plan.md) | designs measured but not implemented: the functions plan (58 functions, Export/Watch and key indexes among them) and store speedups |
| [`docs/paper/`](docs/paper/README.md) | case-study paper on how an LLM coding agent built KvNode in assembly (Word document, Markdown source, figures, and the scripts behind its numbers) |
| [`Dockerfile`](Dockerfile) | toolchain (pinned and verified nghttp2 for the test clients, and h2spec) → build + all tests → scratch runtime |
| [`.github/`](.github) | CI (every test suite on every push and pull request) and the release workflow (GHCR image, NuGet package, GitHub release) |
| [`CHANGELOG.md`](CHANGELOG.md), [`CONTRIBUTING.md`](CONTRIBUTING.md), [`SECURITY.md`](SECURITY.md), [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) | release notes; how to build, test, measure and propose changes; how to report a vulnerability; community rules |
| [`LICENSE`](LICENSE), [`NOTICE`](NOTICE), [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) | Apache-2.0, and the licenses of the glibc and libgcc code linked into the binary |

## Development

```sh
docker build --target toolchain -t kvnode-toolchain .
docker run --rm -v "$PWD:/src" -w /src/server kvnode-toolchain make all test itest
docker build --target runtime -t kvnode:0.2.0 .           # the image the .NET tests start
cd client/dotnet && KVNODE_REQUIRE_DOCKER=1 dotnet test --solution KvNode.Client.slnx
```

Run `dotnet test` from `client/dotnet`. Its `global.json` selects the
Microsoft.Testing.Platform runner, and from the repository root
`--solution` is rejected. [CONTRIBUTING.md](CONTRIBUTING.md) has the
verification gates every change must pass, and the measurement rules for
performance claims.

## Status

Version 0.2.0, the first public release ([CHANGELOG](CHANGELOG.md)).

- **Clients:** .NET is done. Two more languages are planned. They must
  implement [docs/PROTOCOL.md](docs/PROTOCOL.md), including long-key
  compaction and the snapshot conformance vector.
- **Next:** the store speedups (8-lane AVX2 hashing, flat-combining writes)
  and the functions plan, phase by phase, as
  [docs/proposals](docs/proposals/functions-plan.md) lays out. Each lands
  with its tests and a paired measurement.

## Contributing, security and license

- **Contributing:** issues and pull requests are welcome. Read
  [CONTRIBUTING.md](CONTRIBUTING.md) first. A performance change needs a
  paired measurement, not an argument.
- **Security:** report vulnerabilities privately, as
  [SECURITY.md](SECURITY.md) describes. Do not open a public issue.
- **License:** KvNode is licensed under the
  [Apache License 2.0](LICENSE). The server binary statically links glibc
  (LGPL-2.1-or-later) and libgcc.
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) explains what that
  means for redistribution.
