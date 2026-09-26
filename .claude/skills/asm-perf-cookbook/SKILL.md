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
  - *KvNode:* ✅ the cuckoo table. 🔜 records live on malloc pages: a slab on
    huge pages would cut TLB misses *and* memory per key.
  - *Measured (E1, 100-byte values, 116 raw bytes):* KvNode 178 B/key,
    Dragonfly 159, Redis 174.
  - *Measured (E6, 16 KiB values):* 19.5 KB/key, about 3 KB above the
    payload plus headers, against Dragonfly's 16.7 KB. Worth investigating
    first: glibc arenas with 4 writer threads are a suspect.
- **Prefault what you will touch**
  - *What:* `MAP_POPULATE` or touch at startup, so first requests don't pay
    page faults.
  - *KvNode:* 🔜 the table is `MAP_NORESERVE` (lazy). Pre-touching would
    trade RSS for predictable first-minute latency.
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
  - *KvNode:* ✅ static-PIE. Function-pointer callbacks (nghttp2) remain
    inherent.
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
  startup), with `vzeroupper` before calling non-AVX code.
- **Copies:** `rep movsb` is fast with ERMS/FSRM for medium copies; small
  fixed sizes are best as plain moves.

## 5. Allocation

- **Per-thread size-class pools**
  - *What:* LIFO free lists (a freed block is reused while still cached),
    bump-carved chunks, no locks (single owner).
  - *KvNode:* ✅ `pool.asm` behind nghttp2's `nghttp2_mem` hook and for
    Streams. Rotated ablation: paired median +16% at 1 core and +10% at 4
    cores, faster in 6 of 8 repetitions at both. Probably positive, not
    conclusive on a noisy host; kept because it is small, tested, and gives
    each worker full control of allocation locality.
- **Slabs for fixed-size records:** lower overhead, no fragmentation, can
  sit on huge pages. *KvNode:* 🔜 records (see §1).
- **Request-scoped arenas:** bump-allocate everything one request needs and
  drop it in one step.
- **Skip zeroing:** `calloc` zeroes every byte. Initialize fields explicitly
  when all of them are set anyway.
- **Size to the class:** a 240-byte first buffer plus a 16-byte header fills
  a 256-byte block exactly. *KvNode:* ✅ `MIN_REQ_CAP`.

## 6. System calls and I/O (Linux)

- **Batch syscalls**
  - *What:* one `send` per connection per loop pass (coalesced frames), big
    `recv` chunks.
  - *KvNode:* ✅ output coalescing; 64 KiB reads.
- **Gather instead of copy**
  - *What:* `writev`/`sendmsg` over the library's frame buffers instead of
    `memcpy` into one buffer.
  - *KvNode:* 🔜 `conn_flush` copies each `mem_send2` chunk.
- **Accept and steering**
  - *What:* `SO_REUSEPORT` listeners per worker.
  - *KvNode:* ✅, plus 🔜 BPF steering (`SO_ATTACH_REUSEPORT_CBPF`) to
    balance by CPU instead of by hash.
- **io_uring** (batched submission, multishot recv, registered buffers)
  - *Catch:* blocked by Docker's default seccomp (≥ 25).
  - *KvNode:* ✗ as the default (portability); 🔜 as an opt-in backend.
- **Busy polling** (`SO_BUSY_POLL`, epoll busy-poll): lower latency, higher
  CPU. Useful for latency-SLO deployments.
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
- **Share nothing where possible:** per-worker sessions, pools and loops.
  *KvNode:* ✅.

## 8. Protocol and parsing (KvNode's largest remaining lever)

- **Zero-copy decode:** fields point into the receive buffer. *KvNode:* ✅
  protobuf keys and values.
- **Deferred, batched execution:** parse everything first, then execute (it
  enables §1). *KvNode:* ✅ per read.
  - Across all readable connections: ✗ for pipelined clients, since b64 ≈
    b16 (ADR 0006). Possibly still worth it for unpipelined clients, whose
    batches are 1.
- **Precomputed responses:** static HPACK blocks and ready-made frame
  headers for the fixed parts of a gRPC reply.
- **Own the HTTP/2 hot path**
  - *What:* a gRPC-subset HTTP/2 engine would remove most of nghttp2's
    ~35% share (the stream map, header validation callbacks, generic frame
    queues).
  - *Catch:* HPACK and flow-control state are per connection, so it must
    own whole connections: no mixing with nghttp2.
  - *Cost:* a security-critical rewrite that needs fuzzing (AFL++/libFuzzer)
    and h2spec before it can ship. It would revise ADR 0002.
  - *Payoff:* the only path far past about 920k requests/s per core, the
    measured protocol floor:
    - E5: 1,086 ns of server CPU per no-op gRPC request, against 422 ns for
      a Redis `PING`;
    - it is why 1-core KvNode runs at 0.56× Redis.
- **Cache lookups the library repeats**
  - *KvNode:* 🧪 one-entry stream cache. Its effect is within noise
    (paired median 0.99 at 1 core, 0.98 at 4): nghttp2's own internal
    lookups dominate.
- **Keyed hash choice**
  - SipHash-2-4 is the conservative HashDoS defense (*KvNode:* ✅, ~0.8% of
    time).
  - SipHash-1-3 (Rust's choice) is cheaper. Keep a keyed hash either way.

## 9. Code layout and the front end

- **Hot/cold splitting:** move error handling out of line; keep the hot
  path contiguous for the i-cache and uop cache.
- **Avoid LCP stalls:** no 16-bit immediates with operand-size prefixes in
  hot loops.
- **Break false dependencies:** zero idioms (`xor`), and watch
  `popcnt`/`lzcnt` output dependencies on some Intel cores.
- **Build C dependencies well:** `-O2`/`-O3`, optionally PGO/LTO for
  nghttp2 (🔜: measure first), keep static linking.

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
