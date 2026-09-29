# Proposal: key indexes for O(1) prefix, suffix and pattern queries

- **Status:** proposed, 2026-09-29. Nothing here is implemented or decided.
  It extends [export-watch.md](export-watch.md): without an index every
  filtered Export scans the whole table.
- **Evidence:**
  [benchmarks/results/2026-09-29-index-probe](../../benchmarks/results/2026-09-29-index-probe/README.md).
  It compares a full scan, hash-addressed segment directories and adaptive
  radix tries (libart) on the server's real store, at 1M and 16M keys.

## Summary

- **What O(1) can mean.** A query that returns k keys costs at least O(k).
  The achievable goal is **O(1) or O(pattern length) to locate the matches,
  plus O(k) to emit them, whatever the number of keys n**. Measured at 16M
  keys, a 4-key prefix query drops from **447 ms (scan) to 0.2 µs**.
- **The price is paid by key creation and deletion only.** Replacing a value
  leaves the key set, and so every key index, unchanged. Get is untouched.
- **Three opt-in index kinds, one planner:**
  1. **Segment directories.** Every key joins the directory of each of its
     delimiter-terminated prefixes and of its last segment.
     - Answers aligned prefixes and suffixes in O(1) + O(k).
     - The cheapest to maintain: **+45-89%** on a key's create or delete,
       and 110-116 bytes per key for three directories.
  2. **Radix tries** over the keys (any prefix) and over the reversed keys
     (any suffix).
     - Also answer globs and regexes anchored by a literal prefix or suffix.
     - Cost per create or delete: **0.9-2.1× the base write per direction**,
       plus 69-85 bytes per key per direction.
  3. **Registered patterns.** Named filters, arbitrary regexes included,
     whose match sets are maintained as keys come and go. They make even an
     unanchored regex O(k), at the cost of one DFA run per created or deleted
     key.
  - **The planner** picks an index or the scan per query. An index loses on
    unselective queries: returning a quarter of all keys through per-key
    lookups measured 0.45-0.89× the scan.
- **What stays O(n):** an arbitrary regex with no literal anchor that was not
  registered. No key index can answer that without looking at every key.

## 1. The lower bound, and what "O(1)" means here

- **Emitting results.** Emitting k keys costs Ω(k), so no index makes a
  query that returns a quarter of the store cheap.
- **Locating them.** An index removes the other term: the cost of *finding*
  the matches.
  - Without one, that is a full scan: about 1 ns per key to walk the
    table, 11-41 ns with each key read and matched (export probe).
  - With one, it is a hash lookup (directories), a descent by the pattern's
    length (tries), or nothing at all (registered patterns).
  - None of those grows with n.
- **Maintenance.** It is O(1) per key (directories: a fixed number of hash
  operations) or O(key length) (tries), also independent of n.
  - Measured costs still rise from 1M to 16M keys, because deeper cache and
    TLB misses make each step slower. The number of steps does not grow.

## 2. Index kinds

### 2.1 Segment directories

- **Structure.**
  - A SipHash-keyed table maps a directory name to a directory: an
    unordered member array with O(1) swap-remove.
  - For `t007:u0000123:email` with delimiter `:`, depth 2 and suffixes on,
    the key joins three directories: `t007:`, `t007:u0000123:`, and the
    suffix directory `:email`.
  - Each index entry lives next to its record: the record header points to
    it, and a replace carries the pointer to the new record. A delete
    therefore finds its entries without another lookup (the probe models
    this with a direct array).
- **Answers.**
  - A delimiter-aligned prefix or suffix is one hash lookup plus k members.
  - Any other prefix filters the nearest enclosing directory. `t007:u00001`
    measured 1,835× faster than the scan at 16M keys.
  - A glob or regex whose literal prefix reaches a directory is answered
    the same way, verified by its DFA.
- **Cost, measured at 16M keys:**
  - +221 ns per key deletion and +321 ns per key creation, against the
    store's own 486 / 411 ns;
  - 110 bytes per key.
- **Also free:** the planner gets exact result counts, since a directory
  knows its size in O(1).

### 2.2 Radix tries

- **Structure.** An adaptive radix tree (Leis et al., "The Adaptive Radix
  Tree", ICDE 2013) over the keys, and optionally another over the reversed
  keys.
  - Node types 4/16/48/256, with path compression.
  - Keys are NUL-terminated inside the trie, so no key is a prefix of
    another.
  - The Node16 search uses SSE2 (baseline x86-64), with a SWAR fallback,
    as the tag scan does (ADR 0006).
- **Answers.**
  - Any prefix at any byte boundary: descend the prefix, then emit the
    subtree. That is O(prefix length) + O(k) (22,907× the scan at 16M keys
    for a non-aligned prefix).
  - Any suffix, through the reversed trie.
  - Globs and regexes anchored by a literal prefix or suffix: candidates
    from that subtree, verified by the DFA (42,573× for `t007:u00001*:e*`).
  - A later refinement walks the trie guided by the DFA, pruning any
    subtree from which the pattern cannot match, so classes such as `[0-9]`
    prune too.
- **Cost, measured at 16M keys:**
  - forward trie: +758 ns per deletion, +848 ns per creation, 85 bytes per
    key;
  - reversed trie: +582 / +548 ns, 69 bytes per key.
  - Each trie level is a dependent cache miss, which is why tries cost more
    than directories.
- **Concurrent readers.** An export walks the trie while writers change
  it. Readers need optimistic lock coupling: a version per node validated
  on the way down, the bucket seqlock idea again. Freed nodes go through
  QSBR.

### 2.3 Registered patterns

- **How it works.** An operator registers named filters: globs or regexes,
  anchored or not.
  - All registered patterns compile into **one union DFA** whose accepting
    states carry the set of patterns that match.
  - On every key creation or deletion the key runs through it once: about
    20-40 ns for 16-byte keys, whatever the number of patterns (export
    probe's DFA cost).
  - Each matching pattern's member set gets one directory-like operation:
    ~70-110 ns each, from the measured per-directory cost.
- **Answers.** An Export whose filter is exactly a registered pattern
  becomes O(k), with no candidates and no verification.
- **Same machinery for Watch.** export-watch.md §6.8 routes events to
  watchers with a union DFA per worker. Registered patterns and watch
  filters are the same technique pointed in two directions.
- **Limits.** How many patterns can be registered, and the union DFA's
  state budget (§4.5 of export-watch.md). Past the budget, registration is
  refused rather than slowing every write.

## 3. The planner

For each Export (and `initial_state`) filter:

1. **Exact keys only:** point lookups through the cut. A consistent
   multi-get needs no scan (export-watch.md §5.8).
2. **A registered pattern equal to the filter:** its member set.
3. **A prefix or suffix, or a pattern with a literal prefix or suffix:**
   - use the best index that covers it: a directory for an aligned
     literal, a trie for any literal;
   - verify candidates with the filter's DFA.
4. **Otherwise, or when the estimated k is large:** the scan.

The threshold comes from the measurements:

- The scan costs 34 ns per key at 16M keys.
- Fetching a result through an index costs 151 ns (directories) or 296 ns
  (tries): one random lookup per result, in the batched GET pipeline.
- So an index wins below roughly k < 0.2 n (directories) or k < 0.1 n
  (tries).
- Directories report exact counts. Tries can keep subtree counts (a
  write-time cost to measure) or fall back to sampling.

## 4. Maintenance: inside the write, or from the change stream

- **Synchronous.** Index work runs inside the create or delete.
  - The simplest option, and the index is always current.
  - The create/delete latency grows by the index cost above.
- **Asynchronous.** An indexer consumes the Watch history ring
  (export-watch.md §6).
  - Events already carry `created`, so replaces are skipped.
  - Writers pay nothing beyond the history append.
  - The index lags by microseconds to milliseconds. A query at cut revision
    R waits until the indexer has applied every revision ≤ R: the stable
    revision of export-watch.md §3.1 makes "applied up to R" exact.
  - An indexer that falls outside the retained history rebuilds from a cut:
    one full scan, 0.4-0.7 s per 16M keys at the measured scan rates.
- **Proposed default:**
  - directories synchronous: they are cheap;
  - tries and registered patterns asynchronous, because their per-write
    cost is the highest.

  Gate I2 checks that the indexer keeps up with the peak create/delete rate.

## 5. Point-in-time consistency with cuts

An indexed Export must return exactly the keys that existed at the cut's
revision R, like a scanned one:

- **Keys created after R** may already be in the index. The value lookup
  through the cut filters them out: the key does not exist at R.
- **Keys deleted after R** must still be found. So an index deletion is
  deferred while any cut older than it is active: the entry is marked with
  the deleting revision and removed physically when the last such cut
  ends.
- **Values** are read through the cut, from the preserved page or the live
  page (export-watch.md §5), so they are the values at R.

A persistent (path-copying) trie would get snapshots for free, with one
immutable root per revision. It would also copy O(depth) nodes on every
create or delete, so deferred deletion is proposed instead (§8).

## 6. Costs at a glance

| | Answers in O(1)/O(len) + O(k) | Extra write cost per key create/delete (16M keys) | Memory per key |
|---|---|---|---|
| none (scan) | nothing: every query is O(n) | 0 | 0 |
| segment directories (depth 2 + suffix) | aligned prefixes and suffixes; narrows the rest | +221 / +321 ns (+45% / +78%) | 110 B |
| trie, forward | any prefix; literal-prefix patterns | +758 / +848 ns | 85 B |
| trie, reversed | any suffix; literal-suffix patterns | +582 / +548 ns | 69 B |
| registered patterns (m matching) | the registered filters, any regex | ~20-40 ns + m × ~70-110 ns (estimated from measured parts) | one entry per match |

The store itself costs 486 ns per delete and 411 ns per insert at 16M keys
in the same probe, and about 162 bytes per key.

## 7. Configuration (proposed)

| Variable | Values | Default | Meaning |
|---|---|---|---|
| `KVNODE_INDEX_DELIMITERS` | bytes | (unset: no directories) | delimiters that end a directory prefix |
| `KVNODE_INDEX_DEPTH` | 0-8 | 2 | prefix levels indexed |
| `KVNODE_INDEX_SUFFIX` | `off` \| `on` | `on` | also index the last segment |
| `KVNODE_INDEX_TRIE` | `off` \| `prefix` \| `suffix` \| `both` | `off` | radix tries |
| `KVNODE_INDEX_MODE` | `sync` \| `async` | `sync` for directories, `async` for tries and patterns | where index work runs (§4) |
| `KVNODE_PATTERNS` | path | (unset) | registered patterns, one `name = glob:…` or `name = regex:…` per line |

Index memory is data-proportional, so it counts toward
`KVNODE_MAX_MEMORY_MB`. When the cap is reached, a create is refused before
any index is touched (open decision 3).

## 8. Alternatives considered

| Alternative | Why not |
|---|---|
| An ordered structure as the main index (B-tree, skip list) | O(log n) point operations; the bake-off chose a hash table for O(1) (ADR 0001) |
| Placing keys in the cuckoo table by a hash of their prefix | breaks load balance and the two-bucket bound; a query still costs its whole region, not O(k) |
| Persistent (path-copying) tries for free snapshots | copies O(depth) nodes per create/delete; deferred deletion keeps writes cheaper (§5) |
| A trigram index for "contains" queries | ~(key length − 2) posting updates per create, about 16 for these keys, so an estimated 1.1-1.8 µs at the measured per-set cost, plus ~16 postings per key. It prunes only patterns with required trigrams |
| Suffix trees or arrays over all keys | every suffix of every key: memory grows with the key length for each key |

## 9. Verification plan

- **Store-level model check.**
  - Random creates, deletes and replaces against a model.
  - Every index's contents must equal the model's key set: after every
    operation in the small runs, and at random checkpoints in the long ones.
  - At each open cut's revision, the index must answer exactly as the model
    at that revision.
- **Differential queries.** At least 100k random filters (prefixes,
  suffixes, anchored and unanchored globs and regexes). Each is answered by
  every applicable index and by the scan, and all answers must be equal.
- **Concurrency.** Lock-free trie readers against writers under the
  optimistic protocol, with freed nodes poisoned in the test build.
- **Async indexer.** Lag and catch-up at the peak create/delete rate, then
  a rebuild after forced overflow.

## 10. Measurement gates

| Gate | Measures | Pass |
|---|---|---|
| I1 | SET and create/delete throughput with each index on vs off (paired A/B, ADR 0009 method) | documented per index; directories ≤ +90% on create/delete, replace ±noise |
| I2 | async indexer lag at the peak create/delete rate | keeps up, or its limit is documented |
| I3 | query latency per kind vs the scan, and the planner threshold | the planner's choice is never slower than the scan by more than noise |
| I4 | memory per key per index at 1M, 16M, 100M keys | matches §6 within 10% |

## 11. Phases

These phases come after export-watch.md's phases 1-4 (filters, streaming,
cuts, history):

1. **Segment directories** (synchronous) and the planner. This covers the
   common `namespace:id:field` keyspaces.
2. **Registered patterns:** union DFA and member sets, plus the Watch fan-out
   routing that shares them.
3. **Radix tries,** forward and reversed, in assembly with optimistic
   concurrency. Asynchronous or synchronous depending on gate I1 and I2.

## 12. Open decisions

1. **Which kinds, in which order.** Proposed: directories, then registered
   patterns, then tries.
2. **Where tries are maintained:** asynchronously from the history
   (proposed) or inside the write.
3. **Memory.** Does index memory count toward `KVNODE_MAX_MEMORY_MB`
   (proposed: yes)?
4. **How patterns are registered:** a config file read at startup
   (proposed), or an admin RPC that registers them at runtime (needs an
   authorization story KvNode does not have).
