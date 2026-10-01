# Proposal: golden operations at 10M keys — everything within 10× of an exact lookup

- **Status:** proposed catalog, 2026-09-29. Nothing here is implemented or
  decided.
- **The bar** (set for this work): anything that costs at most 10× an exact
  get / exists on a 10M-key collection is *golden*. This document collects
  every such operation found and measured, and how to get it: SIMD and
  other assembly-level techniques, thread safety, and the memory and write
  cost of each.
- **Companions:**
  - [capabilities.md](capabilities.md): the same question at 100k keys,
    where a packed key arena is cache-resident.
  - [key-indexes.md](key-indexes.md): prefix, suffix and pattern indexes.
  - [export-watch.md](export-watch.md): streams, revisions and cuts.
  - [functions-plan.md](functions-plan.md): the 38 user-facing functions
    these measurements support, designed for implementation (API,
    semantics, usage, phases).
  - [golden-services.md](golden-services.md): the sequel. It adds ten
    more golden services and six store speedups (8-lane SipHash, a compact
    one-miss tier, flat combining, UMWAIT, prefix-table seeks, aggregates
    kept on write), measured at 10M keys.
- **Evidence:**
  [benchmarks/results/2026-09-29-golden-probe](../../benchmarks/results/2026-09-29-golden-probe/README.md).
  - `golden_probe.c` measures each prototype next to the store's own
    assembly, at 10M keys.
  - Every answer is checked against a reference.
  - The run's window is certified free of other sessions' work.
  - Numbers without that source are marked *cited* (§8) or *estimated*.

## Summary

**The bar at 10M keys** (20-21-byte keys, 100-byte values, i9-13900K):

| Regime | Exact get | 10× budget |
|---|---|---|
| **D**: a dependent chain (pure latency) | 347 ns | 3.47 µs |
| **I**: an independent plain loop | 327 ns | 3.27 µs |
| **B**: the server's batched pipeline (32 keys, two-stage prefetch) | 54 ns | 541 ns |

Every operation is compared with the bar of its own regime.

**Golden, measured.**

| Area | Operations |
|---|---|
| Point | random key; "definitely absent" (split-block Bloom 12.9 ns, binary fuse 52 ns); counters incremented in place |
| Order (sorted snapshot) | successor, rank, **count by prefix or range (75 ns)**, 100-key range scans, select |
| Typos | edit distance 1 over 10M keys in **0.57-0.70 µs** |
| Sketches | HyperLogLog add, estimate and union (at p=12); Count-Min |
| Hashing | every hash and key-folding kernel |
| Concurrency | concurrent lock-free readers (108M lookups/s on 7 cores) |
| Writes | batched writes |

**Not golden, and why.**

| Case | Measured | Why, and the way out |
|---|---|---|
| HLL union at p=14 | 19.7 B | O(registers); p=12 gives 4.8 B |
| 100-key scan with values | 21 B per scan | output-bound: 11.6 ns per item |
| Concurrent writers | throughput *falls* from 3.35 to 2.25 M puts/s at 7 threads | ADR 0003's single writer mutex |

**The biggest measured wins over today's code:**

1. **Write batching:** replace 3.4× and delete 3.3× faster on one thread.
   Hash and prefetch a batch's buckets and old records before writing, as
   GET already does.
2. **Huge-page records:** exact-get latency −18.5% (dependent chain), −21%
   (plain loop), −11% (batched). Paired, 3/3 blocks, with a control that
   gains nothing.
3. **Per-thread counter cells** for a hot key: 310 vs 65 M increments/s at 7
   threads (4.8×).
4. **AVX2 split-block Bloom filter:** "absent" in 12.9 ns, 9.3× cheaper than
   a table miss in a plain loop.
5. **Interleaved Eytzinger search:** 13.6× textbook binary search. It is
   what makes ordered queries golden at 10M.

## 1. The bar, and what it teaches

| Lookup | D: dependent chain | I: plain loop | B: batched |
|---|---|---|---|
| get (100-byte value copied) | 347.2 ns | 326.9 ns | 54.1 ns |
| exists (hit) | 342.8 | 324.7 | 52.8 |
| exists (absent key) | — | 120.5 | 29.7 |

- **A plain loop over independent lookups gets almost nothing from the
  out-of-order core** (I is only 6% below D). The lookup path has no fence.
  - The likely cause: its branches (which slot matched, whether the key
    compared equal) depend on cache misses, and a mispredict squashes the
    next iteration's work.
  - Absent keys take fewer such branches and never touch a record: 120 ns.
- **Explicit prefetch stages are what buy memory-level parallelism:** 6×
  here.
- **Hence the first rule of this catalog:** *every operation that can be
  batched should be*, and each row below that batches is compared with B.

## 2. The catalog

**Status legend:**
- **M:** measured in the golden probe at 10M keys.
- **E:** estimated from measured parts.
- **C:** cited (§8).

"× bar" is the ratio to the bar of the row's own regime.

### 2.1 Point operations

| Capability | Technique (SIMD / assembly) | Time | × bar | Memory | Per write | Thread safety | Status |
|---|---|---|---|---|---|---|---|
| get / exists / MGET | cuckoo table: SSE2 tag scan of both buckets (one PCMPEQB + PMOVMSKB), two-stage prefetch per batch of 32 | 54.1 / 52.8 ns | 1.0 B | (the store) | — | lock-free readers: QSBR, per-bucket versions (§4.7) | M |
| exists, absent key | the same | 29.7 ns | 0.55 B | — | — | same | M |
| **"definitely absent"** | split-block Bloom: 8 × 32-bit lanes, one bit each; AVX2 VPMULLD + VPSRLD + VPSLLVD build the masks, one VPTEST checks them; fastrange block index | **12.9 ns** (hash included) | 0.04 I | 11 bits (13.1 MB), 0.83% false positives | 37 ns per create | bits set before the key is published (§4.4) | M |
| the same, batched | + prefetch of 32 blocks | 14.2 ns | 0.26 B | | | | M |
| the same, scalar twin (the non-AVX2 path) | 8-step loop | 31.0 ns | 0.09 I | | | | M |
| "definitely absent", static and smaller (shipped to clients) | binary fuse 8 (3 probes) | 52.3 ns | 0.16 I | 9.02 bits (10.8 MB), 0.38% | rebuild: 88 ns per key (0.9 s) | RCU swap of a rebuilt filter | M |
| **random key** (RANDOMKEY) | rejection sampling over the table's slots: 1/load = 1.26 tries | **28.0 ns** | 0.09 I | 0 | 0 | lock-free: one aligned slot load, QSBR (§4.6) | M |
| the same, batched | 32 candidate slots prefetched per round | 23.2 ns | 0.43 B | | | | M |
| count of all keys | the store's counter | O(1) | — | — | — | exists today | — |

### 2.2 Counters (INCR / INCRBY / DECR), in place

| Capability | Technique | Time (per op per thread) | × bar | Thread safety | Status |
|---|---|---|---|---|---|
| increment, random keys, 1 thread | batched lookup + `LOCK XADD` on the record's 8-aligned value word; no allocation, no writer mutex | **57.8 ns** | 1.07 B | linearizable (§4.1) | M |
| the same, 7 threads | | 71.6 ns (97.8 M/s in total) | 1.32 B | | M |
| 1,000 hot keys, 7 threads | | 40.7 ns (171.9 M/s) | 0.75 B | | M |
| **one** hot key, 7 threads | `LOCK XADD` | 108.4 ns (64.6 M/s) | 2.00 B | | M |
| | `LOCK CMPXCHG` loop (needed for overflow checks, GCRA rate limits, max-registers) | 160.9 ns (43.5 M/s) | 2.97 B | lock-free | M |
| | **per-worker cells:** the owner does a plain load + add + store, with no LOCK prefix; a read sums the cells | **22.6 ns (310.1 M/s)** | 0.42 B | exact for unit increments (§4.2) | M |

### 2.3 Order: a sorted snapshot of all keys

Keys are normalized for comparison: the prefix they all share ("user:") is
elided, and the next 16 bytes are stored big-endian. A comparison is then
two integer compares, which CMOV can do.

| Capability | Technique | Time | × bar | Status |
|---|---|---|---|---|
| successor / predecessor / rank | textbook binary search, ids → keys in an arena (2 dependent misses per level) | 959.4 ns | 2.93 I | M |
| | branchless (CMOV) over inline keys, both possible next midpoints prefetched | 309.5 ns | 0.95 I | M |
| | Eytzinger layout (BFS order: children at 2k, 2k+1), 16 descendants prefetched 4 levels ahead (4 lines per step), answer recovered with one `ffs` | 263.3 ns | 0.81 I | M |
| | **16 Eytzinger descents interleaved** level by level: one line prefetched 2 levels ahead per descent, finished lanes parked on a sentinel so the loop has no data-dependent exit | **70.7 ns** | 1.31 B | M |
| **count of keys by prefix or range** | two interleaved descents, no iteration | **75.3 ns** | 1.39 B | M |
| range scan, 100 keys | seek + 100 inline keys (sequential) | 188.1 ns | 3.48 B | M |
| range scan, 100 keys + values | seek + records prefetched 16 ahead (software pipeline) | 1.16 µs per scan = **11.6 ns per item** | 21.45 B per scan, 0.21 B per item | M |
| | the same when records cannot be pinned: a batched lookup per item | ≈54 ns per item | ≈1 B per item | E |
| select (the k-th key) | the inline key array | 2.8 ns | 0.01 I | M |
| min / max key | the first / last element | O(1) | — | E |

**Costs:**
- Memory: 344 MB at 10M, 34 B per key (inline keys, Eytzinger array,
  ranks), plus 8 B per key if values are scanned through pinned records.
- Build: 0.68 s to sort and 0.03 s for the layout, off the hot path.
- Freshness: §4.3.

### 2.4 Typos: every key within edit distance 1

| Capability | Technique | Time | × bar | Status |
|---|---|---|---|---|
| substitution / deletion / insertion typo of any key | SymSpell symmetric delete. The hashes of a key's 21.5 deletion variants come in O(len) from prefix/suffix polynomial hashes (mod 2^61−1, a secret base). A bucketized table (64-byte lines of 8 lanes) is probed with two AVX2 compares (VPCMPEQD + VMOVMSKPS for fingerprint hits and for empty lanes). All ~22 bucket probes, then all candidate keys, are prefetched as groups; Myers' bit-parallel distance verifies. | **572 / 703 / 579 ns** | 1.75 / 2.15 / 1.77 I | M |
| | the same, probing in turn | 840 ns / 1.06 µs / 717 ns | 2.2-3.3 I | M |

**Cost:** 2.19 GB at 10M (235 B per key, 73% lane load), and 988 ns per
created key. It is opt-in by any measure, as at 100k.

### 2.5 Sketches (O(1) in n)

| Capability | Technique | Time | × bar | Memory | Status |
|---|---|---|---|---|---|
| distinct count: add | HyperLogLog p=14, reusing the key's hash from the lookup | 0.6 ns | 0.01 B | 16 KB, 0.81% error | M |
| distinct count: estimate | the harmonic sum is kept current on every register raise, so no pass over the registers | 0.6 ns | 0.01 B | | M |
| distinct count: union (PFMERGE + count) | one fused AVX2 pass: VPMAXUB, store, and the new harmonic sum. 2^-r is built as a float with exponent field 127 − r (VPMOVZXBD, VPSUBD, VPSLLD), summed in float per 32 registers, then in double; zeros by VPCMPEQB + VPMOVMSKB + POPCNT | 1.06 µs | **19.67 B** | | M |
| | the same at p=12 | **261.9 ns** | 4.84 B | 4 KB, 1.63% error | M |
| frequency of a key | Count-Min 4 × 2^21, double hashing | 111.2 ns / batched 21.4 ns | 0.34 I / 0.39 B | 32 MB | M |
| frequency estimate | min of 4 rows, batched | 21.2 ns | 0.39 B | | M |
| hottest keys | Space-Saving + Count-Min | O(1) amortized | — | O(k) | C [SS] |
| quantiles (value sizes, latencies) | DDSketch | O(1) | — | KBs | C [DD] |

### 2.6 Keys: hashing and folding (per 20-21-byte key)

| Kernel | Time | Use |
|---|---|---|
| SipHash-2-4, 128-bit (the store's assembly) | 12.7 ns | today; 23% of a batched get |
| SipHash-1-3, 64-bit | 6.6 ns | the same keyed PRF family with fewer rounds (Rust's and Python's default) |
| AES-NI, 3 rounds | 7.3 ns | keyed but not an analyzed PRF: only where flooding is not a threat |
| multiply-mix | 4.8 ns | the same caveat |
| CRC32C (SSE4.2) | 2.3 ns | unkeyed: fingerprints of trusted data |
| ASCII case folding, AVX2 (32 bytes) | 4.9 ns | case-insensitive keys: fold once, then hash and compare as usual |

### 2.7 Writes and scaling

| Operation | One at a time | Batched (hash + prefetch first) | Status |
|---|---|---|---|
| put, replace | 433.1 ns | **125.9 ns** (3.44×) | M |
| delete | 488.0 ns | **148.8 ns** (3.28×) | M |
| put, create | 212.5 ns | **124.6 ns** (1.71×) | M |

**Concurrent readers:**
- 18.1 / 36.7 / 66.3 / **108.3 M lookups/s** on 1 / 2 / 4 / 7 threads
  (5.97×).
- Next to a writer, readers slow by 1.12× while it does 2.75 M puts/s.

**Concurrent writers:**
- 3.35 / 2.75 / 2.14 / 2.25 M puts/s in total on 1 / 2 / 4 / 7 threads.
- Each put then takes 3.11 µs per thread (9.5 I).
- It is still inside the bar, but the only row whose throughput falls as
  threads are added.

### 2.8 System level: huge-page records

Paired A/B of the bar: the same binary with and without glibc's huge-page
heap tunables, alternating order over 3 blocks.

| Bar | Huge / default |
|---|---|
| get, D | **0.815** (3/3 faster) |
| get, I | **0.787** (3/3) |
| get, B | **0.892** (3/3) |
| exists, D | 0.799 (3/3) |
| absent key, plain loop (reads no record: the control) | 0.977 |

- THP-backed memory grows from 424 MiB to 1.9 GiB, as the records move onto
  huge pages.
- The gain is where records are read, and nearly nothing where they are
  not.

## 3. The techniques that make them golden

1. **Group prefetching / batching** (Chen and Ailamaki's group and
   software-pipelined prefetching; AMAC; coroutine interleaving [INT]).
   - Hash every key of a batch and prefetch its first-level lines; then
     prefetch the second level; then do the work.
   - Applied here to: gets (6.0×), writes (1.7-3.4×), ordered search (3.7×
     over the best single-query layout), SymSpell (1.2-1.5×), Count-Min
     (5.2×), sampling (1.2×).
   - The line-fill buffers (≈16 per core) bound how many misses one core
     keeps in flight. Batches of 16-32 already saturate them (ADR 0006
     measured b64 = b16).
2. **Layouts that turn pointer chasing into arithmetic.**
   - Eytzinger order: the next node is 2k or 2k+1, and 16 descendants sit
     in 4 contiguous lines.
   - Inline normalized keys: no dereference per comparison.
   - Arena offsets stored in index lanes: a candidate is one access, not
     two.
   - Bucketized 64-byte lines: one miss yields 8 candidates.
3. **SIMD where a line holds several candidates.**
   - Existing: SSE2 tags.
   - Measured here:
     - AVX2 filter masks and VPTEST;
     - AVX2 fingerprint compares;
     - VPMAXUB unions;
     - the float-exponent 2^-r;
     - case folding.
   - Each is a small, self-contained kernel with a scalar twin (the SIMD
     policy of ADR 0006).
   - AVX-512 is not available on this CPU (Raptor Lake). Where AVX2 is not
     available, the SSE2/SWAR paths stay.
4. **Branch-free control:** CMOV descents, a fixed-count interleaved loop
   with sentinel parking, `ffs` to recover the answer, fastrange
   (multiply-high) instead of modulo.
5. **The cheapest correct atomic.**
   - `LOCK XADD` for unconditional counters.
   - `LOCK CMPXCHG` only when the update is conditional.
   - No LOCK at all for single-writer cells.
   - Readers never write shared memory (QSBR).
6. **Keep the hash once.** Filters, sketches and Count-Min reuse the
   SipHash the lookup computes anyway. Independent bits come from different
   halves of the 128-bit result.
7. **TLB reach:** huge pages for the table (already) and for records (a
   2 MiB record slab; glibc tunables prove the gain today).

## 4. Thread safety of the new operations

### 4.1 In-place counters are linearizable without extra protocol

**Layout.** A counter record's value is an 8-aligned 64-bit cell.
- Its alignment follows from 16-aligned records, a 24-byte header and keys
  padded to 8.
- Readers load it with one aligned MOV, so no value is ever torn.
- `INCRBY` = batched lookup, then `LOCK XADD [cell], delta`, returning
  old + delta.

**Replace or Delete racing an increment.** Suppose a Put publishes a new
record at time T while an increment still holds the old one.

- That increment read the slot before T: a slot read after T sees the new
  pointer.
- So its interval contains a point just before T. It linearizes there, in
  XADD order with every other operation on the old record, and the Put then
  overwrites it, as in any serial history where INCR precedes SET.
- Gets that read the old cell linearize the same way.
- QSBR keeps the old record's memory valid until every such reader
  announces a quiescent state.

No retire flag, and no second atomic, is needed. Two edge cases remain:

- **INCR on a missing key:** creation goes through the writer path, which
  re-checks under the mutex.
- **Overflow checks** (Redis's "increment or decrement would overflow")
  need the CAS form.

**Status:** reasoned from the store's publication order (x86-TSO, slot
store then retire); not yet model-checked.

### 4.2 Per-worker cells for hot counters

A designated hot counter holds one 64-byte cell per worker.
- The owner updates its cell with plain stores.
- A read sums the cells: W loads.

**For unit increments the sum is exact.** Every cell only grows, so the sum
lies between the true values at the read's start and end. The true value
passes through every integer in between, so it equals the sum at some
instant inside the read.

**With mixed deltas a read is only bounded by those two values.**

**Cost:** 64 B × workers per hot key, which is why it is opt-in per key.

### 4.3 Sorted snapshot: published by RCU, freshened by a delta

**Build and publish.**
- A background thread builds the snapshot from a cut ([export-watch.md](export-watch.md)
  §5): sort, layout and ranks, 0.7 s at 10M.
- It publishes the snapshot with one release store of a pointer.
- Readers load the pointer at the start of a batch; the old snapshot is
  freed after a QSBR grace period. Readers never lock.

**Fresh answers.** An exact "as of now" answer is snapshot ± the delta of
creates and deletes since the cut's revision, read from the history ring
and merged at query time.

**When to switch.** When creates are so frequent that the delta grows
faster than rebuilds shrink it, the ordered index belongs in a dynamic
structure (ART or Wormhole, [key-indexes.md](key-indexes.md)).

**Range scans with values** read values through a batched lookup per item
(≈54 ns per item). They read records directly (11.6 ns per item) only while
the snapshot pins them, like an export cut.

### 4.4 Filters: set the bits before publishing the key

**Split-block Bloom filter.**
- Writers, already serialized, set the key's 8 bits *before* storing its
  record pointer in the table.
- A reader that answers "absent" from stale or partial bits read the block
  before the last bit store became visible. That was before the key's
  publication, so "absent" is linearizable.
- Deletes cannot clear bits. The false-positive rate drifts with churn, so
  a background rebuild (37 ns per key, 0.4 s at 10M) swaps in a fresh
  filter by RCU.
- A counting, cuckoo [CF] or Aleph [ALEPH] filter would support deletes
  instead.

**Binary fuse filter (static).** Rebuilt in 0.9 s at 10M. It is the right
format to export to clients (a `GetFilter` RPC, capabilities.md §6): a
client that skips an RPC for an absent key saves a network round trip,
orders of magnitude more than the 15-100 ns the server saves.

### 4.5 SymSpell maintenance

- **Create:** 21.5 lane inserts, prefetched as a group (≈1 µs).
- **Lane order:** a writer stores the lane's arena offset *before* its
  fingerprint. Under x86-TSO, a reader that sees the fingerprint also sees
  the offset.
- **Delete:** overwrite fingerprints with a tombstone value (not 0, so
  probe chains stay intact). The key's arena bytes are recycled only after
  a QSBR grace period, because readers verify candidates against them.

### 4.6 Random key

- One aligned load of a random slot, and the record is QSBR-protected.
- A cuckoo move in flight can make one key briefly visible in two slots or
  none: a transient, negligible bias. The measured distribution over the
  four fields is 25.08 / 24.96 / 24.97 / 25.00%.
- The expected number of tries is 1/load. After mass deletes (load 5% →
  20 tries), fall back to a bucket-occupancy bitmap.

### 4.7 What "lock-free readers" means in this store

Readers never take a lock and never write shared memory.
- Hits return at once.
- A reader PAUSE-waits only while a writer holds one of its two buckets
  (a handful of stores).
- It retries a miss if a bucket's version changed during the scan, since a
  relocation could have moved the key.

Measured: 7 readers do 5.97× one reader, and a writer next to 6 readers
slows them by 1.12×.

## 5. What is not golden, and the way out

| Case | Measured | Why | Way out |
|---|---|---|---|
| HLL union at p=14 | 1.06 µs = 19.7 B | O(registers) and compute-bound: fusing away the copy and a pass saved nothing. By instruction mix, the widening converts and shuffles (port 5) are the likely limit; there is no PMU here to confirm it | p=12 (262 ns, 4.8 B, 1.6% error); or keep per-worker sketches and merge only on read |
| 100-key scan with values | 21 B per scan | output-bound | judge per item: 11.6 ns (pinned) to ≈54 ns (lookup) per item, ≤ 1 B |
| concurrent writers | 2.25 M puts/s at 7 writers < 3.35 M at 1 | one writer mutex (ADR 0003) | batch writes under one lock acquisition; lock striping with optimistic cuckoo paths (libcuckoo [CUCKOO]) |
| one hot counter via CAS at 7 threads | 161 ns (2.97 B: golden, but 7× the cells) | the line ping-pongs between cores | per-worker cells |
| textbook binary search | 959 ns (2.93 I: golden, but 13.6× the interleaved Eytzinger) | 2 dependent misses per level | Eytzinger + inline keys + interleaving |

## 6. Leads cited but not measured here

| Technique | What it would add | Source |
|---|---|---|
| PtrHash (minimal perfect hash) | a static read-only index at 8-12 ns per key streaming, 3 bits per key | [PTR] |
| static search trees (S+ trees) | up to 40× binary search for batched static ordered search | [S+] |
| Aleph filter / InfiniFilter | expandable filters with O(1) insert, query and delete | [ALEPH] |
| cuckoo filter | deletes, 9.1 bits per item at 1% | [CF] |
| SuRF / Memento | range filters ("any key in [a, b)?") | capabilities.md [SURF], [MEM] |
| libcuckoo | concurrent writers: lock striping + path discovery outside the lock | [CUCKOO] |
| AMAC / coroutine interleaving | batching for variable-length chains (for example a lookup that needs the second bucket only sometimes) | [INT] |
| vqsort | 10-20× std::sort (AVX2/AVX-512): faster snapshot rebuilds | [VQS] |
| SipHash-1-3 / HighwayHash | a cheaper keyed hash: 12.7 → ~7 ns per key here | [SIP], [HWY] |
| CS-Poppy rank/select, Elias-Fano | compressed sorted id sets with O(1) rank/select (POPCNT, PDEP + TZCNT) | [RS], [EF] |
| PGM index | a learned static index; fits read-only sorted data | [PGM] |
| Linux per-CPU counters | the production precedent for §4.2 | [PCPU] |
| SIMD database operators (gathers, scatters) | vectorized probes, if AVX-512 hosts become a target | [SIMDDB] |

## 7. Recommendations, by value for cost

1. **Write batching in the server's PUT/DELETE path.** Hash and prefetch
   buckets and old records before the mutex, as GET already does:
   3.3-3.4× faster replace and delete on one thread. It is the biggest
   single-thread win measured. It needs the gRPC layer to hand the store
   write batches.
2. **Huge-page records.** A 2 MiB record slab in store.asm, the durable
   form of what the glibc tunables prove: −18.5% latency, −11% batched.
3. **The in-place counter family.** INCR, INCRBY and DECR with
   `LOCK XADD`, linearizable as in §4.1, plus per-worker cells for
   designated hot keys (4.8× at 7 threads).
4. **Absent-key filters.**
   - A split-block Bloom filter maintained on create (AVX2 probe, scalar
     twin).
   - A binary fuse filter exported to clients.
5. **RANDOMKEY** by slot rejection sampling: 28 ns, no structure at all.
6. **Ordered queries on a snapshot.** Successor, rank, prefix/range counts,
   range scans and select, at 71-188 ns:
   - Eytzinger layout, inline normalized keys, queries interleaved in
     batches of 16;
   - freshened by a delta (§4.3);
   - ART from key-indexes.md for write-heavy stores.
7. **Sketches as opt-ins:** HyperLogLog (p=12 for golden unions, p=14 for
   0.8% error) and Count-Min, fed by the hash already computed.
8. **SymSpell as an opt-in:** 0.6-0.7 µs typo lookup at 10M for
   2.19 GB.
9. **SipHash-1-3**, if ADR 0004 is amended. The hash is 23% of a batched
   get.
10. **Concurrent writes** (lock striping, per-batch locking). This is the
    one area that needs a design of its own before it can scale.

## 8. Sources

- **[SBBF]** Apple. *Split block Bloom filters*, 2021/2023,
  <https://arxiv.org/abs/2101.01719>. The Apache Parquet specification:
  <https://parquet.apache.org/docs/file-format/bloomfilter/>.
- **[BF]** Graf, Lemire. *Binary Fuse Filters: Fast and Smaller Than Xor
  Filters*, 2022, <https://arxiv.org/abs/2201.01174>. The measured code:
  <https://github.com/FastFilter/xor_singleheader> (pinned in `run.sh`).
- **[EYT]** Khuong, Morin. *Array Layouts for Comparison-Based Searching*,
  2015, <https://arxiv.org/abs/1509.05053>.
- **[S+]** Groot Koerkamp. *Static search trees: 40× faster than binary
  search*, 2024, <https://curiouscoding.nl/posts/static-search-tree/>.
- **[PTR]** Groot Koerkamp. *PtrHash: Minimal Perfect Hashing at RAM
  Throughput*, 2025, <https://arxiv.org/abs/2502.15539>. 8-12 ns per key at
  10^9 keys, against 7.4 ns per random memory access.
- **[GP]** Chen, Ailamaki, Gibbons, Mowry. *Improving Hash Join
  Performance through Prefetching*, ICDE 2004,
  <https://www.pdl.cmu.edu/PDL-FTP/Database/icde04.pdf>. Group and
  software-pipelined prefetching.
- **[INT]** Psaropoulos, Legler, May, Ailamaki. *Interleaving with
  Coroutines: A Practical Approach for Robust Index Joins*, PVLDB 11(2),
  2017, <http://www.vldb.org/pvldb/vol11/p230-psaropoulos.pdf>. Kocberber,
  Falsafi, Grot. *Asynchronous Memory Access Chaining* (AMAC), PVLDB 9(4),
  2015.
- **[SIMDDB]** Polychroniou, Raghavan, Ross. *Rethinking SIMD
  Vectorization for In-Memory Databases*, SIGMOD 2015,
  <https://dl.acm.org/doi/10.1145/2723372.2747645>.
- **[CUCKOO]** Li, Andersen, Kaminsky, Freedman. *Algorithmic Improvements
  for Fast Concurrent Cuckoo Hashing*, EuroSys 2014,
  <https://www.cs.princeton.edu/~mfreed/docs/cuckoo-eurosys14.pdf>.
- **[FR]** Lemire. *A fast alternative to the modulo reduction*, 2016,
  <https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/>.
- **[CF]** Fan, Andersen, Kaminsky, Mitzenmacher. *Cuckoo Filter:
  Practically Better Than Bloom*, CoNEXT 2014,
  <https://www.cs.cmu.edu/~dga/papers/cuckoo-conext2014.pdf>. 9.1 bits per
  item at 1% with deletes, against Bloom's 9.6.
- **[ALEPH]** Dayan et al. *Aleph Filter: To Infinity in Constant Time*,
  2024, <https://arxiv.org/abs/2404.04703>.
- **[VQS]** Blacher, Giesen, Sanders, Wassenberg. *Vectorized and
  performance-portable Quicksort*, 2022,
  <https://arxiv.org/abs/2205.05982>. 10-20× the standard library's sort.
- **[SIP]** Aumasson, Bernstein. SipHash; SipHash-1-3 is used by Rust's
  and Python's hash tables.
  <https://en.wikipedia.org/wiki/SipHash>,
  <https://docs.kernel.org/security/siphash.html>.
- **[HWY]** Google. *HighwayHash*, <https://github.com/google/highwayhash>.
  0.24 cycles per byte on large inputs, and faster than SipHash at all
  sizes.
- **[RS]** Zhou, Andersen, Kaminsky. Poppy rank/select, SEA 2013. Pandey,
  Bender, Johnson. *A Fast x86 Implementation of Select*, 2017,
  <https://arxiv.org/abs/1706.00990>.
- **[EF]** Pibiri, Venturini. *Dynamic Elias-Fano Representation*, CPM
  2017,
  <https://drops.dagstuhl.de/storage/00lipics/lipics-vol078-cpm2017/LIPIcs.CPM.2017.30/LIPIcs.CPM.2017.30.pdf>.
- **[PGM]** Ferragina, Vinciguerra. The PGM-index, PVLDB 2020. Context:
  <https://arxiv.org/html/2410.00846v1>.
- **[PCPU]** *Scalable Statistics Counters* (Linux per-CPU counters),
  <https://lse.sourceforge.net/counters/statctr.html>.
- **[HLL]** Flajolet et al. *HyperLogLog*, 2007: standard error 1.04/√m.
  <https://algo.inria.fr/flajolet/Publications/FlFuGaMe07.pdf>.
- **[CM]** Cormode, Muthukrishnan. Count-Min sketch, 2005. Kirsch,
  Mitzenmacher. *Less hashing, same performance*, 2006: two hashes suffice
  (the double hashing used here).
- **[SS]**, **[DD]**, **[SURF]**, **[MEM]:** see
  [capabilities.md §4](capabilities.md#4-sources-for-the-cited-techniques).
- **Myers**, J. ACM 1999, and **SymSpell** (Garbe): see capabilities.md §4.
- **Branchless binary search:** <https://probablydance.com/2023/04/27/beautiful-branchless-binary-search/>.

## 9. Limits

- **One host.** The i9-13900K under WSL2, whose hypervisor backs guest
  memory with 4 KiB pages. Absolute times vary 5-20% between runs here;
  ratios within one run are the robust quantity. The huge-page result is
  paired.
- **These are store-level prototypes in C next to the store's assembly,
  not server features.** In the server a GET costs ~270-580 ns of CPU
  end to end, most of it protocol. A golden store operation still has to
  fit that budget, and the batch shapes the protocol layer can deliver.
- **SIMD kernels are AVX2 prototypes.** Each production version needs its
  SSE2/SWAR twin and runtime dispatch, per ADR 0006.
- **The snapshot costs memory and freshness.** Ordered structures pay
  344 MB and a 0.7 s rebuild at 10M. SymSpell pays 2.19 GB.
- **The key set is synthetic.** It is 20-21-byte keys sharing a prefix.
  Longer keys need a tie-break past the 16 normalized bytes, and a
  different key distribution changes SymSpell's candidate counts.

## 10. Open decisions

1. **Which operations become RPCs, and in what order.** Proposed: the
   counter family and RANDOMKEY first (no new structure), then filters and
   the ordered snapshot.
2. **Write batching in the server:** the batch shape, and one mutex
   acquisition per batch or per write.
3. **The record slab (huge pages)** as the next store change.
4. **Snapshot freshness:** periodic rebuild + delta, or a dynamic ordered
   index from the start.
5. **Memory budgets for the opt-ins:** SymSpell (2.19 GB at 10M), the
   snapshot (344 MB), sketch precision (p=12 or p=14).
6. **SipHash-1-3:** an amendment to ADR 0004.
