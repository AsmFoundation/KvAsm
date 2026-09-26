# ADR 0006: SIMD tag scans by default; private L1/L2 record caches measured and left off

- **Status:** accepted
- **Date:** 2026-09-26

## Context

The request was multi-level ("L-level") caching of keys and values, SIMD
and non-SIMD scans integrated with the fast read path, and a benchmark for
them that is separate from the cross-system comparison.

That separation is necessary. On one core a GET costs about 1.4 µs of
server time. The store accounts for only about 0.1–0.2 µs of it; nghttp2
and the kernel take the rest (profile in ADR 0007). Store-level differences
disappear in that noise, so they must be measured at the store's own API.

The lookup path before this change:

- two candidate buckets per key;
- a SWAR match of one bucket's 8-bit tags at a time (the second bucket only
  when the first misses);
- an exact key compare.

The batched GET path prefetches both buckets at parse time and the
candidate records in a second pass (ADR 0005).

## Decision

### 1. Tag scan: SSE2 by default, SWAR kept

`kv_cuckoo_get_sse2` loads both buckets' tag words into one XMM register
and matches all 12 slots with one `PCMPEQB` and `PMOVMSKB`. The record
prefetch stage does the same.

SSE2 belongs to the x86-64 baseline, so KvNode's rule of "no SIMD
requirement" still holds: every x86-64 CPU runs it.

`KVNODE_SCAN=swar|sse2` selects the path at startup. A predictable branch
on a read-only byte dispatches it. Writers always use SWAR.

### 2. Private L1/L2 record caches: built, measured, off by default

`hcache.asm` gives each worker two direct-mapped levels of 32-byte entries
in front of the shared table:

- Each entry holds the key's two hashes, the record pointer and a
  generation.
- `KVNODE_CACHE_L1_BITS` and `KVNODE_CACHE_L2_BITS` size the levels. The
  default 0 means off.

How cached pointers stay correct:

- The table carries an array of invalidation generations, enabled only when
  caches are configured.
- Every replace or delete bumps the key's generation. That happens after
  the index update and before the old record is retired.
- A fill reads the generation before its table lookup.
- A hit is used only if both hashes match and the generation is unchanged.
- Under x86-TSO and QSBR, a cached record can therefore never be stale
  after a completed write, and never freed while in use. The proof is in
  the `hcache.asm` header.
- Keys are still compared exactly on every hit.

### 3. A store-level benchmark: `bench_store`

It calls the server's exact functions on one pinned thread:

- **Working sets** sized to each level of the memory hierarchy: 256, 8 Ki,
  128 Ki, 1 Mi and 4 Mi keys, which are L1d, L2, L3, DRAM and DRAM with TLB
  pressure.
- **Patterns:** a dependent chain, independent lookups, the batched GET
  pipeline in batches of 16 and 64, Zipf 0.99 variants, and a Zipf mix with
  10% puts.
- **Method:** variants run interleaved in rotated order, 7 repetitions,
  with paired ratios. Every answer is verified.
- `make test` runs its quick mode, so every variant's correctness is checked
  on each build.

## Measurements

Full output: [benchmarks/results/2026-09-26-store](../../benchmarks/results/2026-09-26-store/README.md).

The figures below are paired ratios to SWAR with no cache, where below 1
means faster, followed by how many of 7 repetitions were faster. "b16" is
the server's batched GET pipeline.

| Working set | SSE2, b16 | SSE2, single lookups | L1+L2 cache, b16 | L1+L2 cache, Zipf b16 |
|---|---|---|---|---|
| L1d (256 keys) | 0.985 (5/7) | 1.040 (0/7) | 0.990 (4/7) | 0.976 (7/7) |
| L2 (8 Ki) | 0.982 (7/7) | 1.088 (0/7) | 1.159 (1/7) | 1.140 (0/7) |
| L3 (128 Ki) | 0.964 (7/7) | 1.086 (1/7) | 1.311 (0/7) | 1.184 (0/7) |
| DRAM (1 Mi) | 0.974 (5/7) | 1.095 (0/7) | 1.261 (0/7) | 1.181 (0/7) |
| DRAM (4 Mi) | 0.954 (6/7) | 1.102 (0/7) | 1.220 (0/7) | 1.122 (0/7) |

- **SSE2 wins where the server looks keys up.** Every GET goes through the
  batched pipeline, where SSE2 is 2–5% faster from L2 to DRAM. Lone,
  unprefetched lookups are 4–10% slower because SSE2 always reads both tag
  lines. The server never does those, so SSE2 is the default.
- **Private caches lose to the CPU's own caches.** Hot buckets and records
  already sit in L1/L2. A second, software level adds a probe, a generation
  load and fills, and saves almost nothing. The only gains:
  - independent Zipf lookups when everything fits in L1d (0.91);
  - batched Zipf at the L1d size (0.976).

  Everywhere else the caches are 7–46% slower.
- **Deeper batches do not help.** b64 costs the same as b16 at every size
  (DRAM: 90.6 vs 88.7 ns). Sixteen overlapping lookups already hide DRAM
  latency, so a worker-wide batch across connections is not worth its
  latency cost.
- In absolute terms, a batched GET lookup costs about 89 ns at 1 Mi keys,
  including the SipHash. That is the store's share of a request on one
  core, and the budget the HTTP/2 engine works around (ADR 0007).

## Consequences

- **Default behavior changes** only in the tag scan, and the results are
  identical.
- **Invalidation cost:** when caches are enabled, writers pay one plain
  increment per replace or delete. With caches off, nothing changes.
- **Tests:**
  - `test_store_mt` gained cached and SSE2 phases with a freshness check.
    Once a write returns, no reader may observe an older version, which is
    exactly what a stale cache would violate.
  - The end-to-end suite runs once more through tiny private caches, with
    SWAR.
- **A latent bug surfaced and was fixed.** `store_reclaim_step` skipped
  its forced rescan when the cached bound already covered the limbo head.
  That broke its documented "force always rescans" contract and could leave
  retired records in limbo at shutdown. The new timing exposed it in
  `test_store_mt`.

## Alternatives rejected

- **AVX2/AVX-512 scans.** 16 bytes already cover both buckets' tags, so
  wider vectors match nothing more. They would also add a CPU requirement
  and need runtime dispatch.
- **SIMD key compare.** For keys under 16 bytes a 16-byte load can read
  past the key. That is safe only with page-boundary checks. The existing
  overlapping 8-byte compare is already about four loads.
- **Negative caching and set-associative levels.** These refine a design
  that loses to the hardware caches at every measured size beyond L1d; they
  would not change the conclusion.
