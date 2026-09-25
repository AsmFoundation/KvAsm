# ADR 0001: The store is a bucketized cuckoo hash table

- **Status:** accepted
- **Date:** 2026-09-26
- **Evidence:** `server/bench/bench_ds.c` (run with `make -C server bench`); the
  rivals live in `server/bench/ds/` and never enter the server binary.

## Context

The requirement is **O(1) for every operation** (Get, Put, Delete) on exact
binary keys, with many concurrent readers. One hypothesis was that a linked
structure, "a linked list to produce a binary tree with assembly tricks",
could beat a hash table. It deserved a measurement, not an opinion.

## Candidates

All three are x86-64 assembly behind the same intrusive API. They share the
same records, and the same SipHash-2-4-128 of the key is computed *inside*
every timed operation, as the server does.

| | Structure | Design |
|---|---|---|
| **cuckoo** | `src/cuckoo.asm`, the production code | 2 candidate buckets × 6 slots, 64-byte buckets (one cache line), 8-bit SWAR tag match, version word per bucket (all the concurrency bookkeeping included) |
| **chain** | `bench/ds/chain.asm` | separate chaining, a cached hash per node, load ≈ 1.58 |
| **trie** | `bench/ds/trie.asm` | binary crit-bit trie over the hash with the "assembly tricks": branchless child selection (`bt`/`sbb`), the crit bit kept in the top byte of tagged pointers, a leaf flag in bit 0, 16-byte nodes |

## Results

Single thread, pinned, ns per operation (lower is better). Every answer was
verified, and a wrong one fails the run. WSL2 vCPUs, so the ratios are the
result and absolute numbers are indicative.

**4,194,304 keys (256 MiB of records):**

| | build | get-lat | get-thr | miss | update | churn | p50 | p99 | p99.9 |
|---|---|---|---|---|---|---|---|---|---|
| cuckoo | **180** | **279** | **260** | **166** | **180** | **315** | **258** | **518** | **714** |
| chain | 192 | 351 | 350 | 271 | 209 | 406 | 321 | 807 | 1897 |
| trie | 849 | 1060 | 1064 | 978 | 860 | 1107 | 1099 | 1954 | 13329 |

**1,048,576 keys:** cuckoo 232 get-lat / 636 p99.9; chain 275 / 897;
trie 778 / 6549.

**65,536 keys (cache-resident):** chain wins hits (get-lat 102 vs 125) and
builds faster. Cuckoo still wins misses (27 vs 59) and the tail (p99.9 467
vs 626).

Column meanings: *get-lat* is a dependent chain of lookups, so nothing
overlaps and it measures pure per-request latency. *get-thr* and *miss* are
independent hits and misses. *churn* is a delete plus a re-insert.

## Decision

**Bucketized cuckoo hashing.** The hypothesis is disproved at every scale
that matters:

- **The binary trie loses by 3.8–5.9×** on every operation at 4M keys, and
  its p99.9 tail is **18.7× worse** (13.3 µs vs 0.7 µs). Branchless descent
  removes branch mispredictions but not the real cost: a dependent cache
  miss per level, about 22 levels deep at 4M keys. It is O(log n) with a
  large constant, so it fails the O(1) requirement by construction.
- **Chaining is close on small, cache-resident sets but loses at scale**: 26%
  slower hits, 63% slower misses, 2.7× worse p99.9 at 4M keys. Its worst
  case is O(chain length), not O(1). Lock-free chaining also needs
  per-node reclamation on every pointer hop.
- **Cuckoo lookups are O(1) in the worst case:** at most 2 buckets, each
  one cache line, with 6 tags compared in one SWAR step. Inserts are O(1)
  expected, with a bounded BFS (at most 256 nodes) for displacement; when
  the BFS fails, the insert reports `RESOURCE_EXHAUSTED` instead of
  degrading.

## Consequences

- The store has a hard **key capacity** (`KVNODE_CAPACITY`), because the
  table is sized up front: buckets = next power of two of
  ⌈capacity / 5⌉, at most about 83% load. It does not resize while serving.
  Resizing would need a stop-the-world rehash or a doubled footprint, which
  would break the latency profile above.
- Concurrency builds directly on the bucket layout (ADR 0003). The version
  word in each 64-byte bucket gives lock-free readers a seqlock at no extra
  cache-line cost.
- Revisit this decision if range or prefix queries ever become a
  requirement: that is the one thing an ordered structure does and a hash
  table cannot.
