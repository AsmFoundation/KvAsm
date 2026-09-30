# Proposal: O(1) and near-instant capabilities for a 100k-key KvNode

- **Status:** proposed catalog, 2026-09-29. Nothing here is implemented or
  decided. It collects what KvNode could answer instantly beyond Get, Put
  and Delete, with the technique and the cost of each.
- **Companions:** [export-watch.md](export-watch.md) (streaming queries and
  the revision model), [key-indexes.md](key-indexes.md) (prefix, suffix
  and pattern indexes at 1M-16M keys) and
  [golden-operations.md](golden-operations.md) (the same question at 10M
  keys, against a bar of 10× an exact lookup).
- **Evidence:**
  [benchmarks/results/2026-09-29-cap-probe](../../benchmarks/results/2026-09-29-cap-probe/README.md),
  measured on the store's real code at 100k keys (and 1M for scale). Every
  other number below is marked *cited*, with its source.

## Summary

**100k keys is a special size.** Packed back to back, the keys take 2.4 MB,
which fits in the CPU's L2/L3 cache. So there are three tiers, not two:

| Tier | Latency at 100k | How | Examples |
|---|---|---|---|
| **O(1) / O(log n)** | < 1 µs; typo lookup ~1.3 µs | a dedicated structure per capability | get, exists, "definitely absent", successor, rank, prefix *counts*, range seeks, typo lookup (edit distance 1) |
| **near instant** | ≤ 100 µs | an index for selective queries, or SIMD over a packed key arena | any substring (85 µs with no index, 8 µs with trigrams) |
| **interactive** | 1-4 ms | brute force over the packed arena: O(n), no index | any glob or regex; typos at any edit distance |

Two measured facts shape the recommendation:

- **At 100k, one packed copy of the keys** (25 bytes per key, 11 ns per
  create) turns *any* key-pattern query into a millisecond-class scan.
  - That is 1.6-6.5× faster than scanning the store's records, which are spread
    over ~16 MB.
  - It is 25× faster for substrings with SIMD.
- **Indexes are what make queries flat as the store grows.**
  - From 100k to 1M keys, trigram lookups go 8 → 91 µs, SymSpell stays at
    1.2-2.2 µs, and sorted operations stay under 0.33 µs.
  - Arena scans grow 10×, to 10-37 ms.

**Proposed bundle for "near-instant everything" at 100k:** a packed key
arena, a sorted snapshot (or a trie as the collection grows) and a trigram
index. That is about 150 bytes per key in total, next to the store's ~162.
SymSpell (675 bytes per key) is an opt-in for typo-tolerant lookup.

## 1. The capability catalog

**Status legend:**
- **M:** measured in the cap probe (100k keys; 1M in brackets).
- **K:** measured in [key-indexes.md](key-indexes.md)'s probe (1M/16M keys).
- **C:** cited from the source linked in §4.
- **E:** estimated from measured parts.

### 1.1 Point and existence

| Capability | Technique | Query | Per create/delete | Memory/key | Status |
|---|---|---|---|---|---|
| get / put / delete | the cuckoo table (ADR 0001) | **27 ns** [41] batched | — | (the store) | M |
| exists | the same lookup, no value copied | ≤ get | — | 0 | M |
| count of all keys | the store's counter | O(1) | — | 0 | (exists today) |
| "definitely absent" (e.g. shipped to clients to skip round trips) | blocked Bloom filter, 10 bits/key, 1.04% false positives | **38 ns**, hashing included | 34 ns | 10 bits | M |
| the same, smaller and static | binary fuse filter, ~9 bits/key at 0.39% false positives | a few ns | rebuild | 9 bits | C [BF] |
| O(1) index for a read-only snapshot | minimal perfect hash (PtrHash, ShockHash) | ~1 cache miss | rebuild | 1.5-3 bits (+ the data) | C [MPH] |

### 1.2 Order: successor, rank, ranges, counts

| Capability | Technique | Query | Per create | Memory/key | Status |
|---|---|---|---|---|---|
| successor / predecessor / rank | sorted snapshot (ids into the key arena) | **165 ns** [324] | **2.25 µs** [30 µs] (array insert) | 4 B | M |
| count of keys with any prefix (or in any range) | two binary searches, no iteration | **87 ns** [156] | as above | — | M |
| range scan in key order | seek + walk | **160 ns** per 100 keys [192] | as above | — | M |
| the same, scaling past 1M | adaptive radix trie (fwd), HOT | O(key length) | 0.3-0.8 µs (K) | 85 B | K, C [HOT] |
| ordered lookups in O(log key length), independent of n | Wormhole (hash of anchors over a trie + sorted leaves) | O(log L) | O(log L) | — | C [WH] |
| compact static prefix/range structure | succinct trie (SuRF / FST, LOUDS) | O(key length) | rebuild | ~10 bits/node | C [SURF] |
| "is there any key in [a, b)?", deletions supported | Memento range filter | 1-2 cache misses | 1 miss | a few bytes | C [MEM] |

### 1.3 Prefix, suffix, wildcard and regex over keys

| Capability | Technique | Query (k = results) | Per create | Memory/key | Status |
|---|---|---|---|---|---|
| prefix / suffix listing | segment directories, tries | O(1) or O(len) + O(k): 0.2 µs for k = 4 at 16M | 0.2-0.8 µs | 69-116 B | K |
| any glob or regex, no index | scan of the packed key arena | **1.28 ms** [9.2 ms] for all 100k keys | 11 ns (append) | 25.5 B | M |
| the same over the store's records | Export's plain scan | 2.49 ms [28.1] | 0 | 0 | M |
| regex with required literals | trigram prefilter + DFA verification (Cox) | µs when selective | 119 ns | 121 B | M (trigrams), C [TRI] |
| any regex, registered in advance | maintained match set (union DFA) | O(k) | ~20-40 ns + ~0.1 µs per matching pattern | 1 entry per match | E |
| many patterns at once (routing watchers) | Aho-Corasick / Teddy (SIMD multi-literal) / union DFA | O(key length + matches) | — | per pattern set | C [TED] |

### 1.4 Substring ("contains")

| Capability | Technique | Query | Per create | Memory/key | Status |
|---|---|---|---|---|---|
| any substring, selective (96 hits) | trigram index | **7.9 µs** [91] | 119 ns | 121 B | M |
| any substring, no index | AVX2 first/last-byte filter over the arena (Muła) | **85 µs** [1.29 ms] | 11 ns | 25.5 B | M |
| the same with glibc `memmem` | arena scan | 0.32 ms [3.2] | 11 ns | 25.5 B | M |
| substring count over a static snapshot | FM-index (compressed suffix array) | O(pattern length) | rebuild | compressed | C [FM] |
| substring existence over all keys | suffix automaton (DAWG) | O(pattern length) | amortized O(1) per appended byte | ≤ 2 states per key byte | C [SAM] |

### 1.5 Typos and similarity

| Capability | Technique | Query | Per create | Memory/key | Status |
|---|---|---|---|---|---|
| keys within edit distance 1 | SymSpell symmetric delete (21.5 hashed variants per key) | **1.2-1.5 µs** [1.5-2.2]: flat in n | 2.6 µs | **675 B** | M, C [SYM] |
| any edit distance, no index | Myers bit-parallel DP over the arena, length-filtered | **1.9-3.7 ms** [19-37] | 11 ns | 25.5 B | M |
| distance ≥ 2 without SymSpell's memory | Levenshtein automaton intersected with a trie (Lucene's FuzzyQuery) | ~visited trie paths | trie cost | trie | C [LEV] |
| "similar keys" (shared n-grams) | MinHash + LSH banding | ~O(bands) candidates | O(signature) | bytes per key | C [LSH] |

### 1.6 Aggregates and sketches (answers in O(1), maintained per write)

| Capability | Technique | Query | Per write | Memory | Status |
|---|---|---|---|---|---|
| count per directory or registered pattern | a counter updated on create/delete | O(1) | O(1) | 8 B per group | E |
| distinct values of something (e.g. per prefix) | HyperLogLog, standard error 1.04/√m | O(1) (O(m) merge) | O(1) | ~1.5 KB for ~2% error | C [HLL] |
| hottest keys (reads or writes) | Space-Saving (+ Count-Min) | O(k) | O(1) amortized | O(k) counters | C [SS] |
| value-size or latency quantiles | DDSketch (relative error, mergeable) | O(1) | O(1) | KB | C [DD] |

### 1.7 Change, time and sync

| Capability | Technique | Query | Per write | Status |
|---|---|---|---|---|
| change feed, resume, point-in-time export | revisions + history ring + cuts | O(k) | 3.3 ns + slot store | M ([export-watch.md](export-watch.md)) |
| "what changed recently" | read the history ring backwards | O(k) | 0 extra | E |
| TTL / expiry | hashed timing wheel: start, stop and tick in O(1) | O(1) | O(1) | C [TW] |
| compare-and-set, per-key versions | a revision per record | O(1) | +8 B/record | E |
| sync a client or replica by its difference only | Rateless IBLT: communication proportional to the difference | O(difference) | O(1) | C [IBLT] |
| the same, the classic way | Merkle tree over key-hash ranges (Dynamo-style anti-entropy) | O(difference × log n) | O(log n) | C [DYN] |
| O(1) sharding if KvNode ever spans nodes | jump consistent hash (no table, minimal movement) | O(log buckets) arithmetic | — | C [JCH] |

### 1.8 Values (outside today's opaque-bytes model)

| Capability | Technique | Note |
|---|---|---|
| find keys by exact value | hash index value → keys | O(1) + O(k); memory per key |
| index fields inside values | extract fields at write time into hash or sorted indexes | needs a value schema (JSON or protobuf paths) |
| combine several filters (AND/OR of result sets) | Roaring bitmaps over key ids | intersections in microseconds for 100k-id sets, C [ROAR] |
| similarity over embedding values | brute force (~ms at 100k × 384 dims) or HNSW (sub-ms), C [HNSW] | only if values become vectors |

## 2. What the measurements show

Numbers are from the cap probe at 100k keys, 1M in brackets:

1. **The key arena is the 100k multiplier.**
   - One contiguous copy of the keys (25.5 bytes per key, 11 ns per append)
     turns pattern, substring and typo queries into cache-speed scans:
     substring 85 µs with AVX2, any glob 1.3 ms, any typo distance 2-4 ms.
   - Scanning the store's records instead costs 1.6-6.5× more, because the
     records are spread over 16 MB.
   - SIMD matters. The AVX2 filter is 3.8× glibc `memmem` on a selective
     substring, 1.5× on a broad one. A production version dispatches to
     SSE2/SWAR on other CPUs, per the SIMD policy (ADR 0006).
2. **Indexes stay flat in n.** Selective substring by trigrams: 7.9 µs →
   91 µs. Typo lookup by SymSpell: 1.2-2.2 µs at both sizes. Every sorted
   operation stays under 0.33 µs. Arena scans grow 10×.
3. **The result size bounds everything.** `contains "kalo"` returns 34% of
   the keys. Even the trigram index needs 0.49 ms, and the arena scan is
   only 2× slower. An index pays off for selective queries only, the planner
   rule of key-indexes.md §3.
4. **At 100k a sorted array suffices for order.**
   - 2.25 µs per create keeps it sorted.
   - Successor, rank and ranges take ≤ 0.17 µs, and a count by any prefix
     or range takes 87 ns without touching the matching keys.
   - At 1M a create costs 30 µs, where a trie (O(key length)) or Wormhole
     (O(log key length)) takes over.
5. **Fast typo lookup costs memory.** SymSpell answers in about a
   microsecond but needs 675 bytes per key, 4× what the store needs per
   key, and 2.6 µs per create. Distance 2 would need ~10× more variants.
   Hence an opt-in, and Levenshtein automata over a trie for distance ≥ 2.
6. **A Bloom filter says "definitely absent" in 38 ns** (hashing included)
   at 10 bits per key and 1.04% false positives. That makes a client-side
   filter practical, where the client skips the network entirely for most
   absent keys. A static binary fuse filter does it in ~9 bits per key at
   0.39% (cited).

## 3. A bundle for a 100k collection

| Part | Gives | Per create | Memory/key |
|---|---|---|---|
| packed key arena (append; delete = tombstone; compact at 25% dead) | any glob/regex/substring/typo query in ≤ 4 ms, substring in 85 µs | ~11 ns | 25.5 B |
| sorted snapshot over the arena (or a trie once creates are frequent or n grows) | successor, rank, ranges, prefix/range counts in < 0.2 µs | 2.25 µs at 100k | 4 B |
| trigram index | selective substring and regex prefilter in µs | ~119 ns | 121 B |
| *opt-in:* SymSpell | typo lookup in ~1.3 µs | ~2.6 µs | 675 B |
| *opt-in:* exported binary fuse / Bloom filter | clients skip round trips for absent keys | rebuild or ~34 ns | 9-10 bits |
| *opt-in:* sketches (HLL, Space-Saving, DDSketch) | distinct counts, hot keys, size quantiles | O(1) | KBs total |

- **Total:** ~150 bytes per key without SymSpell. The store itself is ~162.
- **Only creates and deletes pay.** Value replacements, usually the most
  common write, change no key structure.
- **Get is never touched.**

**Consistency.** Queries that promise a point in time (Export, and
initial_state) need these structures to defer deletions while a cut is open,
exactly as in key-indexes.md §5. Queries that don't promise one (a typo
lookup, a count) read the current state.

## 4. Sources for the cited techniques

- **[BF]** Graf, Lemire. *Binary Fuse Filters: Fast and Smaller Than Xor
  Filters*, 2022. <https://arxiv.org/abs/2201.01174>. About 9 bits per key
  at 0.39% false positives (8-bit fingerprints), per the FastFilter
  library.
- **[MPH]** *Modern Minimal Perfect Hashing: A Survey*, 2025,
  <https://arxiv.org/abs/2506.06536>: ShockHash at ~1.52 bits per key.
  Groot Koerkamp, *PtrHash*, 2025, <https://arxiv.org/abs/2502.15539>:
  queries at RAM throughput with up to 3 bits per key.
- **[HOT]** Binna et al. *HOT: A Height Optimized Trie Index for
  Main-Memory Database Systems*, SIGMOD 2018; *Height Optimized Tries*,
  TODS 2022, <https://dl.acm.org/doi/10.1145/3506692>.
- **[WH]** Wu, Ni, Jiang. *Wormhole: A Fast Ordered Index for In-memory
  Data Management*, EuroSys 2019,
  <https://dl.acm.org/doi/10.1145/3302424.3303955>: O(log L) worst-case
  lookup for a key of length L.
- **[SURF]** Zhang et al. *SuRF: Practical Range Query Filtering with Fast
  Succinct Tries*, SIGMOD 2018,
  <https://dl.acm.org/doi/10.1145/3183713.3196931>.
- **[MEM]** *Memento Filter: A Fast, Dynamic, and Robust Range Filter*,
  2024, <https://arxiv.org/abs/2408.05625>: one cache miss per insert,
  delete and point query; up to two per range query.
- **[TRI]** Cox. *Regular Expression Matching with a Trigram Index*, 2012,
  <https://swtch.com/~rsc/regexp/regexp4.html>.
- **[TED]** Teddy, Hyperscan's SIMD multi-literal matcher as implemented in
  the Rust aho-corasick crate,
  <https://github.com/BurntSushi/aho-corasick/blob/master/src/packed/teddy/README.md>.
- **[FM]** Ferragina, Manzini. The FM-index, 2000: count occurrences in
  O(pattern length). <https://en.wikipedia.org/wiki/FM-index>.
- **[SAM]** Suffix automaton / DAWG,
  <https://cp-algorithms.com/string/suffix-automaton.html>.
- **[SYM]** Garbe. SymSpell, symmetric delete,
  <https://github.com/wolfgarbe/SymSpell>.
- **[LEV]** Schulz, Mihov. *Fast String Correction with Levenshtein
  Automata*, 2002. Lucene's FuzzyQuery became ~100× faster with it:
  <https://blog.mikemccandless.com/2011/03/lucenes-fuzzyquery-is-100-times-faster.html>.
- **Myers.** *A fast bit-vector algorithm for approximate string matching
  based on dynamic programming*, J. ACM 1999: the measured brute-force typo
  search.
- **Muła.** *SIMD-friendly algorithms for substring searching*, 2016,
  <http://0x80.pl/notesen/2016-11-28-simd-strfind.html>: the measured AVX2
  filter.
- **[HLL]** Flajolet et al. *HyperLogLog*, 2007: standard error 1.04/√m.
  <https://algo.inria.fr/flajolet/Publications/FlFuGaMe07.pdf>.
- **[SS]** Metwally et al. Space-Saving, and Cormode, Muthukrishnan
  Count-Min, for heavy hitters. Notes:
  <https://web.stanford.edu/class/cs168/l/l2.pdf>.
- **[DD]** Masson, Rim, Lee. *DDSketch*, VLDB 2019,
  <https://arxiv.org/abs/1908.10693>.
- **[TW]** Varghese, Lauck. *Hashed and Hierarchical Timing Wheels*, SOSP
  1987, <https://dl.acm.org/doi/10.1145/41457.37504>: O(1) start, stop and
  maintenance of timers.
- **[IBLT]** Yang, Gilad, Alizadeh. *Practical Rateless Set
  Reconciliation*, SIGCOMM 2024,
  <https://dl.acm.org/doi/10.1145/3651890.3672219>.
- **[JCH]** Lamping, Veach. *A Fast, Minimal Memory, Consistent Hash
  Algorithm*, 2014.
- **[ROAR]** Chambi et al. *Better bitmap performance with Roaring
  bitmaps*, 2014, <https://arxiv.org/abs/1402.6407>: up to 900× faster
  intersections than older compressed bitmaps.
- **[LSH]** Broder. *On the resemblance and containment of documents*
  (MinHash), 1997. Indyk, Motwani, locality-sensitive hashing, 1998.
- **[DYN]** DeCandia et al. *Dynamo: Amazon's Highly Available Key-value
  Store*, SOSP 2007: Merkle trees for replica anti-entropy.
- **[HNSW]** Malkov, Yashunin. *Efficient and robust approximate nearest
  neighbor search using Hierarchical Navigable Small World graphs*, 2016,
  <https://arxiv.org/abs/1603.09320>.
- **Learned indexes for strings** (not proposed: they fit static, sorted
  data): SIndex, APSys 2020,
  <https://ipads.se.sjtu.edu.cn/_media/publications/apsys20-final71.pdf>.

## 5. Limits

- **An arbitrary unanchored regex, or a fuzzy query beyond an index, is
  O(n).** That is 1-4 ms at 100k and 10-37 ms at 1M. At 100M it would be
  seconds, and only registration (maintained match sets) avoids it.
- **KvNode stores values as opaque bytes,** so every value-content
  capability (§1.8) needs a schema decision first.
- **Sketches are approximate by design.** Exact distinct counts over large
  groups cost memory proportional to the group.
- **Every key structure is paid for by creates and deletes.** A
  create-heavy workload should pick the smallest bundle that answers its
  queries.

## 6. Open decisions

1. **The first bundle.** Proposed: arena + sorted snapshot + trigram
   index, with SymSpell and client filters as opt-ins.
2. **Approximate answers** (sketches): first-class RPCs, or internal
   statistics only?
3. **Client-side filters:** a `GetFilter` RPC that returns a binary fuse
   filter the client can hold, rebuilt every N writes or seconds.
4. **Structured values:** whether KvNode should ever index inside values.
   That changes its opaque-bytes contract.
