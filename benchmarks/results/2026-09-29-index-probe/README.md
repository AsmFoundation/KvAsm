# Revision sequence and key-index probes (2026-09-29)

Design-stage evidence for two review points on
[docs/proposals/export-watch.md](../../../docs/proposals/export-watch.md):

1. "Why do we need a lock? We can just atomically increment."
   ([`seq_probe.c`](seq_probe.c))
2. "How do we get O(1) for prefix or suffix, and maybe even regex?"
   ([`index_probe.c`](index_probe.c), which led to
   [docs/proposals/key-indexes.md](../../../docs/proposals/key-indexes.md))

**Run conditions**

- **Revision:** 5b5f2ae. **Host:** i9-13900K, WSL2, THP `always`.
- **Pinning:** `seq_probe` on vCPUs 1-7 (its verifier on 16), `index_probe`
  on vCPU 4. 5 repetitions, medians.
- **Store code:** assembled fresh from `server/src` by [`run.sh`](run.sh).
- **libart:** the rival trie, fetched at commit `3010468` and verified by
  SHA-256. It is not committed.
- **Isolation.** `docker events` for the run window shows only this run's
  execs.
  - An earlier `seq_probe` timing run overlapped another session's
    `make itest`, so it was discarded and re-run.
  - Other programs on the Windows host were not controlled, so absolute
    numbers are indicative.
- **Raw output:** [`output.txt`](output.txt).
- **Checks.** Every count, watermark and returned value is checked, and a
  wrong one exits non-zero.

## 1. Revisions without the writer lock

An atomic increment gives unique numbers. It does not tell a reader which
revisions are *finished*: writer A takes 7, writer B takes 8, and B's write
can land first. Watchers and cuts need a monotonic watermark: every revision
at or below it is done. The probe measures the counter and three ways to
keep that watermark.

**The shapes compared** (from the file header):

- `mutex+inc`: the original proposal, a plain increment inside the writer
  mutex.
- `mutex+lazy`: today's serialized writers running the fixed protocol.
- `xadd`: the counter alone.
- `seq+cas`: every writer CAS-advances one shared watermark.
- `seq+inflight`: each writer announces a bound in its own cache line;
  readers take the minimum.
- `seq+lazy`: writers publish their own slot and nothing else; readers
  advance the watermark over published slots.

| Variant | 1 thread, ns/op | 7 threads, M ops/s (system) | Watermark safe? | Monotonic? |
|---|---|---|---|---|
| `mutex+inc` | 8.62 | 15.1 | (serialized) | (serialized) |
| `mutex+lazy` | 11.94 | 12.5 | (serialized) | (serialized) |
| `xadd` | **3.00** | **136.6** | — | — |
| `seq+cas` | 9.31 | 13.4 | yes | yes |
| `seq+inflight` | 8.37 | 30.2 | yes | **no: went back 312-3,070 times per verified run** |
| **`seq+lazy`** | **4.42** | **41.6** | **yes** | **yes, by construction** |

The verifier followed each watermark concurrently. Across 4M-28M revisions
per configuration, no lock-free watermark ever covered an unpublished
revision.

What it decides:

- **The counter needs no lock.** `LOCK XADD` costs 3 ns and hands out
  ~137M revisions per second across 7 cores. That is about 60× today's write
  rate (2.23M SET/s at 4 cores), so the sequencer is not a future bottleneck.
- **The watermark is where the lock hides.**
  - A shared CAS-advanced watermark collapses to mutex speed (13.4 vs
    15.1M/s), because every writer serializes on one cache line.
  - Per-writer announcements scale better, but can go **backwards**. A
    writer's announced bound can be stale by the time it becomes visible,
    and re-announcing until the counter holds still narrows that window
    without closing it. Every consumer would then have to clamp.
- **`seq+lazy` wins.**
  - A writer reserves with `LOCK XADD` and publishes its own slot, the
    history event it writes anyway.
  - Readers advance the watermark over contiguous published slots.
  - No writer-side coordination, monotonic by construction, and 2.75× the
    mutex at 7 threads.
  - Under today's single writer mutex it costs +3.3 ns per write (11.94 vs
    8.62 ns, empty critical section), noise against a real Put.
- **Remaining headroom.** `seq+lazy` at 7 threads is still below `xadd`
  alone. The gap is concurrent writers publishing adjacent slots of one
  cache line. Spreading consecutive revisions across lines is an
  optimization to measure if writes ever become concurrent.

## 2. O(1) prefix, suffix and pattern queries

A query that returns k keys costs at least O(k). "O(1)" here means locating
the matches independently of the store size n. The probe compares the full
scan with two index shapes on hierarchical keys (`t%03u:u%07u:<field>`,
1,000 tenants, 4 fields).

- **DIR:** hash-addressed segment directories. Each key joins the directory
  of each of its `:`-terminated prefixes (depth 1 and 2) and of its last
  segment.
- **ART:** adaptive radix tries, one over the keys and one over the reversed
  keys.

**Write path** (ns per key, index work only; values are replaced without
touching any key index):

| | 1M keys: delete / insert | 16M keys: delete / insert | Bytes per key |
|---|---|---|---|
| store itself (base) | 298 / 270 | 486 / 411 | (~162 today) |
| DIR (3 directories) | 159 / 241 | 221 / 321 | 110-116 |
| ART forward (prefixes) | 333 / 389 | 758 / 848 | 85 |
| ART reversed (suffixes) | 267 / 286 | 582 / 548 | 69 |
| ART both | 600 / 675 | 1,340 / 1,396 | 154 |

**Queries at 16M keys** (median ms to produce every matching key and value):

| Query | k | Full scan | DIR | ART |
|---|---|---|---|---|
| Q1 prefix `t007:` | 16,000 | 452 | 3.88 (116×) | 2.71 (167×) |
| Q2 prefix `t007:u0000123:` | 4 | 447 | **0.0002 (1.9 M×)** | **0.0002 (2.2 M×)** |
| Q3 prefix `t007:u00001` (not at a delimiter) | 400 | 453 | 0.247 (1,835×) | 0.020 (22,907×) |
| Q4 suffix `:email` (a quarter of all keys) | 4,000,000 | **538** | 604 (0.89×) | 1,184 (0.45×) |
| Q5 glob `t007:u00001*:e*` | 100 | 437 | 0.362 (1,207×) | 0.010 (42,573×) |
| Q6 glob `*u000012*` (no literal prefix or suffix) | 40,000 | 752 | scan | scan |

The 1M-key figures show the same pattern ([`output.txt`](output.txt)).

What it decides:

- **Selective prefix and suffix queries become O(1) + O(k).** A 4-key query
  drops from 447 ms to 0.2 µs.
- **Directories are the cheap index.**
  - They add +45-89% to a key's create or delete and 110-116 bytes per key,
    for delimiter-aligned prefixes and suffixes.
  - A non-aligned prefix filters its nearest directory: Q3 is still 1,835×
    faster than the scan.
- **Tries are the general index.**
  - Any prefix at any byte, any suffix, and globs or regexes anchored by a
    literal prefix or suffix (Q5: 42,573×).
  - The cost is 1.1-2× the base write per direction per create or delete,
    and 69-85 bytes per key per direction.
  - Their cost grows from 1M to 16M keys through cache and TLB misses per
    trie level, not algorithmically: it is O(key length), independent of n.
- **Unselective queries must scan.**
  - Returning 4M values through per-key lookups is slower than one
    prefetched pass over the table (Q4).
  - So a query needs a planner. Directory sizes are exact counts, available
    in O(1).
- **Regex with no literal anchor stays O(n)** (Q6): no key index answers an
  arbitrary unanchored regex without scanning. Such a pattern can only be
  O(k) if it is registered in advance and its match set is maintained as
  keys are created and deleted (the proposal's "registered patterns").
