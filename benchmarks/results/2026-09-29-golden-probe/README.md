# Golden probe: what stays within 10× of an exact lookup at 10M keys (2026-09-29)

Evidence for
[docs/proposals/golden-operations.md](../../../docs/proposals/golden-operations.md),
answering: "gather all the O(1) or ultra-fast [operations] for 10M items,
including SIMD or any other assembly-level optimizations, thread-safe.
Anything within 10× of exact get / exists in a 10M collection is a golden
standard."

**Run conditions**

- **What:** [`golden_probe.c`](golden_probe.c) on the server's real store,
  run by [`run.sh`](run.sh).
  - `store.asm`, `cuckoo.asm` and `siphash.asm` are assembled fresh with
    the server's flags.
  - Every candidate is prototyped in C next to the store, at the same 10M
    keys.
  - The binary fuse filter is Graf and Lemire's `binaryfusefilter.h`,
    fetched at a pinned commit and SHA-256-checked. It is not committed.
- **Revision:** 5005a13 for the store code. The probe is the committed file.
- **Host:** i9-13900K under WSL2.
  - Hyper-V backs guest memory with 4 KiB pages. THP is `always` inside the
    guest.
  - Single-thread rows run on one pinned P-core vCPU (4). Threads use
    vCPUs 1-7, which are distinct P-cores.
  - 5 repetitions, medians; 3 for the threaded rows.
- **Data:** 10M keys `user:<5 syllables>:<field>`, 20-21 bytes, with
  100-byte values. The table has 2,097,152 buckets at 79.5% slot load.
- **Isolation.**
  - [`output.txt`](output.txt) is a certified attempt. A host-side wrapper
    waited until `docker exec kvasm-dev ps` showed nothing running on
    three polls 5 s apart, then ran `run.sh`. `docker events` for the run's
    window (09:54:30-09:56:29 UTC) shows only its own exec.
  - The Windows host had 2.5 of 32 CPUs busy just before.
  - Two earlier attempts were discarded:
    - The first died on a GCC 14.2 internal compiler error, a segfault in
      the SLP vectorizer. It was transient: three rebuilds of the same
      source succeeded.
    - The second had the dependent-chain bug described below. Another
      session's `build/probe/repeat.sh` also overlapped its first 44 s.
- **Checks.** Each one exits non-zero before its numbers can print.
  - Every search layout agrees with textbook binary search (4,096 probes).
  - A prefix count matches a full scan.
  - SymSpell's answers match a brute-force Myers scan of all 10M keys, for
    six typos in both modes. Myers itself is checked against the DP on
    20,000 pairs first.
  - Both filters have no false negatives over all 10M keys.
  - Sampled records are current, and fields come out 25% each (±0.3%).
  - HLL:
    - The kept harmonic sum equals a full recomputation.
    - The AVX2 sum and the fused AVX2 union equal scalar references.
    - The estimate of one set and of the union of two disjoint sets are
      within 3σ.
  - Count-Min never under-counts.
  - Concurrent counters and writes lose nothing (exact totals).
  - Delete + create restores the key count.
- **Wording fix after the run.** The THREADS header in `output.txt` says
  lookups "never lock or retry". Readers do take no lock and write no
  shared memory. However, `kv_cuckoo_get` PAUSE-waits while a writer holds
  one of its buckets odd, and it retries a miss that raced a relocation.
  That printed line and one comment (on the HLL union's bottleneck) were
  reworded in the source afterwards; no code changed.

**A measurement bug the numbers exposed.** The first dependent chain picked
its next key as position `i + x`, with x ∈ {0, 1} read from the previous
record.

- A quarter of the time (x = 1, then 0) that looked the same key up twice in
  a row. The repeat hit the cache.
- So the "pure latency" bar came out 25% low (296 ns) and below the plain
  loop (388 ns).
- The fix jumps to `i` or `i + NQ/2`, so consecutive keys never repeat. The
  chain then measures 347 ns, just above the plain loop's 327 ns, as it
  should.

## The bar at 10M keys

| Lookup | D: dependent chain (latency) | I: independent loop | B: batched pipeline (the server's GET path) |
|---|---|---|---|
| **get**, 100-byte value copied | **347.2 ns** | **326.9 ns** | **54.1 ns** |
| exists (hit) | 342.8 | 324.7 | 52.8 |
| exists (absent key) | — | 120.5 | 29.7 |
| **10× budget** | 3.47 µs | 3.27 µs | 541 ns |

Every row below is compared with the bar of its own regime ("×D", "×I",
"×B"). **Golden** means ≤ 10×.

**Why a plain loop gets almost no overlap.** I is only 6% below D, while
the batched pipeline is 6.0× faster than I.

- There is no fence on the lookup path; the only PAUSE is the writer-busy
  wait.
- The likely cause is the lookup's data-dependent branches. Which slot
  matched, and whether the key compared equal, depend on cache misses. A
  mispredict squashes the next iteration's work.
- This host has no PMU in WSL2, so branch misses cannot be counted
  directly.
- Absent keys take fewer such branches and never touch a record: 120 ns
  in a plain loop.
- Explicit two-stage prefetch, as in the server's batch, is what creates
  memory-level parallelism.

## Results

### Point operations

| Operation | Technique | Time | × bar |
|---|---|---|---|
| random key (RANDOMKEY) | rejection sampling over the table's slots (1.26 tries expected) | 28.0 ns | 0.09 I |
| | the same, 32 candidate slots prefetched per round | 23.2 ns | 0.43 B |
| "definitely absent", hashing included | split-block Bloom, AVX2 (VPMULLD + VPSLLVD + VPTEST); 11 bits/key, 13.1 MB, 0.83% false positives | **12.9 ns** | 0.04 I |
| | the same, scalar | 31.0 ns | 0.09 I |
| | the same, AVX2 batched | 14.2 ns | 0.26 B |
| | binary fuse 8 (static), 9.02 bits/key, 10.8 MB, 0.38% false positives | 52.3 ns | 0.16 I |

- Adding a key to the split-block filter costs 37 ns including its hash.
- Building the binary fuse filter costs 88 ns per key, 0.9 s for 10M.

### Counters: increment in place (lookup + atomic add, no allocation)

One thread, random keys: batched lookup + `LOCK XADD` **57.8 ns** (1.07 B).
Concurrent, time per operation per thread, then the total rate:

| Threads | random keys, XADD | 1,000 hot keys, XADD | one key, XADD | one key, CAS loop | one key, per-thread cells (no LOCK) |
|---|---|---|---|---|---|
| 1 | 61.4 ns / 16.3 M/s | 35.8 / 27.9 | 23.8 / 42.0 | 24.0 / 41.6 | 19.8 / 50.4 |
| 2 | 63.3 / 31.6 | 35.3 / 56.7 | 40.5 / 49.4 | 43.2 / 46.3 | 20.5 / 97.3 |
| 4 | 66.3 / 60.3 | 34.9 / 114.6 | 60.2 / 66.4 | 92.4 / 43.3 | 20.4 / 195.8 |
| 7 | 71.6 / 97.8 | 40.7 / 171.9 | **108.4 / 64.6** | **160.9 / 43.5** | **22.6 / 310.1** |

### Order: a sorted snapshot of all 10M keys

- **Build:** the sort takes 0.68 s and the Eytzinger layout 0.03 s. The
  keys share a 5-byte prefix; the tree has 24 levels.
- **Memory:** 153 MB of inline 16-byte keys, 153 MB of Eytzinger array and
  38 MB of ranks.

| Operation | Technique | Time | × bar |
|---|---|---|---|
| successor / rank | textbook binary search, ids → keys in the arena | 959.4 ns | 2.93 I |
| | branchless (CMOV) over inline normalized keys, both next midpoints prefetched | 309.5 ns | 0.95 I |
| | Eytzinger layout, prefetch 4 levels ahead | 263.3 ns | 0.81 I |
| | Eytzinger, 16 descents interleaved | **70.7 ns** | 1.31 B |
| count of keys by prefix or range | 2 interleaved descents, no iteration ("user:kalo" = 32,000 keys, checked) | **75.3 ns** | 1.39 B |
| range scan, 100 keys | seek + 100 inline keys in order | 188.1 ns | 3.48 B |
| range scan, 100 keys + values | seek + records, prefetched 16 ahead | 1.16 µs (11.6 ns per item) | 21.45 B per scan, 0.21 B per item |
| select (k-th key) | inline key array | 2.8 ns | 0.01 I |

### Sketches

| Operation | Technique | Time | × bar |
|---|---|---|---|
| distinct count: add | HyperLogLog p=14 (16 KB), hash reused from the lookup | 0.6 ns | 0.01 B |
| distinct count: estimate | incrementally kept harmonic sum: no register pass | 0.6 ns | 0.01 B |
| distinct count: union | PFMERGE into a new sketch + estimate, one fused AVX2 pass: VPMAXUB and a harmonic sum built from float exponent bits | 1.06 µs | 19.67 B |
| | the same at p=12 (4 KB, 1.63% error), timed on 4,096 registers | 261.9 ns | 4.84 B |
| frequency: count a key | Count-Min 4 × 2^21 (32 MB), hashing included | 111.2 ns | 0.34 I |
| | the same, batched prefetch | 21.4 ns | 0.39 B |
| frequency: estimate | min of 4, batched | 21.2 ns | 0.39 B |

- The HLL estimate over 10M keys is +0.41%. The union of 10M and 856,729
  disjoint keys is estimated at +0.35%.

### Typos: every key within edit distance 1 (SymSpell) at 10M keys

- **Build:** 9.9 s, 988 ns per key, 21.5 variants per key.
- **Memory:** 2.19 GB, 235 bytes per key.
  - The table is 64-byte buckets of 8 lanes at 73% lane load.
  - Each lane holds a 27-bit fingerprint and the key length.
  - It is paired with the key's arena offset.

| Typo | Probes in turn | Probes + candidate keys prefetched as groups | Matches per typo |
|---|---|---|---|
| substitution | 840.0 ns (2.57 I) | **572.1 ns** (1.75 I) | 1.39 |
| deletion | 1.06 µs (3.26 I) | **703.0 ns** (2.15 I) | 4.29 |
| insertion | 716.8 ns (2.19 I) | **578.5 ns** (1.77 I) | 1.00 |

### Hashing and key preprocessing (per 20-21-byte key, throughput)

| Kernel | Time | Note |
|---|---|---|
| SipHash-2-4, 128-bit, the store's assembly | 12.7 ns | today: 23% of a batched get |
| SipHash-2-4, 64-bit, C | 9.8 ns | |
| SipHash-1-3, 64-bit, C | 6.6 ns | Rust's and Python's default |
| AES-NI, 3 rounds | 7.3 ns | keyed, but not an analyzed PRF |
| multiply-mix (64×64→128) | 4.8 ns | keyed, but not an analyzed PRF |
| CRC32C (SSE4.2) | 2.3 ns | unkeyed: fingerprints of trusted data only |
| ASCII case folding, AVX2, 32 bytes | 4.9 ns | case-insensitive keys |

### Threads (lock-free readers, one writer mutex)

| Workload | Per op per thread | Total |
|---|---|---|
| readers, batched exists, 1 / 2 / 4 / 7 threads | 55.1 / 54.4 / 60.3 / 64.6 ns | 18.1 / 36.7 / 66.3 / **108.3 M/s** (5.97× one thread) |
| 6 readers alone vs next to 1 writer | 57.4 → 64.4 ns (1.12×) | the writer does 2.75 M puts/s meanwhile |

### Writes

| Operation | One at a time | Batched (hash + prefetch buckets and old records first) |
|---|---|---|
| put, replace | 433.1 ns | **125.9 ns** (3.44× faster) |
| delete | 488.0 ns | **148.8 ns** (3.28×) |
| put, create | 212.5 ns | **124.6 ns** (1.71×) |

Concurrent replace-puts, 1 / 2 / 4 / 7 writers: 3.35 / 2.75 / 2.14 / 2.25
M puts/s in total. A put takes 298 ns / 729 ns / 1.87 µs / 3.11 µs per
thread. The writer mutex serializes them, and the handoffs cost extra.

## Huge-page records (paired A/B of the bar)

Same binary, run with and without
`GLIBC_TUNABLES=glibc.malloc.arena_max=1:glibc.malloc.hugetlb=1:glibc.malloc.top_pad=268435456`.
Order alternates over three blocks. Ratios are huge-page / default.

| Bar | Default (3 blocks) | Huge-page heap | Ratio (geometric mean) | Faster |
|---|---|---|---|---|
| get, D | 373.7 / 366.5 / 371.7 ns | 294.6 / 294.7 / 316.9 | **0.815** | 3/3 |
| get, I | 326.7 / 366.7 / 347.5 | 276.2 / 269.0 / 272.8 | **0.787** | 3/3 |
| get, B | 52.8 / 56.1 / 54.4 | 49.0 / 47.2 / 49.4 | **0.892** | 3/3 |
| exists, D | 344.0 / 346.8 / 354.6 | 280.6 / 272.9 / 281.6 | 0.799 | 3/3 |
| absent, I (control: never reads a record) | 123.0 / 120.3 / 121.2 | 118.1 / 117.0 / 121.1 | 0.977 | 3/3 |
| absent, B (control) | 31.3 / 30.7 / 31.6 | 27.4 / 27.4 / 31.7 | 0.922 | 2/3 |

- THP-backed memory: 424 MiB by default (table and probe buffers) against
  1.9 GiB with the tunables (the records too).
- The gain is where records are read, and nearly absent where they are
  not. That matches the earlier in-server result: 69 → 62 ns per lookup,
  GET/s ×1.059
  ([2026-09-27-store-lookup](../2026-09-27-store-lookup/README.md)).

## What it decides

1. **At 10M keys the bar is 347 ns latency, 327 ns in a plain loop and
   54 ns batched.** Batching is worth 6× on this store. Every capability
   below that can batch, should.
2. **Golden (≤ 10× of its regime):** the following, all measured.
   - Point operations: random key; "definitely absent" with either filter;
     in-place counters.
   - Order: every ordered query (successor, rank, prefix/range count,
     select, 100-key range scans). Interleaved Eytzinger brings a seek to
     71 ns.
   - Typos: SymSpell at distance 1, 0.57-0.70 µs grouped.
   - Sketches: HLL add and estimate; the HLL union at p=12; Count-Min.
   - Every hash kernel.
   - Concurrent readers, up to 108M lookups/s on 7 cores.
   - Batched writes.
3. **Not golden as measured.**
   - **The p=14 HLL union** at 19.7 B. It costs O(registers) whatever n
     is, and p=12 brings it to 4.8 B at 1.6% error.
   - **A 100-key scan with values**, per scan: 21 B. Per returned item it is
     11.6 ns (0.21 B), so output-bound work is judged per item.
   - **Writes do not scale with threads.** One writer does 3.35 M/s and
     seven do 2.25 M/s, each put costing 9.5 I at seven. This is ADR 0003's
     single mutex; lock striping (libcuckoo) is the known fix.
4. **Biggest measured wins over today's code.**
   - **Write batching:** 3.3-3.4× faster replace and delete on one thread.
   - **Huge-page records:** the latency bar drops 18-21%, batched 11%.
   - **Per-thread counter cells** for a hot key: 4.8× the throughput of
     `LOCK XADD` at 7 threads (310 vs 65 M/s).
   - **The AVX2 split-block filter:** 2.4× its scalar twin. It answers
     "absent" 9.3× faster than a plain-loop table miss (12.9 vs 120.5 ns).
   - **Group prefetching for ordered search:** 3.7× the best single-query
     layout (70.7 vs 263.3 ns), and 13.6× textbook binary search.
5. **Each structure has its costs:**
   - sorted snapshot: 344 MB and a 0.7 s rebuild;
   - SymSpell: 2.19 GB and 1 µs per key;
   - filters: 11-14 MB;
   - sketches: KBs.

   Only creates and deletes maintain any of them.
