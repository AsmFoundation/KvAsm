# Capability probe: what is O(1) or near instant at 100k keys (2026-09-29)

Evidence for [docs/proposals/capabilities.md](../../../docs/proposals/capabilities.md),
answering: "what O(1) or near-instant capabilities can a 100k-key collection
have?"

**Run conditions**

- **What:** [`cap_probe.c`](cap_probe.c) on the server's real store, run by
  [`run.sh`](run.sh).
- **Revision:** 29ab560. **Host:** i9-13900K, WSL2, one pinned vCPU (4), 7
  repetitions, medians. 100k keys, and 1M for scale.
- **Keys:** `user:<5 syllables>:<field>`, 20-21 bytes, 100-byte values.
- **Isolation.**
  - The committed [`output.txt`](output.txt) is the second run. `docker
    events` for its window shows only its own exec.
  - The first run overlapped another session's `build/probe/mode.sh` probes
    and was discarded. It agreed within ~10-20% except SymSpell, which was
    ~2× slower under the overlap.
  - Other programs on the Windows host were not controlled.
- **Checks.**
  - Every result count is checked against a plain reference (per-key
    memmem, glob, or full-DP edit distance), and a mismatch exits non-zero.
  - Myers' routine is cross-checked against the DP on 20,000 random pairs
    first.
  - SymSpell is timed on 1,000 *different* typos, each of which must find
    its original key.

## Results at 100k keys (1M in brackets)

**Sub-microsecond (O(1) or O(log n)):**

| Capability | Technique | Latency |
|---|---|---|
| exact get | the cuckoo table, batched pipeline | **27 ns** [41] |
| "definitely absent" | blocked Bloom filter, 10 bits/key, 1.04% false positives | **38 ns** [39], hashing included |
| successor / predecessor / rank | sorted snapshot, binary search | **165 ns** [324] |
| count of keys with any prefix | sorted snapshot, two binary searches, no iteration | **87 ns** [156] |
| range scan, 100 keys in order | sorted snapshot | **160 ns** [192] for all 100 |
| typo lookup, edit distance 1 | SymSpell (symmetric delete) index | **1.2-1.5 µs** [1.5-2.2]: flat in n |

**Near instant (≤ 100 µs):**

| Capability | Technique | Latency |
|---|---|---|
| contains `lomika` (96 keys) | trigram index | **7.9 µs** [91 µs] |
| contains `lomika`, any substring, no index | AVX2 scan of a packed key arena | **85 µs** [1.29 ms] |

**Interactive (milliseconds, O(n), no index needed):**

| Capability | Technique | Latency |
|---|---|---|
| contains `lomika` | glibc `memmem` over the arena | 0.32 ms [3.2] |
| contains `lomika` | scan of the store's records | 2.07 ms [25.0] |
| contains `kalo` (34,008 keys: output-bound) | trigram / AVX2 arena / memmem arena / records | 0.49 / 1.05 / 1.53 / 2.44 ms |
| glob `user:ka*mi*:e*` (10,560 keys) | arena / records | 1.28 / 2.49 ms [9.2 / 28.1] |
| typos within distance 1 | Myers bit-parallel over the arena | 1.9-3.7 ms [19-37] |
| typos within distance 2 | Myers bit-parallel over the arena | 3.7 ms [36.8] |

**What each structure costs.** Per-key build time stands in for what a create
pays to keep the structure current.

| Structure | Per key (build) | Memory per key |
|---|---|---|
| packed key arena | 10.6 ns | 25.5 B (2.4 MB at 100k) |
| sorted snapshot (ids into the arena) | a full sort is 4.8 ms at 100k; **a create must insert: 2.25 µs at 100k, 30 µs at 1M** (memmove) | 4 B |
| trigram index | 119 ns | 121 B |
| SymSpell, distance 1 | 2.6 µs (21.5 hashed variants per key) | **675 B** |
| blocked Bloom filter | 34 ns | 10 bits |

For scale, the store itself holds ~162 bytes per key.

## What it decides

1. **At 100k, a packed key arena makes *any* key query near instant without
   an index.**
   - 2.4 MB of keys fits in cache. A substring over all keys takes 85 µs
     with AVX2 (0.32 ms with `memmem`); any glob takes 1.3 ms; any typo
     search, at any distance, takes 2-4 ms.
   - Scanning the store's own records is 1.6-6.5× slower, because the records
     are spread over ~16 MB.
   - At 1M everything O(n) grows ~10×, to 10-37 ms.
2. **Indexes make selective queries flat in n.** Trigram: 8 µs → 91 µs from
   100k to 1M, output included. SymSpell: 1.2-2.2 µs at both sizes. Sorted
   operations: 87-324 ns.
3. **Output bounds everything.** A query that returns a third of the keys
   (`kalo`) costs about half a millisecond even with an index.
4. **A plain sorted array is enough at 100k.**
   - Keeping it sorted costs 2.25 µs per create. It answers successor,
     rank, ranges and prefix *counts* in under 0.2 µs.
   - At 1M a create costs 30 µs, which is where a trie (key-indexes.md) or
     Wormhole (O(log key length)) takes over.
5. **SymSpell is fast but heavy.** At 675 bytes per key, 4× the store's
   whole per-key cost, it belongs behind an opt-in. Distance 2 would need
   ~10× more. Levenshtein automata over a trie are the scalable route for
   distance ≥ 2.
6. **Bloom filter.** At 10 bits per key it answers "definitely absent" in
   38 ns, hashing included, which makes it a candidate for a client-side
   filter. Binary fuse filters do better: ~9 bits per key at 0.39% false
   positives (Graf and Lemire, 2022), for a static filter shipped to
   clients.
