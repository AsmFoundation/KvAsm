# Plan: KvNode functions — design, usage and rollout

- **Status:** proposed plan, 2026-10-01. Nothing here is implemented or
  decided. Each phase would land with its own ADR.
- **What it covers:** the 46 functions KvNode could offer beyond Get, Put,
  Delete, Upload and Download: 38 in groups A-G, and 8 more in Group H,
  added on 2026-10-01 from [golden-services.md](golden-services.md). They
  were chosen from the measured catalogs for the business value customers
  get from them. The same date added Track S (§V.10): three store speedups
  with no API, which make existing functions faster. For each function:
  - why customers want it;
  - the API;
  - the exact semantics;
  - how the server does it (thread safety, SIMD, memory);
  - what it costs at 10M keys;
  - how to use it, and its pitfalls.

  Then end-to-end usage recipes, and the engineering plan: protocol, server
  modules, configuration, security, persistence, the client library,
  verification, measurement gates and phases.
- **Companions:**
  - [golden-operations.md](golden-operations.md): the measured costs at 10M
    keys.
  - [golden-services.md](golden-services.md): ten more golden services
    (Group H: compact values, struct fields, bitmaps, small sets,
    near-duplicates, geo, TopK, quantiles, sliding windows, scoring) and
    six store speedups (8-lane SipHash, flat combining, UMWAIT, ...),
    measured at 10M keys. It also measures F3 and F4 below.
  - [capabilities.md](capabilities.md): the same question at 100k keys.
  - [key-indexes.md](key-indexes.md): prefix, suffix and pattern indexes.
  - [export-watch.md](export-watch.md): Watch, Export, revisions and cuts.
  - [ADR 0010](../adr/0010-streaming-upload-download.md): the streaming layer
    and record pins.
- **Evidence labels:**

  | Label | Source |
  |---|---|
  | **M** | measured at 10M keys ([golden probe](../../benchmarks/results/2026-09-29-golden-probe/README.md)) |
  | **K** | the [index probe](../../benchmarks/results/2026-09-29-index-probe/README.md), 16M keys |
  | **X** | the [export probe](../../benchmarks/results/2026-09-29-export-probe/README.md) |
  | **S** | the revision (sequence) probe in the index-probe directory |
  | **V** | the [services probe](../../benchmarks/results/2026-10-01-services-probe/README.md), 10M keys ([golden-services.md](golden-services.md)) |
  | **E** | estimated from measured parts |
  | **C** | cited (Appendix B) |

  One exact get costs **347 ns** alone and **54 ns** in the server's batched
  pipeline (M). "× get" in this document compares with the 347 ns latency.
- **Reading guide:**

  | Part | What it holds |
  |---|---|
  | I | the catalog and a "which function do I need" table |
  | II | the foundations every function shares |
  | III | the 46 functions, one fixed template each: Why, API, Semantics, How it works, Cost, Usage, Pitfalls |
  | IV | recipes that combine functions |
  | V | the engineering plan |

## Summary

**The 46 functions, by group.**

| Group | Functions | What the store needs | Phase |
|---|---|---|---|
| A. Everyday data access | Exists; GetMany / PutMany / DeleteMany; PutIfAbsent / PutIfPresent; CompareAndSet; GetAndDelete / GetAndSet; Expire / TTL; leases; atomic batches and Move | revisions (Phase 0); an expiry wheel for TTL | 1, 2 |
| B. Counters and limits | Increment family; bounded counters; RateLimit; hot counters | nothing new: in-place cells | 1 (hot counters: 6) |
| C. Membership and sampling | the absent-key filter (internal); GetFilter; Sample; Stats | nothing; filters build on demand | 0, 1, 5 |
| D. Ordered queries | Scan; Count; Seek; Rank / Select; Pop; DeleteRange; LongestPrefix; prefix and suffix listing | one ordered index (a radix tree with subtree counts) | 4 |
| E. Search over keys | FindSimilar (1 and 2 edits); FindContaining; Matching; ad hoc patterns | opt-in indexes, about 2 GB each at 10M keys | 6 (ad hoc: 3) |
| F. Sketches | CountDistinct; Frequency; TopK; Quantiles | typed values, updated in place | 5 |
| G. Streams and sync | Watch; ChangesSince; Export; Reconcile; StatsByPrefix | the history ring and cuts (export-watch.md); directories | 3, 4 (Reconcile: 6) |
| H. More golden services ([golden-services.md](golden-services.md)) | compact keys; Fields (struct fields); Bits (bitmaps); Sets; FindNear; GeoAdd / GeoRadius; WindowAdd; Score (TopK and quantiles are F3, F4) | typed values; a compact table; two opt-in indexes | 5 (Fields, Bits, Sets, WindowAdd, Score); 6 (compact keys, FindNear, Geo) |

**Seven decisions this plan proposes.**

1. **New RPCs, never new fields on Put or Delete.** The server skips unknown
   protobuf fields ([PROTOCOL.md](../PROTOCOL.md)). A condition added to
   `PutRequest` would therefore be *ignored* by an older server, and the
   write applied unconditionally. A new method fails safely with
   `UNIMPLEMENTED`.
2. **A failed condition is an answer, not an error.** The call returns `OK`
   with `applied = false` and the key's current version. A compare-and-set
   loop then needs no second read, and a busy lock is not an exception.
3. **Every write gets a revision, and a record's version is the revision
   that wrote it.** Revisions are seeded from the clock at startup, so
   versions keep growing across restarts and a stale version never matches
   a new record.
4. **Synchronous calls stay inside 1000× a get.** That is about 350 µs of
   store work per call, and at most 1 MiB per response. Anything larger
   pages with a cursor or streams.
5. **Counters, rate-limit state and sketches are typed values updated in
   place**, lock-free, with `LOCK XADD` or `LOCK CMPXCHG`. Exports of
   counters stay exact at the cut: the exporter subtracts the deltas logged
   after it.
6. **One ordered index serves every ordered function.** It is an adaptive
   radix tree with subtree counts, maintained inside creates and deletes so
   that readers see their own writes.
7. **Big indexes are opt-in, with their memory stated up front:** fuzzy
   lookup 2.19 GB and substring search about 1.9 GB at 10M keys.

**Rollout.**

| Phase | Ships | Depends on | Gate (must hold to land) |
|---|---|---|---|
| 0 Foundations | revisions and versions; the Mutate engine; batch limits; Stats | — | ≤ +5 ns per write; Get/Put unchanged ±2% |
| 1 No new structures | Exists, GetMany, Mutate (batches, conditions, swap, atomic batches, Move), Increment (with bounds), RateLimit, Sample | 0 | batched puts ≥ 2.5× single puts per key; Increment ≤ 1.2× a batched get |
| 2 Time | TTL (Expire, `ttl_ms` on writes), leases | 1 | Get ±1%; put with TTL ≤ +50 ns |
| 3 Streams | Watch, ChangesSince, Export, ad hoc patterns; ADD and EXPIRE events | 0 | export-watch.md §12 |
| 4 Order | the ordered index; Scan, Count, Seek, Rank / Select, Pop, DeleteRange, LongestPrefix; directories (StatsByPrefix, prefix sampling) | 0 | ≤ +0.85 µs per create or delete; nothing per replace |
| 5 Analytics and typed values | sketches (HLL, Count-Min, TopK, quantiles); GetFilter; Fields, Bits, Sets, WindowAdd, Score (Group H) | 1 | an add ≤ 1.2× a batched get per item; filter build ≤ 100 ns per key; Group H gates in §V.9 |
| 6 Opt-ins | FindSimilar, FindContaining, Matching, hot counters, Reconcile; compact keys, FindNear, GeoAdd / GeoRadius (Group H) | 3, 4 | per function, Part V §9 |
| S Store speedups (any time, no API) | 8-lane SipHash in every batched hash stage; flat combining for writers; UMWAIT waits | — | batched get ≤ 0.97×; puts/s at 7 writers ≥ 1.5×, at 1 ≥ 0.95× (§V.9) |

---

# Part I — The catalog at a glance

## I.1 All 46 functions

| # | Function | What it answers | RPC | Cost at 10M keys | Store needs | Phase | Ev. |
|---|---|---|---|---|---|---|---|
| A1 | Exists | does each key exist? | `Exists` | 53 ns per key, batched | — | 1 | M |
| A2 | GetMany / PutMany / DeleteMany | many keys in one call | `GetMany`, `Mutate` | 54 / 126 / 149 ns per key | — | 1 | M |
| A3 | PutIfAbsent / PutIfPresent | write only if the key is (not) there | `Mutate` | ≈ a put: 0.43 µs alone | — | 1 | E |
| A4 | CompareAndSet | write only if unchanged since read | `Mutate` | ≈ a put | versions | 1 | E |
| A5 | GetAndDelete / GetAndSet | take or swap atomically | `Mutate` | ≈ a delete or put | — | 1 | E |
| A6 | Expire / TTL | forget the key at a deadline | `Mutate`, `GetMany` | O(1); +16 B per key with a TTL | expiry wheel | 2 | C, E |
| A7 | Leases | exclusive ownership with a deadline | client over `Mutate` | ≈ a put per acquire or renew | TTL | 2 | E |
| A8 | Atomic batch / Move | several writes, all or nothing | `Mutate` | the sum of its writes, under one lock hold | — | 1 | E |
| B1 | Increment / IncrementBy / Decrement | add to a number | `Increment` | 58 ns | — | 1 | M |
| B2 | Bounded counters | add only within [min, max] | `Increment` | ≈ an increment; 161 ns on one key at 7 threads | — | 1 | M |
| B3 | RateLimit | may this caller proceed now? | `RateLimit` | ≈ an increment | — | 1 | E |
| B4 | Hot counters | one counter hit from every core | `Increment` (sharded) | 22.6 ns per increment at 7 threads | 64 B × workers per key | 6 | M |
| C1 | Absent-key filter (internal) | skip the table for absent keys | — | 12.9 ns vs 30-120 ns for a table miss | 13 MB | 5, measured first | M |
| C2 | GetFilter | a filter clients check locally | `GetFilter` | 52 ns check on the client | build 88 ns per key | 5 | M |
| C3 | Sample / RandomKey | random keys | `Sample` | 28 ns; 1,000 keys in 23 µs | — (prefix: an index) | 1 | M |
| C4 | Stats (Count) | how many keys, how much memory | `Stats` | O(1) | — | 0 | — |
| D1 | Scan | keys in order, page by page | `Scan` | a 100-key page ≈ 5 µs with values | ordered index | 4 | M, K, E |
| D2 | Count | keys in a prefix or a range | `Count` | O(key length), no key visited | ordered index | 4 | M, E |
| D3 | Seek | the key at, after or before a point | `Seek` | 71 ns batched, 263 ns alone | ordered index | 4 | M |
| D4 | Rank / Select | a key's position; the key at a position | `Rank`, `Select` | 71 ns / 2.8 ns (snapshot) | ordered index | 4 | M |
| D5 | Pop | take the first or last keys of a queue | `Pop` | ≈ 0.5-0.8 µs per key | ordered index | 4 | E |
| D6 | DeleteRange / DeletePrefix | delete a prefix or a range | `DeleteRange` | ≈ 0.9 µs per key with the index | ordered index | 4 | M, K |
| D7 | LongestPrefix | the most specific stored prefix of a key | `LongestPrefix` | ≈ 0.2 µs | ordered index | 4 | E |
| D8 | Prefix and suffix listing | keys under a prefix, or with a suffix | `Scan` | 4 keys 0.2 µs; 400 keys 20 µs | ordered index (+ reversed) | 4 | K |
| E1 | FindSimilar, 1 edit | keys one typo away | `FindSimilar` | 0.57-0.70 µs | opt-in, 2.19 GB | 6 | M |
| E2 | FindSimilar, 2 edits | keys two typos away | `FindSimilar` | est. 0.2-0.4 ms | the same | 6 | E |
| E3 | FindContaining | keys containing a fragment | `FindContaining` | est. 3-6 µs to count | opt-in, ~1.9 GB | 6 | E |
| E4 | Matching | keys matching a registered pattern | `Matching` | O(1) count, O(k) list | match sets | 6 | E |
| E5 | Ad hoc patterns | keys matching any glob or regex | `Export` (filter) | ~92 ms at 10M keys, streamed | — | 3 | E |
| F1 | CountDistinct | how many distinct items (HLL) | `Sketch*` | 0.6 ns add and count; union 0.26-1.06 µs | 4-16 KB per sketch | 5 | M |
| F2 | Frequency | how often an item was seen (Count-Min) | `Sketch*` | 21 ns per item, batched | sized per sketch | 5 | M |
| F3 | TopK | the most frequent items | `Sketch*` | est. 1-2 µs to read; measured since: 16.7 ns per add (HeavyKeeper, golden-services H7) | KBs | 5 | E, V |
| F4 | Quantiles | p50 / p99 of a measure (DDSketch) | `Sketch*` | O(buckets); measured since: 16.7 ns per add, 16.2 ns per quantile (golden-services H8) | KBs | 5 | C, V |
| G1 | Watch | the stream of changes to matching keys | `Watch` | 20-40 ns per write to route | history ring | 3 | E, X |
| G2 | ChangesSince | what changed after revision R | `Watch` (`start_revision`) | O(changes) | history ring | 3 | E |
| G3 | Export | a consistent copy of matching keys | `Export` | cut O(1); 25-40 ns per key streamed | cuts | 3 | X |
| G4 | Reconcile | sync by the difference only | `Reconcile` | O(difference) | per-scope tables | 6 | C |
| G5 | StatsByPrefix | keys and bytes per prefix | `Stats` (prefix) | O(1) | directories | 4 | E |
| H1 | Compact keys | get, put or increment a ≤ 8-byte value at one cache miss | Get, GetMany, Mutate, Increment (on compact prefixes) | 28.1 ns batched (ordinary keys: 53.1); 189.9 ns alone | a compact table; `KVNODE_COMPACT_PREFIXES` | 6 | V |
| H2 | Struct fields | read or update one field of a structured value | `Fields` | read 59.6 ns, increment 62.5 ns (the whole 440-byte value: 102.0) | typed value | 5 | V |
| H3 | Bitmaps | set, test, find and count bits | `Bits` | set 10.1 ns; range count 67.6 ns; whole count one load | typed value + block counts | 5 | V |
| H4 | Small sets | membership and overlap of id sets | `Sets` | membership 2.1 ns (64 ids); overlap 552.8 ns (1,024 × 1,024) | typed value | 5 | V |
| H5 | FindNear | keys whose 64-bit fingerprint is within 3 bits | `FindNear` | 695 ns among 10M | opt-in, 48 B per key | 6 | V |
| H6 | GeoAdd / GeoRadius | members within a radius of a point | `GeoAdd`, `GeoRadius` | 697.5 ns for 200 m (15 results) among 10M | opt-in, 28 B per point + 4 MiB | 6 | V |
| H9 | Sliding windows | a count over the last N periods | `WindowAdd` | ≈ an increment; the sum 4.5 ns | typed value | 5 | V, E |
| H10 | Score | similarity of given keys' stored vectors to a query | `Score` | a batched get + 1.5-5.6 ns per key | typed vector values | 5 | V |

## I.2 "I want to…": choosing a function

| I want to… | Use | Not |
|---|---|---|
| process each payment, webhook or event exactly once | PutIfAbsent on an idempotency key (A3, R1) | Exists then Put: two workers both see "absent" |
| claim a username, email or coupon code | PutIfAbsent (A3) | |
| edit a shared document safely | CompareAndSet on its version (A4) | |
| redeem a one-time token | GetAndDelete (A5) + a TTL | Get then Delete |
| keep sessions, holds and caches that clean themselves | TTL (A6) | a cleanup job |
| run a job on only one machine | a lease (A7, R2) | |
| move value between keys or change several at once | an atomic batch (A8) | several Puts |
| count views, usage or API calls | Increment (B1) | Get then Put |
| sell stock without overselling | a bounded counter (B2, R4) | |
| throttle callers | RateLimit (B3, R3) | a counter per window |
| count one very hot thing from everywhere | a hot counter (B4) | |
| let clients skip lookups of absent keys | GetFilter (C2, R17) | a remote "might contain" call |
| audit or sample records | Sample (C3, R18) | |
| list a customer's items, newest first, with pages | Scan with a cursor (D1, R6) | Export (no pages) |
| show "page 3 of 57" or enforce item quotas | Count (D2) | listing everything |
| read a value "as of" a time; find the next due job | Seek (D3, R9) | |
| show a leaderboard position | Rank / Select (D4, R8) | |
| run a queue or delayed jobs | Pop, or claims (D5, R7) | |
| erase a user's data | DeleteRange on their prefix (D6, R10) | |
| find the most specific config or route | LongestPrefix (D7, R15) | |
| suggest "did you mean" | FindSimilar (E1, R13) | |
| find keys containing a fragment | FindContaining (E3) | scanning the store |
| keep a live count of a key pattern | Matching (E4) | counting by scan |
| run a one-off pattern report | Export with a filter (E5) | a synchronous call |
| count unique visitors | CountDistinct (F1, R16) | a set of all visitor IDs |
| spot abusive callers or trending items | Frequency / TopK (F2, F3) | |
| report p99 latency | Quantiles (F4) | |
| invalidate caches when data changes | Watch (G1, R11) | polling |
| sync an offline client | ChangesSince (G2, R12) | re-downloading everything |
| back up or ETL a consistent copy | Export (G3) | Scan pages (not one point in time) |
| bill tenants for what they store | StatsByPrefix (G5) | |
| serve counters and flags at the lowest latency | compact keys (H1) | ordinary keys: twice the latency |
| update one field of a profile, cart or player record | Fields (H2, R22) | Get, edit, CompareAndSet on the whole value |
| count daily actives, or detect a first visit, by user number | Bits (H3, R19) | a set of ids; CountDistinct (F1) when ids are not small integers |
| check a group, role or entitlement | Sets (H4) | one key per membership, unless the sets are large |
| find duplicate listings, tickets or images | FindNear (H5, R20) | FindSimilar, which compares keys, not contents |
| find what is near a point | GeoRadius (H6, R21) | Scan over a grid of keys |
| count events over a rolling window, with thresholds | WindowAdd (H9, R23) | RateLimit, when throttling is all you need (B3) |
| re-rank candidates by embedding similarity | Score (H10) | fetching every vector to the client |

## I.3 Not in this plan

- **Indexes over value contents** (secondary indexes inside values): values
  stay opaque bytes. capabilities.md §1.8 lists the options.
- **Vector, geo and full-text search.**
  - **Now in this plan (Group H):** geo radius (H6) and near-duplicate
    search (H5), both measured golden at 10M keys, and the scoring of
    stored vectors for given keys (H10).
  - **Still out:** approximate nearest-neighbor search over all vectors,
    and full-text search.
- **Multi-node replication and sharding.** Reconcile (G4) and Watch are
  their building blocks, not the feature.
- **Durable persistence.** That is the persistence proposal; §V.6 covers
  how these functions interact with it.
- **Authentication.** KvNode trusts its network and puts TLS and identity
  in a proxy. §1.10 proposes how a proxy can scope tenants.

---

# Part II — Foundations

## 1. Principles

### 1.1 Additive, and safe against older servers

- Every function is a **new RPC** in the existing `kvnode.v1.KvNode`
  service. Get, Put, Delete, Upload and Download keep their exact behavior.
- Old clients never call the new methods. A new client talking to an older
  server gets `UNIMPLEMENTED` and fails safely. A condition or TTL is never
  silently dropped (decision 1).
- A function whose feature is disabled answers `UNIMPLEMENTED`, with the
  setting to change in the message (e.g. "Scan needs
  KVNODE_INDEX_ORDER=on").
- Messages only ever gain fields: new numbers, never reused; removed ones
  become `reserved`. Every enum keeps `…_UNSPECIFIED = 0`.

### 1.2 The cost budget

| Tier | Budget at 10M keys | Rule |
|---|---|---|
| golden | ≤ 10× a get (≤ 3.5 µs alone, ≤ 541 ns batched) | point operations; nothing needed but the function itself |
| synchronous | ≤ 1000× a get (≈ 350 µs of store work) | every unary call; per-call limits enforce it |
| streamed | anything larger | Export, Watch, GetFilter: paced by flow control, off the critical path |

The per-call limits below follow from measured per-item costs, so that one
call cannot exceed the synchronous budget:

| Call | Per item | Limit per call | Worst call |
|---|---|---|---|
| GetMany, Exists | 54 ns | 1,024 keys | ~55 µs |
| Mutate (no ordered index) | 126-450 ns | 256 mutations | ~115 µs |
| Mutate creates and deletes with the ordered index | ~0.9 µs | 256 | ~230 µs |
| Scan with values | ~50-60 ns | 1,000 keys | ~60 µs |
| Pop, DeleteRange | ~0.9 µs | 256 keys | ~230 µs |
| Sample | 23-28 ns | 1,024 keys | ~29 µs |
| Fields, Bits, WindowAdd (H2, H3, H9) | ~60-70 ns (V) | 256 ops | ~18 µs |
| Sets INTER_CARD (H4) | 0.55 µs at 1,024 × 1,024 (V); ~35 µs at 65,536 × 65,536 (E) | 4M members compared per call | ~140 µs |
| FindNear (H5) | ~0.7 µs per query (V) | one query, ≤ 1,000 matches | ~1 µs + the matches |
| GeoRadius (H6) | ~17 ns per result (V) | one query, ≤ 10,000 results | ~170 µs |
| Score (H10) | a batched get + ≤ 6 ns (V) | 1,024 keys | ~65 µs |

### 1.3 Answers versus errors

| Outcome | Reported as |
|---|---|
| a condition did not hold | `OK`, `applied = false`, the current version (and value, when asked) |
| a bound refused an increment | `OK`, outcome `OUT_OF_BOUNDS`, the current value |
| the rate limit refused a request | `OK`, `allowed = false`, `retry_after_ms` |
| the key is absent | `OK`, `found = false` |
| the value has the wrong type (Increment on a non-counter, SketchAdd on a non-sketch; Fields, Bits, Sets, WindowAdd or Score on a value of another type) | `FAILED_PRECONDITION` (9) for a single item; a per-item outcome in batches |
| a compact key or value over its limit (H1); a value under a near-duplicate or geo prefix too short for its fingerprint or point (H5, H6) | `INVALID_ARGUMENT` (3), with `reason` |
| a counter would overflow int64 | `OUT_OF_RANGE` (11), or a per-item outcome |
| the feature is disabled | `UNIMPLEMENTED` (12) |
| a limit was hit (batch, page, memory, capacity) | `RESOURCE_EXHAUSTED` (8); `INVALID_ARGUMENT` (3) for a malformed request |
| the key is outside the caller's scope (§1.10) | `PERMISSION_DENIED` (7) |

The rule: anything a correct program expects as a normal outcome is a field
in an `OK` reply. Status codes are for requests that cannot be served as
asked.

### 1.4 Batches

- **One request message per call**, never a stream.
  - Reads take 1..`KVNODE_MAX_READ_BATCH` items (default 1,024).
  - Writes take 1..`KVNODE_MAX_WRITE_BATCH` (default 256).
  - Atomic batches take 1..`KVNODE_MAX_ATOMIC` (default 64).
- **Every item is validated before any item is applied.** A malformed or
  oversized item fails the whole call with `INVALID_ARGUMENT`, and nothing
  changes.
- **Responses stop before `KVNODE_MAX_RESPONSE_KB`** (default 1024). The
  .NET client's default receive limit is 1 MiB + 1 KiB. A read batch that
  would exceed it returns the items so far plus `next_index`; the client
  library continues automatically.
- **Non-atomic write batches** apply in order, each item atomically. Each
  item reports its own outcome: applied, condition failed, or no space (the
  memory or key limit was reached at that item).
- **The server pipelines a batch.**
  1. It hashes every key and prefetches both candidate buckets, then the
     old records.
  2. It then applies the items, taking the writer lock per chunk of at most
     8 mutations: enough to amortize the lock, short enough that other
     writers wait at most a few microseconds.
  - The measured gain (3.4× for replaces, 3.3× for deletes, 1.7× for
    creates per key, M) comes from the prefetching; the probe took the lock
    per put.

### 1.5 Revisions and versions

- **One 64-bit revision counter.** Every applied write takes the next value
  with `LOCK XADD`: 3 ns uncontended (S). Phase 0 adds it; export-watch.md
  §3.1 designs the visibility watermark that Watch and Export need on top.
- **Seeding.** At startup the counter starts at `unix_ms × 2^20`.
  - Revisions keep growing across restarts, as long as the clock moves
    forward.
  - There is headroom for 2^20 writes per millisecond, about 10^9 per
    second.
  - The value fits in 63 bits until the year 2248.
- **A record's version is the revision of the write that created it.** It
  is stored in the record header; export-watch.md places it in `Rec.aux`,
  so it costs no extra bytes.
  - Version 0 means "absent".
  - A Put, a Mutate put and a Move set a new version.
  - In-place updates (counters, rate limits, sketches) and a TTL `touch` do
    not: the value cell changes, the record does not (§2.3).
- **Incarnation.** A random 64-bit identity per process start, as in
  export-watch.md. Versions alone are safe across restarts thanks to the
  clock seed. Watch resumption needs the incarnation, because the history
  itself is per process.

### 1.6 Value types

| Type | Layout | Created by | Get returns |
|---|---|---|---|
| bytes | any | Put, Mutate, Upload | the bytes |
| counter | exactly 8 bytes: int64, little-endian | Increment on an absent key, or a Put of any 8 bytes | the 8 bytes |
| hot counter | 8-byte header + one 64-byte cell per worker; a header flag marks it | Increment with `sharded` | the 8-byte sum |
| rate-limit state | 8 bytes: the theoretical arrival time | RateLimit | the 8 bytes (internal) |
| sketch | a magic `KVSK`, a kind, its parameters, then its body (Part III F) | SketchAdd | the documented bytes, so clients can merge locally |
| struct | a magic `KVST`, a 56-byte directory (tags, offsets), then 16 field records (H2) | Fields with `create`, or a Put of the encoding | the encoded struct |
| bitmap | a magic `KVBM`, the size, the total, one count per 4 KiB block, then the bits (H3) | Bits SET or CLEAR with `create_bits` | the documented bytes |
| set | a magic `KVSE`, the count, then the sorted u64 members (H4) | Sets ADD | the documented bytes |
| window | a magic `KVWN`, the bucket count and width, the newest epoch, then the buckets (H9) | WindowAdd | the documented bytes |
| vector | a magic `KVVC`, the element type and dimension, then the elements (H10) | a Put of the encoding (a client helper) | the bytes |

- A Put of other bytes over any typed value makes it a plain value again.
- Increment on a value that is not exactly 8 bytes is `NOT_A_COUNTER`.
- SketchAdd on a value without the sketch magic of the right kind is
  `FAILED_PRECONDITION`.
- **The same rule holds for Group H.** Fields, Bits, Sets, WindowAdd and
  Score on a value without their magic are `FAILED_PRECONDITION`, or a
  per-item outcome.
- **Some prefixes fix the start of the value.** Under
  `KVNODE_NEAR_PREFIXES` a value starts with its 8-byte fingerprint (H5),
  and under `KVNODE_GEO_INDEXES` with its 16-byte point (H6). Compact keys
  (H1) are not a value type: they are a key space whose values are at most
  8 bytes.

### 1.7 Time

- **The clock.** The server's coarse monotonic clock
  (`CLOCK_MONOTONIC_COARSE` through the vDSO, about 5 ns, 1-4 ms
  granularity), read once per event batch.
  - TTL deadlines and rate limits use it.
  - Wall-clock jumps never expire or extend anything.
- **Durations in requests** are milliseconds (`ttl_ms`, `period_ms`).
  Deadlines are internal.
- **Wall-clock time** appears only where state leaves the process:
  snapshots carry absolute expiry times (§V.6).

### 1.8 Retries and idempotency

| Call | Safe to retry on `UNAVAILABLE`? | Why |
|---|---|---|
| Exists, GetMany, Scan, Count, Seek, Rank, Select, Sample, Stats, SketchQuery, GetFilter, FindSimilar, FindContaining, Matching; FindNear, GeoRadius, Score; the reads of Fields (GET), Bits (GET, COUNT, POS), Sets (IS_MEMBER, CARD, INTER_CARD) and WindowAdd (delta 0) | yes | read-only |
| DeleteRange | yes | deleting again is harmless; only the reported count differs |
| GeoAdd; Fields SET and DEL; Bits SET and CLEAR; Sets ADD and REMOVE | yes | a retry writes the same state; only the reply (`was`, counts) describes the retry instead of the first attempt |
| Mutate with conditions | yes, with care | a retry after a lost reply reports `CONDITION_FAILED` for your own write: put a unique token in the value and compare `previous` |
| Mutate without conditions, Increment, RateLimit, Pop, SketchAdd, SketchMerge; Fields INCR; WindowAdd with a delta | no | a retry applies twice (or pops another key) |

The .NET client auto-retries only the first group (as today for Get and
Download). For the others, idempotency comes from the application, usually
through PutIfAbsent on a request ID (R1).

### 1.9 Keys: what the functions see, and how to design them

- **Every function sees wire keys.** Official clients send a key longer
  than 64 bytes as `0xFF ‖ SHA-512(key)` ([PROTOCOL.md](../PROTOCOL.md)).
  - Ordered, prefix, pattern, fuzzy and substring functions therefore see
    the digest of such a key. It sorts after every printable key and
    matches no prefix.
  - **Keep keys you want to query structurally at 64 bytes or less**, or
    use raw mode on every client of the server.
- **Byte order is the only order.** Comparisons are `memcmp`, and a shorter
  key that is a prefix of another sorts first.

**A key design guide for the ordered and prefix functions:**

| Rule | Example | Why |
|---|---|---|
| Put the entity that scopes queries first | `order:<customer>:<time>:<id>` | one prefix per customer: listing, counting and erasure become prefix operations |
| Separate segments with a delimiter that never occurs inside them | `:` | prefixes end cleanly; directories can index aligned prefixes (§2.6) |
| Write numbers at a fixed width, in an order-preserving form | `0000001712345678` (zero-padded decimal) or big-endian bytes | `"10"` sorts before `"9"`; fixed width does not |
| For newest first, invert time | `(long.MaxValue − t)` at a fixed width | a forward scan returns the newest first; or scan with `reverse` |
| Encode scores so that the best sorts first | `(long.MaxValue − score)` | Rank 0 is first place |
| Keep one key family per prefix | `sess:…`, `cart:…`, `idem:…` | per-family TTLs, Watch filters and stats |
| Avoid unbounded per-key growth; use one key per item | `msg:<thread>:<seq>`, not one growing value | values are rewritten whole; ordered functions page through keys |

The examples in this document write fixed-width numbers with a helper
`Fixed(n)`: zero-padded decimal, 19 digits for `long`, 20 for `ulong`.

### 1.10 Scopes for multi-tenant deployments (proposal)

KvNode has no authentication; TLS and identity live in a proxy. To host
several tenants safely, **the proxy can scope each connection**:

- **Setup.** `KVNODE_SCOPE_HEADER=kvnode-scope` names a request header that
  a *trusted* proxy sets after it authenticates the caller, for example
  `kvnode-scope: t42:`.
- **Effect.** The server prepends the scope to every key in the request
  and strips it from every key in the answer. A tenant cannot name another
  tenant's key at all.
- **Clamping.** Listing, sampling, counting, Watch, Export, Stats and
  GetFilter are clamped to the scope. Stats on the scope is the tenant's
  usage.
- **Refusals.** A request without the header is `PERMISSION_DENIED` when
  scopes are enabled.
- **Threat model.** The header is trusted, so the server must only be
  reachable through the proxy. That is a deployment rule (network policy).

This is open decision 5.

### 1.11 Memory and opt-ins

Every structure counts toward `KVNODE_MAX_MEMORY_MB`. A create that would
cross it is refused before any structure is touched. The full picture at
10M keys of 20 bytes with 100-byte values:

| Structure | Per key | At 10M keys | Default |
|---|---|---|---|
| the store itself (records + table) | ~162 B + the table | ~1.75 GB | on |
| revisions / versions | 0 B (in `Rec.aux`) | — | on (Phase 0) |
| TTL | +16 B per key *with* a TTL | — | on |
| ordered index (radix tree + counts) | ~93 B (85 measured, K, + counts) | ~0.93 GB | off |
| its leaf back-pointer in each record | +8 B | +80 MB | with the index |
| reversed index (suffix listing) | ~69 B (K) | ~0.69 GB | off |
| directories | ~110 B (K) | ~1.1 GB | off |
| fuzzy index (SymSpell, 1 edit) | ~235 B (M) | ~2.19 GB | off (per prefix) |
| substring index | ~190 B (E) | ~1.9 GB | off (per prefix) |
| a cached client filter | 9 bits | ~11 MB per scope | built on demand |
| compact table (H1) | 54 B per compact key, instead of ~61 B as an ordinary key of that size (V) | — | off (per prefix) |
| near-duplicate index (H5) | 48 B per indexed key (V) | ~0.48 GB if all are indexed | off (per prefix) |
| geo index (H6) | 28 B per point + 4 MiB (V) | ~0.28 GB if all are points | off (per index) |
| typed values (H2-H4, H9, H10) | their own size; structs +56 B of directory, bitmaps +0.1% of counts | — | on |
| history ring | fixed size (`KVNODE_WATCH_HISTORY`) | — | export-watch.md |
| sketches | they are values: 4-16 KB per HLL, sized Count-Mins | — | — |

With every opt-in enabled the store needs about 5× its base memory. That is
why the indexes are per feature, and the two large ones per prefix.

## 2. Shared mechanisms

### 2.1 The Mutate engine

All writes beyond Put, Delete and Upload go through one engine. Put and
Delete become its single-mutation fast paths, with unchanged behavior.

1. **Decode and validate everything:** sizes, ops, conditions, limits,
   scopes. On any failure: `INVALID_ARGUMENT`, nothing applied.
2. **Prepare outside the lock.**
   - Hash every key and prefetch both buckets, then the old records.
   - Allocate the new records, with TTL extensions where asked, and reserve
     their memory.
   - An atomic batch reserves for *all* of its records, so it can never
     fail halfway.
3. **Apply under the writer lock.**
   - Non-atomic batches take the lock per chunk of ≤ 8 mutations; atomic
     batches take it once.
   - For each mutation:
     1. locate the key;
     2. treat it as absent if its TTL has passed;
     3. check its condition;
     4. apply (insert, replace, remove, touch, move);
     5. take its revision;
     6. append the history slot (Phase 3);
     7. run the index hooks.
   - **Hook order matters.** The filter bits (§2.7) are set before the
     record pointer is published. The ordered index, directories and fuzzy
     index are updated before the lock is released.
4. **Retire replaced and removed records** through QSBR, as today.
5. **Reply** with outcomes, versions and previous values.
   - A previous value is copied from the retired record. QSBR keeps it
     valid until the worker's next quiescent point, which comes after the
     reply is framed.

**Atomic batches** check *every* condition before applying any mutation.
They take **one revision for the whole batch**, which gives two properties:

- **Watch** delivers the batch as one revision.
- **Export cuts** see all of it or none. A cut waits for the stable
  watermark, which passes the batch's revision only after every mutation
  of it is in place.

### 2.2 The revision counter

| Property | Value |
|---|---|
| assignment | `LOCK XADD` on one cache line: 3 ns single-threaded, 136.6 M/s across 7 threads (S) |
| seed | `unix_ms × 2^20` at startup (§1.5) |
| visibility | the lazy stable watermark of export-watch.md §6 (+3.3 ns per write under today's mutex, S) |
| cost when nothing consumes it | in-place updates skip it when Watch and Export are disabled (`KVNODE_WATCH_HISTORY=0`); Put-class writes always take one, since versions need it |

**The shared line is a ceiling.** With Watch enabled, every in-place update
also takes a revision: one more `LOCK XADD` on that shared line. That puts
a ceiling of about 137M event-producing updates per second, across all
cores. Hot counters (B4) coalesce their events to stay under it.

### 2.3 In-place cells: counters, rate limits, sketches

**Rule 1: the cheapest correct atomic.**

| Update | Instruction |
|---|---|
| unbounded counter | `LOCK XADD` on the 8-aligned value cell |
| bounded counter, rate limit | `LOCK CMPXCHG` loop |
| hot counter | a plain store to the worker's own cell |
| HLL register | byte `LOCK CMPXCHG` |
| Count-Min and quantile buckets | `LOCK XADD` |
| TopK | a per-sketch spinlock |
| compact value (H1) | `LOCK XADD` or an aligned 8-byte store, inside the bucket |
| struct field (H2) | `LOCK XADD` or an aligned 8-byte store |
| bitmap bit (H3) | `LOCK BTS` / `LOCK BTR`; on a flip, `LOCK XADD` on the block count and the total |
| window bucket (H9) | `LOCK CMPXCHG16B` to reset a stale bucket, then `LOCK XADD` |

The 8-byte value cell is aligned because records are 16-aligned, the header
is 24 bytes and keys are padded to 8 (golden-operations §4.1).

**Rule 2: a replacement wins.**
- An update that reached a record which a Put or Delete then replaced
  linearizes just *before* the replacement.
- It read the slot before the new pointer was published, and QSBR keeps the
  old record's memory valid until it finishes.
- The result is the Put's value, as in any serial history where INCR
  precedes SET (golden-operations §4.1).

**Rule 3: an update's event.** With Watch on, an in-place update:

1. takes its revision;
2. applies;
3. publishes an `ADD` event carrying the delta and the value after.

Concurrent updates can publish their "value after" out of revision order.
The deltas commute, though, so a consumer that sums deltas is exact; the
value is informative.

**Rule 4: counters export exactly.** The exporter:

1. reads a counter's live value `v`;
2. reads the revision counter `R` right after;
3. waits for the stable watermark to reach `R`;
4. emits `v − Σ deltas of that key in (C, R]`, taken from the history, for
   a cut at revision C.

No record copy is needed. Every update with a revision ≤ C applied before
the cut began, because updates publish after applying and the cut waits for
its watermark.

Sketch and rate-limit state are exported as read: approximate by nature,
and documented as such.

**Rule 4b: fields and bits export exactly through undo records.**
- **Struct fields and bitmap bits are not sums,** so a delta cannot undo
  them.
- **With Watch on, each in-place change logs what it overwrote:**
  - a field SET logs the field's old 8 bytes;
  - an INCR logs its delta, as a counter does;
  - a flip logs the bit's old value.
- **An export reads the live value,** then replays the undo records logged
  after its cut, newest first.
- **Windows (H9) export as read,** like rate-limit state.

**Rule 5: versions do not change.** In-place updates do not touch the
record header (§1.5).

### 2.4 Expiry (TTL)

- **The record extension.** A record with a TTL carries 16 more bytes
  after its header:
  - the deadline (coarse monotonic nanoseconds);
  - the link in the timing wheel.

  A header flag says the extension exists, and the value starts after it.
  Records without a TTL are unchanged.
- **Lazy expiry: exact.** Every read path treats a record whose deadline
  has passed as absent: Get, GetMany, Exists, Download, conditions, Scan
  entries, Sample and Seek.
  - The cost is one flag test, perfectly predicted when no TTLs are in use.
  - Leases and one-time tokens depend on this exactness.
- **Active expiry: bounded work.**
  - A hashed timing wheel (Varghese and Lauck, C): `KVNODE_EXPIRE_TICK_MS`
    slots (default 1 s), plus an overflow tier for distant deadlines.
  - One worker's housekeeping tick removes due records, at most
    `KVNODE_EXPIRE_BATCH` per tick, through the Mutate engine in chunks.
  - Each removal is an `EXPIRE` event. Reads already treat the keys as
    gone, so a burst of expiries only spreads memory reclamation over a few
    ticks.
- **Writes.**
  - A put with `ttl_ms` links its new record.
  - A put without `ttl_ms` clears the TTL unless `keep_ttl` (Redis's SET
    does the same).
  - `touch` relinks; delete unlinks.
  - Increments and sketch updates keep the TTL.
- **Counts.** Count (D2) and StatsByPrefix (G5) can include keys that
  expired within the last tick, at most 1 s by default, until the sweeper
  removes them.

### 2.5 The ordered index

| Aspect | Design |
|---|---|
| structure | adaptive radix tree: Node4/16/48/256, path compression, keys NUL-terminated inside the tree (key-indexes.md §2.2). The Node16 search uses SSE2, with a SWAR fallback |
| writers | already serialized by the writer mutex: no second lock |
| readers | lock-free with optimistic lock coupling. Every node has a version; a reader validates it on the way down and restarts from its last emitted key if a node changed. Nodes are freed through QSBR |
| order statistics | every child slot carries its subtree's key count. Count and Rank sum the counts left of the path (AVX2 sums over a Node48/256 count array; scalar twin). Select descends by counts |
| values | each record header points to its leaf. A replace moves the pointer to the new record in O(1), with no tree walk, so Scan reads values directly (11.6 ns per item in the pinned-record measurement, M) |
| maintenance | inside creates and deletes (synchronous), so queues, pagination and claims read their own writes. Replaces cost nothing: the key set is unchanged |
| cost | +0.76-0.85 µs per create or delete for the measured rival (libart, K): the gate. About 93 B per key with counts; +8 B per record for the leaf pointer |
| reversed index | a second tree over reversed keys for suffix listing (`KVNODE_INDEX_ORDER=both`): +0.55-0.58 µs per create or delete and 69 B per key (K) |
| optional accelerator | the Eytzinger sorted snapshot (golden-operations §2.3) answers Seek, Count, Rank and Select at 71-75 ns batched, for read-mostly data loaded from a snapshot. 344 MB and a 0.7 s rebuild at 10M keys; kept fresh by a delta overlay. Phase 6, opt-in |

### 2.6 Directories (opt-in)

Segment directories (key-indexes.md §2.1) index delimiter-aligned
prefixes: `t42:`, `t42:user7:`.

- **Structure.** Each directory is an unordered member array.
- **Size:** O(1) counts for exact prefix counts.
- **Bytes:** a running sum of value sizes, for StatsByPrefix.
- **Sampling:** a uniform member in O(1), for Sample within a prefix.
- **Cost (K):** +221 ns per delete and +321 ns per create, 110 B per key.
- **Enabled by** `KVNODE_INDEX_DELIMITERS`.

### 2.7 Background builders

Client filters (C2), the snapshot accelerator (§2.5) and the substring
index (E3) are rebuilt off the event loop:

- They run on one low-priority builder thread, never on a worker.
- Each result is published by one pointer swap (RCU); old versions are
  freed after a QSBR grace period.
- A build that would exceed the memory limit is refused, and the previous
  version stays.

For the internal absent-key filter (C1), writers set the key's bits before
publishing the record. Section C1 shows why that makes "absent" answers
linearizable.

---

# Part III — The functions

Each function follows one template: **Why** (business uses) · **API** ·
**Semantics** · **How it works** · **Cost** · **Usage** · **Pitfalls**.
Code samples use the *proposed* .NET client API (§V.7).

## Group A — Everyday data access

### A1. Exists / ExistsMany

**Why.**
- **Idempotency and dedupe:** "was this payment ID, webhook delivery or
  event ID already processed?"
- **Flags:** "has this user seen step 3 of onboarding?"
- **Bulk dedupe** of an incoming batch: which of these 1,000 event IDs are
  new?

**API.**
```proto
rpc Exists(ExistsRequest) returns (ExistsResponse);
message ExistsRequest  { repeated bytes keys = 1; }   // 1 .. KVNODE_MAX_READ_BATCH
message ExistsResponse { repeated bool found = 1; }   // packed, request order
```

**Semantics.**
- Each answer is a linearizable read of its key at some instant during the
  call. A batch is not a snapshot: two keys may be read at different
  instants.
- Expired keys are absent.
- No value bytes move: one byte per key on the wire.

**How it works.** The batched GET pipeline without the value copy: hash,
prefetch both buckets, prefetch the candidate records' first line (header
and key), compare. Lock-free (QSBR).

**Cost (M).**
- 52.8 ns per key batched; 29.7 ns for an absent key.
- One key alone: 343 ns.

**Usage.**
```csharp
bool seen = await kv.ExistsAsync("evt:" + eventId);
bool[] seen2 = await kv.ExistsAsync(batch.Select(e => "evt:" + e.Id));   // splits above 1,024
```
- To *claim* an ID, do not call Exists and then Put: two workers can both
  see "absent". PutIfAbsent (A3) checks and writes in one step.

**Pitfalls.** Exists is a read. Anything that acts on the answer races with
writers unless it goes through a condition.

### A2. GetMany / PutMany / DeleteMany

**Why.**
- **Fewer round trips:** render a page from many keys in one call (profile
  fields, feature flags, cart lines).
- **Bulk work:** imports, nightly price updates, cleanup jobs.
- **Throughput:** the server prefetches a write batch. Writes measured 3.4×
  (replace), 3.3× (delete) and 1.7× (create) faster per key than one at a
  time (M).

**API.**
```proto
rpc GetMany(GetManyRequest) returns (GetManyResponse);
message GetManyRequest {
  repeated bytes keys = 1;        // 1 .. KVNODE_MAX_READ_BATCH
  bool keys_only = 2;             // found, version and ttl only: no values
  bool with_versions = 3;
  bool with_ttl = 4;
}
message GetManyResponse {
  repeated Item items = 1;        // request order; may stop early
  uint32 next_index = 2;          // 0: complete; else the first key not answered
}
message Item {
  bool found = 1;
  bytes value = 2;
  uint64 version = 3;             // with_versions
  optional uint64 ttl_ms = 4;     // with_ttl: remaining; absent = no expiry
  bool too_large = 5;             // value above KVNODE_MAX_VALUE_BYTES: use Download
}
```
PutMany and DeleteMany are `Mutate` (§2.1, full messages in A3) with one
mutation per key and `atomic = false`.

**Semantics.**
- **GetMany** gives per-key linearizable reads, not a snapshot. For a
  consistent multi-key read, use Export with exact keys (export-watch.md
  §5.8), or read versions and verify them with an atomic batch (A8).
- **The response cap** (§1.4) can stop a batch early. The client library
  continues from `next_index`, invisibly to the caller.
- **PutMany and DeleteMany** apply in request order, each mutation
  atomically, with one outcome per item.

**How it works.**
- GetMany is the batched GET pipeline (ADR 0005) over the request's keys,
  32 at a time.
- Write batches run through the Mutate engine (§2.1).

**Cost.**

| Batch | Per key (M) | 1,000 keys |
|---|---|---|
| GetMany | 54 ns | 54 µs |
| PutMany, replace | 126 ns | 126 µs |
| PutMany, create | 125 ns | 125 µs |
| DeleteMany | 149 ns | 149 µs |

With the ordered index, creates and deletes cost ≈ +0.8 µs each (§2.5).

**Usage.**
```csharp
var fields = await kv.GetManyAsync(profileKeys);
await kv.PutManyAsync(prices.Select(p => ($"price:{p.Sku}", p.Bytes)));
await kv.DeleteManyAsync(staleKeys);
```
- 32-256 keys per call captures most of the gain. Bigger batches only add
  latency per call.
- To read, then write depending on what you read: GetMany with versions,
  then a Mutate with version conditions (A4).

**Pitfalls.** A batch is not a transaction: a reader can see half of a
PutMany. When keys must change together, use an atomic batch (A8).

### A3. PutIfAbsent / PutIfPresent

**Why.**
- **Unique claims:** usernames, emails, coupon codes, a seat number,
  claimed without a race.
- **Idempotency keys:** the first request with an ID wins; retries find the
  stored response (R1).
- **Create-only and update-only writes:** never resurrect a deleted record;
  never overwrite an existing one.
- **Lock acquisition** (with a TTL: leases, A7).

**API.** The Mutate engine. These messages serve A2-A8:
```proto
rpc Mutate(MutateRequest) returns (MutateResponse);

message MutateRequest {
  repeated Mutation mutations = 1;   // 1 .. KVNODE_MAX_WRITE_BATCH (atomic: KVNODE_MAX_ATOMIC)
  bool atomic = 2;                    // all or nothing, one revision (A8)
}
message Mutation {
  bytes key = 1;
  oneof op {
    bytes put = 2;                    // store this value (Put's limits)
    DeleteOp delete = 3;              // remove the key
    TouchOp touch = 4;                // change only the expiry; needs ttl_ms (A6)
    MoveOp move = 5;                  // move value and expiry to another key (A8)
  }
  Condition condition = 6;            // unset: unconditional
  optional uint64 ttl_ms = 7;         // put/touch: expire this many ms from now; 0 = never
  bool keep_ttl = 8;                  // put without ttl_ms: keep the current expiry
  bool return_previous = 9;           // the value before; when not applied, the current one
}
message DeleteOp {}
message TouchOp {}
message MoveOp {
  bytes to_key = 1;
  bool overwrite = 2;                 // false: not applied when to_key exists
}
message Condition {
  oneof require {
    Absent absent = 1;                // the key must not exist (expired counts as absent)
    Present present = 2;              // the key must exist
    uint64 version = 3;               // the key's version must equal this (A4)
    bytes value_equals = 4;           // the key's value must equal these bytes (A4)
  }
}
message Absent {}
message Present {}

message MutateResponse {
  bool applied = 1;                   // every mutation applied
  repeated MutationResult results = 2;
  uint64 revision = 3;                // of the last applied mutation (atomic: of the batch)
}
message MutationResult {
  MutationOutcome outcome = 1;
  bool existed = 2;                   // the key existed before (put: false = created)
  uint64 version = 3;                 // the key's version now; 0 = absent
  optional bytes previous = 4;        // return_previous, when the key existed
  bool target_existed = 5;            // move: to_key existed (and was overwritten)
}
enum MutationOutcome {
  MUTATION_OUTCOME_UNSPECIFIED = 0;
  MUTATION_OUTCOME_APPLIED = 1;
  MUTATION_OUTCOME_CONDITION_FAILED = 2;
  MUTATION_OUTCOME_NOT_ATTEMPTED = 3; // atomic batch: another mutation's condition failed
  MUTATION_OUTCOME_NO_SPACE = 4;      // non-atomic batch: key or memory limit at this item
}
```

PutIfAbsent is a `put` with `condition.absent`; PutIfPresent is a `put`
with `condition.present`.

**Semantics.**
- The condition and the write are one atomic step under the writer lock.
  Of two concurrent PutIfAbsent on one key, exactly one is applied.
- An expired key counts as absent.
- When the condition fails, the reply carries `CONDITION_FAILED`, the
  current version, and (with `return_previous`) the current value: for
  example, the stored idempotent response.

**How it works.** Under the writer mutex, the write path already locates
the key (`kv_cuckoo_upsert`). The condition is tested at that point:
absent → insert, present → replace, otherwise report.

**Cost (E).** About one Put: 0.43 µs alone, 0.13 µs batched.

**Usage.**
```csharp
// An idempotent endpoint: the first request does the work, retries replay its answer.
var r = await kv.PutIfAbsentAsync($"idem:{requestId}", Pending(owner: myId),
                                  ttl: TimeSpan.FromHours(24), returnPrevious: true);
if (!r.Applied)
    return r.Previous is { } prev && IsDone(prev) ? Replay(prev) : Conflict();   // in progress elsewhere
var result = await ChargeCardAsync(order);
await kv.CompareAndSetAsync($"idem:{requestId}", Done(result), expectedVersion: r.Version);

// A unique handle: normalize first (case-fold), then claim.
bool mine = (await kv.PutIfAbsentAsync($"handle:{Normalize(handle)}", userId)).Applied;
```

**Pitfalls.**
- A PutIfAbsent retried after a lost reply reports `CONDITION_FAILED` for
  what was *your own* write. Put a unique token (request ID, owner ID) in
  the value, and compare `previous` to recognize it.
- Normalize keys that humans type (case, whitespace, Unicode) before
  claiming. `Alice` and `alice` are different bytes.

### A4. CompareAndSet (on versions or values)

**Why.**
- **Optimistic concurrency:** two services or users edit one document;
  each writes only if nobody wrote since it read.
- **Read-modify-write on any value:** a JSON profile, a small state
  machine, a settings blob.
- **State transitions:** move a job from `queued` to `running` only if it
  is still queued.

**API.** `Mutate` with `condition.version` (the version a read returned)
or `condition.value_equals` (the bytes you expect).

**Semantics.**
- A version is the revision that created the current record (§1.5).
  - Any Put, Mutate put or Move changes it.
  - A TTL `touch` and in-place updates do not.
- `version = 0` means "must be absent", the same as `condition.absent`.
- `value_equals` compares whole values. It suits small values when no
  version is at hand: for example `"queued"` → `"running"`.

**How it works.** The record header carries its revision (Phase 0). The
condition compares it under the writer lock.

**Cost (E).** About one Put.

**Usage.**
```csharp
while (true)
{
    var cur = await kv.GetWithVersionAsync(key);                 // GetMany(with_versions) of one key
    var next = Apply(cur.Value, change);
    var r = await kv.CompareAndSetAsync(key, next, expectedVersion: cur.Version);
    if (r.Applied) break;                                        // else someone wrote; loop with fresh data
}
```
- Contended hot spots (thousands of writers per second on one key) spin in
  this loop. For numbers, use counters (B1, B2), which never retry on a
  fresh read.

**Pitfalls.**
- Versions identify records, not contents. Writing back the same bytes
  still gives a new version.
- After a restore from a snapshot, every record has a new version (the
  clock-seeded counter, §1.5). A client holding old versions re-reads; it
  never wrongly succeeds.

### A5. GetAndDelete / GetAndSet

**Why.**
- **One-time tokens:** magic links, OTPs, password resets, invitation
  codes, redeemed exactly once.
- **Swap and read:** rotate a secret and get the old one; reset an
  accumulator and process what it held.
- **Hand-off:** a single-slot mailbox between two processes.

**API.** `Mutate` with `delete` or `put`, and `return_previous = true`.

**Semantics.** The read and the write are one atomic step. Of two
concurrent GetAndDelete calls on one key, one gets the value and the other
gets "not found".

**How it works.** The write path holds the old record when it replaces or
removes it. The record is retired, not freed, so it stays valid until the
reply is framed (§2.1).

**Cost (E).** About one delete or put, plus the value copy.

**Usage.**
```csharp
var token = await kv.GetAndDeleteAsync($"reset:{tokenId}");     // null: used, expired or never issued
if (token is null) return LinkExpired();
var old = await kv.GetAndSetAsync($"apikey:{svc}", newSecret);  // rotate, keep the old one for a grace period
```
- Give tokens a TTL (A6) so that unused ones disappear by themselves.

**Pitfalls.** If the reply is lost, the token is consumed but its holder
never saw it, and a retry gets "not found". For one-time tokens that is the
safe way to fail.

### A6. Expire / TTL

**Why.**
- **Self-cleaning state:** sessions and caches.
- **Deadlines:** OTPs and reset tokens that must stop working at a fixed
  time.
- **Temporary holds:** a cart reservation or a seat held for 10 minutes.
- **Idle state that vanishes:** leases (A7), rate-limit state (B3),
  per-day counters.

**API.**
- `ttl_ms` on a Mutate `put`, on Increment's creating call, and on
  SketchAdd's creating call.
- `Mutate` op `touch` with `ttl_ms` changes only the expiry; `ttl_ms = 0`
  removes it (persist).
- `GetMany(with_ttl = true)` returns the remaining milliseconds.
- **Plain Put clears a TTL,** as Redis's SET does. `keep_ttl = true` on a
  Mutate put keeps it.

**Semantics.**
- **Exact for reads and conditions:** a key is absent from the instant its
  deadline passes, whether or not it has been removed yet (§2.4).
- Removal from memory follows within one wheel tick (≤ 1 s by default).
  Watch sees an `EXPIRE` event then.
- Increments and sketch updates keep the TTL.

**How it works.** §2.4: a 16-byte record extension, a coarse clock, the
lazy check and a timing wheel.

**Cost (E, C).**
- Setting or cancelling a TTL is O(1).
- Reads pay one predictable flag test.
- Each key with a TTL takes +16 bytes.
- Gates: Get ±1%; a put with TTL ≤ +50 ns.

**Usage.**
```csharp
await kv.PutAsync($"sess:{uid}:{sid}", session, ttl: TimeSpan.FromMinutes(30));
await kv.ExpireAsync($"sess:{uid}:{sid}", TimeSpan.FromMinutes(30));   // sliding: touch on each request
var hold = await kv.PutIfAbsentAsync($"hold:seat:{show}:{seat}", orderId, ttl: TimeSpan.FromMinutes(10));
await kv.PersistAsync($"hold:seat:{show}:{seat}");                      // paid: keep it (ttl_ms = 0)
```
- Sliding expiry costs a `touch`, not a value rewrite.

**Pitfalls.**
- TTLs run on the server's monotonic clock: wall-clock changes don't move
  them.
- A restart loses deadlines unless snapshots carry them (§V.6).
- Millions of keys expiring in the same second are removed over several
  ticks. Reads already treat them as gone.

### A7. Leases (acquire, renew, release)

**Why.**
- **Distributed locks for cron jobs:** "only one instance runs the billing
  job".
- **Leader election:** one active scheduler, with failover when it dies.
- **Exclusive processing** of a partition, a tenant or a file.

**API.** A client-library composition over Mutate; there is no new RPC.

| Step | Mutate |
|---|---|
| acquire | `put(lease_key, owner_id)`, `condition.absent`, `ttl_ms` = lease time |
| renew | `touch(lease_key)`, `condition.version = token`, `ttl_ms` |
| release | `delete(lease_key)`, `condition.version = token` |

The **token** is the version that acquire returned. Revisions increase,
including across restarts, so the token is a **fencing token**.

**Semantics.**
- At most one holder at any instant, by the server's clock: lazy expiry is
  exact.
- Each new acquisition gets a larger token. A renewal keeps the token,
  because `touch` does not change versions.
- **The lease hazard.** A holder that pauses longer than its lease (a GC
  pause, a VM freeze) may still *believe* it holds it. Fencing tokens are
  the remedy: downstream systems reject writes carrying an older token.

**How it works.** Conditions (A3, A4) plus TTL (A6).

**Cost (E).** About one put per acquire, renew or release.

**Usage.**
```csharp
await using var lease = await kv.AcquireLeaseAsync("lock:billing-cron",
    owner: Environment.MachineName, ttl: TimeSpan.FromSeconds(30), autoRenew: true);
if (lease is null) return;                          // another instance holds it
await RunBillingAsync(fencingToken: lease.Token, cancellation: lease.Lost);
```
- **Auto-renew** runs at a third of the TTL. `lease.Lost` fires when a
  renewal fails or the deadline passes without one.
- **Followers** can Watch the lease key (G1) and retry the moment it is
  deleted or expires, instead of polling.

**Pitfalls.** Without fencing, a lease is an optimization, not a safety
guarantee. Don't choose a TTL shorter than a few round trips.

### A8. Atomic batches and Move

**Why.**
- **Change several keys together:** move credits between two balances,
  transfer an item's ownership, swap two keys.
- **Rename without a gap:** promote `draft:42` to `post:42` without any
  moment where it is missing.
- **Invariants across keys:** keep a record and its own index entry
  (`email:<addr>` → user) consistent, all or nothing.

**API.** `Mutate(atomic = true)` with up to `KVNODE_MAX_ATOMIC` mutations
(default 64). The `move` op moves a value and its TTL to `to_key`.

**Semantics.**
- Every condition is checked first. Then every mutation applies, under one
  hold of the writer lock, as **one revision**.
- If any condition fails, nothing applies: that mutation reports
  `CONDITION_FAILED` and the rest `NOT_ATTEMPTED`.
- **Writers, conditions, Watch and Export cuts see the batch atomically**
  (§2.1).
- **Lock-free readers do not.** Get and GetMany can see it half-applied for
  the microseconds it takes, because there is no MVCC. A reader that needs
  atomic visibility reads with versions and verifies them, or reads through
  Export.
- **Move** gives the destination the batch's revision as its version.
  `overwrite = false` fails the condition if the destination exists.

**How it works.** Validate everything; reserve memory for every new record
(so the batch cannot fail halfway); take the lock; check; apply; take one
revision; release (§2.1).

**Cost (E).** The sum of its writes, 126-450 ns each, within one lock
hold. That hold is bounded by `KVNODE_MAX_ATOMIC`: about 30 µs, or about
60 µs with the ordered index.

**Usage.**
```csharp
// Transfer 100 credits, never overdrawing; retry from fresh reads on conflict.
var (a, b) = (await kv.GetWithVersionAsync("bal:alice"), await kv.GetWithVersionAsync("bal:bob"));
if (Decode(a.Value) < 100) return Insufficient();
var r = await kv.TransactAsync(t => t
    .Put("bal:alice", Encode(Decode(a.Value) - 100), ifVersion: a.Version)
    .Put("bal:bob",   Encode(Decode(b.Value) + 100), ifVersion: b.Version));

await kv.MoveAsync("draft:42", "post:42", overwrite: false);    // rename: never missing, never two
```

**Pitfalls.**
- Atomic batches hold the writer lock, and every other write waits. Keep
  them small and rare.
- **Counters cannot take part.** They change lock-free (B1). To move money
  atomically, store balances as plain values with version conditions, as
  above. Use bounded counters for a single balance.

## Group B — Counters, quotas and rate limits

### B1. Increment / IncrementBy / Decrement

**Why.**
- **Metering:** API usage and usage-based billing (calls per customer per
  day).
- **Engagement:** views, likes, downloads.
- **Stock and play:** inventory counts, game scores.
- **Sequences:** order numbers, invoice numbers.

**API.**
```proto
rpc Increment(IncrementRequest) returns (IncrementResponse);
message IncrementRequest { repeated CounterUpdate updates = 1; }   // 1 .. KVNODE_MAX_WRITE_BATCH
message CounterUpdate {
  bytes key = 1;
  sint64 delta = 2;              // negative to decrement; 0 = read
  optional sint64 min = 3;       // B2: refuse when the result would be below
  optional sint64 max = 4;       // B2: refuse when the result would be above
  bool no_create = 5;            // absent key: refuse instead of starting at 0
  optional uint64 ttl_ms = 6;    // only when this call creates the counter
  bool sharded = 7;              // B4: create it as a hot counter
}
message IncrementResponse { repeated CounterResult results = 1; }
message CounterResult {
  CounterOutcome outcome = 1;
  sint64 value = 2;              // after (applied) or current (refused); 0 for hot counters
  bool created = 3;
}
enum CounterOutcome {
  COUNTER_OUTCOME_UNSPECIFIED = 0;
  COUNTER_OUTCOME_APPLIED = 1;
  COUNTER_OUTCOME_OUT_OF_BOUNDS = 2;   // min or max refused it (B2)
  COUNTER_OUTCOME_ABSENT = 3;          // no_create, and the key does not exist
  COUNTER_OUTCOME_NOT_A_COUNTER = 4;   // the value is not exactly 8 bytes
  COUNTER_OUTCOME_OVERFLOW = 5;        // the result would leave int64
}
```

**Semantics.**
- **What a counter is:** a key whose value is exactly 8 bytes, int64
  little-endian. Get returns those bytes, and a Put of any 8 bytes sets or
  resets it.
- **Never lost:** an unbounded update is one `LOCK XADD`. It is
  linearizable, and no increment is lost under any contention (exact totals
  measured at 7 threads, M).
- **A Put or Delete racing an increment:** the replacement wins (§2.3
  rule 2).
- **Absent keys:** an absent key starts at 0, with `ttl_ms` if given. The
  creation goes through the writer path and re-checks under the lock, so
  two creators never lose an increment.
- **Untouched metadata:** increments keep the TTL and the version (§1.5).
- **Watch** sees an `ADD` event (delta and value after). **Export** is
  exact as of the cut (§2.3 rule 4).
- **Overflow:** `XADD` cannot check it first. The server detects it from
  the returned old value, undoes the add and reports `OVERFLOW`; a
  concurrent reader can glimpse the wrapped value for nanoseconds. Counters
  that may approach ±9.2 × 10^18 should use bounds (B2), which never apply
  an out-of-range result.

**How it works.** The batched lookup pipeline (prefetch buckets, then
records), then `LOCK XADD` on the 8-aligned cell, with no allocation and no
writer lock.

**Cost (M).**
- 58 ns per update, batched, on one thread.
- 71.6 ns per update per thread at 7 threads: 97.8 M/s in total.
- 1,000 hot keys at 7 threads: 40.7 ns, 171.9 M/s.
- One key from 7 threads: 108 ns per update (64.6 M/s); see B4.

**Usage.**
```csharp
long views = await kv.IncrementAsync($"views:post:{postId}");
await kv.IncrementAsync($"usage:{cust}:{day}", delta: units, ttl: TimeSpan.FromDays(40));
long orderNo = await kv.IncrementAsync("seq:orders");            // unique and increasing
var month = await kv.GetCountersAsync(DaysOf(month).Select(d => $"usage:{cust}:{d}"));  // GetMany
```
- **Metering:** one counter per customer per day, with a TTL past the
  billing period. Read a month with GetMany.
- **Batching:** a request handler can increment many counters in one call
  (up to 256 updates).

**Pitfalls.**
- **Binary, not text.** A counter is 8 binary bytes, and Get returns int64
  LE for the client library to decode. A Put of `"42"` (2 bytes) is not a
  counter.
- Versions don't change on increments, so a CompareAndSet on a counter
  only sees Put-level changes.

### B2. Bounded counters (DecrementIfAtLeast)

**Why.**
- **Never oversell:** seats, tickets, stock.
- **Caps:** coupon redemptions, license seats, prepaid credits that never
  go negative.
- **Quotas** checked and consumed in one step: "projects remaining".

**API.** `Increment` with `min` and/or `max`.

**Semantics.** The bound check and the update are one atomic step. The
result is applied only if it stays within [min, max]; otherwise the reply
is `OUT_OF_BOUNDS` with the current value. A bounded counter never
overshoots, under any contention.

**How it works.** A `LOCK CMPXCHG` loop: read v; if v + delta is out of
bounds, refuse; otherwise CAS v → v + delta and retry if it changed.

**Cost (M).**
- Uncontended: about an increment (24 ns on a cached key; ~58 ns on a
  random one, where the lookup dominates).
- One key from 7 threads: 161 ns per update per thread, 43.5 M/s in total.

**Usage.**
```csharp
var r = await kv.IncrementAsync($"stock:{sku}", delta: -qty, min: 0);
if (r.Outcome == CounterOutcome.OutOfBounds) return SoldOut(available: r.Value);
var seat = await kv.IncrementAsync($"seats:{license}", delta: 1, max: plan.Seats);
```
- **Reserve, then confirm:** decrement with `min: 0` at checkout, and
  increment back if payment fails. Holds with a TTL release themselves in
  R4.

**Pitfalls.** Bounded counters cannot be hot counters (B4): a bound needs
one global value.

### B3. RateLimit

**Why.**
- **Request limits** per customer and per API key: fair use, plan tiers.
- **Brute-force protection** for logins and OTPs, per account and per IP.
- **Outgoing caps:** SMS and email per user per hour.
- **Scraping protection.**

**API.**
```proto
rpc RateLimit(RateLimitRequest) returns (RateLimitResponse);
message RateLimitRequest {
  bytes key = 1;            // e.g. "rl:api:" + apiKey
  uint32 limit = 2;         // requests per period, sustained
  uint64 period_ms = 3;     // e.g. 60000
  uint32 burst = 4;         // requests allowed at once; 0 = limit
  uint32 cost = 5;          // this request's weight; 0 = 1
}
message RateLimitResponse {
  bool allowed = 1;
  uint32 remaining = 2;     // requests still allowed right now
  uint64 retry_after_ms = 3;  // refused: when this request would be allowed
  uint64 reset_after_ms = 4;  // until the limiter is back to full
}
```

**Semantics.** The algorithm is GCRA, the generic cell rate algorithm,
which redis-cell uses (C).

| Symbol | Meaning |
|---|---|
| TAT | the state: one 8-byte theoretical arrival time |
| T | `period / limit` |
| τ | `burst × T` |

1. A request of cost c at time t computes `new = max(TAT, t) + c·T`.
2. It is allowed when `new − t ≤ τ`; then `TAT ← new`.
3. `remaining = ⌊(τ − (new − t)) / T⌋`.
4. `retry_after = new − t − τ` when refused.
5. `reset_after = new − t`.

**Properties.**
- **Exact and atomic** per key: one CAS.
- **Smooth:** no burst at window boundaries, unlike fixed windows.
- **Self-cleaning:** the state key expires after `reset_after` (A6), so
  idle callers cost no memory.

**How it works.**
- A lookup, then a CAS loop on the 8-byte TAT, on the coarse monotonic
  clock (§1.7).
- A first call for a key creates the state through the writer path.
- RateLimit state emits no Watch events: it is internal.

**Cost (E from M).** About an increment for a known key; about a create on
the first call.

**Usage.**
```csharp
var rl = await kv.RateLimitAsync($"rl:api:{apiKey}", limit: 600, period: TimeSpan.FromMinutes(1), burst: 50);
if (!rl.Allowed) return TooManyRequests(retryAfter: rl.RetryAfter);   // HTTP 429 + Retry-After
Response.Headers["X-RateLimit-Remaining"] = rl.Remaining.ToString();
```
- **Layer limits:** check per account and per IP (`rl:login:acct:{id}`
  5/min; `rl:login:ip:{ip}` 50/min), and deny when either denies.
- **Weighted costs:** an expensive endpoint passes `cost: 10`.

**Pitfalls.**
- Client retries count as requests.
- Active callers hold one small record each until their state expires.

### B4. Hot counters

**Why.** One key incremented from every core at once: a flash sale's
"tickets sold", a live vote, a viral post's likes.

**API.** `Increment` with `sharded = true` on the call that creates the
counter; later calls need nothing special.

**Semantics.**
- **Each server worker adds to its own 64-byte cell** with a plain store:
  no `LOCK` prefix, and no cache line bouncing between cores.
- **Reads sum the cells.** Get, GetMany and Download return the 8-byte
  sum.
  - For unit increments the sum is exact: it equals the true value at some
    instant during the read.
  - With mixed deltas it lies between the values at the read's start and
    end (golden-operations §4.2).
- **No per-increment value:** an increment's reply has `value = 0`. Read
  the total when you need it.
- **No bounds** (B2).
- **Events:** each worker publishes one coalesced `ADD` event per counter
  per housekeeping tick (§2.2's ceiling), not one per increment.

**How it works.** The record's value area holds one cell per worker; the
worker's index selects its cell. A header flag tells reads to sum.

**Cost (M).**
- 22.6 ns per increment per thread at 7 threads: 310 M/s in total, against
  64.6 M/s for `LOCK XADD` and 43.5 M/s for a CAS loop on one key.
- Memory: 64 bytes × workers per hot counter (512 B with 8 workers).

**Usage.**
```csharp
await kv.IncrementAsync("votes:final", delta: 1, sharded: true);   // creating call (idempotent intent)
await kv.IncrementAsync("votes:final");                             // every later vote
long total = await kv.GetCounterAsync("votes:final");               // sums the cells
```

**Pitfalls.** Only worth it above roughly 100k increments per second on
one key; below that, a plain counter is cheaper to read.

## Group C — Membership and sampling

### C1. The absent-key filter, inside the server

**Not a callable function, and why.**
- A remote "might it exist?" call costs a network round trip. That is
  orders of magnitude more than the server's own answer for an absent key:
  29.7 ns batched, 120.5 ns in a plain loop (M).
- The business need behind it is shielding the store from floods of
  lookups for keys that don't exist (cache penetration). Three things meet
  it already:
  - absent keys are the store's *cheapest* answer;
  - client-side filters (C2);
  - rate limits (B3).

**What remains is an internal accelerator.**
- A split-block Bloom filter consulted inside the batched GET pipeline:
  - 12.9 ns per check with AVX2, 31 ns scalar (M);
  - 13 MB at 10M keys (11 bits per key, 0.83% false positives);
  - 37 ns per create.
- **Bits before publish.** Writers set a key's bits before publishing its
  record. A reader that answers "absent" from stale or partial bits read
  the block before the last bit store became visible. That was before the
  key existed, so the answer is linearizable.
- **Deletes cannot clear bits.** A background rebuild (0.37 s at 10M keys,
  §2.7) restores the false-positive rate after churn.
- **The catch:** every *hit* also pays the check. The filter ships only if
  a miss-heavy workload gains ≥ 10% GET/s at ≥ 50% misses, with no
  regression at 0% misses (gate).

### C2. GetFilter (filters clients check locally)

**Why.**
- **Skip the network for absent keys:** apps, edge servers and batch jobs
  pre-validate coupon codes, check blocklists, dedupe crawled URLs, or ask
  "is this SKU in the catalog?".
- **Offline validation** in mobile apps.
- **Miss-heavy workloads:** every skipped miss saves a whole round trip
  (tens to hundreds of microseconds) and the server's CPU for it.

**API.**
```proto
rpc GetFilter(GetFilterRequest) returns (stream GetFilterResponse);
message GetFilterRequest {
  bytes prefix = 1;               // only keys with this prefix; empty: all keys
  uint64 if_newer_than = 2;       // a revision the client holds: not_modified when nothing changed
}
message GetFilterResponse {
  oneof item {
    FilterHeader header = 1;      // always first
    bytes chunk = 2;              // then the filter's bytes, in order, ≤ 1 MiB each
  }
}
message FilterHeader {
  uint64 revision = 1;            // the keys as of this revision
  uint64 keys = 2;
  uint64 size_bytes = 3;
  fixed64 seed0 = 4;              // the public SipHash key of this filter's key hashes
  fixed64 seed1 = 5;
  bool not_modified = 6;
}
// Format: a binary fuse filter with 8-bit fingerprints, 3-wise (Graf and Lemire),
// over SipHash-2-4(seed0, seed1; key) — documented byte for byte in PROTOCOL.md.
```

**Semantics.**
- **"No" is definite:** the key did not exist at `revision`.
- **"Maybe" is wrong 0.38% of the time** (8-bit fingerprints, M).
- **Keys created after `revision` are not in the filter.** Clients refresh
  with `if_newer_than`, or always ask the server about keys they just
  created.
- **Hashes use a public per-filter seed**, not the store's secret key. A
  crafted key can at worst cause a false "maybe", which the client resolves
  with the server.

**How it works.**
- On demand, the background builder (§2.7) walks the scope: the whole
  table, or a prefix through the ordered index.
- It hashes each key with the filter's seed and builds the filter.
- The filter is cached per (scope, revision) and streamed in chunks.
- Builds are rate-limited per scope (`KVNODE_FILTER_MIN_INTERVAL_SEC`) and
  size-capped.

**Cost (M).**
- A client-side check takes 52 ns.
- 9.02 bits per key: 10.8 MB at 10M keys, 1.1 MB for 1M.
- A build takes 88 ns per key (0.9 s at 10M), off the event loop.

**Usage.**
```csharp
var coupons = await kv.GetFilterAsync(prefix: "coupon:");           // ~1.1 MB for 1M coupons
if (!coupons.MightContain($"coupon:{code}")) return InvalidCoupon(); // no network at all
var c = await kv.GetAsync($"coupon:{code}");                         // confirm a "maybe"
_ = coupons.RefreshEvery(TimeSpan.FromMinutes(5));                   // if_newer_than under the hood
```

**Pitfalls.**
- **A filter reveals which keys exist,** approximately. Anyone holding it
  can test candidate keys offline, without rate limits. Export filters only
  for key sets that are not sensitive: never usernames or emails of a
  private service. The server can restrict it to configured prefixes
  (`KVNODE_FILTER_PREFIXES`).
- **Stale by design:** new keys answer "no" until the next refresh.

### C3. Sample / RandomKey

**Why.**
- **Audits:** compliance and QA checks of a random 1% of records.
- **Real-key workloads:** cache warm-up, load tests, ML training samples,
  A/B cohort draws.
- **"Random item" features:** a random product, a random quote, a random
  question.
- **Estimates:** average value size, or the share of records with some
  property.

**API.**
```proto
rpc Sample(SampleRequest) returns (SampleResponse);
message SampleRequest {
  uint32 count = 1;       // 1 .. KVNODE_MAX_READ_BATCH
  bytes prefix = 2;       // empty: the whole store; else needs the ordered index or a directory
  bool keys_only = 3;
  bool distinct = 4;      // no key twice in one answer
}
message SampleResponse { repeated Entry entries = 1; }
message Entry {           // shared with Scan, Seek, Pop and Export (export-watch.md fields 1-2)
  bytes key = 1;
  bytes value = 2;
  uint64 version = 3;
  optional uint64 ttl_ms = 4;
}
```

**Semantics.** Uniform over live keys: each key is equally likely, with
replacement unless `distinct`. Expired keys are skipped.

**How it works.**
- **Whole store:** pick random table slots and retry the empty ones (1.26
  tries expected at the default load), prefetching 32 candidates per round
  (golden-operations §4.6).
- **Within a prefix:** random ranks within the prefix's range in the
  ordered index (Select), or random members of a directory, O(1) each.

**Cost (M).** 28 ns per key, 23.2 ns batched: 1,000 samples in 23 µs.

**Usage.**
```csharp
var audit = await kv.SampleAsync(count: 500, prefix: "order:2026-09:", distinct: true);
double share = audit.Count(e => Violates(e.Value)) / 500.0;      // ±~4.5% at 95% for n = 500
```

**Pitfalls.** After mass deletes (load far below 80%), whole-store
sampling needs 1/load tries per key. The server bounds the tries and
returns fewer keys rather than spinning.

### C4. Stats (Count)

**Why.**
- Capacity dashboards and alerts before the store fills.
- Client-side backpressure.
- Admin consoles.

**API.**
```proto
rpc Stats(StatsRequest) returns (StatsResponse);
message StatsRequest { bytes prefix = 1; }       // empty: the whole store; a prefix: G5
message StatsResponse {
  uint64 keys = 1;
  uint64 value_bytes = 2;
  uint64 memory_bytes = 3;        // whole store
  uint64 capacity_keys = 4;
  uint64 memory_limit_bytes = 5;
  fixed64 incarnation = 6;
  uint64 revision = 7;            // the latest stable revision
  uint64 expiring_keys = 8;       // keys with a TTL
}
```

**Semantics.** Each number is exact at some instant during the call; they
are not a joint snapshot.

**How it works.** Counters the store already keeps (`count`, `mem_used`),
plus Phase 0's revision and the TTL count.

**Cost.** O(1).

**Usage.**
- Poll every few seconds for dashboards.
- Alert at 80% of `capacity_keys` or `memory_limit_bytes`.

## Group D — Ordered queries

All of these run on the ordered index (§2.5), enabled with
`KVNODE_INDEX_ORDER=on`; without it they answer `UNIMPLEMENTED`.

- **Order** is byte order: `memcmp`, a prefix first.
- **Long keys:** keys longer than 64 bytes are digests (§1.9).
- **Freshness:** reads are live; each call sees the current state.
- **Costs** are measured on two structures and gated on both:
  - the sorted-snapshot accelerator (golden probe, M);
  - the radix tree (index probe, K).

### D1. Scan (ranges and pagination)

**Why.**
- **Paginated listings:** a customer's orders, messages or files.
- **Browsing:** admin consoles and support tools over a namespace.
- **Ordered processing:** an outbox or event log in key order.
- **Time ranges:** every reading of a sensor between two times.

**API.**
```proto
rpc Scan(ScanRequest) returns (ScanResponse);
message KeyRange {
  oneof kind {
    bytes prefix = 1;             // keys starting with these bytes
    Bounds bounds = 2;            // start <= key < end
    bytes suffix = 3;             // keys ending with these bytes (needs KVNODE_INDEX_ORDER=both)
  }
}
message Bounds {
  bytes start = 1;                // empty: from the first key
  bytes end = 2;                  // empty: to the last key
  bool start_exclusive = 3;
  bool end_inclusive = 4;
}
message ScanRequest {
  KeyRange range = 1;
  uint32 limit = 2;               // keys per page; 0 = 100; at most KVNODE_MAX_SCAN_PAGE (1,000)
  bool keys_only = 3;
  bool reverse = 4;               // descending
  bytes cursor = 5;               // the previous page's next_cursor
  bool with_versions = 6;
  bool with_ttl = 7;
}
message ScanResponse {
  repeated Entry entries = 1;
  bytes next_cursor = 2;          // empty: the range is exhausted
}
```

**Semantics.**
- **Each page** lists keys in order from the cursor, live, and stops early
  at the response cap.
- **Across pages,** a key present for the whole scan appears **exactly
  once**. A key created or deleted during the scan may or may not appear,
  but **never twice**: the cursor is the last key returned, exclusive.
  That is stronger than Redis `SCAN`, which can repeat keys.
- **The cursor is opaque:** the last key plus a fingerprint of the range,
  so it cannot be replayed against another range.
- **For one point-in-time listing** across all pages, use Export with the
  same filter (G3).

**How it works.**
- Seek the radix tree to the start, which costs O(key length).
- Walk the leaves in order under optimistic lock coupling.
- Read values through each leaf's record pointer (§2.5).

**Cost.**

| Measurement | Result |
|---|---|
| seek (M, snapshot) | 71 ns batched, 263 ns alone |
| 100 keys (M, snapshot) | 188 ns keys-only; 1.16 µs with values |
| radix tree, 400 keys (K) | 20 µs, ~50 ns per key with iteration |
| a 100-key page with values (E) | ≈ 5 µs |
| gate | ≤ 10 µs for a 100-key page with values |

**Usage.**
```csharp
// A customer's orders, newest first, 50 per page; the cursor goes to the browser as opaque base64.
var page = await kv.ScanAsync(prefix: $"order:{cust}:", reverse: true, limit: 50, cursor: fromBrowser);
// Every reading of sensor 7 during an hour (fixed-width timestamps sort numerically):
await foreach (var e in kv.ScanAllAsync(start: $"temp:s7:{Fixed(t0)}", end: $"temp:s7:{Fixed(t1)}"))
    Plot(e);
```

**Pitfalls.** Keys sort the way they will be read only if numbers are
fixed-width (§1.9): `"10"` sorts before `"9"`.

### D2. Count (prefix or range)

**Why.**
- **Quotas:** "free plan: 100 projects", checked by counting
  `proj:<tenant>:`.
- **Reporting:** per-tenant item counts, totals in dashboards, "page 3 of
  57".
- **Windows:** orders in the last hour, as a count over a timestamp range.

**API.**
```proto
rpc Count(CountRequest) returns (CountResponse);
message CountRequest { KeyRange range = 1; }
message CountResponse { uint64 count = 1; }
```

**Semantics.** Exact at some instant during the call. It includes keys
whose TTL passed within the last sweep tick (§2.4).

**How it works.**
- Subtree counts: a prefix's count is its node's count.
- A range is `rank(end) − rank(start)` (D4).
- O(key length); no key is visited.

**Cost.** 75 ns batched on the snapshot (M). On the radix tree, about a
seek (E; gate ≤ 1 µs).

**Usage.**
```csharp
long pages = (await kv.CountAsync(prefix: $"order:{cust}:") + 49) / 50;
long lastHour = await kv.CountAsync(start: $"evt:{Fixed(now - 3_600_000)}", end: $"evt:{Fixed(now)}");
```
- **Hard quotas race:** count-then-create can overshoot with concurrent
  creators. Keep the quota in a bounded counter (B2), and use Count for
  reporting and reconciliation.

### D3. Seek (Next, Prev, Floor, Ceiling)

**Why.**
- **"As of" reads:** the last price at or before time T (`Floor` on
  `px:<sym>:<t>`).
- **Versions:** the latest version ≤ v of a document.
- **Scheduling:** the next job due after now (`Ceiling` on
  `job:<due>:<id>`).
- **Range tables:** IP ranges keyed by start address, price tiers by
  threshold, tax brackets (`Floor`).

**API.**
```proto
rpc Seek(SeekRequest) returns (SeekResponse);
enum SeekMode {
  SEEK_MODE_UNSPECIFIED = 0;
  SEEK_MODE_CEILING = 1;          // the first key >= target
  SEEK_MODE_HIGHER = 2;           // the first key > target
  SEEK_MODE_FLOOR = 3;            // the last key <= target
  SEEK_MODE_LOWER = 4;            // the last key < target
}
message SeekRequest {
  bytes target = 1;
  SeekMode mode = 2;
  bytes within_prefix = 3;        // the answer must start with this; empty: anywhere
  bool keys_only = 4;
}
message SeekResponse { bool found = 1; Entry entry = 2; }
```

**Semantics.** Live and linearizable per call. `within_prefix` keeps a
floor from crossing into a neighboring series.

**Cost (M, snapshot).** 263 ns alone, 71 ns batched. The radix tree is
O(key length) (K).

**Usage.**
```csharp
var px = await kv.SeekAsync($"px:EURUSD:{Fixed(t)}", SeekMode.Floor, withinPrefix: "px:EURUSD:");
var row = await kv.SeekAsync($"ip4:{Fixed(addr)}", SeekMode.Floor, withinPrefix: "ip4:");
if (row.Found && addr <= RangeEnd(row.Value)) country = Country(row.Value);
```

### D4. Rank / Select

**Why.**
- **Leaderboards:** a player's position, when the score is in the key
  (`lb:<board>:<inverted score>:<player>`).
- **Queue position:** "you are #57 in line".
- **Jump to page N** without scanning: Select(N × page size).
- **Percentiles** over sorted keys: Select(0.99 × count).

**API.**
```proto
rpc Rank(RankRequest) returns (RankResponse);
rpc Select(SelectRequest) returns (SelectResponse);
message RankRequest { bytes key = 1; bytes within_prefix = 2; }
message RankResponse {
  uint64 rank = 1;                // keys in the scope ordered before key
  bool found = 2;                 // key itself exists (then rank is its 0-based position)
  uint64 total = 3;               // keys in the scope
}
message SelectRequest { uint64 index = 1; bytes within_prefix = 2; bool from_end = 3; bool keys_only = 4; }
message SelectResponse { bool found = 1; Entry entry = 2; uint64 total = 3; }
```

**Semantics.** Exact at some instant. An index beyond the scope gives
`found = false`.

**How it works.** The order-statistic radix tree (§2.5).
- Rank sums the counts of the subtrees left of the path; Node48/256 use an
  AVX2 sum.
- Select descends by counts.

**Cost.**
- Snapshot (M): Rank 71 ns batched (263 ns alone); Select 2.8 ns.
- Radix tree (E): O(key length × node scan); gate ≤ 1 µs.

**Usage.**
```csharp
string Key(long score, string p) => $"lb:weekly:{Fixed(long.MaxValue - score)}:{p}";   // best first
await kv.TransactAsync(t => t.Delete(Key(oldScore, p)).Put(Key(newScore, p), meta));  // move the player
var me = await kv.RankAsync(Key(newScore, p), withinPrefix: "lb:weekly:");            // 0 = first place
var top10 = await kv.ScanAsync(prefix: "lb:weekly:", limit: 10);
var around = await kv.ScanAsync(start: (await kv.SelectAsync(Math.Max(0, me.Rank - 5), "lb:weekly:")).Key, limit: 11);
```

**Pitfalls.** A score change is a delete plus a create: two index updates,
≈1.6 µs of index work (E). That is fine for thousands of score changes per
second, not millions. For very hot boards, keep scores in counters and
rebuild the board key periodically.

### D5. Pop (queues and delayed jobs)

**Why.**
- **Work queues:** take the oldest job.
- **Delayed jobs and reminders:** take the jobs whose due time has passed
  (`job:<due>:<id>`, popped before = now).
- **Priority queues:** the priority goes in the key.

**API.**
```proto
rpc Pop(PopRequest) returns (PopResponse);
message PopRequest {
  bytes prefix = 1;               // the queue
  uint32 count = 2;               // 0 = 1; at most 256
  bool from_end = 3;              // the largest keys instead (LIFO, highest priority)
  bytes before = 4;               // only keys < this (e.g. prefix + Fixed(now)); empty: no bound
  bool keys_only = 5;
}
message PopResponse { repeated Entry entries = 1; }
```

**Semantics.**
- Each popped key is removed and returned to exactly one caller: finding
  and deleting happen under the writer lock. Watch sees a `DELETE` event.
- **Delivery is at most once.** A lost reply loses the job. For at least
  once, use claims (R7).

**How it works.** Under the writer lock, `count` times: seek the first key
of the prefix (below `before`), then delete it from the table and the
index.

**Cost (E).** About a seek plus a delete with index maintenance: 0.5-0.8 µs
per job (1.4-2.3× get).

**Usage.**
```csharp
await kv.PutAsync($"job:{Fixed(dueMs)}:{jobId}", payload);                              // schedule
var due = await kv.PopAsync(prefix: "job:", before: $"job:{Fixed(NowMs())}", count: 32); // run what's due
```

**Pitfalls.** Pop is destructive: a worker that crashes loses the jobs it
popped. Use claims (R7) where every job must run.

### D6. DeleteRange / DeletePrefix

**Why.**
- **Erasure:** GDPR and CCPA "right to be forgotten" for everything under
  `u:<id>:` (R10).
- **Bulk cleanup:** tenant offboarding, test-data cleanup, flushing a cache
  namespace.
- **Retention:** delete events older than 30 days (a range of timestamped
  keys).

**API.**
```proto
rpc DeleteRange(DeleteRangeRequest) returns (DeleteRangeResponse);
message DeleteRangeRequest {
  KeyRange range = 1;
  uint32 limit = 2;               // keys per call; 0 = 256; at most 256 (the 1000× budget)
  bytes cursor = 3;
}
message DeleteRangeResponse { uint64 deleted = 1; bytes next_cursor = 2; }
```

**Semantics.**
- Each key is deleted atomically, but the range is not deleted atomically
  across calls.
- Keys created inside the range during the deletion can survive. Loop
  until `next_cursor` is empty, then confirm with Count.
- The writer lock is taken per chunk of 8 keys.

**Cost (M, K).** About 149 ns per key for the delete, plus ~0.76 µs for
index maintenance: ≈0.9 µs per key, 256 keys in ≈ 230 µs per call.

**Usage.**
```csharp
long n = await kv.DeletePrefixAsync($"u:{userId}:");                     // loops calls until done
await kv.DeleteRangeAllAsync(start: "evt:", end: $"evt:{Fixed(now - 30 * Day)}");   // retention
```

**Pitfalls.** Erasure must also reach the key families that point to the
user from outside the prefix, such as `email:<addr>` → user. Keep such
pointers in a per-user manifest, or design every user key under one prefix.

### D7. LongestPrefix

**Why.**
- **Routing and configuration inheritance:** the most specific config for
  a path (`cfg:/shop/books/` beats `cfg:/shop/`).
- **Telephony:** a phone number → carrier and rate, by longest matching
  prefix.
- **Hierarchical overrides** for feature flags and permissions (org →
  team → user).

**API.**
```proto
rpc LongestPrefix(LongestPrefixRequest) returns (LongestPrefixResponse);
message LongestPrefixRequest { bytes key = 1; bytes within_prefix = 2; bool keys_only = 3; }
message LongestPrefixResponse { bool found = 1; Entry entry = 2; }
```

**Semantics.** Returns the longest stored key that is a prefix of `key`
and starts with `within_prefix`.

**How it works.** Descend the radix tree along `key`, remembering the last
node that ends a stored key. O(key length).

**Cost (E, from K).** About a seek: ~0.2 µs.

**Usage.**
```csharp
var rate = await kv.LongestPrefixAsync($"rate:{e164}", withinPrefix: "rate:");   // "rate:+4420" beats "rate:+44"
var cfg  = await kv.LongestPrefixAsync($"cfg:{path}", withinPrefix: "cfg:");
```

### D8. Prefix and suffix listing

**Why.**
- **Everything under one parent:** "log out all my sessions" (every key
  under `sess:<user>:`), a user's devices, an entity's sub-records.
- **Everything of one kind:** every key ending `:email`, through the
  suffix index.

**API.** `Scan` with `range.prefix` or `range.suffix`. Suffix listing
needs `KVNODE_INDEX_ORDER=both`, a second radix tree over reversed keys.

**Semantics.**
- Suffix listing returns keys in reversed-key order (sorted by suffix), not
  by key.
- A suffix shared by a quarter of all keys is a full scan in disguise:
  0.5-1.2 s for 4M of 16M keys (K). Use Export for that.

**Cost (K, 16M keys).** 4 keys in 0.2 µs; 400 keys in 20 µs.

**Usage.**
```csharp
await foreach (var s in kv.ScanAllAsync(prefix: $"sess:{uid}:", keysOnly: true)) ShowDevice(s.Key);
long n = await kv.DeletePrefixAsync($"sess:{uid}:");                       // "log out everywhere"
```

## Group E — Search over keys

The indexes here are opt-in, enabled **per key prefix**, with their memory
known up front (§1.11). Keys longer than 64 bytes are digests and cannot
be searched (§1.9).

### E1. FindSimilar, one edit

**Why.**
- **"Did you mean…?"** for product SKUs, usernames, city names, commands.
- **Typo-tolerant lookups** of identifiers read over the phone or typed by
  hand: order IDs, license plates, coupon codes.
- **Dedupe at write time:** near-identical customer names or emails with
  one typo.
- **Fraud and phishing:** names one edit away from protected ones
  (`paypa1`, `arnazon`).

**API.**
```proto
rpc FindSimilar(FindSimilarRequest) returns (FindSimilarResponse);
message FindSimilarRequest {
  bytes key = 1;
  uint32 max_edits = 2;           // 1 (default) or 2 (E2)
  bytes within_prefix = 3;        // must be inside an indexed prefix
  uint32 limit = 4;               // 0 = 20; at most 1,000
  bool keys_only = 5;
}
message FindSimilarResponse {
  repeated SimilarEntry entries = 1;   // closest first, then by key
  bool truncated = 2;                  // more matches than limit
}
message SimilarEntry { Entry entry = 1; uint32 edits = 2; }
```

**Semantics.**
- Exact Levenshtein distance over key **bytes**: insert, delete or
  substitute one byte. A non-ASCII UTF-8 character is several bytes, so
  one changed character can be several edits.
- The key itself (0 edits) is included when present.

**How it works.** A SymSpell symmetric-delete index (golden-operations
§2.4).
- **Variants:** every key's ~21.5 single-byte deletion variants are hashed
  in O(length), from prefix and suffix polynomial hashes under a secret
  base.
- **Storage:** 64-byte buckets of 8 lanes, each lane a 27-bit fingerprint
  plus the key length, paired with the key's location.
- **Query:**
  1. hash the query's own ~22 variants;
  2. prefetch all their buckets as a group;
  3. scan each bucket with two AVX2 compares, for fingerprint hits and
     empty lanes;
  4. prefetch the candidate keys;
  5. verify them with Myers' bit-parallel distance.
- **Maintenance:**
  - 21.5 lane inserts per created key, prefetched (~1 µs);
  - tombstones on delete;
  - each lane writes its location before its fingerprint
    (golden-operations §4.5).

**Cost (M).**
- 0.57-0.70 µs per query at 10M keys.
- 2.19 GB (235 bytes per key).
- ~1 µs per create.

**Usage.**
```csharp
var hits = await kv.FindSimilarAsync($"sku:{typed}", withinPrefix: "sku:");
if (hits.Count == 1) return Redirect(hits[0].Key);          // unambiguous correction
var lookalike = await kv.FindSimilarAsync($"protected:{newHandle}", withinPrefix: "protected:");
if (lookalike.Count > 0) return RejectHandle();              // one edit from a protected brand
```

**Pitfalls.** At 2.19 GB per 10M keys, index only the prefixes that need
it (`KVNODE_INDEX_FUZZY=sku:,protected:`).

### E2. FindSimilar, two edits

**Why.** More tolerant matching: two typos. A transposed pair of letters
is also two edits under Levenshtein distance.

**How it works.**
1. **Expand** the query into its one-edit neighbors over the key alphabet.
2. **De-duplicate** their deletion variants: ~15-20k for a 20-byte key.
3. **Probe** them all as one prefetched batch against the E1 index.
4. **Verify** with Myers distance ≤ 2.

It is complete: a key two edits away is one edit away from some
neighbor.

**Cost (E).**
- **Estimate:** 0.2-0.4 ms at 10M keys (600-1,200× get), at the edge of
  the synchronous budget. Not measured.
- **Gate:** measure first. Above 0.35 ms it ships only restricted to small
  `within_prefix` scopes, or as a streamed query.
- **The cited alternative:** a Levenshtein automaton walked over the radix
  tree (Lucene's FuzzyQuery approach).

### E3. FindContaining (substring)

**Why.**
- **Support tools:** find a customer from part of an email or an order
  number.
- **Admin search** over key names.
- **Log keys:** find events whose key contains a correlation fragment.

**API.**
```proto
rpc FindContaining(FindContainingRequest) returns (FindContainingResponse);
message FindContainingRequest {
  bytes fragment = 1;             // 3 .. 64 bytes
  bytes within_prefix = 2;        // must be inside an indexed prefix
  bool count_only = 3;
  uint32 limit = 4;               // 0 = 100; at most 1,000
  bool keys_only = 5;
  bytes cursor = 6;
}
message FindContainingResponse { uint64 count = 1; repeated Entry entries = 2; bytes next_cursor = 3; }
```

**How it works (proposed).**
- **The index** is a suffix array over the indexed keys' suffixes. Each
  entry holds the suffix's first 8 bytes, big-endian, plus the key's
  location and offset: 12 bytes.
- **A query** binary-searches the fragment's range: two searches,
  interleaved as in golden-operations §2.3.
- **The range length is the count;** the entries give the keys.
- **Maintenance:** background rebuilds (§2.7) plus a small sorted delta of
  recent creates.

**Cost (E).**
- **Estimate:** 3-6 µs to count, plus ~10 ns per listed match; ~1.9 GB at
  10M keys (160M suffixes × 12 bytes). Not measured.
- **Why not trigrams:** the measured trigram index took 7.9 µs at 100k
  keys and 91 µs at 1M. That extrapolates to ~0.9 ms at 10M, outside the
  synchronous budget.
- **Gate:** ≤ 10 µs per count at 10M keys.

### E4. Matching (registered patterns)

**Why.**
- **Live segment counts:** "how many orders match `order:*:pending`
  right now?"
- **Dashboards** over key patterns.
- **Cheap segments:** an Export of a segment, or an alert when matching
  keys appear (with Watch).

**API.** Patterns are registered in configuration (`KVNODE_PATTERNS`,
key-indexes.md §2.3).
```proto
rpc Matching(MatchingRequest) returns (MatchingResponse);
message MatchingRequest { string pattern = 1; bool count_only = 2; uint32 limit = 3; bool keys_only = 4; bytes cursor = 5; }
message MatchingResponse { uint64 count = 1; repeated Entry entries = 2; bytes next_cursor = 3; }
```

**How it works.**
- One union DFA over all registered patterns runs on every create and
  delete: about 20-40 ns per write for 16-byte keys (X).
- Each matching pattern's member set is updated: ~70-110 ns per matching
  pattern (K).
- Counting is O(1); listing is O(k).

**Cost (E).** ~20-40 ns per write, plus ~0.1 µs per matching pattern.

**Usage.**
- `CountMatchingAsync("pending-orders")` for a dashboard tile.
- Watch with the same pattern for live updates.

### E5. Ad hoc patterns

**Why.** Migrations, investigations, one-off reports: "every key matching
`order:*:2026-0[1-3]:*`".

**API.** Export with a `KeyFilter` (glob or regex: export-watch.md §2,
§4).
- **Streamed, never a unary call:** at 10M keys a full scan is ~92 ms of
  CPU (extrapolated), ~265,000× a get.
- **The planner** (key-indexes.md §3) answers patterns with a literal
  prefix from the ordered index, visiting only that prefix.

**Usage.**
```csharp
await foreach (var e in kv.ExportAsync(glob: "order:*:2026-0[1-3]:*", keysOnly: true)) Report(e.Key);
```

## Group F — Sketches

**Sketches are typed values.**
- **A sketch lives under a key** like any value:
  - created by its first SketchAdd, with its parameters;
  - deleted with Delete;
  - expirable with a TTL;
  - readable with Get (the documented layout).
- **Items are hashed with the server's secret SipHash key,** so nobody can
  craft items that collide inside a sketch.
- **Updates are in place and lock-free** (§2.3); TopK takes a small
  per-sketch lock.
- **Not streamed by Watch:** each sketch update would flood watchers. Poll
  SketchQuery instead (open decision 8).

```proto
rpc SketchAdd(SketchAddRequest) returns (SketchAddResponse);
rpc SketchQuery(SketchQueryRequest) returns (SketchQueryResponse);
rpc SketchMerge(SketchMergeRequest) returns (SketchMergeResponse);

enum SketchKind {
  SKETCH_KIND_UNSPECIFIED = 0;
  SKETCH_KIND_DISTINCT = 1;       // HyperLogLog (F1)
  SKETCH_KIND_FREQUENCY = 2;      // Count-Min (F2)
  SKETCH_KIND_TOP_K = 3;          // Space-Saving (F3)
  SKETCH_KIND_QUANTILES = 4;      // DDSketch (F4)
}
message SketchParams {
  SketchKind kind = 1;
  uint32 precision = 2;           // DISTINCT: p, 4..18 (default 14: 16 KB, 0.81%)
  uint32 width = 3;               // FREQUENCY: columns (default 2^16)
  uint32 depth = 4;               // FREQUENCY: rows (default 4)
  uint32 k = 5;                   // TOP_K: items tracked (default 100, at most 10,000)
  double relative_accuracy = 6;   // QUANTILES: alpha (default 0.01)
}
message SketchAddRequest {
  bytes key = 1;
  SketchParams create = 2;        // used when the key does not exist yet
  repeated bytes items = 3;       // DISTINCT, FREQUENCY, TOP_K
  repeated uint64 weights = 4;    // FREQUENCY, TOP_K: per item (default 1)
  repeated double values = 5;     // QUANTILES
  optional uint64 ttl_ms = 6;     // on create
}
message SketchAddResponse { bool created = 1; }
message SketchQueryRequest {
  repeated bytes keys = 1;        // DISTINCT: several = the count of their union
  repeated bytes items = 2;       // FREQUENCY: estimate these
  repeated double quantiles = 3;  // QUANTILES: q in [0, 1]
}
message SketchQueryResponse {
  SketchParams params = 1;
  uint64 distinct = 2;
  repeated uint64 frequencies = 3;
  repeated TopItem top = 4;
  repeated double quantile_values = 5;
  uint64 total_weight = 6;        // FREQUENCY, TOP_K, QUANTILES
}
message TopItem { bytes item = 1; uint64 count = 2; uint64 max_error = 3; }
message SketchMergeRequest { bytes destination = 1; repeated bytes sources = 2; optional uint64 ttl_ms = 3; }
message SketchMergeResponse { bool created = 1; }
```

### F1. CountDistinct (HyperLogLog)

**Why.**
- **Unique counts without storing IDs:** visitors per page, per campaign,
  per day (DAU and MAU).
- **Audience overlap** between campaigns: the union, and intersections by
  inclusion-exclusion.
- **Account sharing:** distinct IPs or devices per account.
- **Cardinality monitoring:** distinct API keys per hour.

**Semantics.**
- **Error:** standard error 1.04/√(2^p): 0.81% at p=14 (16 KB), 1.63% at
  p=12 (4 KB).
- **Small sets:** near-exact below a few hundred items (linear counting).
- **Unions** are exact operations on the sketches.
- **Intersections** are estimated as |A| + |B| − |A ∪ B|, with large
  relative error when the overlap is small.

**How it works.**
- **Layout:**
  - a header (magic, kind, p);
  - a fixed-point harmonic sum (u64, scale 2^48) and a zero-register count;
  - 2^p one-byte registers.
- **Add:**
  1. Hash the item; the hash gives the register and the rank.
  2. Raise the register with a byte CAS when the rank is larger.
  3. The thread that wins the CAS adds the exact difference
     `2^(48−new) − 2^(48−old)` to the sum with `LOCK XADD`, and decrements
     the zero count if the register was 0.

  The estimate is therefore O(1) and in exact integer arithmetic (no
  floating-point drift), even with concurrent writers. Ranks above 48,
  which have probability 2^−48 per item, add nothing.
- **Count:**
  - one sketch: O(1) from the sum and the zero count;
  - several sketches: one fused AVX2 pass of VPMAXUB plus the
    float-exponent harmonic sum (golden-operations §2.5), with a scalar
    twin.

**Cost (M).**
- Add: 0.6 ns per item, plus the key lookup (once per request, shared by
  its items).
- Count: 0.6 ns.
- Union of several keys: 1.06 µs at p=14; 262 ns at p=12.

**Usage.**
```csharp
await kv.DistinctAddAsync($"uv:{page}:{day}", visitorId);                         // on every view
long uv = await kv.DistinctCountAsync($"uv:{page}:{day}");
long weekUv = await kv.DistinctCountAsync(Last7Days(day).Select(d => $"uv:{page}:{d}"));   // union
await kv.SketchMergeAsync($"uv:{page}:{month}", DaysOf(month).Select(d => $"uv:{page}:{d}"));
```

**Pitfalls.** Approximate by design. Never use it for billing.
Intersections of small sets are unreliable.

### F2. Frequency (Count-Min)

**Why.**
- **Abuse and fraud signals:** how often an IP, API key, card or search
  term was seen.
- **Heavy users** for billing tiers.
- **Popularity** per item without one counter per item.

**Semantics.** Never under-counts. It over-counts by at most ε·N with
probability 1 − δ, where ε = e/width, δ = e^−depth and N is the total
weight.

**How it works.**
- `depth` rows of `width` 32-bit counters.
- An add is `depth` `LOCK XADD`s, positioned by double hashing from one
  SipHash.
- A query is the minimum over the rows; AVX2 serves multi-item queries.

**Cost.**
- 21 ns per item batched (4 rows of 2^21 counters, 32 MB, M); 111 ns
  unbatched with hashing.
- A 1 MB sketch (4 × 2^16) stays in cache and is faster (E).

**Usage.**
```csharp
await kv.FrequencyAddAsync($"freq:login-ip:{day}", ip);
if (await kv.FrequencyAsync($"freq:login-ip:{day}", ip) > 1_000) Flag(ip);
```

### F3. TopK

**Why.**
- **Trends:** trending products, searches and hashtags.
- **Capacity planning:** hot keys.
- **Abuse:** the top abusers by request volume.

**Semantics.** Space-Saving with k counters: every item whose frequency
exceeds N/k is in the list, and each count has a known maximum error.

**How it works.**
- k counters, a small hash index and a min-heap, inside the value.
- A one-byte per-sketch spinlock (`LOCK BTS`, PAUSE while busy) serializes
  updates to that sketch; reads copy under it.

**Cost (E).** An add is ≈ 50-100 ns (a hash probe and a heap fix);
reading the top 100 is ≈ 1-2 µs.

**Measured since (V, [golden-services.md](golden-services.md) H7).**
- **The sketch:** HeavyKeeper (2 × 4,096 buckets, 64 KiB) with a 100-entry
  heap whose membership test is 13 AVX2 compares.
- **Cost:** 16.7 ns per add on a Zipf(1) stream over 10M keys.
- **Accuracy:** all 100 reported keys were truly top-100, with a 0.1%
  count error.
- **What this leaves open:** Space-Saving's per-item error bounds remain
  the reason to prefer it (golden-services §10).

**Usage.**
```csharp
await kv.TopKAddAsync($"top:viewed:{hour}", productId);
var trending = await kv.TopKAsync($"top:viewed:{hour}");     // items with counts and error bounds
```

**Pitfalls.** One very hot TopK sketch serializes its updaters. Above
~10M updates per second, shard it (by worker or hash) and merge on read:
Space-Saving summaries are mergeable.

### F4. Quantiles (DDSketch)

**Why.**
- **SLO dashboards:** p50 and p99 latency per endpoint.
- **Distributions:** order values, price percentiles, value sizes.

**Semantics.** Relative error ≤ α for every quantile (DDSketch, VLDB
2019, C). Sketches merge exactly.

**How it works.**
- Logarithmic buckets, with γ = (1 + α)/(1 − α), as 32- or 64-bit
  counters updated with `LOCK XADD`.
- A query scans the bucket counts with AVX2 prefix sums.

**Cost (E, C).** An add is one `XADD` after a logarithm (computed as an
exponent extraction); a query is O(buckets), a few hundred: ≈ µs.

**Measured since (V, [golden-services.md](golden-services.md) H8).** With
α = 1% and 2,048 buckets:

| Operation | Cost |
|---|---|
| add (glibc `log` included) | 16.7 ns |
| query, walking every bucket | 102.2 ns |
| query, through per-64-bucket group counts kept on add | 16.2 ns |

- **Accuracy:** the worst error at p50, p90, p99 and p99.9 was 0.19%.

**Usage.**
```csharp
await kv.QuantileAddAsync($"lat:checkout:{minute}", elapsedMs);
var (p50, p99) = await kv.QuantilesAsync($"lat:checkout:{minute}", 0.5, 0.99);
```

## Group G — Change streams, sync and operations

### G1. Watch

**Why.**
- **Cache invalidation across services:** each service keeps a local cache
  and drops entries on change events (R11).
- **Live UIs:** dashboards and collaborative screens.
- **Event-driven workflows:** order status changed → notify, ship, bill.
- **Fleet configuration:** push config and feature flags instead of
  polling.
- **Change data capture** into search indexes and warehouses.

**API and semantics.** Defined in export-watch.md: the API in §2, the
semantics in §3.3, the history in §6.
- Events arrive in revision order with no gaps.
- `start_revision` resumes a watch.
- `initial_state` lists the matching keys first, then streams events
  (list-then-watch).
- A watcher that falls behind the retained history gets `OUT_OF_RANGE`,
  and then resumes or re-exports.

**What this plan adds to export-watch.md.**
- **New event types:**
  - `EVENT_TYPE_EXPIRE`: TTL removal, distinct from DELETE. Unlike DELETE
    it carries the expired value (unless `keys_only`), because nobody can
    read it any more and expiry handlers usually need it (R4). The
    history ring already holds removed records by revision
    (export-watch.md §6.3), so this costs no copy.
  - `EVENT_TYPE_ADD`: a counter delta plus the value after (§2.3 rule 3).
- **Atomic batches** share one revision (§2.1).
- **Hot counters** emit coalesced ADD events per tick (B4).
- **Sketches and rate-limit state** emit nothing.

**Cost.**

| Who | Cost |
|---|---|
| each write | +3-5 ns for the revision and its history slot (S) |
| routing | ~20-40 ns per write through one union DFA of all watch filters (X, E) |
| delivery | O(1) per event |

**Usage.**
```csharp
await foreach (var ev in kv.WatchAsync(prefix: "cfg:", initialState: true))
    config.Apply(ev);                     // the current values first, then every change, in order
```

### G2. ChangesSince

**Why.**
- **Offline-first apps:** sync what changed since the last session.
- **Replica catch-up.**
- **Incremental ETL** and audit trails.

**API.** Watch with `start_revision` = the last revision the client
processed + 1.
- A Watch never ends by itself. "Changes since R, up to now" stops at the
  first `Progress` message at or beyond the revision current when the call
  began. The .NET client's `ChangesSinceAsync` does exactly that.

**Semantics.**
- Complete back to the retained history (`KVNODE_WATCH_HISTORY`).
- Older than that, `OUT_OF_RANGE`: fall back to Export (G3), or later to
  Reconcile (G4).

**Usage.**
```csharp
var (changes, upTo) = await kv.ChangesSinceAsync(prefix: $"u:{uid}:", revision: device.LastRevision);
Apply(changes); device.LastRevision = upTo;
```

### G3. Export

**Why.**
- **Backups** and warehouse ETL.
- **Migrations** and re-sharding.
- **GDPR data-access requests:** everything under `u:<id>:` as one
  consistent snapshot.
- **Point-in-time analytics.**

**API and semantics.** export-watch.md §2, §3.2 and §5: a point-in-time
cut, streamed, with filters, `keys_only` and `count_only`.

**Cost (X).**
- Starting a cut is O(1).
- Emitting costs 25-40 ns per key: 10M keys ≈ 0.25-0.4 s of one worker's
  CPU, bound by the network.
- A writer's first touch of a bucket page during an export pays
  0.2-1.4 µs.

**This plan adds.**
- Counters exact at the cut (§2.3 rule 4).
- Keys expired at the cut's time are excluded.
- With the ordered index, a prefix Export visits only the prefix: O(k)
  instead of a scan.

### G4. Reconcile

**Why.** Sync edge caches, mobile clients or replicas by their difference
only, when:
- they were away longer than the history retains (ChangesSince →
  `OUT_OF_RANGE`), and
- a full Export would move the whole data set.

**How it works (research).**
- **Maintained tables:** an invertible Bloom lookup table over (key hash,
  value hash) per registered scope, updated on every write with a few XORs
  per scope.
- **Decoding:** a client subtracts its own table and decodes the
  difference.
- **Rateless IBLTs** (SIGCOMM 2024, C) stream coded symbols until the
  difference decodes, so neither side has to guess its size.

**Cost (C, E).** O(difference) to transfer and decode; a few XORs per
write per maintained scope.

**Plan.** A separate proposal, after Watch ships.

### G5. StatsByPrefix

**Why.**
- **Billing:** per-tenant usage (keys and bytes stored), chargeback.
- **Capacity planning** per namespace.
- **Quota reports.**

**API.** `Stats` with a prefix.
- **Exact and O(1)** for delimiter-aligned prefixes with directories
  (`KVNODE_INDEX_DELIMITERS`, §2.6).
- **Other prefixes:** the count comes from the ordered index (D2); bytes
  need a per-node byte sum (an open measurement) or a scan.

**Cost (K).** O(1). Directories cost +221 / +321 ns per delete / create
and 110 bytes per key.

**Usage.**
```csharp
var usage = await kv.StatsAsync(prefix: $"t:{tenant}:");    // keys and value bytes for the invoice
```

## Group H — More golden services

**Where these come from.**
- **Added 2026-10-01** from [golden-services.md](golden-services.md). That
  document holds each function's measurement, layout and thread-safety
  argument.
- **This section** holds what the other groups hold: why, API, semantics,
  cost, usage and pitfalls.
- **Numbering:** golden-services.md's numbers are kept. Its H7 and H8 are
  F3 and F4 here, so this group lists H1-H6, H9 and H10.
- **Costs** are measured by the services probe at 10M keys (V). Its bars
  that day were D 435.5 / I 406.0 / B 57.3 ns: a slower day for latency
  than the plan's 347 ns. Ratios within that run are what carry over.

**Shared rules.**
- **Typed values, like sketches (§1.6).** Structs, bitmaps, sets, windows
  and vectors are values with a 4-byte magic and a documented layout:
  - created by their first write, or by a Put of the encoded bytes;
  - deleted with Delete;
  - expirable with a TTL;
  - readable with Get.

  The wrong type is `FAILED_PRECONDITION`, or a per-item outcome in
  batches (§1.3).
- **In place where one atomic instruction does it (§2.3):**
  - a field's `LOCK XADD` or aligned 8-byte store;
  - a bit's `LOCK BTS` or `LOCK BTR`;
  - a window bucket's `LOCK XADD`.

  Everything else goes through the writer path as a replace with a new
  version: adding a field, changing a set.
- **Exports and Watch.** In-place changes to struct fields and bitmaps
  follow §2.3 rule 3 (events) and rule 4b (undo records):
  - each change takes a revision when Watch is on;
  - it logs what it changed: the field's old 8 bytes, or the bit's old
    value;
  - an export undoes the changes logged after its cut, as counters
    subtract their deltas.

  Windows, like rate-limit state, export as read.
- **Indexed attributes ride in the value.** Under a configured prefix:
  - a near-duplicate key's value starts with its 64-bit fingerprint;
  - a geo key's value starts with its point.

  Put and Delete then maintain the index through the existing write hooks,
  snapshots need no change, and the index is rebuilt at load (§V.13
  decision 11).

### H1. Compact keys (counters, flags and small values at one cache miss)

**Why.**
- **Most hot keys are small:**
  - counters: views, likes, quotas, stock;
  - flags: feature switches, "verified", "seen";
  - small ids: short link → target, session → user;
  - timestamps: last seen, cooldowns.
- **The store spends a second dependent miss on them:** the bucket, then
  the record. For an 8-byte value that miss is all overhead. Compact keys
  keep key and value inside the bucket's cache line.

**API.** No new RPC: a configured key space.
```text
KVNODE_COMPACT_PREFIXES=c:      # keys under these prefixes live in the compact table
```
- **These work on compact keys as on any key:** Get, GetMany, Exists,
  Increment, and Mutate (put, delete, and the conditions `absent`,
  `present` and `value_equals`).
- **Limits:** a compact key is at most 16 bytes and its value at most 8
  bytes. Anything else is `INVALID_ARGUMENT`, with
  `reason = "compact-limit"`.
- **A key never moves between tables,** so a miss costs one probe.

**Semantics.**
- **No versions.** An 8-byte version per entry would halve the entries per
  line. `condition.version` on a compact key is `INVALID_ARGUMENT`;
  compare values instead, which for ≤ 8 bytes is one `LOCK CMPXCHG`.
- **No TTL:** there is no room for a deadline, so `ttl_ms` on a compact key
  is `INVALID_ARGUMENT`. Use a time-bucketed key (`c:v:<day>…`) or an
  ordinary key.
- **Point access only.** Compact keys are outside the ordered, fuzzy and
  substring indexes, so Scan, Count and the search functions do not see
  them. Watch and Export do: their writes take revisions like any write.
- **A Put that changes the length is two stores,** the length byte first.
  A racing reader can glimpse the new length with the old value.
  Fixed-length values (counters, timestamps) never see it.

**How it works.**
- **The table:** a separate cuckoo table of 64-byte buckets. Each holds a
  seqlock version, two tags, lengths, and two entries of (16-byte key,
  8-byte value).
- **Lookups** use the batched pipeline: prefetch both lines, then compare
  the key with SSE2.
- **Increments** are `LOCK XADD` in the bucket.
- **Structural changes** take the writer lock and bump bucket versions:
  creates, deletes and displacements.
- **Nothing is malloc'd,** so nothing is reclaimed.

Design and thread safety: golden-services.md H1.

**Cost (V, 16-byte keys, 8-byte values).**

| | The store, same keys | Compact |
|---|---|---|
| get, alone (dependent chain) | 349.2 ns | **189.9 ns** (0.54×) |
| get, batched | 53.1 ns | 37.2 ns; **28.1 ns** with 8-lane hashing (Track S, §V.10) |
| increment, batched | 58.1 ns | **39.8 ns** (0.68×) |
| memory per key | 61 B | 54 B |

**Usage.**
```csharp
// server: KVNODE_COMPACT_PREFIXES=c:
await kv.IncrementAsync($"c:views:{postId:x8}");                 // 16 bytes: "c:views:" + 8 hex digits
await kv.PutAsync($"c:seen:{userId:x8}", BitConverter.GetBytes(nowUnixMs));
var flag = await kv.GetAsync($"c:ff:{accountId:x8}");             // a feature flag at one cache miss
```

**Pitfalls.**
- **A typed key space.** A prefix chosen for compact keys cannot later
  hold a 9-byte value or a 17-byte key.
- **No versions, TTLs or ordered queries.** Compact keys are for hot point
  data; documents stay ordinary keys.
- **Load.** Two-way buckets of two need headroom. The table grows (a
  rehash under the writer lock) at 60% entry load.

### H2. Struct fields (HGET, HSET, HINCRBY)

**Why.**
- **Profiles, sessions, carts and game state** are small records with
  named fields. Today a client reads the whole value, edits it and writes
  it back:
  - two round trips;
  - the whole value on the wire, both ways;
  - a lost update unless it loops on CompareAndSet.
- **Field-level calls** move the edit to the server, atomically, for 8
  bytes.

**API.**
```proto
rpc Fields(FieldsRequest) returns (FieldsResponse);
message FieldsRequest { repeated FieldOp ops = 1; }   // 1 .. KVNODE_MAX_WRITE_BATCH
message FieldOp {
  bytes key = 1;
  FieldOpKind kind = 2;
  repeated string names = 3;       // GET: the fields; SET, INCR, DEL: exactly one
  bytes value = 4;                 // SET: 0..8 bytes
  sint64 delta = 5;                // INCR
  bool create = 6;                 // SET, INCR: create an empty struct when the key is absent
  optional uint64 ttl_ms = 7;      // only when this op creates the struct
}
enum FieldOpKind { FIELD_OP_UNSPECIFIED = 0; FIELD_GET = 1; FIELD_SET = 2; FIELD_INCR = 3; FIELD_DEL = 4; }
message FieldsResponse { repeated FieldResult results = 1; }
message FieldResult {
  FieldOutcome outcome = 1;
  repeated bytes values = 2;       // GET: one per name; empty when the field is absent
  sint64 number = 3;               // INCR: the value after
}
enum FieldOutcome {
  FIELD_OUTCOME_UNSPECIFIED = 0;
  FIELD_OUTCOME_APPLIED = 1;
  FIELD_OUTCOME_NO_KEY = 2;        // absent, and create was not set
  FIELD_OUTCOME_NO_FIELD = 3;      // DEL of an absent field
  FIELD_OUTCOME_NOT_A_STRUCT = 4;
  FIELD_OUTCOME_FULL = 5;          // SET of a new field with every slot taken
  FIELD_OUTCOME_NOT_A_NUMBER = 6;  // INCR of a field that is not 8 bytes
  FIELD_OUTCOME_OVERFLOW = 7;
}
```

**Semantics.**
- **A struct** holds up to 16 fields (32 with an AVX2 tag compare: an open
  decision).
  - Names are ≤ 15 bytes.
  - Values are ≤ 8 bytes: int64, float64, or a short string. Longer data
    goes in its own key.
- **Fields never move once placed,** so every field operation is one
  atomic step, linearizable per field:

  | Op | Instruction |
  |---|---|
  | SET of an existing field | an aligned 8-byte store |
  | INCR | `LOCK XADD`, exact under any contention, as counters |
  | DEL | clears the tag |
  | SET of a new field | claims a free slot under the writer lock and publishes its tag last, so readers see it appear whole |

  A freed slot is reused only after a grace period.
- **The struct as a whole:**
  - Get returns its encoded bytes.
  - A Put replaces it, and wins over racing field updates (§2.3 rule 2).
  - Growth beyond 16 slots is a Put of a bigger struct.
- **Versions** change on structural changes (a new field, a Put), not on
  in-place SET or INCR (§2.3 rule 5).

**How it works.**
- **Layout:** a 56-byte directory leads the value: 16 one-byte tags (the
  name's CRC32C, via SSE4.2), then 16 offsets. Each field record holds its
  name and its 8-byte value together.
- **Lookup:** one `PCMPEQB` + `PMOVMSKB` finds the candidate, and one name
  compare confirms it.
- **Batches:** two extra prefetch stages (the directory, then the field)
  keep 32 lookups in flight.

Design: golden-services.md H2.

**Cost (V, 1M structs of 440 bytes).**

| | Time | vs the whole value |
|---|---|---|
| Get the whole value | 102.0 ns | 1× |
| FIELD_GET of one field | **59.6 ns** | 0.58×; 8 bytes on the wire instead of 440 |
| FIELD_INCR | **62.5 ns** | 0.61× (exact, checked) |

**Usage.**
```csharp
long coins = await kv.IncrementFieldAsync("player:42", "coins", +50, create: true);
await kv.SetFieldAsync("player:42", "level", 7L);
var (plan, verified) = await kv.GetFieldsAsync<string, bool>("profile:9", "plan", "verified");
await kv.DeleteFieldAsync("cart:9", "coupon");
var whole = await kv.GetStructAsync("profile:9");                    // every field, decoded
```

**Pitfalls.**
- **Fixed slots:** a 17th field is `FULL` until a Put grows the struct.
- **8-byte fields:** names, emails or JSON belong in their own keys.
- **A Put races field updates and wins.** Don't mix whole-value writes
  with field increments unless that is the intent.

### H3. Bitmaps (SETBIT, GETBIT, BITCOUNT, BITPOS)

**Why.**
- **One bit per user number per day:** daily actives, feature exposure,
  announcements seen, check-ins and streaks.
- **First-time detection:** a SET that returns the previous bit says
  "first visit" in the same call.
- **Allocation:** the first free seat, slot or id.
- **Cohorts:** how many of users 0..999,999 were active.

**API.**
```proto
rpc Bits(BitsRequest) returns (BitsResponse);
message BitsRequest { repeated BitOp ops = 1; }      // 1 .. KVNODE_MAX_WRITE_BATCH
message BitOp {
  bytes key = 1;
  BitOpKind kind = 2;
  uint64 bit = 3;                  // SET, CLEAR, GET; POS: where to start
  optional uint64 start_byte = 4;  // COUNT: the range [start, end); unset = the whole bitmap
  optional uint64 end_byte = 5;
  bool want = 6;                   // POS: find a 1 (true) or a 0 (false)
  uint64 create_bits = 7;          // SET, CLEAR: the size when this op creates the bitmap
  optional uint64 ttl_ms = 8;      // on create
}
enum BitOpKind { BIT_OP_UNSPECIFIED = 0; BIT_SET = 1; BIT_CLEAR = 2; BIT_GET = 3; BIT_COUNT = 4; BIT_POS = 5; }
message BitsResponse { repeated BitResult results = 1; }
message BitResult {
  BitOutcome outcome = 1;          // APPLIED, NO_KEY, NOT_A_BITMAP, OUT_OF_RANGE
  bool was = 2;                    // SET, CLEAR: the bit before; GET: the bit
  uint64 number = 3;               // COUNT: the count; POS: the position (the size when none)
}
```

**Semantics.**
- **A fixed size,** set at creation:
  - rounded up to 4 KiB;
  - at most `KVNODE_MAX_BITMAP_MB` (default 128: about 10^9 bits);
  - a bit beyond it is `OUT_OF_RANGE`.
- **SET and CLEAR** return the previous bit, and are linearizable per bit.
- **COUNT** relies on per-4 KiB counts and a total, updated only when a
  SET or CLEAR flips a bit.
  - **The whole bitmap:** COUNT reads the total, one load.
  - **A range:** it adds the full blocks' counts and scans at most half a
    block per edge.
  - **Under concurrent flips,** a COUNT may be off by the flips in flight,
    never more (an open decision).
- **POS** skips zero blocks with `VPTEST`. It could also skip empty 4 KiB
  blocks by their counts.

**How it works.**
- **Flips:** `LOCK BTS` and `LOCK BTR` report whether this call flipped
  the bit. Only a flip pays two `LOCK XADD`s: the block count and the
  total.
- **Popcounts:** counts use the AVX2 nibble popcount, and long scans
  Harley-Seal at 82 GB/s, each with a `POPCNT` twin.

Design: golden-services.md H3.

**Cost (V, 1 MiB = 8.4M bits; add the key's lookup, about one batched
get).**

| Op | Time |
|---|---|
| SET, counts kept | **10.1 ns** (3.3 ns without counts) |
| GET | 0.6 ns |
| POS, sparse | **92.1 ns** (221.3 scalar) |
| COUNT of a range (average 341 KiB) | **67.6 ns** (a scan: 7.44 µs) |
| COUNT of the whole bitmap | one load (a scan: 12.8 µs) |

**Usage.**
```csharp
bool seenBefore = await kv.SetBitAsync($"active:{day}", userNo, createBits: 16_777_216);
if (!seenBefore) await OnFirstVisitTodayAsync(userNo);
long dau = await kv.CountBitsAsync($"active:{day}");
long cohort = await kv.CountBitsAsync($"active:{day}", startByte: 0, endByte: 125_000);   // users 0..999,999
long seat = await kv.FindBitAsync($"taken:{showId}", value: false);                         // the first free seat
```

**Pitfalls.**
- **Dense integer ids only.** Map users to numbers 0..n; for arbitrary
  strings, use CountDistinct (F1).
- **Sparse sets waste memory,** one bit per possible id. Roaring
  containers are the way out (golden-services §6).
- **AND or OR of two bitmaps is a full scan** (17.9 µs per MiB pair). That
  is fine for a dashboard, within the synchronous budget, but it is not a
  golden call (R19).

### H4. Small sets (SADD, SREM, SISMEMBER, SCARD, SINTERCARD)

**Why.**
- **Authorization:** is the user in this group or role?
- **Entitlements and tags:** an account's features, an item's categories.
- **Rooms and teams:** "is X a member".
- **Overlap:** mutual friends, shared interests.

**API.**
```proto
rpc Sets(SetsRequest) returns (SetsResponse);
message SetsRequest { repeated SetOp ops = 1; }      // 1 .. KVNODE_MAX_WRITE_BATCH
message SetOp {
  bytes key = 1;
  SetOpKind kind = 2;
  repeated uint64 members = 3;     // ADD, REMOVE, IS_MEMBER
  bytes other_key = 4;             // INTER_CARD
  optional uint64 ttl_ms = 5;      // ADD: on create
}
enum SetOpKind { SET_OP_UNSPECIFIED = 0; SET_ADD = 1; SET_REMOVE = 2; SET_IS_MEMBER = 3; SET_CARD = 4; SET_INTER_CARD = 5; }
message SetsResponse { repeated SetResult results = 1; }
message SetResult {
  SetOutcome outcome = 1;          // APPLIED, NO_KEY, NOT_A_SET, TOO_LARGE
  repeated bool member = 2;        // IS_MEMBER: one per member asked
  uint64 number = 3;               // ADD, REMOVE: members changed; CARD; INTER_CARD
}
```

**Semantics.**
- **Members are unsigned 64-bit ids,** at most `KVNODE_MAX_SET_MEMBERS`
  (default 65,536) per set.
- **ADD and REMOVE** write a new sorted array through the writer path: a
  replace, with a new version. Concurrent ADDs to one set serialize there.
- **Reads** see one immutable version of each set, so IS_MEMBER, CARD and
  INTER_CARD are consistent snapshots.

**How it works.**
- **The value:** a sorted array, with its count in the header.
- **IS_MEMBER** scans with AVX2 up to about 256 members, 8 per
  `VPCMPEQD`, and uses a branchless binary search above that.
- **INTER_CARD** compares 8 × 8 blocks with `VPCMPEQD` over `VPERMD`
  rotations.

Design: golden-services.md H4.

**Cost (V, after the lookup).**

| Op | Time |
|---|---|
| IS_MEMBER, 64 members | **2.1 ns** (15.5 ns branchless search) |
| IS_MEMBER, 1,024 members | 32.8 ns (the search ties the scan) |
| INTER_CARD, 1,024 × 1,024 | **552.8 ns** (1.64 µs merge) |

**Usage.**
```csharp
await kv.SetAddAsync($"groups:{userId}", AdminGroupId);
bool isAdmin = await kv.SetContainsAsync($"groups:{userId}", AdminGroupId);
long mutual = await kv.SetIntersectCountAsync($"friends:{a}", $"friends:{b}");
long size = await kv.SetCountAsync($"members:{roomId}");
```

**Pitfalls.**
- **ADD copies the set:** O(n) per write. Large or write-heavy sets belong
  in bitmaps (H3), or in one key per member (ordered queries).
- **Ids, not strings:** map strings to ids first.

### H5. FindNear (near-duplicates)

**Why.**
- **Marketplaces:** listings re-posted with small edits.
- **Support and bug trackers:** "similar tickets already exist".
- **Spam and fraud:** campaigns of near-identical messages, reviews or
  bios.
- **Moderation:** re-uploads of a banned image (a 64-bit perceptual hash).
- **Aggregation and crawling:** near-identical articles and pages.

**API.**
```proto
rpc FindNear(FindNearRequest) returns (FindNearResponse);
message FindNearRequest {
  bytes prefix = 1;                // a prefix listed in KVNODE_NEAR_PREFIXES
  fixed64 fingerprint = 2;         // a 64-bit SimHash or perceptual hash, computed by the client
  uint32 max_distance = 3;         // 0 .. 3 bits
  uint32 limit = 4;                // 1 .. 1,000
  bool with_values = 5;
}
message FindNearResponse { repeated NearMatch matches = 1; bool truncated = 2; }
message NearMatch { bytes key = 1; uint32 distance = 2; bytes value = 3; }
```
- **Fingerprints ride in the value.** Under a near-duplicate prefix, a
  value starts with its 8-byte fingerprint (little-endian). A Put sets or
  changes it; a Delete removes it.

**Semantics.**
- **Exact:** every key under the prefix within `max_distance` bits, and
  only those; nearest first; each key once.
- **Read-your-writes:** a Put is visible to FindNear when it returns.

**How it works.**
- **Index:** multi-index hashing, with four tables keyed by the code's
  four 16-bit blocks. Any code within 3 bits shares a block with the
  query (pigeonhole), so four posting lists hold every answer: 612
  candidates at 10M.
- **Filter:** an AVX2 XOR + popcount + compare checks four codes per
  step.
- **Live updates** go to a small delta beside the static base, merged by
  the background builder (§2.7).

Design: golden-services.md H5.

**Cost (V, 10M random fingerprints).**
- **695 ns** with AVX2, 890 ns scalar; brute force over 10M takes 10.8 ms.
- Memory: 48 B per indexed key.

**Usage.**
```csharp
ulong fp = SimHash.Of(Shingles(listing.Title + " " + listing.Body));
var dups = await kv.FindNearAsync("listing:", fp, maxDistance: 3, limit: 10);
if (dups.Count == 0) await kv.PutWithFingerprintAsync($"listing:{id}", fp, listing.Encode());
```

**Pitfalls.**
- **Real fingerprints cluster,** and popular block values lengthen their
  lists. Measure on real data before relying on the budget (§V.9).
- **Distance > 3** needs more, narrower blocks and slows quickly; it is
  not offered.
- **The fingerprint's quality is the client's:** SimHash over shingles for
  text, a perceptual hash for images.

### H6. GeoAdd / GeoRadius

**Why.**
- **Dispatch:** the nearest free drivers or couriers.
- **Locators:** stores, branches, ATMs.
- **Social:** people nearby.
- **Geofencing and coverage:** which zones are near a vehicle; is any
  warehouse within 5 km?

**API.**
```proto
rpc GeoAdd(GeoAddRequest) returns (GeoAddResponse);
rpc GeoRadius(GeoRadiusRequest) returns (GeoRadiusResponse);
message GeoAddRequest { bytes index = 1; repeated GeoPoint points = 2; }      // 1 .. KVNODE_MAX_WRITE_BATCH
message GeoPoint { bytes member = 1; double lat = 2; double lon = 3; }
message GeoAddResponse { uint32 created = 1; uint32 moved = 2; }
message GeoRadiusRequest {
  bytes index = 1;                 // a prefix listed in KVNODE_GEO_INDEXES
  double lat = 2; double lon = 3;
  double radius_m = 4;             // at most KVNODE_GEO_MAX_RADIUS_M (default 50 km)
  uint32 limit = 5;                // 1 .. 10,000
  bool sorted = 6;                 // nearest first
  bytes cursor = 7;                // continues a truncated answer
}
message GeoRadiusResponse { repeated GeoHit hits = 1; bytes cursor = 2; }
message GeoHit { bytes member = 1; double distance_m = 2; }
```
- **Members are keys.** `GeoAdd(index, member, lat, lon)` writes the key
  `<index><member>` with the point as its value (two little-endian
  doubles).
  - A Put may append a payload after the point.
  - Delete removes the member; Get reads it.

**Semantics.**
- **Every member within `radius_m`,** by the equirectangular distance
  (well under 0.1% error at city scale), each once.
- **Read-your-writes,** as FindNear.

**How it works.**
- **Codes:** points become 64-bit Morton codes (two `PDEP`), kept sorted,
  with a 2^20-entry prefix table for seeks (Track S, P5).
- **A query** covers its box with at most four Morton ranges, seeks them
  together, and scans each with a `PEXT` box test and the exact distance.
- **Writes** go to a delta, merged by the background builder.

Design: golden-services.md H6.

**Cost (V, 10M uniform points over 445 × 190 km).**

| Radius | Results | Time |
|---|---|---|
| 200 m | 15 | **697.5 ns** |
| 1,000 m | 371 | 6.18 µs (output-bound: 16.6 ns per result) |

Memory: 28 B per point, plus 4 MiB.

**Usage.**
```csharp
await kv.GeoAddAsync("drivers:", driverId, lat, lon);                          // every few seconds
var near = await kv.GeoRadiusAsync("drivers:", riderLat, riderLon, radiusMeters: 800, limit: 20, sorted: true);
```

**Pitfalls.**
- **Real points cluster.** A city center holds far more points per cell
  than uniform data, and the cost tracks the codes scanned.
- **Large radii are output-bound:** page with `limit` and `cursor`.
- **Moving points write constantly.** Each GeoAdd is a Put; the delta
  absorbs it until the next merge.

### H9. Sliding windows

**Why.**
- **Rolling counts with thresholds:** "lock the account after 5 failed
  logins in 15 minutes".
- **Live metrics:** requests in the last minute per endpoint or tenant; a
  60-second sparkline.
- **Spike detection:** the last minute against a baseline.
- **Not for throttling alone.** RateLimit (B3) is already smooth (GCRA).
  Use a window when you need the count itself.

**API.**
```proto
rpc WindowAdd(WindowAddRequest) returns (WindowAddResponse);
message WindowAddRequest { repeated WindowUpdate updates = 1; }    // 1 .. KVNODE_MAX_WRITE_BATCH
message WindowUpdate {
  bytes key = 1;
  sint64 delta = 2;                // 0 = read
  uint32 buckets = 3;              // on create: 1 .. 256 (default 60)
  uint32 bucket_ms = 4;            // on create (default 1000)
  bool with_buckets = 5;           // return every bucket (a sparkline)
  optional uint64 ttl_ms = 6;      // on create
}
message WindowAddResponse { repeated WindowResult results = 1; }
message WindowResult { bool applied = 1; sint64 sum = 2; repeated sint64 buckets = 3; }
```

**Semantics.**
- **The sum** covers the last `buckets × bucket_ms` of server time, at
  bucket granularity.
- **A sum is a snapshot,** within ± the adds in flight.

**How it works.**
- **The value:** a ring of buckets, and the epoch of the newest.
- **An add** zeroes a stale bucket with one `LOCK CMPXCHG16B` on (epoch,
  count), then `LOCK XADD`s it.
- **The sum** widens and adds eight buckets per AVX2 step.

Design: golden-services.md H9.

**Cost (V, E).**
- **The sum** of 64 buckets: **4.5 ns** (17.2 ns scalar).
- **An add** costs about one in-place increment (58 ns batched, M).

**Usage.**
```csharp
long fails = await kv.WindowAddAsync($"fail:{user}", +1, buckets: 15, bucketMs: 60_000);
if (fails >= 5) await LockAccountAsync(user);
long lastMinute = await kv.WindowSumAsync($"req:{endpoint}");
```

**Pitfalls.** The resolution is one bucket: a 60 × 1 s window answers
"the last 59-60 seconds".

### H10. Score (similarity of stored vectors)

**Why.**
- **Recommendations:** re-rank a few hundred candidates by embedding
  similarity.
- **Ranking:** personalized feeds; semantic re-ranking of keyword results.
- **Matching:** jobs and candidates, buyers and products.
- **Duplicate grading:** a MinHash Jaccard estimate for FindNear's matches.

**API.**
```proto
rpc Score(ScoreRequest) returns (ScoreResponse);
message ScoreRequest {
  ScoreKind kind = 1;              // INT8_DOT, FP32_DOT, MINHASH
  bytes query = 2;                 // the query vector or signature, in the kind's encoding
  repeated bytes keys = 3;         // 1 .. KVNODE_MAX_READ_BATCH
}
message ScoreResponse { repeated ScoreResult results = 1; }
message ScoreResult { ScoreOutcome outcome = 1; double score = 2; }     // OK, NO_KEY, MISMATCH
```
- **Vector values are typed:** a header with the element type (int8,
  fp32, u32 MinHash) and the dimension, then the elements. A query of
  another shape is `MISMATCH`.

**How it works.** The batched GET pipeline, then a kernel per key. Design:
golden-services.md H10.

**Cost (V, after the lookup; 256 dimensions or 128 hashes).**

| Kernel | Scalar | AVX2 | AVX-VNNI |
|---|---|---|---|
| int8 dot | 61.3 ns | 3.2 ns | **1.5 ns** |
| fp32 dot | 71.9 ns | **5.2 ns** | — |
| MinHash agreement | 42.9 ns | **5.6 ns** | — |

A score therefore costs about one batched get per key.

**Usage.**
```csharp
var scores = await kv.ScoreAsync(userEmbedding, candidateKeys, ScoreKind.Int8Dot);
var top = candidateKeys.Zip(scores).OrderByDescending(p => p.Second).Take(20);
```

**Pitfalls.**
- **Not nearest-neighbor search.** The caller brings the candidates, from
  Scan, FindNear, TopK or a search engine. Searching all vectors stays out
  of scope (I.3).
- **int8 quantization is the client's:** scale each vector into
  −127..127.

---

# Part IV — Recipes

Each recipe states the **problem**, the **keys**, the **calls**, what is
**guaranteed**, and what to **watch out** for.

### R1. Idempotent APIs (payments, webhooks)

- **Problem:** a client retries a payment request after a timeout. It must
  be charged once, and every retry must see the same answer.
- **Keys:** `idem:<request id>` → `{status: pending|done, owner, response}`,
  with a TTL of 24 h.
- **Calls:**
  1. `PutIfAbsent(idem:id, pending(owner = me), ttl 24h, return_previous)`.
  2. If **applied**: do the work, then
     `CompareAndSet(idem:id, done(response), version from step 1)`.
  3. If **not applied** and `previous.status = done`: replay
     `previous.response`.
  4. If **not applied** and `pending` with another owner: answer 409 or
     "retry later". If `pending` with my owner, this is my own retry:
     continue the work.
- **Guaranteed:** exactly one worker does the work per request ID, and
  every retry sees the stored answer.
- **Watch out:**
  - a worker that dies while `pending` blocks the ID until its TTL. A
    shorter "pending" TTL, extended with `touch` while working, bounds
    that.
  - Webhook senders reuse IDs: key on (sender, ID).

### R2. Distributed cron lock and leader election

- **Problem:** only one instance should run the nightly billing job, and a
  standby should take over within seconds when the leader dies.
- **Keys:** `lock:<job>` → owner ID, TTL 30 s.
- **Calls:**
  - `AcquireLease(lock:job, ttl 30s, autoRenew)` (A7). Pass `lease.Token`
    to every downstream write as a fencing token.
  - Standbys `Watch(lock:job)` and try to acquire on DELETE or EXPIRE.
- **Guaranteed:**
  - at most one holder by the server's clock;
  - tokens increase with each acquisition, also across restarts.
- **Watch out:** a paused holder may act after its lease is gone.
  Downstream stores must reject writes with a token older than the newest
  they have seen.

### R3. API metering, rate limits and plan quotas

- **Problem:** limit each API key by its plan, bill usage per day, and
  enforce a monthly quota.
- **Keys:**
  - `rl:api:<key>` (limiter state);
  - `usage:<cust>:<yyyymmdd>` (counter, TTL 40 days);
  - `quota:<cust>:<yyyymm>` (bounded counter: the remaining units);
  - `uv:api:<cust>:<day>` (distinct end users, HLL).
- **Calls, per request:**
  1. `RateLimit(rl:api:key, plan.limit, period, plan.burst)` → 429 when
     refused.
  2. `Increment(quota:cust:month, −cost, min 0)` → 402 when out of bounds.
  3. `Increment(usage:cust:day, +cost)`.
  4. `DistinctAdd(uv:api:cust:day, endUserId)`.

  Batch steps 2-3 in one Increment call.
- **Billing:** GetMany of the month's usage counters; `StatsByPrefix(t:cust:)`
  for storage.
- **Watch out:** a refund must increment the quota back. Keep an audit
  trail of each with an idempotency key (R1).

### R4. Inventory and ticketing without overselling

- **Problem:** sell limited stock. Holds expire if payment does not
  complete.
- **Keys:**
  - `stock:<sku>` (bounded counter);
  - `hold:<order>` → `{sku, qty}`, TTL 10 min.
- **Calls:**
  1. At checkout: `Increment(stock:sku, −qty, min 0)` (OUT_OF_BOUNDS → sold
     out), then `Put(hold:order, {sku, qty}, ttl 10min)`.
  2. Payment success: `Delete(hold:order)`.
  3. A releaser service runs `Watch(prefix hold:)`. On **EXPIRE**, whose
     event carries the expired value `{sku, qty}` (G1), it calls
     `Increment(stock:sku, +qty)`.
- **Guaranteed:** stock never goes negative, and expired holds return
  their units.
- **Watch out:** the release must be idempotent, because a releaser that
  restarts can see an event twice (resuming from `start_revision`).
  Record the release with `PutIfAbsent(released:<order>)` first, and add
  the units back only when that applied.

### R5. Sessions with sliding expiry and "log out everywhere"

- **Keys:** `sess:<user>:<session>` → device info, TTL 30 min.
- **Calls:**
  - **Each request:** Get, then `Expire(… 30 min)` (sliding). That is two
    calls. To save the second, an app that caches the session touches it
    only every few minutes, since it knows when it last did.
  - **List devices:** `Scan(prefix sess:user:)`.
  - **Log out everywhere:** `DeletePrefix(sess:user:)`.
  - **Cap sessions:** `Count(prefix sess:user:) < 10` before creating; the
    race is harmless here.
- **Guaranteed:** a session stops working the instant its TTL passes
  (lazy check).

### R6. Paginated listings with totals

- **Keys:** `order:<cust>:<Fixed(time)>:<id>`.
- **Calls:**
  - **Page:** `Scan(prefix, reverse, limit 50, cursor)`, returning
    `next_cursor` to the browser.
  - **Total:** `Count(prefix)`.
  - **Jump to page N:** `Select(N × 50, within_prefix, from_end)` → start
    the Scan from that key.
- **Guaranteed:** across pages, no duplicates, and no misses for orders
  that existed throughout.
- **Watch out:** cursors are not snapshots. For an export of exactly what
  existed at one moment, use Export.

### R7. Delayed jobs with at-least-once delivery (claims)

- **Keys:**
  - `q:<queue>:ready:<Fixed(due)>:<id>` → payload;
  - `q:<queue>:run:<Fixed(lease deadline)>:<id>` → payload.
- **Calls:**
  1. **Enqueue:** `Put(ready key, payload)`.
  2. **Claim:** `Seek(Ceiling, q:queue:ready:, within_prefix)` finds the
     first due job (stop if its due time > now). Then
     `Mutate(atomic, move ready key → run key with deadline = now + 60s,
     condition present)`. The move succeeds for exactly one worker; the
     others see `CONDITION_FAILED` and claim the next job.
  3. **Ack:** `Delete(run key, condition present)`.
  4. **Recover:** a sweeper Scans `q:queue:run:` up to
     `q:queue:run:<Fixed(now)>`, the claims whose lease deadline has
     passed. It moves each back to `ready` with the same conditional
     atomic move.
- **Guaranteed:** every job runs at least once, and is never run by two
  workers at once unless a lease expires mid-run.
- **Watch out:** handlers must be idempotent (R1). Pop (D5) is the simpler
  at-most-once alternative.

### R8. Leaderboards

- **Keys:** `lb:<board>:<Fixed(long.MaxValue − score)>:<player>` → player
  metadata. The best score sorts first.
- **Calls:**
  - **Score change:** an atomic batch deletes the old key and puts the new
    one.
  - **Top 10:** `Scan(prefix, limit 10)`.
  - **My rank:** `Rank(my key, within_prefix)`.
  - **Around me:** `Select(rank − 5)`, then `Scan` 11 keys.
  - **Board size:** `Count(prefix)`.
- **Watch out:**
  - keep the player's current key in a small `lbme:<board>:<player>` record
    to find it on the next change;
  - ties order by player ID (part of the key).

### R9. Time series: as-of reads and retention

- **Keys:** `ts:<series>:<Fixed(t)>` → a reading, or a block of readings.
- **Calls:**
  - **As of:** `Seek(Floor, ts:series:<t>, within ts:series:)`.
  - **Window:** `Scan(bounds)`.
  - **Count in window:** `Count(bounds)`.
  - **Retention:** `DeleteRange(bounds up to now − 30 days)` in a loop.
  - **Downsampling:** Export a day, then write `ts1h:` keys.
- **Watch out:** one key per reading costs ~160 bytes each. Group readings
  into blocks (one key per minute) when volume is high.

### R10. GDPR: erase and export a user's data

- **Keys:**
  - all of a user's data under `u:<id>:`;
  - pointers from outside (`email:<addr>` → id) listed in
    `u:<id>:manifest`.
- **Calls:**
  - **Access request:** `Export(prefix u:id:)`: one consistent snapshot,
    streamed into a JSON file.
  - **Erasure:**
    1. delete the pointers from the manifest;
    2. `DeletePrefix(u:id:)` in a loop;
    3. confirm with `Count(prefix u:id:) == 0`.
- **Watch out:** backups and snapshots taken earlier still contain the
  data. Erasure in backups is a retention policy, not a KvNode call.

### R11. Cache invalidation across services

- **Calls:** each service runs `Watch(prefix product:, keys_only = true)`
  and drops local cache entries on PUT, DELETE and EXPIRE. On
  `OUT_OF_RANGE` (the service fell too far behind) it clears the whole
  cache and restarts the watch.
- **Guaranteed:**
  - revision order;
  - no lost invalidation while the watch is connected;
  - after a reconnect, `start_revision` resumes without gaps when the
    history still covers it.
- **Watch out:** cache the value together with its version, and ignore
  older events that arrive after a fresher read.

### R12. Offline-first sync

- **Calls:**
  - **First run:** `Export(prefix u:id:)`, and remember the cut revision.
  - **Later runs:** `ChangesSince(u:id:, last revision)`. On
    `OUT_OF_RANGE`, run Export again; later, Reconcile (G4).
- **Conflicts:** the device writes back with `CompareAndSet` on the
  versions it holds. A failed condition means someone else changed it, and
  the app merges.

### R13. Search-as-you-type and "did you mean"

- **Keys:** `name:<normalized name>:<id>`, where normalized means
  lowercase ASCII-folded, with accents stripped by the client.
- **Calls:**
  - **Completions:** `Scan(prefix name:<typed>, limit 10, keys_only)`.
  - **No completions?** `FindSimilar(name:<typed>, within name:)`.
  - **Rank** suggestions by a popularity counter (GetMany of
    `pop:<id>`).

### R14. Fraud: lookalikes and velocity

- **Calls:**
  - **At signup:** `FindSimilar(protected:<handle>, within protected:)` →
    refuse lookalikes of protected brands.
  - **Velocity:** `RateLimit(rl:card:<card>)` and
    `RateLimit(rl:ip:<ip>)`.
  - **Signals:** `Frequency(freq:ip:<day>, ip)` for anomalous volume;
    `DistinctCount(dev-accts:<device>)` for one device behind many
    accounts.

### R15. Hierarchical configuration and feature flags

- **Keys:** `cfg:/` (defaults), `cfg:/org42/`, `cfg:/org42/team7/`,
  `cfg:/org42/team7/user9/`.
- **Calls:**
  - **Read:** `LongestPrefix(cfg:/org42/team7/user9/…)` returns the most
    specific override. Merge up the chain with Seeks for field-level
    inheritance.
  - **Edit:** with CompareAndSet.
  - **Push:** `Watch(prefix cfg:)`.

### R16. Product analytics

- **Calls:**
  - **Unique visitors:** `DistinctAdd(uv:<page>:<day>)`, then
    `DistinctCount` with a union for weeks.
  - **Trending:** `TopKAdd(top:viewed:<hour>)`.
  - **Latency SLOs:** `QuantileAdd(lat:<endpoint>:<minute>)`.
  - **Cleanup:** every sketch carries a TTL, so old windows clean
    themselves.
- **Watch out:** approximate numbers. Label them in dashboards; don't bill
  from them.

### R17. Shielding against lookups of absent keys

- **Calls:**
  - **Clients:** hold `GetFilter(prefix)` for miss-heavy lookups (catalog
    checks, coupon validation) and refresh every few minutes.
  - **Server:** RateLimit per caller blunts enumeration floods. Absent keys
    remain the store's cheapest answer (C1).
- **Watch out:** never ship a filter of sensitive keys (C2).

### R18. Audits and sampling

- **Calls:**
  - **Daily:** `Sample(500, prefix order:<month>:)`, then check
    invariants; extrapolate violation rates (±1/√n).
  - **Coverage:** `Stats(prefix)`.
  - **Long-term audit trail:** Watch into a log store.

### R19. Daily actives, first visits and cohorts (bitmaps)

- **Problem:** count daily and monthly actives exactly, run "first visit
  today" logic exactly once per user, and count activity by signup cohort.
- **Keys:**
  - `act:<yyyymmdd>` and `act:m:<yyyymm>`: bitmaps indexed by user number,
    sized for the user base, with a TTL of 400 days;
  - `c:un:<user hex>` → the user's number: a compact key (H1).
- **Calls, per visit:** one Bits call with two ops:
  1. `SET act:<day>, n`. `was = false` means the first visit today, so run
     the once-a-day logic.
  2. `SET act:m:<month>, n`.
- **Reports:**
  - **DAU:** `COUNT act:<day>` reads the kept total.
  - **MAU:** `COUNT act:m:<month>`.
  - **A cohort with contiguous user numbers:** `COUNT` over its byte range,
    in tens of nanoseconds.
- **Guaranteed:**
  - exact counts;
  - first-visit logic runs once per user per day, because `LOCK BTS`
    reports one flip.
- **Watch out:**
  - "Active on both day A and day B" is an AND of two bitmaps: a full scan
    (18 µs per MiB pair). It is fine for dashboards; cache the results.
  - Number users densely: gaps cost bitmap memory.

### R20. A duplicate-listing guard

- **Problem:** stop sellers from re-posting a listing with small edits,
  and group near-identical tickets.
- **Keys:**
  - `listing:<id>` under `KVNODE_NEAR_PREFIXES=listing:`. Its value is the
    8-byte SimHash, then the listing.
  - `mh:<id>`: a 128 × u32 MinHash signature (a vector value, H10).
- **Calls, on create:**
  1. The client computes `fp = SimHash(shingles(title + body))` and a
     MinHash signature.
  2. `FindNear(listing:, fp, max_distance 3, limit 10)`.
  3. For each match, `Score(MINHASH, signature, [mh:<match>])`. A Jaccard
     estimate ≥ 0.8 is a duplicate: reject, or flag it.
  4. Otherwise, one atomic Mutate: `put listing:<id> (fp ‖ listing)`,
     `put mh:<id> (signature)`.
- **Guaranteed:**
  - every stored listing within 3 bits is found (the search is exact);
  - the atomic batch keeps a listing and its signature together.
- **Watch out:** two near-identical listings created at the same moment
  can both pass (check, then act). If that matters, serialize per seller
  with a lease (A7), or dedupe in a background pass.

### R21. Nearby dispatch

- **Problem:** match a rider with the nearest free driver within a
  second, and never book a driver twice.
- **Keys:**
  - geo index `drv:`: members are driver ids, values their points;
  - `c:free:<driver hex>`: a compact flag (1 = free);
  - `lease:drv:<id>`: the booking lease (A7).
- **Calls:**
  - **Drivers, every 3-5 s:** `GeoAdd(drv:, id, lat, lon)`.
  - **A ride request:**
    1. `GeoRadius(drv:, rider's point, 1,500 m, limit 20, sorted)`;
    2. `GetMany(c:free:<each>)`: compact keys, one miss each;
    3. for the nearest free driver, `AcquireLease(lease:drv:<id>, ttl 30 s)`;
    4. when it is acquired, `Put(c:free:<id>, 0)` and dispatch.
- **Guaranteed:** at most one rider holds a driver, even when two requests
  pick the same one: the lease decides.
- **Watch out:**
  - positions are seconds old, so the driver's app confirms;
  - in dense areas, page with `limit` instead of widening the radius.

### R22. Live player and profile state (struct fields)

- **Problem:** a game server updates coins, XP and levels for millions of
  players; profile pages read two fields.
- **Keys:** `player:<id>`: a struct (coins, xp, level, last_login, …).
- **Calls:**
  - **Reward:** one Fields call, `INCR coins +50` and `INCR xp +120`: two
    in-place adds.
  - **Level up:** `Fields(SET level 8)`.
  - **Profile page:** `Fields(GET level, coins)`: 8 bytes each on the
    wire.
  - **Leaderboard:** mirror the score into an ordered key (R8) when it
    changes.
- **Guaranteed:** concurrent rewards never lose an update (`LOCK XADD` per
  field).
- **Watch out:**
  - a whole-value Put, such as a profile import, wins over in-flight
    increments. Use Fields for live updates;
  - beyond 16 fields, split the struct by feature (`player:<id>:stats`,
    `player:<id>:inv`).

### R23. Rolling-window thresholds (failed logins, traffic spikes)

- **Problem:** lock an account after 5 failed logins in 15 minutes, and
  alert when an endpoint's last minute runs at 3× its hourly average.
- **Keys:**
  - `fail:<user>`: a window of 15 × 60 s, TTL 1 h;
  - `req:<endpoint>`: 60 × 1 s;
  - `req:h:<endpoint>`: 60 × 60 s.
- **Calls:**
  - **On a failed login:** `WindowAdd(fail:<user>, +1)`. At 5 or more:
    lock (a Put of `lock:<user>` with a TTL) and notify.
  - **Per request:** one WindowAdd call for both `req:<endpoint>` and
    `req:h:<endpoint>`.
  - **Monitor:** `WindowAdd(req:<endpoint>, 0, with_buckets)` returns the
    last minute and its sparkline. Compare it with the `req:h` sum ÷ 60.
- **Guaranteed:** counts are exact at bucket granularity.
- **Watch out:** to throttle callers, RateLimit (B3) is cheaper and
  smoother. Windows are for counts you read or compare.

---

# Part V — Engineering plan

## V.1 Protocol

- **New RPCs in `kvnode.v1.KvNode`:**

  | Phase | RPCs |
  |---|---|
  | 0 | `Stats` |
  | 1 | `Exists`, `GetMany`, `Mutate`, `Increment`, `RateLimit`, `Sample` |
  | 3 | `Export`, `Watch` (export-watch.md) |
  | 4 | `Scan`, `Count`, `Seek`, `Rank`, `Select`, `Pop`, `DeleteRange`, `LongestPrefix` |
  | 5 | `SketchAdd`, `SketchQuery`, `SketchMerge`, `GetFilter`; `Fields`, `Bits`, `Sets`, `WindowAdd`, `Score` |
  | 6 | `FindSimilar`, `FindContaining`, `Matching`, `Reconcile`; `FindNear`, `GeoAdd`, `GeoRadius` (compact keys need no RPC) |
  | S | none: Track S changes no message |

- **Shared messages:** `Entry` (fields 1-2 as in export-watch.md, plus
  `version` and `ttl_ms`), `KeyRange`, `Bounds`, `Condition`.
- **[PROTOCOL.md](../PROTOCOL.md)** gains:
  - the new status codes: `FAILED_PRECONDITION` (9), `PERMISSION_DENIED`
    (7);
  - the batch, response-cap and cursor rules;
  - the counter, sketch and filter byte layouts;
  - the struct, bitmap, set, window and vector layouts (§1.6), the
    compact-key limits, and the fingerprint and point prefixes of near and
    geo values;
  - one conformance vector per RPC, in the existing hex format.

## V.2 Server changes by module

| Module | Change |
|---|---|
| `server.inc`, `hpack.asm` | one route id and `:path` string per new RPC (`M_EXISTS` … `M_RECONCILE`) |
| `grpc.asm` | dispatch, validation and budgets per RPC; Exists and GetMany inside the batched GET path; the front half of the Mutate engine (decode, validate, prefetch); reply framing; cursors |
| `proto.asm` | decoders for repeated messages, oneofs, packed bools and varints; encoders for the new replies |
| `store.asm` | the back half of the Mutate engine (conditions, apply, Move, atomic batches, memory reservation); revisions and versions; in-place cells (counters, rate limits); the TTL record extension and lazy checks |
| `cuckoo.asm` | unchanged lookup. TTL checks live in the callers, so the tag scan stays as measured |
| new `ttl.asm` | the timing wheel and the sweeper |
| new `order.asm` | the radix tree with optimistic lock coupling, subtree counts and leaf back-pointers; the reversed tree |
| new `dir.asm` | segment directories (key-indexes.md §2.1) |
| new `sketch.asm` | HLL, Count-Min, TopK and DDSketch layouts; AVX2 kernels, each with an SSE2 or scalar twin and runtime dispatch (ADR 0006) |
| new `filter.asm` | the binary fuse builder on the builder thread; SipHash with public seeds |
| new `fuzzy.asm`, `substr.asm` | the opt-in search indexes (after their gates) |
| `wlock.asm` | Track S: flat combining (per-thread publication slots; the combiner prefetches and applies every pending write) and UMWAIT waits with a PAUSE fallback |
| `siphash.asm`, `grpc.asm` | Track S: SipHash-2-4-128 in 8 AVX2 lanes (two interleaved 4-lane groups, bit-exact) in every batched hash stage, behind CPUID dispatch |
| new `compact.asm` | H1: the compact table (64-byte buckets, seqlock, partial-key cuckoo) and routing by prefix |
| new `typed.asm` | H2-H4, H9, H10: struct fields, bitmaps with block counts, sets, windows and vectors; AVX2 kernels (nibble and Harley-Seal popcount, all-pairs set intersection, int8 and fp32 dot products) each with a scalar twin (ADR 0006) |
| new `near.asm`, `geo.asm` | H5, H6: static base + live delta, merged by the builder thread; multi-index hashing; Morton codes (PDEP and PEXT, with a shift-and-mask twin); prefix tables for seeks |
| export-watch.md §8 modules | the history ring, cuts, ADD and EXPIRE events, counter exports (§2.3 rule 4) |
| `worker.asm` | the sweeper and builder scheduling; the per-worker cell index for hot counters |
| `config.asm` | the settings of §V.3 |
| `main.asm`, `log.asm` | startup order (seed revisions, build indexes before loading the snapshot); stats fields |
| `snapshot.asm` | expiry in snapshots (§V.6) |

## V.3 Configuration (new settings)

| Variable | Default | Meaning |
|---|---|---|
| `KVNODE_MAX_READ_BATCH` | 1024 | keys per Exists, GetMany and Sample call |
| `KVNODE_MAX_WRITE_BATCH` | 256 | mutations per Mutate or Increment call |
| `KVNODE_MAX_ATOMIC` | 64 | mutations per atomic batch (bounds the writer-lock hold) |
| `KVNODE_MAX_RESPONSE_KB` | 1024 | bytes per unary response before it stops early |
| `KVNODE_MAX_SCAN_PAGE` | 1000 | keys per Scan page |
| `KVNODE_TTL` | on | expiry support; `off` removes even the flag test |
| `KVNODE_EXPIRE_TICK_MS` | 1000 | timing-wheel slot width |
| `KVNODE_EXPIRE_BATCH` | 1024 | removals per sweeper tick |
| `KVNODE_INDEX_ORDER` | off | `off` \| `on` \| `both` (also a reversed tree, for suffixes) |
| `KVNODE_INDEX_DELIMITERS` and the other `KVNODE_INDEX_*`, `KVNODE_PATTERNS` | — | key-indexes.md §7 |
| `KVNODE_INDEX_FUZZY` | (unset) | key prefixes indexed for FindSimilar |
| `KVNODE_INDEX_SUBSTR` | (unset) | key prefixes indexed for FindContaining |
| `KVNODE_SKETCHES` | on | sketch RPCs |
| `KVNODE_FILTERS` | on | GetFilter |
| `KVNODE_FILTER_PREFIXES` | (unset: any) | prefixes GetFilter may export |
| `KVNODE_FILTER_MIN_INTERVAL_SEC` | 60 | minimum time between rebuilds of one scope |
| `KVNODE_SCOPE_HEADER` | (unset) | multi-tenant scopes (§1.10) |
| `KVNODE_WATCH_HISTORY`, `KVNODE_EXPORT*` | — | export-watch.md §9 |
| `KVNODE_COMPACT_PREFIXES` | (unset) | key prefixes stored in the compact table (H1) |
| `KVNODE_TYPED_VALUES` | on | Fields, Bits, Sets, WindowAdd and Score |
| `KVNODE_MAX_BITMAP_MB` | 128 | the largest bitmap (H3) |
| `KVNODE_MAX_SET_MEMBERS` | 65536 | the largest set (H4) |
| `KVNODE_NEAR_PREFIXES` | (unset) | prefixes indexed for FindNear (H5) |
| `KVNODE_GEO_INDEXES` | (unset) | prefixes indexed for GeoRadius (H6) |
| `KVNODE_GEO_MAX_RADIUS_M` | 50000 | the largest radius GeoRadius accepts |
| `KVNODE_HASH_LANES` | auto | Track S: `auto` (8 lanes with AVX2) \| `1` |
| `KVNODE_WRITE_COMBINING` | on | Track S: flat combining for writers |
| `KVNODE_WAIT` | auto | Track S: `auto` (UMWAIT when the CPU and the hypervisor allow it) \| `pause` |

Every setting follows today's strict rules: validated at startup, logged,
and refused with the allowed range when invalid.

## V.4 Observability

- **Stats** (C4) for clients.
- **Log fields** at the periodic stats line and at shutdown:
  - `revision`, `expiring`, `expired`, `sweep_ms`;
  - `index_keys`, `index_bytes`;
  - `filters_built`, `filter_bytes`;
  - `conditions_failed`, `atomic_batches`;
  - `counter_overflows`;
  - `sketch_updates`;
  - `fuzzy_bytes`, `substr_bytes`;
  - Group H: `compact_keys`, `compact_load`, `typed_updates`, `near_keys`,
    `geo_points`;
  - Track S: `hash_lanes`, `wait_mode`, `combined_passes`,
    `combined_writes_avg`.
- **Every per-call limit that refuses a request** logs it at most once per
  second, with the setting to change.

## V.5 Security

- **Scopes:** `PERMISSION_DENIED`, with the trusted-proxy model (§1.10).
- **Bounded work per call:** batch sizes, pages, the response cap,
  atomic-batch size, fuzzy candidates, pattern complexity (DFAs only, so
  no ReDoS: export-watch.md §4.4), sketch parameters, and the rate of
  filter builds.
  - Group H adds its own bounds: FindNear's distance (≤ 3) and limit,
    GeoRadius's radius and limit, the members INTER_CARD may compare per
    call, bitmap and set sizes, and the keys per Score call.
- **Hashing:**
  - Sketch items hash with the server's secret key.
  - Client filters use public seeds: a crafted key can only produce a false
    "maybe".
  - The ordered index has no hash to flood: its depth is bounded by key
    length.
- **Filters leak membership** (C2): they are exportable only for
  configured prefixes when `KVNODE_FILTER_PREFIXES` is set.

## V.6 Persistence and snapshots

- **Values persist unchanged.** Counters, sketches and rate-limit state
  are values: today's `KVNSNAP1` stores them as they are.
- **TTL needs a format change.** Version 1 cannot carry deadlines. The
  proposal:
  - flags bit 0 means "records carry an expiry";
  - each record gains a `u64 expires_unix_ms` (0 = none) after its
    `value_len`;
  - version 1 readers already reject non-zero flags, so an old server
    refuses a new snapshot instead of misreading it;
  - records expired at load time are skipped.
- **Revisions need no persistence for monotonicity:** they are re-seeded
  from the clock (§1.5).
- **Group H persists as values.**
  - Typed values are values: structs, bitmaps with their counts, sets,
    windows and vectors.
  - Fingerprints and points live at the start of values, so the
    FindNear and GeoRadius indexes are rebuilt at load.
  - The snapshot writer walks the compact table too. A compact key is a
    key, so the format does not change.
- **The persistence proposal (write-ahead log)** must log *effects*
  (resulting values), not deltas or conditions. Replay is then
  deterministic even for counters and conditional writes.

## V.7 The .NET client

| Methods | RPC | Notes |
|---|---|---|
| `ExistsAsync` (one or many) | Exists | splits batches above the limit |
| `GetManyAsync`, `GetWithVersionAsync`, `GetCountersAsync` | GetMany | follows `next_index` |
| `PutManyAsync`, `DeleteManyAsync`, `PutIfAbsentAsync`, `PutIfPresentAsync`, `CompareAndSetAsync`, `GetAndDeleteAsync`, `GetAndSetAsync`, `ExpireAsync`, `PersistAsync`, `MoveAsync`, `TransactAsync` | Mutate | typed results: `Applied`, `Version`, `Previous` |
| `AcquireLeaseAsync` → `Lease` (`IAsyncDisposable`: `Token`, `Lost`, auto-renew) | Mutate | renews at TTL/3; releases on dispose |
| `IncrementAsync` (one or many), `GetCounterAsync` | Increment, Get | int64 encoding handled |
| `RateLimitAsync` | RateLimit | |
| `SampleAsync`, `StatsAsync` | Sample, Stats | |
| `ScanAsync` (one page), `ScanAllAsync` (`IAsyncEnumerable`), `CountAsync`, `SeekAsync`, `RankAsync`, `SelectAsync`, `PopAsync`, `DeletePrefixAsync`, `DeleteRangeAllAsync`, `LongestPrefixAsync` | Order RPCs | cursors handled |
| `FindSimilarAsync`, `FindContainingAsync`, `CountMatchingAsync` | Search RPCs | |
| `DistinctAddAsync`, `DistinctCountAsync`, `FrequencyAddAsync`, `FrequencyAsync`, `TopKAddAsync`, `TopKAsync`, `QuantileAddAsync`, `QuantilesAsync`, `SketchMergeAsync` | Sketch RPCs | |
| `GetFilterAsync` → `KeyFilter` (`MightContain`, `RefreshEvery`) | GetFilter | binary fuse check in C# |
| `WatchAsync`, `ChangesSinceAsync`, `ExportAsync` | Watch, Export | export-watch.md |
| `GetAsync`, `GetManyAsync`, `PutAsync`, `IncrementAsync` on compact keys | Get, GetMany, Mutate, Increment | optional client-side checks of the compact limits |
| `GetFieldsAsync`, `GetFieldAsync<T>`, `SetFieldAsync`, `IncrementFieldAsync`, `DeleteFieldAsync`, `GetStructAsync`, `PutStructAsync` | Fields, Get, Put | the struct encoding in `KvStruct` |
| `SetBitAsync`, `ClearBitAsync`, `GetBitAsync`, `CountBitsAsync`, `FindBitAsync` | Bits | |
| `SetAddAsync`, `SetRemoveAsync`, `SetContainsAsync`, `SetCountAsync`, `SetIntersectCountAsync` | Sets | |
| `FindNearAsync`, `PutWithFingerprintAsync`; `SimHash` helpers | FindNear, Mutate | SimHash over shingles, in C# |
| `GeoAddAsync`, `GeoRadiusAsync` (one page), `GeoRadiusAllAsync` (`IAsyncEnumerable`), `GeoRemoveAsync` | GeoAdd, GeoRadius, Mutate | cursors handled |
| `WindowAddAsync`, `WindowSumAsync` | WindowAdd | |
| `ScoreAsync`; `KvVector` encoders (int8, fp32, MinHash) | Score, Put | quantization helpers |

**Plumbing:**
- **Retries** follow §1.8.
- **The receive limit** grows by the maximum key size for Scan-class
  calls: a 1 MiB value plus a 1 KiB key and framing exceeds today's
  1 MiB + 1 KiB.
- **`Fixed(n)`** and big-endian key helpers ship in `KeyParts`.

## V.8 Verification plan

**Store level (C, like `test_store_mt`).**
- **Concurrency:** conditions, atomic batches and counters, with exact
  totals and a linearizability checker (Porcupine-style) over recorded
  histories of concurrent clients.
- **TTL** with a test clock (a test-build hook): lazy exactness, sweeps,
  interactions with conditions and Move.
- **The radix tree:** against a sorted reference under random operations,
  with concurrent optimistic readers. No torn read; every key exactly once
  in scans; counts equal the reference.
- **Sketches:** statistical bounds over many trials (HLL within 3σ;
  Count-Min never under-counting; DDSketch relative error ≤ α); AVX2 and
  scalar twins agree byte for byte.
- **Search and filters:** filters have no false negatives; SymSpell and
  the substring index agree with brute force.
- **Group H:**
  - **The compact table** against an ordinary-store reference under random
    operations, with concurrent readers retrying on the seqlock.
  - **Struct fields and bitmaps** with exact totals under concurrent INCR,
    SET and flips (history checker). Exports rebuilt from undo records
    must equal the state at the cut (§2.3 rule 4b).
  - **Sets, FindNear and GeoRadius** against brute force, as the services
    probe does.
  - **Every AVX2 kernel** against its scalar twin, byte for byte.
- **Track S:**
  - the 8-lane hash bit-exact for every key-length class;
  - flat combining linearizable, with no lost hand-off (history checker,
    and a stress test with a watchdog);
  - the PAUSE fallback when the hypervisor refuses UMWAIT (a test hook
    forces the refusal).

**Protocol.**
- Every new decoder is fuzzed, as `proto.asm` is today.
- A conformance vector per RPC.
- nghttp2-based integration tests for every status code and limit.

**Client.**
- Unit tests for cursors, batch splitting, leases (with a fake clock) and
  filters.
- Testcontainers end-to-end tests per RPC.

**Mutation checks** (as in ADR 0010; each must make a test fail):
- skip a condition check;
- drop the TTL lazy check;
- skip index maintenance on delete;
- forget the delta subtraction in counter exports;
- skip a bitmap block-count update (a range COUNT test must fail);
- drop an undo record (an export test must fail);
- let the combiner skip one slot's DONE store (the watchdog must fire).

## V.9 Measurement gates

Each gate must hold on the quiet host before its phase lands. They are
measured with the certified-run method of the golden probe (an idle
container, `docker events` for the window).

| Function or phase | Gate |
|---|---|
| Phase 0: revisions | ≤ +5 ns per write (S: 3.0 ns, +3.3 ns for the watermark); Get/Put/Delete ±2% with every feature off |
| GetMany, Exists | ≤ 1.1× the batched GET bar per key (54 ns) |
| Mutate batches | ≥ 2.5× faster per key than single Puts on one core (3.4× measured store-level) |
| conditional single write | ≤ 1.1× Put latency |
| Increment | ≤ 1.2× a batched get per update (58 ns measured) |
| RateLimit | ≤ 1.3× a batched get |
| TTL | Get ±1%; put with TTL ≤ +50 ns |
| ordered index | ≤ +0.85 µs per create or delete (the libart rival), 0 per replace; a 100-key Scan page with values ≤ 10 µs; Count, Rank and Seek ≤ 1 µs |
| sketches | an HLL or Count-Min add ≤ 1.2× a batched get per item; an HLL union ≤ 1.2 µs at p=14 |
| GetFilter | build ≤ 100 ns per key |
| FindSimilar, 1 edit | ≤ 1 µs at 10M keys (0.57-0.70 measured) |
| FindSimilar, 2 edits | ≤ 0.35 ms, or restricted (E2) |
| FindContaining | count ≤ 10 µs at 10M keys |
| internal absent-key filter | ≥ 10% GET/s at ≥ 50% misses, no loss at 0% |
| hot counters | ≥ 3× `LOCK XADD` throughput on one key at 7 workers (4.8× measured) |
| Track S: 8-lane hashing | batched GET ≤ 0.97× today's per key (0.943 measured, V); bit-exact for every key |
| Track S: flat combining | puts/s at 7 writers ≥ 1.5× the mutex (1.91× measured, V); ≥ 0.95× at 1 writer (0.98×); p99 put latency ≤ 1.2× today's |
| Track S: UMWAIT | wake-up ≤ the PAUSE spin's (0.90× measured, V); the PAUSE fallback verified |
| compact keys (H1) | batched get ≤ 0.75× an ordinary key of the same size (0.53-0.70× measured, V); memory ≤ the ordinary key's |
| Fields (H2) | FIELD_GET ≤ 1.2× a batched get of a 100-byte value; FIELD_INCR ≤ 1.2× an Increment |
| Bits (H3) | SET with counts ≤ 1.2× an Increment; COUNT of any range ≤ 200 ns after the lookup (67.6 ns measured) |
| Sets (H4) | IS_MEMBER ≤ 1.2× a batched get; INTER_CARD of 1,024 × 1,024 ≤ 1 µs (552.8 ns measured) |
| FindNear (H5) | ≤ 2 µs at 10M uniform fingerprints (695 ns measured), and on the customer's real data before enabling |
| GeoRadius (H6) | 200 m with ≤ 20 results ≤ 2 µs at 10M uniform points (697.5 ns measured), and on real data |
| WindowAdd (H9) | ≤ 1.3× an Increment |
| Score (H10) | ≤ 1.2× a batched get per key |

## V.10 Phases

| Phase | Scope | Depends on | Deliverables | Size |
|---|---|---|---|---|
| 0 Foundations | revisions and versions (clock seed, `Rec.aux`); the Mutate engine with Put and Delete as its fast paths; batch and response limits; Stats | — | its ADR; PROTOCOL.md; store tests; a .NET client skeleton | M |
| 1 No new structures | Exists, GetMany, Mutate (batches, conditions, return_previous, atomic batches, Move), Increment with bounds, RateLimit, Sample (whole store); the batched write pipeline | 0 | its ADR; the client methods; example programs for conditional writes and bounded counters | L |
| 2 Time | TTL (record extension, lazy checks, wheel, sweeper); leases in the client; KVNSNAP expiry | 1 | its ADR; R1, R2, R3 and R5 as example programs | M |
| 3 Streams | export-watch.md's phases, plus ADD and EXPIRE events and exact counter exports | 0 | export-watch.md §13; R4 (hold release), R11, R12 | XL |
| 4 Order | the radix tree with counts and back-pointers; the order RPCs; directories; prefix sampling; StatsByPrefix | 0 | its ADR; R6-R10 | XL |
| 5 Analytics and typed values | the sketch RPCs; GetFilter; the internal filter experiment; Fields, Bits, Sets, WindowAdd and Score (Group H) | 1 | its ADR; R16, R17, R19, R22, R23 | L |
| 6 Opt-ins | FindSimilar (1 edit; 2 edits after its gate); FindContaining (after its gate); Matching; hot counters; the snapshot accelerator; the Reconcile proposal; compact keys, FindNear, GeoAdd / GeoRadius (Group H) | 3, 4 | an ADR per feature; R13, R14, R20, R21 | L each |
| S Store speedups | 8-lane SipHash; flat combining for writers; UMWAIT waits (golden-services.md P2-P4) | — | one ADR; the services probe's rows as its gate | M |

ADR numbers are assigned when each phase lands, not reserved here: other
work takes numbers in between.

**Order and parallelism.**
- Phase 0 first.
- Phases 1, 3 and 4 can then proceed in parallel worktrees, as Upload and
  Download did. They meet in the Mutate engine's hooks, so the hook
  interface is part of Phase 0.
- **Track S can land any time, even before Phase 0.** It touches only the
  hash stage and the writer lock, changes no message, and speeds up every
  later phase. Its flat combiner is also where Phase 1's batched writes
  meet, so landing it first leaves Phase 1 less to build.

## V.11 Risks

| Risk | Mitigation |
|---|---|
| Write-path creep: every index adds work to creates and deletes | per-feature switches; gates per index; replaces stay free |
| The single writer mutex becomes the bottleneck as more work moves into writes (concurrent writers already fall from 3.35 to 2.25 M puts/s at 7) | first, flat combining (Track S): measured 1.91× at 7 writers (V), which restores the single-writer rate and keeps the one-writer invariant. Only if that is not enough, lock striping with optimistic cuckoo paths (libcuckoo, C); atomic batches would then lock their stripes in order |
| Typed-value sprawl (structs, bitmaps, sets, windows, vectors) | one typed-value header and one module (`typed.asm`); each type behind `KVNODE_TYPED_VALUES` and its own gate |
| Real data clusters, unlike the probe's uniform data (FindNear, GeoRadius) | gates on the customer's data before enabling; per-call limits bound the worst case |
| The combiner's tail latency: one thread applies other threads' writes | cap the writes per combining pass; p99 put latency is part of the gate |
| Memory growth from opt-ins | per-prefix enablement; stated costs (§1.11); refusal at the memory limit |
| Assembly complexity: the radix tree with optimistic locking and counts is the hardest part | prototype in C beside the store first (as the probes did); a reference model; mutation checks |
| In-place updates versus cuts | rule 4 (§2.3) needs the history ring: counters in exports wait for Phase 3; until then, exports aren't offered at all |
| API sprawl | orthogonal RPCs that share `Entry`, `KeyRange` and `Condition`; every function maps to an existing need (Part IV) |

## V.12 Alternatives considered

| Alternative | Why not |
|---|---|
| Conditions and TTL as new fields on Put and Delete | older servers skip unknown fields and would apply the write unconditionally |
| Errors for failed conditions | exceptions for normal outcomes; batches need per-item outcomes anyway |
| Counters as decimal text (Redis) | an in-place atomic add needs a fixed binary cell; clients convert in one line |
| One generic "Command" RPC | weaker typing, harder validation and budgets, no per-call limits |
| Only the sorted snapshot for order | stale without a delta and 0.7 s to rebuild at 10M keys; kept as an accelerator (§2.5) |
| The ordered index maintained asynchronously | queues, claims and pagination need to read their own writes |
| A scripting engine (Lua-like) for custom atomic logic | unbounded lock holds and a large security surface. Atomic batches with conditions cover the measured needs |
| A remote MightContain call | a round trip costs far more than the miss it would save (C1) |
| Reference counts for in-place updates under cuts | delta subtraction (§2.3 rule 4) needs no per-record state |
| Structs, bitmaps and sets encoded by clients over Get and CompareAndSet | two round trips per update, the whole value on the wire, and retry loops under contention. A field-level call is one atomic instruction (H2-H4) |
| Approximate nearest-neighbor search (HNSW and kin) over all vectors | hundreds of µs and several times the data in memory: not golden. Score covers re-ranking (H10) |
| Lock striping before flat combining | more invasive (every cuckoo path changes), while combining keeps the one-writer invariant and measured 1.91× (Track S) |

## V.13 Open decisions

1. **The first public functions.** Proposed: the Phase 1 set, since it
   needs no new structure.
2. **Counter representation:** int64 little-endian (proposed) or decimal
   text.
3. **The ordered index:** a synchronous radix tree (proposed) or a
   snapshot + delta first.
4. **TTL in snapshots:** the flags-bit format (§V.6) or a `KVNSNAP2`.
5. **Multi-tenant scopes:** server-side prefixing through a trusted proxy
   (proposed) or client-side prefixes with checks only.
6. **Which opt-ins ship at all:** FindSimilar (measured) and
   FindContaining (estimated) have very different confidence.
7. **The internal absent-key filter:** only if its gate passes.
8. **Events for sketches and rate-limit state:** none (proposed), or
   coalesced per tick like hot counters.
9. **SipHash-1-3** for the store's hash (ADR 0004 amendment): the hash is
   23% of a batched get.
10. **Default limits:** the batch sizes, the response cap and the
    atomic-batch size of §V.3.
11. **Where indexed attributes live.** Proposed: fingerprints (H5) and
    points (H6) at the start of the value under their prefixes, so Put and
    Delete maintain the indexes and snapshots need no change. The
    alternative keeps them beside the value, with their own RPCs and
    snapshot records (golden-services.md §10.6).
12. **Group H's own decisions** (golden-services.md §10):
    - compact keys as a typed key space (proposed) or a transparent tier;
    - 16 or 32 struct slots;
    - an exact bitmap COUNT under concurrency, via a sequence word;
    - HeavyKeeper (measured) or Space-Saving (planned) for TopK (F3);
    - the geo index structure.
13. **Track S defaults:** on (proposed) once its gates hold, each with a
    switch for A/B tests and fallback (§V.3).

---

# Appendix A — The same functions in Redis terms

| Redis | KvNode |
|---|---|
| `EXISTS` | Exists |
| `MGET`, `MSET`, `DEL k1 k2 …` | GetMany, Mutate |
| `SET NX` / `SET XX` | Mutate, `condition.absent` / `present` |
| `WATCH` + `MULTI`/`EXEC` (optimistic) | Mutate, `condition.version`, `atomic` |
| `GETDEL`, `SET … GET`, `GETSET` | Mutate, `return_previous` |
| `EXPIRE`, `PEXPIRE`, `TTL`, `PERSIST`, `SET … KEEPTTL` | Mutate `touch` / `ttl_ms` / `keep_ttl`; GetMany `with_ttl` |
| `RENAME`, `RENAMENX` | Mutate `move` (`overwrite`) |
| `INCR`, `INCRBY`, `DECR`, `DECRBY` | Increment (int64 binary, not decimal text) |
| `CL.THROTTLE` (redis-cell) | RateLimit |
| `RANDOMKEY` | Sample(1) |
| `DBSIZE`, `INFO` | Stats |
| `SCAN MATCH`, `KEYS` | Scan (prefix), Export (pattern) |
| `ZRANGEBYLEX`, `ZLEXCOUNT`, `ZRANK`, `ZPOPMIN` | Scan, Count, Rank, Pop, over the whole keyspace (no separate sorted set to maintain) |
| `PFADD`, `PFCOUNT`, `PFMERGE` | SketchAdd, SketchQuery, SketchMerge (DISTINCT) |
| `CMS.*`, `TOPK.*`, `TDIGEST.*` (RedisBloom) | the sketches (FREQUENCY, TOP_K, QUANTILES) |
| `BF.EXISTS` (RedisBloom) | GetFilter, checked on the client |
| keyspace notifications, `XREAD` | Watch, ChangesSince |
| `BGSAVE`, RDB files | Export (streamed, point in time), KVNSNAP |
| `FT.SEARCH` fuzzy and wildcard (RediSearch) | FindSimilar, FindContaining |
| `INCR`, `GET`, `SET` on small keys | the same calls on compact keys (H1), at one cache miss |
| `HGET`, `HMGET`, `HSET`, `HINCRBY`, `HDEL`, `HGETALL` | Fields (GET, SET, INCR, DEL); Get for the whole struct (H2) |
| `SETBIT`, `GETBIT`, `BITCOUNT`, `BITPOS` | Bits (SET / CLEAR, GET, COUNT, POS) (H3) |
| `SADD`, `SREM`, `SISMEMBER`, `SCARD`, `SINTERCARD` | Sets (H4) |
| `GEOADD`, `GEOSEARCH` (`GEORADIUS`) | GeoAdd, GeoRadius (H6) |
| no direct equivalent | FindNear (H5), WindowAdd (H9; in Redis, a key per second plus a sum), Score (H10) |

# Appendix B — Evidence and sources

**Measured evidence:**
- [golden-probe](../../benchmarks/results/2026-09-29-golden-probe/README.md) (10M keys: M)
- [index-probe](../../benchmarks/results/2026-09-29-index-probe/README.md) (16M keys: K, and the revision probe S)
- [export-probe](../../benchmarks/results/2026-09-29-export-probe/README.md) (X)
- [cap-probe](../../benchmarks/results/2026-09-29-cap-probe/README.md) (100k / 1M)
- [services-probe](../../benchmarks/results/2026-10-01-services-probe/README.md) (10M keys: V; Group H and Track S)

**Cited techniques:**
- Most sources are listed in golden-operations.md §8 and capabilities.md
  §4. Among them: SBBF, binary fuse filters, Eytzinger, HLL, Count-Min,
  Space-Saving, DDSketch, SymSpell, Myers, the timing wheel, Rateless
  IBLT, libcuckoo and the adaptive radix tree.
- **Group H and Track S** cite golden-services.md §8: AVX2 popcount and
  Harley-Seal, SimHash and multi-index hashing, SIMD set intersection,
  Roaring, HeavyKeeper, DDSketch, learned and radix indexes, Morton codes
  with BMI2, flat combining, ffwd, and UMWAIT.
- **GCRA:** the generic cell rate algorithm (ATM Forum traffic
  management), as implemented by redis-cell,
  <https://github.com/brandur/redis-cell>.
- **Porcupine:** a fast linearizability checker,
  <https://github.com/anishathalye/porcupine>, the model for the history
  checks of §V.8.
- **Optimistic lock coupling:** Leis, Haubenschild, Neumann, *Optimistic
  Lock Coupling: A Scalable and Efficient General-Purpose Synchronization
  Method*, IEEE Data Engineering Bulletin, 2019.
