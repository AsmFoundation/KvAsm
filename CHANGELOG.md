# Changelog

All notable changes to KvNode — the server, the .NET client and the gRPC
contract, which are released together under one version. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). Before 1.0, a
minor version may change the configuration or the client API; the wire
protocol `kvnode.v1` only grows backward-compatibly.

Each entry names the commits and decision records (`docs/adr`) behind it,
so every line can be checked against the code and the measurements.

## [0.2.0] — 2026-11-15

The first public release, under the Apache License 2.0.

### Added

- **KvNode's own HTTP/2 engine and HPACK decoder, in assembly**
  (`h2n.asm`, `hpack.asm`; ADR 0007).
  - It passes h2spec 146/146. The HPACK decoder is differential-tested
    against nghttp2, and every build fuzzes the engine.
  - One core serves twice the requests it served on nghttp2 (`9885e64`,
    `ec83e0e`).
- **Streaming Upload and Download** (ADR 0010; `1372f7d`, `18ba0f0`).
  - Values up to 64 MiB (configurable to 2 GiB), and byte ranges of them,
    move in CRC-32C-verified chunks.
  - An upload is copied once, straight into the record it publishes
    atomically.
  - A download is generated frame by frame from a pinned record: one
    version per call, and no response buffer.
- **CRC-32C in four implementations**, picked at startup by
  `KVNODE_CRC32C` (`auto` takes the fastest the CPU has; `b751a97`):
  - VPCLMULQDQ + AVX2 256-bit folding;
  - PCLMULQDQ 128-bit folding;
  - the SSE4.2 `crc32` instruction;
  - slicing-by-8 tables.

  Non-temporal stores fill cold upload records.
- **Batched GET pipeline** (ADR 0005; `ba9e005`). Every GET of a read
  prefetches its two candidate buckets, then its record lines, before any
  of them is answered. A per-worker pool allocator serves stream state.
- **SSE2 tag matching** of both candidate buckets in one compare, the
  default (ADR 0006; `dd28221`). `KVNODE_SCAN=swar` keeps the scalar path.
  The optional per-worker L1/L2 record caches are off by default; they
  measured slower.
- **.NET client** (`18ba0f0`, `bb42b44`, `27866e1`):
  - `UploadAsync`, `DownloadAsync` and `DownloadToAsync`, with a
    zero-allocation streaming codec and folding CRC-32C;
  - 4 HTTP/2 connections per client and a 16 MiB receive window;
  - fault-injection tests through a TCP proxy that resets, hangs and
    throttles connections.
- **Benchmark suite** (`benchmarks/`; `ba9e005`, `8cb0fef`, `171ae57`,
  `18adf13`):
  - `kvbench`, one C client for RESP, memcached meta and gRPC;
  - a harness with paired, blocked statistics and outside-load
    monitoring;
  - the committed results of every session.
- **A case-study paper** (`docs/paper`; `9f3e290`) on how an LLM coding
  agent built KvNode.
- **Design proposals** (`docs/proposals`): Export/Watch, key indexes, and a
  58-function plan with measured prototypes. None of them is implemented.
- **Publication files:**
  - `LICENSE` (Apache-2.0), `NOTICE` and `THIRD_PARTY_NOTICES.md`;
  - `CONTRIBUTING.md`, `SECURITY.md` and `CODE_OF_CONDUCT.md`;
  - `docs/CLAIMS.md`, every public claim with its evidence;
  - CI and release workflows;
  - NuGet package metadata with Source Link and symbols.
- The runtime image now carries its license texts under
  `/usr/share/doc/kvnode/`. That includes the exact glibc and libgcc
  versions statically linked into the binary.

### Changed

- **nghttp2 left the server** (ADR 0008; `864a130`). The server links no
  HTTP/2 library, and `KVNODE_H2_ENGINE` no longer exists. nghttp2 stays
  as the independent client of the tests and of `kvbench`.
- **Two-phase event loop** (ADR 0009; `c3dec96`). A worker reads and
  processes its whole epoll batch, then flushes every connection once. This
  gave +12% GET/s with 4 connections per client thread and +26% with 8.
- Put, Delete and health replies are built on the stack, without
  allocation (`a0264f7`).
- Running out of memory while replying now closes the connection. It used
  to answer `INTERNAL` "server out of memory" (`a0264f7`; docs/PROTOCOL.md).
- Version 0.2.0 everywhere: the startup log line, the client package and
  the local image tag `kvnode:0.2.0`. Results recorded before this release
  keep the version they measured.

### Fixed

- A latent use-after-free when a drain began in the middle of an epoll
  batch: the drain now begins after the batch's flush (`c3dec96`).
- An end-to-end test race: drain clients now complete a PING handshake
  before they count as ready (`a0264f7`).
- A .NET upload that the server refused at once could hang (`bb42b44`).

### Performance

Measured on an i9-13900K under Docker Desktop on WSL2, over loopback. The
full session ran 747 runs on revision `a0264f7`
([results](https://github.com/Nucs/KvNode/blob/main/benchmarks/results/2026-09-27-standard/README.md);
every claim with its scope:
[docs/CLAIMS.md](https://github.com/Nucs/KvNode/blob/main/docs/CLAIMS.md)).

- **4 cores:** 8.84M GET/s, 2.29–3.68× Redis, Valkey, Dragonfly, Garnet and
  Memcached, ahead of each in all ten paired blocks. That is 3.0× the last
  nghttp2 build.
- **1 core:** 1.76M GET/s, 1.26× Redis, with 576 ns of server CPU per GET.
- **Streaming** (ADR 0010 §6; `b751a97`, `27866e1`):
  - The final server against the first streaming build: uploads
    1.56–2.30× and downloads 1.22–1.28×.
  - One server core takes about 11 GB/s of 1 MiB uploads and 6 GB/s of
    1 MiB downloads.
  - One .NET client is 2.5–3.9× faster.

### Known issues

- **16 KiB values** regressed against the last nghttp2 build: 481k GET/s
  against 691k. The cause has not been bisected yet.
- **Writes take one lock.**
  - 2.23M SET/s at 4 cores: 0.87× Redis, 0.60× Garnet.
  - In a store-level probe (10M keys, 100-byte replace-puts), total Put
    throughput fell from 4.51M/s with one writer to 2.07–2.24M/s with four
    to seven. Flat combining restored 4.29M/s at seven writers (1.91×) in
    a prototype (docs/proposals/golden-services.md, P3).
- **More than 4 cores is unmeasured.** On the test machine the benchmark
  client saturates first. Measuring beyond 4 cores needs the client on a
  second machine.
- **Not built yet:** persistence, TLS, authentication, replication, key
  expiry (TTL) and an IPv6 listener (docs/OPERATIONS.md, Security notes).

## [0.1.0] — 2026-09-26

The first working server, built 4.6 hours after the first prompt (`d95793d`,
tagged afterwards; never released). The case-study paper calls it "v0.1.0".

- A bucketized cuckoo store with lock-free seqlocked readers, serialized
  writers and quiescent-state reclamation (ADR 0001, ADR 0003). A
  bake-off chose it over chaining and a branchless trie.
- Exact keys placed by SipHash-2-4-128 under a per-process secret.
  Clients compact long keys (ADR 0004).
- gRPC on nghttp2 (ADR 0002):
  - Get, Put and Delete;
  - the standard health service;
  - graceful drain, idle timeouts and strict configuration.
- The `KVNSNAP1` snapshot loader, verified before any record is stored.
- A .NET 8/10 client and a snapshot writer.
- Four layers of tests: native unit tests, the data-structure bake-off, an
  end-to-end suite with h2spec, and the .NET suite.

[0.2.0]: https://github.com/Nucs/KvNode/releases/tag/v0.2.0
[0.1.0]: https://github.com/Nucs/KvNode/tree/v0.1.0
