# Store-level lookup benchmark, 2026-09-26

`bench_store` measures the store's lookup variants directly on one pinned
thread. It uses the server's own functions: SipHash, the two prefetch
stages, then the lookup. The protocol layer is left out on purpose: over
the network the store is only 0.1–0.2 µs of a 1.4 µs request, so variant
differences would drown there.

- **Output:** [`bench_store.txt`](bench_store.txt).
- **Decision and reading:**
  [ADR 0006](../../../docs/adr/0006-store-memory-levels-simd-and-private-caches.md).
- **Reproduce:** `make -C server bench-store`, or
  `server/build/bench/bench_store --cpu 2 --reps 7`.

## Setup

- Intel i9-13900K, WSL2, pinned to vCPU 2. That vCPU reports a 48 KiB L1d,
  2 MiB L2 and 36 MiB L3.
- The host was shared with other programs during the run, so absolute
  numbers are indicative and the paired ratios are the result.
- 16-byte keys and 100-byte values, like the network benchmark.
- **Working sets:** 256 keys (L1d), 8 Ki (L2), 128 Ki (L3), 1 Mi (DRAM),
  4 Mi (DRAM with TLB pressure).
- **Patterns:**
  - `lat`: a dependent chain;
  - `thr`: independent lookups;
  - `b16`/`b64`: the server's batched pipeline;
  - `zthr`/`zb16`: Zipf 0.99;
  - `zmix`: Zipf with 10% puts, so writes invalidate cached entries.
- **Variants:**
  - `swar`, the production default before this run;
  - `sse2`, one compare over both buckets;
  - private caches with L1 at 2^10 entries (32 KiB) and L2 at 2^16 entries
    (2 MiB), in front of either scan.
- 7 repetitions, variant order rotated per repetition, 2,000,000 timed
  operations per run after a warm-up. Every answer was verified; the run
  ended with `bench_store: all checks passed`.

## Results in one paragraph

**SSE2** is faster in the batched pipeline at every size from L2 to DRAM.
Its paired medians are 0.94–0.98, faster in 5–7 of 7 repetitions. It is
slower for lone lookups (1.04–1.18), because it always loads both tag
lines. The server only looks keys up through the batched pipeline, so SSE2
is now the default.

**Private caches** help only when the whole working set fits in L1d
(`zthr` 0.91). Everywhere else they are 7–46% slower, because the CPU's own
caches already keep hot buckets and records close. They stay available and
off by default.

**Deeper batches** gain nothing: `b64` ≈ `b16` at every size.
