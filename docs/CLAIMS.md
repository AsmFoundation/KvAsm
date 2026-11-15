# Claims and their evidence

Every public claim about KvNode, the exact wording, and the evidence behind
it. The README, release notes, package descriptions and announcements may
only make the claims listed here, scoped as they are here.

**Keeping this file true.** When a change of code or a new measurement
moves a number, update the claim in the same commit. A claim with no
evidence left is removed, not softened.

**The test machine.** Unless a claim says otherwise, the numbers come from
one machine:

- an i9-13900K (8 P-cores with Hyper-Threading and 16 E-cores, 32 logical
  CPUs);
- Docker Desktop on WSL2, over loopback;
- the protocol `kvbench` client (`benchmarks/`), with paired, blocked
  statistics and outside-load monitoring (`benchmarks/METHODOLOGY.md`).

The other servers run their own protocols: RESP for Redis, Valkey, Dragonfly
and Garnet, and memcached meta for Memcached. Their persistence is off.

| # | Claim, as published | Status |
|---|---|---|
| 1 | FOSAS — Free Open Source Assembly Software | true |
| 2 | To our knowledge, the first gRPC server written in assembly | true as scoped |
| 3 | Outcompetes Redis, Valkey, Dragonfly, Garnet and Memcached on reads of small values | measured, scoped |
| 4 | Its data structures are hand-written assembly | true |
| 5 | Every key operation is O(1) | true |
| 6 | SIMD and AVX where measurement says they pay | true as scoped |
| 7 | Reads scale across cores | measured to 4 cores |
| 8 | Thread-safe by construction | true, tested |
| 9 | gRPC from scratch, in assembly | true |
| 10 | CPU-accelerated O(n) streaming | true as scoped |

## 1. FOSAS — Free Open Source Assembly Software

- **Evidence:** [LICENSE](../LICENSE) (Apache License 2.0) covers the whole
  repository. [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) lists the
  third-party code linked into the binary (glibc, libgcc) and how the
  release meets its licenses.
- **Scope:** the server is assembly (NASM). The client, tests, tools and
  benchmark harness are C#, C and Python.

## 2. To our knowledge, the first gRPC server written in assembly

- **Wording:** "To our knowledge, the first gRPC server written in
  assembly: its HTTP/2 engine, HPACK decoder, gRPC layer and Protocol
  Buffers codec are all hand-written x86-64 NASM."
- **Evidence for "in assembly":**
  - `server/Makefile` builds the server binary from NASM objects only
    (`CORE_ASM`, `SERVER_ASM`).
  - The protocol stack is `server/src/h2n.asm` (HTTP/2, 3,422 lines),
    `hpack.asm` (1,110), `grpc.asm` (2,112) and `proto.asm`.
  - [ADR 0008](adr/0008-remove-nghttp2-from-the-server.md) records how the
    last protocol library left the server.
- **Evidence for "first", a prior-art search on 2026-11-15** (web search,
  Brave Search and the GitHub API):
  - **Assembly servers that do exist all speak HTTP/1.x:** for example
    poletaevvlad/http-asm, bergkvist/asm-http-server, barrettotte/HTTP-ASM64
    and cerber-os/HttpAsm.
  - **One pure-assembly key-value store exists:**
    [whispem/asm.kvstore](https://github.com/whispem/asm.kvstore) (created
    July 2026). It speaks its own TCP protocol. So KvNode is **not** the
    first key-value store in assembly, and the claim never says it is.
  - **The nearest gRPC/HTTP/2 projects are written in C:** okba14/NeuroHTTP
    (C, with assembly SIMD kernels) and the HTTP/2 libraries of
    VBWizard/os64.
  - **None of them implements HTTP/2, HPACK or gRPC in assembly.**
- **Not claimed:**
  - the first key-value store in assembly;
  - the first server in assembly;
  - a general-purpose gRPC framework. KvNode serves its own service and
    the health service, over h2c.

## 3. Outcompetes Redis, Valkey, Dragonfly, Garnet and Memcached on reads of small values

- **Wording:** "Outcompetes Redis, Valkey, Dragonfly, Garnet and Memcached
  on reads of small values: 8.84M GET/s on 4 cores, 2.29–3.68× each of
  them, and 1.26× Redis on one core."
- **Evidence:** the full session of 2026-09-27, revision `a0264f7`, 747
  valid runs
  ([results](../benchmarks/results/2026-09-27-standard/README.md)):
  - **4 cores, 100% GET, 1M keys, 100-byte values (S1):**
    - KvNode served 8.84M GET/s;
    - Redis 2.88M, Valkey 3.20M, Dragonfly 2.40M, Garnet 3.86M and
      Memcached 3.01M;
    - KvNode was ahead of each in all ten paired blocks (p = 0.002).
  - **1 core (N1):** 1.76M GET/s against Redis's 1.40M (1.26×) and
    Valkey's 1.42M (1.24×), in 6 of 6 blocks. Server CPU per GET was
    576 ns against Redis's 722 ns.
  - **90% GET (E2):** 7.66M ops/s, 1.88–3.16× each.
  - **1 KiB values (E6):** 5.59M GET/s, 1.77× Garnet and 2.3–3.7× the
    others.
  - **No pipelining (E3):** 815k requests/s, 1.34–2.07×.
  - **Concurrency (E7):** ahead of every server from 64 requests in flight.
- **Where KvNode is behind or level** (stated wherever this claim
  appears):
  - **Writes (E2, 100% SET):** 2.23M SET/s, which is 0.87× Redis, 0.90×
    Dragonfly and 0.60× Garnet.
  - **50/50 mix:** level with Garnet (0.99×).
  - **16 KiB values:** 0.50× Garnet and 0.48× Memcached. This is a known
    regression (CHANGELOG, Known issues).
  - **Tail latency at fixed load (E4):** Valkey's p99 is lower.
  - **16 requests in flight (E7):** Memcached is ahead (0.90×).
  - **8 cores:** not measured. The benchmark client saturates first
    ([why](../benchmarks/results/2026-09-28-client-ceiling/README.md)).
    In the loaded-host scaling run KvNode was not clearly ahead of Garnet
    at 8 cores (1.21× [0.88–1.29], 5 of 6 blocks).
- **Not claimed:** that KvNode outcompetes *all* key-value stores,
  "enterprise" or otherwise.
  - It has no persistence, TLS, authentication, replication, expiry
    (TTL), clustering or data types beyond bytes.
  - It was compared with five stores, not with Aerospike, KeyDB, etcd,
    FoundationDB, ScyllaDB or cloud services.
  - All results come from one machine over loopback.

## 4. Its data structures are hand-written assembly

- **Wording:** "Its data structures are hand-written assembly: a
  bucketized cuckoo table of 64-byte seqlocked buckets, chosen over
  chaining and a branchless trie — both also written in assembly — by a
  measured bake-off."
- **Evidence:**
  - **The bake-off:** [ADR 0001](adr/0001-store-data-structure.md). At 4M
    keys the crit-bit trie was 3.8–5.9× slower, with an 18.7× worse p99.9.
    Chaining was 26% slower on hits and 63% slower on misses.
  - **The structures in the server:**
    - `server/src/store.asm` and `cuckoo.asm`: buckets with a version word,
      6 tags and 6 record pointers;
    - `pool.asm`: the per-worker allocator;
    - `hcache.asm`: the header-block cache;
    - the HPACK ring and the stream maps.
  - **The rivals:** `server/bench/ds/chain.asm` and `trie.asm`.

## 5. Every key operation is O(1)

- **Wording:** "Every key operation is O(1). A lookup reads at most two
  64-byte buckets, an insert's displacement search is bounded, and the
  table is sized once at startup, so it never pauses to rehash."
- **Evidence:**
  - [ADR 0001](adr/0001-store-data-structure.md) and
    [ARCHITECTURE.md](ARCHITECTURE.md).
  - **Get:** two candidate buckets, then an exact key compare.
  - **Put:** a free slot in either bucket, or a breadth-first displacement
    search of at most 256 nodes.
  - **Delete:** one bucket slot.
  - **Health:** a constant reply.
  - **The table:** `KVNODE_CAPACITY` sizes it once, resident from startup
    ([OPERATIONS.md](OPERATIONS.md#memory)).
- **Scope:**
  - "O(1)" is in the number of keys. Comparing and copying a key or value
    is linear in its length.
  - Upload and Download are O(value size) by nature (claim 10).
  - A Put into a table near its load limit can fail with
    `RESOURCE_EXHAUSTED` rather than grow the table.
- **Not claimed:** "many" O(1) operations. Today there are three key
  operations, Get, Put and Delete, plus streaming transfers and health.
  The 58-function plan ([docs/proposals](proposals/functions-plan.md)) is
  a proposal, not shipped.

## 6. SIMD and AVX where measurement says they pay

- **Wording:** "SIMD where measurement says it pays: one SSE2 compare
  matches all 12 candidate tags of every lookup, and AVX2 + VPCLMULQDQ
  fold the CRC-32C of streamed values at up to ~88 GB/s. CPU features are
  detected at startup, so any x86-64 CPU runs it."
- **Evidence:**
  - **Tag matching:** [ADR 0006](adr/0006-store-memory-levels-simd-and-private-caches.md).
    The SSE2 scan is 2–6% faster per batched lookup than the SWAR path,
    and it is the default (`KVNODE_SCAN`).
  - **CRC-32C** (`server/src/crc32c.asm`; the `KVNODE_CRC32C` row of
    [OPERATIONS.md](OPERATIONS.md#configuration)):
    - VPCLMULQDQ + AVX2 256-bit folding: ~88 GB/s fused with a 16 KiB
      in-cache copy;
    - PCLMULQDQ: ~45 GB/s;
    - SSE4.2 `crc32`: ~15 GB/s;
    - table: ~3.2 GB/s.

    `CPUID` and `XGETBV` choose the path.
  - **Non-temporal stores:** `MOVNTDQ` fills cold upload records, +13–31%
    on uploads ([ADR 0010](adr/0010-streaming-upload-download.md) §6).
- **Not claimed:** AVX in every operation.
  - The one-core profile puts about three quarters of a GET in the kernel.
    The store's share is address-translation and DRAM stalls.
  - Arithmetic or copying that SIMD could still shrink is ~10% of the
    core, and half of it is already AVX2 inside glibc's `memmove` and
    `memset`
    ([profile](../benchmarks/results/2026-09-26-profile-memory/README.md)).
  - An 8-lane AVX2 SipHash for batched GETs measured 0.943× the time per
    lookup. It is a prototype, not shipped
    ([Track S](proposals/functions-plan.md)).

## 7. Reads scale across cores

- **Wording:** "Reads scale across cores. Readers take no lock and write
  no shared memory, and each worker thread owns its connections. Measured
  from 1 to 4 cores: 1.76M → 8.84M GET/s."
- **Evidence:**
  - **The design:** [ADR 0003](adr/0003-concurrency-model.md).
    Thread-per-core networking with `SO_REUSEPORT` and one `epoll` per
    worker, over one table read through seqlocks.
  - **The numbers:** N1 (1 core) and S1 (4 cores), both on a quiet host
    ([results](../benchmarks/results/2026-09-27-standard/README.md)).
  - **The loaded-host scaling run (E1):** 1.39M, 3.06M, 4.98M and 5.99M
    GET/s at 1, 2, 4 and 8 cores.
- **Scope:**
  - **The 1-core figure is low on this host.** One-core runs share vCPU 0
    with the VM's interrupt work, so every server grows faster than
    linearly from 1 to 4 cores here
    ([benchmarks/README.md](../benchmarks/README.md)).
  - **More than 4 cores is unmeasured.** kvbench's gRPC client tops out
    near 9.3M requests/s. At 8 cores KvNode reached 9.72M GET/s while its
    cores were only 69% busy.
- **Not claimed:** linear scaling, or any scaling of writes.
  - Writes are serialized by one lock (ADR 0003).
  - In a store-level probe, total Put throughput fell from 4.51M/s with
    one writer to 2.07–2.24M/s with four to seven
    ([golden-services P3](proposals/golden-services.md)).

## 8. Thread-safe by construction

- **Wording:** "Thread-safe by construction: lock-free seqlocked reads,
  serialized writes of immutable records, and quiescent-state
  reclamation. Readers see the old value or the new one, never a mix.
  Torture-tested and checked end to end with zero torn values."
- **Evidence:**
  - **The design:** [ADR 0003](adr/0003-concurrency-model.md).
  - **`server/tests/test_store_mt.c`:** 8 readers and 4 writers, with
    relocations near a full table and constant reclamation. Zero errors.
  - **The end-to-end concurrency and streaming-race phases of `make itest`:**
    errors 0 and torn 0 on every build. The CI log shows them.
  - **The .NET `ConcurrentCallsOverOneClientStayConsistent` test.**
  - **[ADR 0005](adr/0005-batched-get-prefetch-and-worker-pool.md):** why
    batched GETs stay linearizable.
- **Not claimed:** a formal proof. The guarantees are argued in the ADRs
  and tested, not machine-verified.

## 9. gRPC from scratch, in assembly

- **Wording:** "gRPC from scratch, in assembly. KvNode's own HTTP/2 engine
  passes h2spec 146/146, where nghttp2, which it replaced, passes 138. It
  doubled the requests a core serves. There is no gRPC, HTTP/2 or protobuf
  library in the server; glibc provides only libc services."
- **Evidence:**
  - **The engine:** [ADR 0007](adr/0007-native-http2-engine.md). Its
    results table records 2.03× the nghttp2 build's GET/s on one core
    (N1).
  - **The removal of nghttp2:** [ADR 0008](adr/0008-remove-nghttp2-from-the-server.md).
  - **The `h2spec` phase of `make itest`,** run on every build.
  - **Interoperability:** the .NET suite has 380 tests. Its integration
    tests call the real image through the standard `Grpc.Net.Client`.
  - **What glibc provides:** the `extern` lines of `server/src/*.asm`
    (allocation, `memcpy`, threads, signals, `getenv`, clocks).
- **Scope:**
  - server side only;
  - h2c (cleartext HTTP/2) only;
  - response headers are precomputed, so there is no general HPACK
    encoder.

## 10. CPU-accelerated O(n) streaming

- **Wording:** "CPU-accelerated streaming. Upload and Download copy each
  chunk and compute its CRC-32C in one pass, at memory speed. One core
  takes ~11 GB/s of 1 MiB uploads."
- **Evidence:** [ADR 0010](adr/0010-streaming-upload-download.md) §6:
  - about 11 GB/s of 1 MiB uploads and about 6 GB/s of 1 MiB downloads
    per server core, over loopback;
  - uploads 1.56–2.30× and downloads 1.22–1.28× the first streaming
    build.
- **Not claimed:** "many" accelerated O(n) operations. Today they are the
  two streaming calls and the snapshot load at startup.
  - The functions plan has measured prototypes of more, but none is
    shipped:
    - Iterate: 54M keys/s per thread, 340M keys/s on 7 threads;
    - JsonGet: 2.4–2.8 GB/s;
    - SHA-256 with SHA-NI: 2.5 GB/s.
  - The [proposals](proposals/functions-plan.md) record them.
