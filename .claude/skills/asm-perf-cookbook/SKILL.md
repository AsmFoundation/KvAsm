---
name: asm-perf-cookbook
description: Cookbook of x86-64 CPU, assembly and Linux-systems optimization techniques for KvNode's hot paths (memory latency hiding, branches, SWAR, allocation, syscalls, concurrency, protocol parsing, code layout, measurement), each with why/when/KvNode status. Use when optimizing KvNode or any low-level x86-64 code, when a profile needs interpreting, or when planning performance work. It is a lead of ideas, never the limit of what to try.
keywords: kvnode performance, assembly optimization, x86-64 optimization, cpu optimization, nasm performance, prefetch, swar, cache miss, perf profile
keywords-sparse: make kvnode faster, optimize assembly, speed up server, reduce cache misses, cpu tricks, low level optimization, hot path
---

# x86-64 performance cookbook (KvNode)

> **A lead, not a limit.** Everything here is a *starting point*. The list is
> not exhaustive and never a reason to stop looking. Profile first, trust
> measurements over this page, look beyond it (manuals, papers, other
> systems), and add every technique that proves itself back here with its
> evidence.

## How to use it

1. **Measure before touching code.**
   - `perf record -e cpu-clock -F 4999 -p <pid>` works inside WSL2/VMs
     without PMU access.
   - `perf annotate --stdio -s <symbol>` shows *which instruction* is hot.
     A hot load right after a pointer dereference means a cache miss.
   - Read the user/kernel split from `/proc/<pid>/stat` (utime vs stime).
2. **Pick candidates from the profile**, using the lists below. Estimate the
   ceiling first (Amdahl): a function at 4% of the time can never give more
   than 4%.
3. **Change one thing at a time** and keep every variant's binary.
4. **Ablate honestly.**
   - Run variants **interleaved and in rotated order** (a Latin square), so
     drift and position effects cancel.
   - Take ≥ 6 repetitions per variant (the fewest at which a paired sign
     test can reach p < 0.05) and compare *within* repetitions.
   - Lesson paid for in this repo: with a fixed A-B-C-D order, the stream
     cache looked 1–16% slower in all 5 repetitions at 4 cores. The rotated
     re-run showed no consistent effect.
   - Check the host first. Other programs competing for the cores were the
     largest noise source measured here (below, §10).
5. **Re-verify correctness** after every change: `make test itest`, which
   includes the torn-value detector, h2spec, and the reset-in-batch test.
6. **Record the outcome** here: ✅ applied (with its measured effect),
   🧪 applied but effect within noise, 🔜 candidate, ✗ rejected (with why).

## Cost model (orders of magnitude, modern x86-64 desktop)

- **Caches:** L1 hit ~4–5 cycles, L2 ~14, L3 ~40–80.
  - DRAM ~80–120 ns; more under virtualization (nested page walks).
  - ~16 line-fill buffers per core: up to ~16 misses can overlap, and hiding
    latency means using them.
- **Branches:** a mispredict costs ~15–20 cycles (a full pipeline refill).
- **Atomics and sharing:**
  - An uncontended `lock` RMW is ~20 cycles.
  - A contended cache line moves between cores in 50–100+ ns, more across
    P/E-core clusters.
- **Syscalls:** ~100–300 ns round trip with mitigations and virtualization.
  Batch them.
- **TLB:** reach with 4 KiB pages is a few MiB; 2 MiB pages cover GBs.
- **KvNode's measured budget** (1 core, GET, 1M keys, before optimizing):
  ~2.2 µs/request.
  - Kernel TCP 31%.
  - nghttp2 ~35% of user time.
  - Cuckoo lookup misses ~25% of user time.
  - malloc/free ~12% of user time.
- **After the native HTTP/2 engine** (same load, 2026-09-26): ~620-650 ns
  per request, 1.6M GET/s per core.
  - Kernel (read + sendto over loopback, one each per 16 requests) ~75-79%.
  - User ~140 ns: the store's two prefetch stages ~43% of it (DRAM
    latency, already overlapped), SipHash ~6%, HTTP/2 ~11%.
  - Next levers: fewer bytes per response, a huge-page record slab (§1),
    io_uring where seccomp allows it (§6).
- **Kernel-inclusive profile, nghttp2 gone** (2026-09-26, one worker on a
  pinned vCPU; `benchmarks/results/2026-09-26-profile-memory`):
  - *Blocking client* (the standard benchmark): 1.72M GET/s, 582 ns per
    GET, 75% kernel. **48% of the core is the co-located client's wakeup**
    (the server's `sendto` → loopback softirq → `sock_def_readable` →
    `__wake_up_common_lock`: a cross-vCPU IPI under WSL2), and 15% is
    TCP/IP.
  - *Busy-polling client* (the server's own cost, closer to remote
    clients): 2.62M GET/s, 382 ns per GET.
    - Kernel 177 ns: TCP/IP 86, syscall/VFS 16, copies 15, slab 11.
    - User 205 ns: store 93 (prefetch stages ~56 of it, stalls; SipHash
      19), HTTP/2 56, gRPC and protobuf 21, glibc copies 20, event loop
      10, pool 8.
  - Next levers: the co-located wakeup (a topology artifact; for same-host
    deployments see §6 busy polling), store address translation (a
    huge-page record slab, §1), SipHash (§8, §4).
- **After flushing once per batch** (ADR 0009, concurrent A/B, idle VM):
  2.24M GET/s on one core with 8 connections per blocking client thread,
  493 ns of server CPU per GET (610 before). The remaining gap to the
  busy-polling client (~280 ns/GET) is the unavoidable wakeup per sleeping
  client thread per burst.

## 1. Memory: hide latency, fit the hierarchy

- **Batch independent lookups and prefetch them** (memory-level parallelism)
  - *What:* split each operation into stages (hash → prefetch bucket →
    prefetch record → use) and run each stage across a whole batch.
  - *Why:* a batch's cache misses then overlap in the fill buffers instead of
    serializing.
  - *When:* the working set exceeds the LLC and requests arrive in batches
    (pipelining, HTTP/2 multiplexing).
  - *KvNode:* ✅ `kv_store_prefetch` at parse time, then
    `kv_store_prefetch_records`, then execution in `kv_grpc_run_batch`.
    **Paired median +28% at 4 cores, +35% at 1 core, faster in 8 of 8
    rotated repetitions at both budgets** (ADR 0005). The standard benchmark
    then measured 3.02M GET/s at 4 cores and 5.18M at 8.
- **One cache line per probe**
  - *What:* size and align a hash bucket to 64 bytes. Keep its tags in one
    word next to the pointers.
  - *KvNode:* ✅ 64-byte cuckoo buckets: at most 2 lines per lookup.
- **Keep hot and cold data apart**
  - *What:* move fields the fast path reads into one line; move rare ones
    elsewhere.
  - *Watch:* struct growth. Adding cold fields to a hot struct costs
    bandwidth on every request.
- **Huge pages for large tables**
  - *What:* `madvise(MADV_HUGEPAGE)` (transparent) or hugetlbfs, so TLB reach
    covers GBs.
  - *Verify, don't assume:* read `AnonHugePages` in `/proc/PID/smaps` (not
    `VmRSS` in `/proc/PID/status`, which the 5.15 WSL2 kernel updates
    lazily per thread) and the `thp_*` counters in `/proc/vmstat`.
  - *KvNode:* ✅ the cuckoo table — but only since it is prefaulted. With
    `MADV_HUGEPAGE` alone a full 16 MiB table had **0 KiB** of huge pages:
    its first touches were lookups (reads → huge zero page) and the inserts
    that followed split them (`thp_split_pmd` +9). Writing one byte per
    2 MiB region at creation gives 7 huge pages (14 MiB, the aligned
    interior).
  - 🔜 records live on glibc malloc pages, which got 0 KiB of huge pages in
    every test here: main and thread arenas alike, even with
    `GLIBC_TUNABLES=glibc.malloc.hugetlb=1`, because arenas grow in small
    `mprotect` steps and no 2 MiB region is whole at fault time. A record
    slab carved from prefaulted 2 MiB chunks would cut a page walk per GET
    *and* memory per key.
  - *Measured (E1, 100-byte values, 116 raw bytes):* KvNode 178 B/key,
    Dragonfly 159, Redis 174.
  - *Measured (E6, 16 KiB values):* 19.5 KB/key, about 3 KB above the
    payload plus headers, against Dragonfly's 16.7 KB. Worth investigating
    first: glibc arenas with 4 writer threads are a suspect.
- **Prefault what you will touch**
  - *What:* `MAP_POPULATE` or touch at startup, so first requests don't pay
    page faults.
  - *KvNode:* ✅ the table: one write per whole 2 MiB region at creation,
    mainly to get real huge pages (see above); the table is resident from
    startup (16 MiB at the default capacity).
- **Avoid pointer chasing**
  - *Evidence:* the bake-off (ADR 0001). A branchless binary trie was
    3.8–5.9× slower at 4M keys: one dependent miss per level.
- **No false sharing**
  - *What:* pad per-thread or frequently written data to 64 B, or 128 B
    because the L2 prefetcher pulls line pairs.
  - *KvNode:* ✅ QSBR reader slots, per-worker structs.
- **Non-temporal stores** (`movnti`, `movntdq`)
  - *When:* large copies that won't be read again soon (bulk loads,
    snapshots). They skip cache pollution.
- **Software prefetch hints**
  - `prefetcht0` (all levels), `prefetchnta` (streaming), `prefetchw`
    (write intent; needs PRFCHW).
  - A prefetch never faults: stale or freed pointers are safe hints.
- **Software caches in front of a shared structure** (per-thread L1/L2 of
  hot entries)
  - *Catch:* the hardware caches already keep hot lines close. A software
    level adds a probe, a validation load and fills. It wins only when the
    whole hot set fits in L1d.
  - *KvNode:* ✗ as a default. `hcache.asm`, generation-validated, measured
    7–46% slower from the L2 working set up (`bench_store`, ADR 0006). It
    stays available and off.
- **Prefetch every line the consumer touches**
  - *What:* a record that starts on any 16-byte boundary and is S bytes
    long spans up to ⌈(S + 48) / 64⌉ lines. Prefetch them all, or the first
    real use of the tail line pays the full DRAM latency.
  - *Evidence:* a 16-byte key plus a 100-byte value makes a 140-byte
    record, which always spans 3 lines, and pass 1 prefetched 2. The value
    copy (glibc `memmove`) was then 4.2% of a busy-polled core; it is 1.05%
    with `[rec + 128]` also prefetched.
  - *Catch:* the size is unknown at prefetch time (the header is in the
    line being fetched), so a small record wastes the third prefetch, and
    each prefetch holds a fill buffer. The prefetch stage's own share rose
    from 8.2% to 9.5%.
  - *KvNode:* 🧪 three lines (ADR 0005 amendment). Paired concurrent A/B,
    6 blocks: 1.001× with a blocking client, 1.025× busy-polling (4/6
    blocks), 0.993× with 16-byte values. Within noise; kept for the stall
    it removes. 🔜 exact coverage: carry a size class in the slot's unused
    pointer bits, so pass 1 prefetches exactly the record's lines.
- **Batch depth:** beyond about 16 independent misses, deeper batches buy
  nothing. The fill buffers are already busy.
  - *KvNode:* ✗ measured: b64 ≈ b16 at every working set (ADR 0006).

## 2. Branches and control flow

- **Branchless selection**
  - *What:* `cmov`, `setcc`, and `sbb reg,reg` masks instead of
    unpredictable branches.
  - *KvNode:* ✅ tag-0 fixup (`cmovz`), bake-off trie descent (`bt`/`sbb`).
- **Order checks by probability**
  - *What:* the common case falls through; errors and rare paths jump away.
- **Tail calls**
  - *What:* `jmp` into the final callee (it keeps the caller's stack
    alignment).
  - *KvNode:* ✅ `kv_store_get` → `kv_cuckoo_get`, `stream_unlink_free`.
- **Direct calls**
  - *What:* static linking removes PLT indirection.
  - *KvNode:* ✅ static-PIE. No function-pointer callbacks left in the
    request path since nghttp2 left the server (ADR 0008); hpack.asm's
    per-field handler is a direct call.
- **Macro-fusion**
  - *What:* keep `cmp`/`test` directly before `jcc`, so the pair decodes as
    one uop.
- **Loop and entry alignment**
  - *What:* align hot loop heads (16/32 bytes) for the decoded-uop cache.
  - *Watch:* Skylake's JCC erratum (branches crossing 32-byte boundaries).

## 3. Integer, bit and SWAR tricks (no SIMD required)

- **Exact SWAR byte match**
  - *What:* `~(((x & 0x7F…) + 0x7F…) | x | 0x7F…)` marks equal bytes with no
    false positives.
  - *Avoid:* the popular `(x - 0x01…) & ~x & 0x80…` gives borrow false
    positives (measured 5.6%).
  - *KvNode:* ✅ cuckoo tags, record prefetch stage.
- **Clear the lowest set bit without BMI:** `lea t, [x-1]; and x, t`.
  *KvNode:* ✅.
- **Multiply-shift (Fibonacci) mixing:** `x * 0x9E3779B97F4A7C15 >> k`.
  *KvNode:* ✅ alternate-bucket offset.
- **Power-of-two tables:** `and mask` instead of `div`. Division by
  constants becomes a multiply by a magic reciprocal.
- **Overlapping loads**
  - *What:* compare or copy 9–16 bytes with two overlapping 8-byte loads,
    with no byte loop and no overread.
  - *KvNode:* ✅ `kv_keyeq`, SipHash tail.
- **Carry-flag tricks:** `sbb reg,reg` gives a 0/-1 mask; `adc` fuses a
  counter into a compare.
- **Zero-extension:** 32-bit writes zero the upper half. Use `xor r32,r32`
  (a zero idiom) and avoid 8/16-bit partial-register merges.
- **Varint fast paths:** handle the one-byte case (< 128) inline and fall
  back to a loop.
- **bsf/bsr with a zero check** instead of `tzcnt`/`lzcnt` (BMI1/LZCNT are
  not baseline x86-64).

## 4. SIMD (optional, never required: KvNode's rule is "no SIMD requirement")

- **SSE2 is part of baseline x86-64.** `pcmpeqb` + `pmovmskb` compare 16
  bytes at once without raising the requirement.
  - *KvNode:* ✅ both buckets' tag words in one XMM register: one compare
    for 12 slots. That is 2–5% faster per lookup in the batched pipeline
    from L2 to DRAM, but 4–10% slower for lone unprefetched lookups (it
    always reads both lines). Default via `KVNODE_SCAN=sse2`; SWAR stays
    selectable (ADR 0006).
  - Measure the pattern that matters: the winner flips between batched and
    lone lookups.
- **AVX2/AVX-512** only behind CPUID dispatch (a function pointer set at
  startup), with `vzeroupper` before calling non-AVX code. The dev host
  (i9-13900K) has AVX2, BMI2, GFNI, VAES and SHA-NI, but no AVX-512: a
  hybrid CPU's E-cores lack it, so it is fused off.
- **Copies:** `rep movsb` is fast with ERMS/FSRM for medium copies; small
  fixed sizes are best as plain moves.
  - *KvNode:* ✅ glibc's IFUNC picks `__memmove_avx_unaligned_erms` and
    `__memset_avx2_unaligned_erms` here; the response head (5-13 bytes)
    and the precomputed header blocks are inline overlapping moves.
- **First find out whether the time is SIMD-shaped at all** (measured
  2026-09-26, one core, busy-polling client so only server work shows,
  382 ns per GET):
  - kernel (TCP/IP, syscalls, copies to and from user, slab): 46%. No
    SIMD lever;
  - the store: 24% (93 ns). Most of it is the two prefetch stages
    stalling on address translation and DRAM: under WSL2 every walk is
    nested, and guest huge pages need not be host huge pages. Latency,
    not arithmetic;
  - SipHash-128: 4.9%, the only arithmetic hot spot;
  - glibc `memmove`/`memset`: 5.1%, already AVX2, and mostly a DRAM miss
    rather than copy work (see §1, "Prefetch every line the consumer
    touches");
  - HTTP/2 (14.7%) and gRPC (5.6%): spread over dozens of small
    functions; the Huffman work that was SIMD-shaped is skipped by the
    header-block cache.
  - **Ceiling:** SIMD can only shrink the ~10% that is arithmetic or
    copying, and most of that already is SIMD.
- **Candidates, ranked by measured ceiling:**
  - 🔜 AVX2 4-lane SipHash across a read's GET batch: ceiling 4.9%;
    estimate −10 ns per GET (~3%). *Catch:* hashing then moves from parse
    time into the batch, which shortens the bucket prefetch's lead time;
    interleave (hash 4 → prefetch 4) and measure. Behind CPUID, with the
    scalar path kept and cross-checked in `test_siphash`.
  - 🔜 inline SSE2 copy for values up to 128 bytes in `reply_fast`
    (overlapping 16-byte moves instead of a `memcpy` call): what is left
    of `memmove` after the three-line prefetch is ~1% of the core. Expect
    <1%.
  - 🔜 SSE2 `blocks_equal` (the header-block cache compare, ≤128 bytes):
    <0.5%.
  - ✗ wider tag scans: one SSE2 compare already covers both buckets'
    12 slots; AVX2 has nothing more to compare.
- **Carry-less folding for CRCs (PCLMULQDQ / VPCLMULQDQ)**
  - *What:* replace a chain of `crc32` instructions (3-cycle latency: ONE
    dependency chain, ~2.7 bytes/cycle) by folding 128-bit blocks forward
    with two 64x32 carry-less multiplies per block and precomputed
    x^e mod P constants; eight independent lanes (four YMM or eight XMM)
    hide the multiplier's latency; finish the last block with two `crc32`
    instructions from a zero register (no Barrett step). Constants from
    the NASM preprocessor (bit-reflected x^(D+63), x^(D-1) mod P for fold
    distance D); prove the math in a scripting language first (the
    2026-09-30 Python prototype had it right before any assembly).
  - *KvNode:* ✅ `crc32c.asm` (Upload/Download, ADR 0010), dispatch
    vpclmul > pclmul > sse42 > table by CPUID + XGETBV. On a 16 KiB chunk
    in cache, fused with the copy: 88 GB/s (vpclmul), 45 (pclmul), 14.8
    (crc32 chain), 3.2 (tables); memcpy 70-210. **Server A/B, 6/6 blocks
    each, p=0.031: uploads +7..+13%, downloads +22..+25%** — the chain had
    been 49% (upload) and 34% (download) of a worker's CPU. The .NET
    client got the same algorithm (Pclmulqdq[.V256]): client CPU/GB −9%.
  - *Catch:* where both sides stream from DRAM every variant hits the
    same ~12-13 GB/s wall — the gain is in cache, and the transfer paths
    always have one side hot (the read buffer, the output buffer).
- **Non-temporal stores into a cold, not-soon-read destination**
  - *What:* `vmovntdq`/`movntdq` skip the read-for-ownership of every
    destination line; the copy streams to memory instead of reading it
    first. `sfence` before publishing (NT stores are weakly ordered).
  - *Why:* a copy into cold lines is bounded by those reads, not by the
    copy: memcpy 16K hot → 64 MiB span measured 16-18 GB/s.
  - *KvNode:* ✅ `kv_crc32c_copy_nt` for uploads of ≥ 64 KiB (the record
    is cold and nobody reads it until a later Get/Download). Microbench
    (16 KiB calls, hot source, 64 MiB of destinations): **40-44 GB/s vs
    16-18 temporal (2.4x)**; server A/B 6/6, p=0.031: **uploads +13%
    (64 KiB values), +31% (1 MiB), +25% (16 MiB)**; download control
    0.997. **Align NT stores to whole cache lines**: 32-byte-aligned
    blocks straddling lines ran 32-39 GB/s, 64-byte-aligned 40-44 (a
    write-combining buffer only streams out cheaply when full).
  - *Catch:* into a HOT destination NT is 2x slower (42 vs 88 GB/s), and
    the data leaves the cache: never for the download's output buffer
    (measured: 16.7 vs 18.8 GB/s from cold records into a hot buffer).

## 5. Allocation

- **Per-thread size-class pools**
  - *What:* LIFO free lists (a freed block is reused while still cached),
    bump-carved chunks, no locks (single owner).
  - *KvNode:* ✅ `pool.asm`: Streams, request buffers, stream maps, HPACK
    rings and entries (it first served nghttp2 through its `nghttp2_mem`
    hook). Rotated ablation: paired median +16% at 1 core and +10% at 4
    cores, faster in 6 of 8 repetitions at both. Probably positive, not
    conclusive on a noisy host; kept because it is small, tested, and gives
    each worker full control of allocation locality. Profiled 2026-09-26:
    ~2% of a core (8 ns per GET).
- **Slabs for fixed-size records:** lower overhead, no fragmentation, can
  sit on huge pages. *KvNode:* 🔜 records (see §1: glibc arenas never got
  a huge page here).
- **Borrow instead of copy:** a request body that arrives whole in one
  frame can be used in place while the read buffer is still live.
  *KvNode:* ✅ native engine (`ST_BORROWED`): no request buffer, no copy
  for a unary call whose DATA frame carries END_STREAM — valid because
  dispatch and the GET batch both finish before the next read.
- **Request-scoped arenas:** bump-allocate everything one request needs and
  drop it in one step.
- **Skip zeroing:** `calloc` zeroes every byte. Initialize fields explicitly
  when all of them are set anyway.
  - *KvNode:* ✗ for the Stream. Inline SSE2 zeroing (12 × 16 bytes)
    instead of `kv_pool_calloc`'s glibc `memset` saved nothing, measured
    exactly with callgrind: 1,424 vs 1,428 instructions per GET. glibc's
    AVX2 memset is already cheap for 184 bytes.
- **No buffer for a reply that fits on the stack**
  - *What:* build a tiny message on the stack and let the framer copy it,
    instead of allocating a buffer the stream frees later.
  - *KvNode:* ✅ Put, Delete and health replies are 5–7 bytes and now go
    through `kv_h2n_reply_parts` like a GET miss. Callgrind: SET 1,812 →
    1,789 and health check 867 → 846 instructions per request (−2.4%),
    one pool allocation and one free fewer each
    (`benchmarks/results/2026-09-27-second-pass`).
- **Size to the class:** a 240-byte first buffer plus a 16-byte header fills
  a 256-byte block exactly. *KvNode:* ✅ `MIN_REQ_CAP`.

## 6. System calls and I/O (Linux)

- **Batch syscalls**
  - *What:* one `send` per connection per loop pass (coalesced frames), big
    `recv` chunks.
  - *KvNode:* ✅ output coalescing; 256 KiB reads (`READ_CHUNK`, was 64
    KiB). A read that completes several uploads sends their responses in
    one `sendto` (one client wakeup) instead of one each, and the
    partial-frame carry is copied 4x less per byte: **uploads of 64 KiB
    values 1.88x (6/6, p=0.031; server CPU/byte −47%), 1 MiB +13%, 16 MiB
    +5%**; GETs flat. 512 KiB and 1 MiB reads lost 7-17% on large uploads
    — the buffer stops fitting in L2 beside the socket data the kernel
    copies in — so the size is a measured optimum, not "bigger is better".
- **Flush once per event-loop pass, after all of its input** (Redis's
  `beforeSleep` writes)
  - *What:* phase 1 reads and processes every ready connection and frames
    the answers into buffers; phase 2 sends all of them back to back.
  - *Why:* whoever sends wakes the receiver if it sleeps. On one machine
    that wakeup runs on the sender's CPU (a VM-exiting IPI under a
    hypervisor). A client thread with several connections, served one
    connection at a time, falls asleep between spaced-out sends and is
    woken once per connection. Sent in one burst, it wakes once.
  - *Catch:* a response waits for its batch's input to be processed
    (bounded by the epoll batch size). Destroyed connections must be
    dropped from the batch before phase 2, and anything that can destroy
    *other* connections (a drain) must wait until after phase 2.
  - *KvNode:* ✅ `worker_main` (ADR 0009). Concurrent paired A/B, idle
    VM, 6/6 blocks each. **The gain scales with the connections a client
    thread owns:**
    - 8 per thread: **+25.7% GET/s** [1.217–1.338]; p50 −20%, p99 −21%;
      server CPU/request −20%; client wakeups/request −36%;
    - 4 per thread (the harness's kvbench): +11.8%;
    - 1 per thread: +4.1%, wakeups identical;
    - busy-polling clients: +2.6%, p99 −14%.
    
    A first run (possibly overlapping another session's benchmarks) gave
    the same 8-per-thread result: +26.3%.
- **Gather instead of copy**
  - *What:* `writev`/`sendmsg` over the library's frame buffers instead of
    `memcpy` into one buffer.
  - *KvNode:* ✅ moot: h2n.asm frames responses straight into the output
    buffer (a GET's value is the one copy, record → DATA frame), so the
    nghttp2-era `mem_send2` copy loop is gone.
- **Accept and steering**
  - *What:* `SO_REUSEPORT` listeners per worker.
  - *KvNode:* ✅, plus 🔜 BPF steering (`SO_ATTACH_REUSEPORT_CBPF`) to
    balance by CPU instead of by hash.
- **io_uring** (batched submission, multishot recv, registered buffers)
  - *Catch:* blocked by Docker's default seccomp (≥ 25).
  - *KvNode:* ✗ as the default (portability); 🔜 as an opt-in backend.
- **Busy polling** (`SO_BUSY_POLL`, epoll busy-poll): lower latency, higher
  CPU. Useful for latency-SLO deployments.
  - *Measured (2026-09-26), the mirror image:* over loopback, whoever
    sends wakes the peer. KvNode's `sendto` ran the client's TCP receive in
    softirq and woke the kvbench thread asleep in `epoll_wait`, via a
    cross-vCPU IPI under WSL2. That was **48% of the server's core**
    (`__wake_up_common_lock` ← `sock_def_readable`, call graph verified),
    ~280 of 582 ns per GET. A busy-polling client (`kvbench --busy-poll`)
    removes it: 1.72M → **2.62M GET/s on one core**.
  - *KvNode:* ✅ partly removed at the source. Flushing once per batch
    (§6 above, ADR 0009) cut client wakeups by up to a third: +12–26% GET/s
    by client shape. What remains (~22 wakeups per 1k requests with 8
    connections per thread, 125–210 ns/request against a busy-polling
    client) is one wakeup per sleeping client thread per burst. That is
    inherent to co-located sleeping clients; remote clients never cost the
    server this.
  - 🔜 server-side busy-poll as an opt-in for latency SLOs. It would remove
    the server's own wakeups, which the clients pay on their CPUs here.
- **Timekeeping and CPU id:** vDSO `clock_gettime`, coarse clocks, `rdpid`
  for the CPU id. Never a syscall per request. *KvNode:* ✅ once per loop
  pass.
- **TCP knobs:** `TCP_NODELAY` (*KvNode:* ✅), `MSG_MORE`/`TCP_CORK` when
  batching, larger socket buffers for big values.
- **Placement (bare metal):**
  - pin workers (`pthread_setaffinity_np`);
  - `isolcpus`/`nohz_full` for dedicated cores;
  - keep the NIC's IRQs on the same NUMA node.

## 7. Concurrency and memory ordering

- **Readers that write nothing**
  - *What:* seqlocks (odd = writing), validated only when needed.
  - *KvNode:* ✅ bucket versions, re-checked on a miss only.
- **Deferred reclamation:** QSBR/epochs/RCU instead of reference counts on
  the read path. *KvNode:* ✅ QSBR.
- **x86-TSO**
  - Loads are acquire and stores are release for free.
  - Only StoreLoad needs `mfence` or a locked op. *KvNode:* ✅ `xchg` in
    QSBR online.
- **Spin, then sleep:** a futex mutex with a PAUSE spin before parking.
  *KvNode:* ✅ writer lock.
- **No shared counters on hot paths**
  - *What:* per-thread counters summed on read; relaxed atomic stores are
    plain `mov`s.
  - *KvNode:* ✅ worker stats; kvbench progress.
- **Amortize the lock**
  - *What:* group commit (apply a read batch's Puts under one mutex hold),
    or flat combining.
  - *KvNode:* 🔜. Writes are serialized; batching them like GETs is the
    write-path analogue of §1.
  - *Evidence (2026-09-27):* a SET runs only 26% more instructions than a
    GET (1,789 vs 1,425, callgrind), yet it took ~4× the time on the same
    host. PUTs execute at dispatch, outside the GET batch's prefetch
    pipeline, so their bucket and old-record misses serialize. Batching
    would prefetch them at parse time and take the writer lock once per
    read. Measure write throughput on a quiet host first.
- **Share nothing where possible:** per-worker sessions, pools and loops.
  *KvNode:* ✅.

## 8. Protocol and parsing (was KvNode's largest lever; now owned by h2n.asm alone)

- **Zero-copy decode:** fields point into the receive buffer. *KvNode:* ✅
  protobuf keys and values.
- **Deferred, batched execution:** parse everything first, then execute (it
  enables §1). *KvNode:* ✅ per read.
  - Across all readable connections: ✗ for pipelined clients, since b64 ≈
    b16 (ADR 0006). Possibly still worth it for unpipelined clients, whose
    batches are 1.
- **Precomputed responses:** static HPACK blocks and ready-made frame
  headers for the fixed parts of a gRPC reply. *KvNode:* ✅ native engine:
  the first response on a connection inserts content-type and
  grpc-status into the peer's table, then every response is `88 BF`
  (HEADERS) + `BE` (trailers); a GET hit's value is copied from the store
  record straight into its DATA frame.
- **Own the HTTP/2 hot path**
  - *What:* a gRPC-subset HTTP/2 engine removes nghttp2's generic
    machinery (stream hash map, per-header rcbufs and callbacks, priority
    and outbound queues, data providers, a full response encoder).
  - *Catch:* HPACK and flow-control state are per connection, so it must
    own whole connections: no mixing with nghttp2.
  - *Cost:* a security-critical rewrite. It needed h2spec (146/146), an
    HPACK decoder differential-tested against nghttp2, a black-box fuzzer
    and an independent client (grpc-dotnet) before shipping; ADR 0007.
  - *KvNode:* ✅ `h2n.asm`, the only engine since ADR 0008 removed nghttp2
    from the server (binary −162 KB of text, one code path, no fallback).
    Measured, paired (N1-N3,
    `benchmarks/results/2026-09-26-native-final`): 1 core 1.51M GET/s,
    2.03× nghttp2, **1.19× Redis, 1.22× Valkey** (6/6 blocks, weakest
    1.10×); 2.07× nghttp2 at 4 cores (7.80M GET/s in the quieter
    `-native-cache` session, 3.06×); the no-op floor went from 0.41× to
    1.00× a Redis `PING`.
    Profile: the HTTP/2 layer costs ~15-18 ns of user time per request;
    ~75% of a request is now kernel (loopback TCP).
- **Cache whole header blocks**
  - *What:* gRPC clients send byte-identical header blocks for identical
    calls once their HPACK table is warm. Key a cache on (block bytes,
    table generation): equal bytes under an unchanged dynamic table decode
    to the same fields, so replay the recorded effects.
  - *Catch:* the generation must move on every insertion and size update,
    and only blocks whose own decode left the table unchanged are
    recorded.
  - *KvNode:* ✅ per connection, one entry (up to 128 bytes). nghttp2-based
    clients never index `:path`, so without it every request Huffman-
    decoded its path (~23% of user time, ~113 ns); with it the whole
    HTTP/2 layer fell from ~122 to ~18 ns per request.
- **O(1) removal from linear probing:** count displaced entries; while
  the count is 0 no entry can move back, so backward-shift deletion skips
  its scan. *KvNode:* ✅ stream map (client ids are consecutive odd numbers
  and land in their home slots).
- **Cache lookups the library repeats**
  - *KvNode:* 🧪 → removed. The one-entry stream cache existed only for
    nghttp2's callbacks (within noise: paired median 0.99 at 1 core, 0.98
    at 4) and left with it (ADR 0008). h2n.asm's stream map is already one
    load per lookup.
- **Keyed hash choice**
  - SipHash-2-4 is the conservative HashDoS defense (*KvNode:* ✅).
    Profiled 2026-09-26 with a busy-polling client: 4.9% of the core,
    ~19 ns per GET, mostly the 128-bit finalization. Scalar SipHash is
    latency-bound (one dependency chain). Callgrind (2026-09-27): 254
    instructions, **18% of a GET's user-space instructions**, the largest
    single item. It runs at a high IPC, so ~12 ns on a quiet host.
  - SipHash-1-3 (Rust's choice) is cheaper: about 40% fewer rounds. Keep a
    keyed hash either way; the snapshot checksum must keep its own
    function.

## 9. Code layout and the front end

- **Hot/cold splitting:** move error handling out of line; keep the hot
  path contiguous for the i-cache and uop cache.
- **Avoid LCP stalls:** no 16-bit immediates with operand-size prefixes in
  hot loops.
- **Break false dependencies:** zero idioms (`xor`), and watch
  `popcnt`/`lzcnt` output dependencies on some Intel cores.
- **Build C dependencies well:** `-O2`/`-O3`, keep static linking. The
  server's only C dependency is glibc now (ADR 0008); its string functions
  are IFUNC-dispatched to AVX2 on this CPU already.

## 10. Measurement discipline (keeps the cookbook honest)

- **Separate latency from throughput:**
  - microbenchmarks with *dependent* chains measure latency;
  - *independent* operations measure throughput (see `bench_ds`).
- **Validate the client:** a benchmark client must match the ecosystem's
  standard tools (E8), or it biases the comparison.
- **Calibrate instruments:** on WSL2 the cgroup CPU clock reads 6–11% fast,
  more when the host is contended (a vCPU taken away by the hypervisor
  still counts as busy). Measure the factor every session and correct for
  it.
- **Watch the whole machine, not just the containers:**
  - Other programs on the host (an optimization job, an emulator, a game)
    once took 14–23 of 32 logical CPUs. Identical runs then differed by up
    to 3×, and throughput changed within single runs.
  - The same benchmark on a quiet host: CV 1.1–1.7% over ten runs.
  - cgroups cannot see that load. `benchmarks/run.py` samples it per run
    (host busy CPU minus the VM process) and the analysis adjusts for it.
- **Expect residual noise on hybrid CPUs under VMs** (P/E-core placement by
  the hypervisor):
  - pair variants within blocks, rotate their order, and report CVs;
  - never trust a single run.
- **Correctness under optimization:** torn-value detector, h2spec baseline,
  reset-in-batch test, full unit suite. Every optimization passes them
  before its numbers count.
- **When the clock cannot be trusted, count instructions** (callgrind).
  - *Use it for:* user-space changes that remove work, on a busy host.
    Exact counts, independent of load; they overstate time for ALU code
    and understate it for stall-bound code.
  - *Method* (`benchmarks/results/2026-09-27-second-pass`):
    - run the server under valgrind in a helper image sharing the dev
      container's network;
    - zero the counters after the preload with `callgrind_control -z`,
      which needs `--cap-add=SYS_PTRACE`;
    - dump after a fixed workload and divide by exact call counts
      (`get_execute`, `kv_store_put`).
  - *Gotchas:* NASM's DWARF has no function entries, so callgrind shows
    addresses; map them with `nm -n`. A tail-`jmp` target is charged to
    its caller.
  - *KvNode's GET* (1,425 user instructions):
    - SipHash 18%;
    - frame parsing 20%;
    - response framing 16%;
    - stream lifecycle 15%;
    - store lookup 11%;
    - gRPC and protobuf 11%.
- **Profile the kernel too, and read call chains.** `perf` in the dev
  container cannot sample kernel symbols (`perf_event_paranoid=2`). Run it
  from a privileged helper container that shares the dev container's PID
  namespace (`--privileged --pid=container:kvnode-dev`), sampling the
  server's vCPU system-wide (`-a -C <cpu>`). WSL2 has no PMU, so
  `cpu-clock` is the event. Samples cannot land while interrupts are off,
  so a hot `…_irqrestore`/`__wake_up_common_lock` is the cost of the whole
  IRQs-off region; use `-g` to see what that region is.
- **Account for the benchmark topology.** On one machine, the server's
  send wakes the client (and the client's send wakes the server): 48% of
  a KvNode core under WSL2. Profile with `kvbench --busy-poll` to see the
  server's own cost; never mix busy-poll and blocking runs in one
  comparison.
- **Pair concurrently when the order matters.** A sequential A/B (fresh
  server per run, order alternating per block) showed the second run of
  every pair ~10% cheaper per request and slower, whichever variant it
  was: host steal and scheduling, larger than the effect under test.
  Running A and B *at the same time* on two pinned vCPUs with separate
  client vCPU sets, swapping them every block, cancels time-varying noise.
  The instrument that proved the mechanism was the within-run profile
  share (`memmove` 4.2% → 1.05%); a single run's throughput is not
  evidence.
- **Lessons of the streaming session (2026-09-30,
  `benchmarks/results/2026-09-30-streaming`):**
  - *Exact per-process CPU:* `/proc/PID/task/*/schedstat` (first field,
    ns) instead of 10 ms ticks (`kvbench --server-pid`). Under WSL2 it
    reads a pinned busy loop as 104.4% of a CPU: the scheduler clock runs
    fast against wall time, which cancels in ratios but not in absolute
    CPU %.
  - *A process stuck in D state poisons every tool that reads all of
    /proc:* `ps`, `top`, `pgrep -f` and `perf record -a` (which reads
    every process's maps at start) hang forever on its mmap lock and
    become D-state themselves. Profile by PID (`perf record -p`), list
    processes from `/proc/*/stat` only (the probe scripts do), and let the
    user restart the VM.
  - *.NET clients:* by default the thread pool spin-waits for work — 40%
    of a client's samples sat on one spin address, and a download was
    FASTER with `DOTNET_ThreadPool_UnfairSemaphoreSpinLimit=0` because the
    spinners took the cores. Symbolize JIT code with
    `DOTNET_PerfMapEnabled=1` plus `DOTNET_EnableWriteXorExecute=0` (the
    W^X double mapping hides JIT frames from perf maps). Two .NET clients
    run concurrently disturbed each other (pairs swung 0.83-1.15):
    compare .NET builds sequentially in ABBA order, one client alone on
    all E-cores; allocation counts (`GC.GetTotalAllocatedBytes(true)`) are
    exact and need no statistics.
  - *Screen, then confirm:* 2-block screens of four read sizes found the
    optimum (and that bigger lost on large uploads) before one 6-block
    confirmation — cheaper than confirming every candidate.

## Beyond this cookbook

Keep looking. Useful sources:

- Agner Fog's optimization manuals and instruction tables; uops.info.
- Intel's and AMD's optimization reference manuals.
- Denis Bakhvalov, *Performance Analysis and Tuning on Modern CPUs*.
- Papers and designs: MICA (batched KV lookups), Masstree, FASTER (epochs
  and hash index), Seastar (shard-per-core), and the Linux kernel's
  networking fast paths.

Every idea that survives a measured ablation in KvNode belongs back in these
lists, with its number.
