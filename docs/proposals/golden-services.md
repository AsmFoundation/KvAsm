# Proposal: golden services — more within 10× of an exact lookup at 10M keys, and the assembly behind them

- **Status:** proposed, 2026-10-01. Nothing here is implemented or decided.
- **The ask:** find more highly efficient key-value services that meet the
  golden standard (at most 10× an exact get or exists on a 10M-key
  collection), make things faster in general, and use assembly-level
  techniques and optional SIMD to get there.
- **What this adds:**
  - **Ten services** (Group H, §2), each measured golden at 10M keys
    except where §6 says otherwise.
  - **Six changes that make the store itself faster** (§3).
  - **A table of the instructions behind them**, each next to its measured
    scalar fallback (§4).
- **Companions:**
  - [golden-operations.md](golden-operations.md): the first golden catalog
    (point, order, typos, sketches, writes).
  - [functions-plan.md](functions-plan.md): the 38 functions designed for
    implementation. Group H here extends it; F3 (TopK) and F4 (Quantiles)
    are measured here for the first time.
  - [capabilities.md](capabilities.md), [key-indexes.md](key-indexes.md),
    [export-watch.md](export-watch.md).
- **Evidence:**
  [benchmarks/results/2026-10-01-services-probe](../../benchmarks/results/2026-10-01-services-probe/README.md).
  - `services_probe.c` builds every structure next to the store's own
    assembly, checks every answer against a scalar or brute-force
    reference, then times it.
  - The quoted run (`output.txt`) is certified: no other session's work ran
    in its window.
  - Labels:

    | Label | Source |
    |---|---|
    | **V** | measured by the services probe, 10M keys |
    | **M** | the golden probe ([golden-operations.md](golden-operations.md)) |
    | **E** | estimated from measured parts |
    | **C** | cited (§8) |

## Summary

**The bar of this run** (10M keys of 20-21 bytes, 100-byte values,
i9-13900K):

| Regime | Exact get | 10× budget |
|---|---|---|
| **D**: dependent chain (pure latency) | 435.5 ns | 4.36 µs |
| **I**: independent plain loop | 406.0 ns | 4.06 µs |
| **B**: the server's batched pipeline | 57.3 ns | 573 ns |

- **B matches the golden probe** (54.1 ns certified on 2026-09-29).
- **D and I are slower today on the machine, not in the method.** The
  golden probe's own bar, re-run as a control the same hour, read D
  432-449 and I 386-414 ns (347 and 327 on 2026-09-29). The host carried
  a game client and desktop load; the VM was idle.
- **So every ratio below compares within this run.**

**New services, all golden (V; H9 and H10 combine a measured kernel with
the measured lookup).**

| # | Service | Answers | Cost at 10M | × bar |
|---|---|---|---|---|
| H1 | Compact values | get / increment a ≤ 16-byte key's ≤ 8-byte value | **28.1 ns** batched get (the store: 53.1); 189.9 ns alone | 0.49 B; 0.44 D |
| H2 | Struct fields | one field of a structured value: HGET / HINCRBY | 59.6 / 62.5 ns (whole 440-byte value: 102.0) | 1.04 / 1.09 B |
| H3 | Bitmaps | SETBIT, GETBIT, BITPOS, BITCOUNT of any byte range | 10.1 / 0.6 / 92.1 / **67.6 ns** (a range scan: 7.44 µs) | ≤ 0.21 D |
| H4 | Small sets | SISMEMBER; SINTERCARD of 1,024 × 1,024 | 2.1 ns (64 members); 552.8 ns | 0.04 B; 1.27 D |
| H5 | Near-duplicates | which of 10M fingerprints are within 3 bits? | **695 ns** (brute force 10.8 ms) | 1.71 I |
| H6 | Geo radius | points within 200 m (15 results) among 10M | **697.5 ns** | 1.72 I |
| H7 | TopK | the 100 heaviest keys of a stream | 16.7 ns per update, 100/100 correct | 0.29 B |
| H8 | Quantiles | p50 … p99.9 within 1% | 16.7 ns per value; 16.2 ns per quantile | 0.29 B; 0.04 D |
| H9 | Sliding windows | requests in the last 64 periods | an in-place add + 4.5 ns to sum | ≈ 1 B |
| H10 | Similarity scoring | int8 / fp32 dot product, MinHash agreement of a stored vector | 1.5 / 5.2 / 5.6 ns after the lookup | ≈ 1 B |

**The store itself, faster (V).**

| # | Change | Measured |
|---|---|---|
| P1 | Compact tier (H1): the value inside the bucket's line | gets 0.54× (alone) to 0.53× (batched, with P2) the store; increments 0.68× |
| P2 | SipHash-2-4 of 8 keys in AVX2 lanes, bit-exact | hash 0.40× (5.6 vs 13.9 ns); batched get 0.943× (paired); compact gets −24% |
| P3 | Flat combining for writers | 7 threads: 4.29 vs 2.24 M puts/s (1.91×); 4: 1.66×; 2: 1.42×; 1: 0.98× |
| P4 | UMONITOR/UMWAIT for waits | wake-up 103.5 vs 114.4 ns (PAUSE spin) vs 2.70 µs (futex); the waiting core idles |
| P5 | Prefix-table seeks into sorted codes | 1.53× per geo query (697.5 vs 1,070 ns) |
| P6 | Aggregates kept on write | range BITCOUNT 110× faster for +6.8 ns per SETBIT; quantile 6.3× faster |

**Not golden, and why (§6).**

| Case | Measured | Way out |
|---|---|---|
| BITCOUNT of a whole 1 MiB bitmap, by scan | 12.8 µs (29× D) at 82 GB/s | the total kept by SETBIT: one load |
| AND-count of two 1 MiB bitmaps | 17.9 µs (41× D) | kept counts for registered pairs; Roaring for sparse sets |
| Geo radius with 371 results (1,000 m) | 6.18 µs (15.2× I) | output-bound: limit or page; finer cells |

**What the measurements teach.**
1. **Fewer dependent misses beat faster instructions.** The compact tier
   removes the record dereference and halves the dependent-chain latency.
   Hashing 2.5× faster moves the batched get only 5.7%.
2. **SIMD across independent items, not inside one.** SipHash's chain is
   serial, yet eight keys in two interleaved 4-lane groups hash 2.5×
   faster. One 4-lane group gained nothing in the pipeline (1.018×), and
   two did (0.943×).
3. **Keep the aggregate on the write.** A SETBIT that also bumps one count
   makes a range BITCOUNT 110× faster; DDSketch group counts make a
   quantile 6.3× faster. This is the same move as the kept harmonic sum
   that makes HLL estimates golden (M).
4. **Measure the probe too.** Four probe bugs would each have printed a
   wrong number without failing a check (§9). A control run caught the
   worst one.

---

## 1. The bar, and the control

| Lookup | D | I | B |
|---|---|---|---|
| get, 100-byte value copied (this run, V) | 435.5 ns | 406.0 ns | 57.3 ns |
| the golden probe's bar, same hour (control) | 432.5-449.0 | 385.9-414.2 | 56.2-63.8 |
| the golden probe's bar, certified 2026-09-29 (M) | 347.2 | 326.9 | 54.1 |

- **The services probe agrees with the golden probe run the same hour.**
  Latency-bound regimes (D, I) moved together by about +25%; the
  batched regime did not.
- **The likely cause is host load on the VM's cores**, not the store or
  the probe:
  - a game client and a browser were running on the host;
  - a test host from another project had been spinning for 5 days;
  - the VM itself was idle (0.2% CPU).

  Without a PMU in WSL2 this is not proven.
- **What it means for the numbers:**
  - Compare ratios within a run (every A/B below alternates its variants
    and takes medians).
  - The absolute numbers of a quiet day are lower: scale D and I rows by
    about 0.8 to compare with golden-operations.md.

---

## 2. New services (Group H)

Each service follows the functions plan's template: Why, API, Semantics,
How it works, Thread safety, Cost, Memory, Pitfalls. They follow its
principles (functions-plan Part II):
- new RPCs, never new fields on Put;
- failed conditions are answers, not errors;
- typed values updated in place;
- batches with per-item outcomes.

### H1. Compact values: counters, flags and small values at one cache miss

**Why.**
- **Most hot keys are small:**
  - counters (views, quotas, inventory);
  - flags (feature switches, "seen", "verified");
  - small ids (a session's user, a short link's target);
  - timestamps (last seen).
- **A record costs them a second dependent miss.** The store keeps every
  value in a malloc'd record, so a lookup reads the bucket, then the
  record. For an 8-byte value, that second miss is pure overhead.

**API.** No new read RPC. Compact keys are ordinary keys in a configured
key space:
```text
KVASM_COMPACT_PREFIXES="c:,flag:,cnt:"    # keys under these prefixes live in the compact table
```
- Get, GetMany, Put, Delete and Increment (functions-plan B1) work
  unchanged.
- **Typed limits:** a compact key is at most 16 bytes and its value at
  most 8 bytes. A Put that breaks either fails with `INVALID_ARGUMENT` and
  `reason = "compact-limit"`. It is never silently moved to the main
  table: a key lives in exactly one table, so a miss costs one probe.

**Semantics.**
- Get, Put, Delete and Increment keep their main-table meanings.
- A compact value's length (0-8) is stored, so Get returns exactly the
  bytes put.
- Versions (functions-plan §1.5) need a revision per entry: an 8-byte
  `aux` per entry would halve the entries per line. Compact keys
  therefore carry no version. CompareAndSet on them compares values,
  which for ≤ 8 bytes is a single `LOCK CMPXCHG`.

**How it works.**
- **Bucket:** 64 bytes, one cache line:

  | Field | Size |
  |---|---|
  | seqlock version | 8 B |
  | 2 tags | 2 × 1 B |
  | 2 key lengths | 2 × 1 B |
  | 2 value lengths | 2 × 1 B (spare bytes in the probe's layout) |
  | 2 entries of (16-byte key, 8-byte value) | 2 × 24 B |

- **Placement:** two-way cuckoo with partial-key alternates, as the store
  places records: the second bucket comes from the first and the tag, so
  a displacement never rehashes a key.
- **Lookup:** the batched pipeline prefetches both candidate lines, then
  compares tag and length, then the 16 key bytes in one SSE2 `PCMPEQB` +
  `PMOVMSKB`. The value is in the same line. Keys are compared exactly
  (ADR 0004).
- **Hashing:** the same SipHash-2-4-128 as the store, so P2's AVX2 lanes
  apply and the compact table could share the store's seed.
- **Memory without records:** nothing is malloc'd, so nothing needs QSBR
  reclamation. A delete clears a tag under the bucket's version.

**Thread safety.**
- **Readers** follow the store's bucket protocol:
  - read the version; spin with PAUSE while it is odd;
  - read the entries, then an acquire fence;
  - re-read the version, and retry if it changed;
  - a miss retries both buckets once more if either version moved, as the
    store does for cuckoo moves.
- **In-place updates take no lock:**
  - an Increment is `LOCK XADD` on the 8-aligned value;
  - a Put of an existing compact key is one aligned 8-byte store, with the
    length byte written first. A reader can see the new length with the
    old value for one instant; the length and value pair is linearizable
    only when the length doesn't change. Fixed-length typed values
    (counters, timestamps) never change it.
- **Structural changes** take the store's writer lock and bump the touched
  buckets' versions: creates, deletes and displacements.

**Cost (V, 10M keys, 16-byte keys, 8-byte values).**

| Regime | The store, same keys | Compact | Ratio |
|---|---|---|---|
| dependent chain | 349.2 ns | 189.9 ns | 0.54× |
| plain loop | 301.7 ns | 167.1 ns | 0.55× |
| batched | 53.1 ns | 37.2 ns | 0.70× |
| batched + 8-lane AVX2 hashing (P2) | — | **28.1 ns** | 0.53× the store's batched |
| increment, batched | 58.1 ns | 39.8 ns | 0.68× |

Every answer was checked (1,048,576 hits, 100,000 absent keys). The
increments were exact against per-key occurrence counts of the stream.

**Memory.** 54 B per key at 60% entry load, against 61 B in the store
(records + buckets) for the same keys. Fill: 2.5 s for 10M keys.

**Pitfalls.**
- **A typed key space.** An application that later needs a 9-byte value
  under a compact prefix must move to another prefix.
- **No versions.** Compact keys suit counters, flags and small ids, not
  documents that need optimistic concurrency.
- **Load.** Two-way buckets of two entries need about 40% headroom. The
  table grows by rehashing, under the writer lock, at 60% entry load.

### H2. Struct values: read or update one field

**Why.**
- **Profiles, sessions and carts** are small records with named fields.
  Today a client reads the whole value, edits it and writes it back:
  - two round trips;
  - the whole value on the wire, both ways;
  - and a race between two editors (lost update) unless it uses
    CompareAndSet.
- **A field-level API** moves the edit to the server, atomically, for one
  field's bytes.

**API.**
```proto
rpc Fields(FieldsRequest) returns (FieldsResponse);
message FieldsRequest { repeated FieldOp ops = 1; }   // 1 .. KVASM_MAX_WRITE_BATCH
message FieldOp {
  bytes key = 1;
  FieldOpKind kind = 2;
  repeated string names = 3;       // GET: the fields; SET / INCR / DEL: one
  bytes value = 4;                 // SET: at most 8 bytes
  sint64 delta = 5;                // INCR
}
enum FieldOpKind { FIELD_OP_UNSPECIFIED = 0; FIELD_GET = 1; FIELD_SET = 2; FIELD_INCR = 3; FIELD_DEL = 4; }
message FieldsResponse { repeated FieldResult results = 1; }
message FieldResult {
  FieldOutcome outcome = 1;        // APPLIED, NO_KEY, NO_FIELD, NOT_A_STRUCT, FULL, NOT_A_NUMBER
  repeated bytes values = 2;       // GET: one per name (empty when absent)
  sint64 number = 3;               // INCR: the value after
}
```
- A struct is created by a Put of a struct-encoded value (the client
  library encodes it). Get returns the encoded struct.

**Semantics.**
- **Fields:** up to 16 per struct (32 with an AVX2 tag compare); names
  ≤ 15 bytes; values ≤ 8 bytes (int64, float64, or a short string).
  Longer data belongs in its own key.
- **Fields never move once placed.** Every field operation is then one
  atomic step:
  - **FIELD_SET** of an existing field: one aligned 8-byte store;
  - **FIELD_INCR:** `LOCK XADD`, linearizable as counters are
    (golden-operations §4.1);
  - **FIELD_DEL:** clears the tag byte;
  - **FIELD_SET of a new field:** claims a free slot under the writer lock
    (adds are rarer than updates), writes the name and value, then
    publishes the tag byte with a release store. Readers see the field
    appear whole.
  - **A freed slot** is reused only after a grace period (QSBR), so a
    reader comparing an old name never meets a new one.
- **Growth:** a struct grows past its slots only by a whole-value Put,
  which replaces the record. A replace racing in-place updates wins, as
  for counters (functions-plan §2.3 rule 2).

**How it works.**
- **Layout** (offsets in the value, which the store keeps 8-aligned):

  | Bytes | Content |
  |---|---|
  | `[0..8)` | magic, field count |
  | `[8..24)` | 16 tags (CRC32C of the name, top byte, via SSE4.2 `CRC32`) |
  | `[24..56)` | 16 offsets |
  | 24-byte field records | `[name_len][name ≤ 15][value: 8 bytes at +16]` |

- **Lookup:** one `PCMPEQB` of the tag against all 16 lanes and
  `PMOVMSKB` (usually one candidate), then one name compare. Name and
  value share the field record, so a field costs the directory line plus
  one more line.
- **Pipeline:** the server's GET pipeline, plus two stages that keep 32
  lookups in flight:
  1. once the record is found, prefetch its directory;
  2. once the directory is read, prefetch the field record (for writing,
     for FIELD_INCR).

**Thread safety.** Lock-free reads, as for any record. Field writes are
single atomic instructions; adds and growth go through the writer lock.

**Cost (V, 1M struct keys, 440-byte values).**

| Operation | Time | vs the whole value |
|---|---|---|
| get the whole value (copied with glibc `memcpy`, as the server does) | 102.0 ns | 1× |
| HGET one field | **59.6 ns** (1.04 B) | 0.58× |
| HINCRBY one field | **62.5 ns** (1.09 B) | 0.61× (exact, checked) |
| the field lookup alone, value in L1 | 2.6 ns | — |

On the wire, HGET returns 8 bytes instead of 440. For a client, an
update becomes one round trip, not a read, edit and CompareAndSet loop.

**Memory.** The directory costs 56 bytes per struct.

**Pitfalls.**
- **Fixed slots.** A struct allocated with 16 slots holds 16 fields until
  a whole-value Put grows it.
- **Short fields.** Strings longer than 8 bytes do not fit a field.

### H3. Bitmaps with kept counts

**Why.**
- **One bit per user id per day:**
  - daily actives;
  - feature exposure;
  - "has seen this announcement";
  - attendance.
- **Retention questions:**
  - BITCOUNT over a range (how many of the first million users were
    active);
  - BITPOS (the first unset id);
  - AND-counts (active on both days).

**API.**
```proto
rpc Bits(BitsRequest) returns (BitsResponse);
message BitsRequest { repeated BitOp ops = 1; }
message BitOp {
  bytes key = 1;
  BitOpKind kind = 2;              // SET, CLEAR, GET, COUNT, POS
  uint64 bit = 3;                  // SET / CLEAR / GET; POS: start
  uint64 start_byte = 4;           // COUNT: range [start, end)
  uint64 end_byte = 5;
  bool want = 6;                   // POS: find a 1 (true) or a 0
}
message BitsResponse { repeated BitResult results = 1; }
message BitResult { BitOutcome outcome = 1; bool was = 2; uint64 number = 3; }
```

**Semantics.**
- **The value:** a fixed-size bitmap, chosen at creation (up to 128 MiB:
  10^9 bits), and a per-4 KiB block count.
- **SET / CLEAR** return the previous bit (`was`), so "first time seen" is
  one call.
- **COUNT** of any byte range is exact when no SET or CLEAR on that bitmap
  is in flight. Under concurrent flips it may be off by the in-flight
  updates, never more: the bit and its count are two atomics.
- **Exact under concurrency:** a per-bitmap sequence word adds two locked
  operations per flip and retries a racing COUNT. Not measured.

**How it works.**
- **SET:** `LOCK BTS` (GCC emits it for a fetch-or tested on the same
  bit) says whether this call flipped the bit. Only a flip pays two
  `LOCK XADD`s, on the block count and the total; a SET of an
  already-set bit counts nothing.
- **COUNT(range):**
  - the full blocks' counts are summed with AVX2 (8 per add);
  - each edge block is counted directly, or as its count minus the
    complement, whichever is shorter. At most 2 × 2 KiB of words is
    scanned.
- **POS:** whole 32-byte blocks of zeros are skipped with one `VPTEST`
  each, then `TZCNT` finds the bit. The block counts can skip 4 KiB
  blocks that are all zeros (not measured).
- **The popcount** is the AVX2 nibble lookup (`VPSHUFB` twice and
  `VPSADBW`, Muła's method). Long scans use Harley-Seal:
  - a tree of carry-save adders (`VPAND`, `VPOR`, `VPXOR`) turns 16
    vectors into one, and only that one pays the lookup;
  - it runs at 82 GB/s, against 44 for scalar `POPCNT`.

**Cost (V, 1 MiB bitmaps = 8.4M bits).**

| Operation | Scalar | SIMD / structure | × bar |
|---|---|---|---|
| SETBIT, plain | — | 3.3 ns `LOCK OR` | 0.06 B |
| SETBIT keeping counts | — | **10.1 ns** (50% of bits flipped) | 0.18 B |
| GETBIT | — | 0.6 ns | 0.01 B |
| BITPOS (next set bit, sparse) | 221.3 ns | **92.1 ns** `VPTEST` skip | 0.21 D |
| BITCOUNT, ranges averaging 341 KiB | scan 7.44 µs | **67.6 ns** block counts (110×) | 0.16 D |
| BITCOUNT, whole bitmap | 23.7 µs `POPCNT`; 20.1 µs lookup | 12.8 µs Harley-Seal (or the kept total: one load) | 29 D (scan) |
| AND-count of two bitmaps | 27.0 µs | 17.9 µs Harley-Seal, fused AND | 41 D |

The costs above exclude the key's lookup (~1 B). Counts were checked
exact after every SET pass, and 4,096 random ranges against scans.

**Memory.** 1 bit per id, plus 4 bytes per 4 KiB block (0.1%).
- **Sparse ids** (a few hundred set among 10^9): use Roaring containers
  instead (C). They are arrays below 4,096 members per 64 Ki-bit chunk,
  bitmaps above.

**Pitfalls.**
- **A fixed size.** Ids beyond it need a resize (a whole-value Put).
- **AND-counts of arbitrary pairs stay O(size)** (§6).

### H4. Small sets in a value

**Why.**
- **Small membership lists:** tags of an item, a user's groups, the
  features enabled for an account, the members of a small room.
- **Mutual counts:** "mutual friends" counts, and overlap of two tag sets.

**API.**
```proto
rpc Sets(SetsRequest) returns (SetsResponse);
message SetOp {
  bytes key = 1;
  SetOpKind kind = 2;              // ADD, REMOVE, IS_MEMBER, CARD, INTER_CARD
  repeated uint64 members = 3;     // ADD / REMOVE / IS_MEMBER
  bytes other_key = 4;             // INTER_CARD
}
```

**Semantics.**
- **Members** are unsigned integers (u32 or u64, chosen at creation): ids,
  not strings.
- **Writes copy the value.** ADD and REMOVE write a new sorted array
  through the writer path (a replace).
- **Reads see a consistent snapshot.** Records are immutable, so
  IS_MEMBER and INTER_CARD always read one version of each set.

**How it works.**
- **The value:** a sorted array, with the count in the header.
- **IS_MEMBER:**
  - up to ~256 members: an AVX2 scan, 8 members per `VPCMPEQD`,
    OR-reduced;
  - above that: a branchless binary search (`CMOV`).
- **INTER_CARD (Schlegel et al., C):**
  - 8 × 8 all-pairs blocks: eight `VPCMPEQD` over the rotations of one
    block (`VPERMD`), counted with `VMOVMSKPS` + `POPCNT`;
  - the block with the smaller maximum advances;
  - the tails finish with a scalar merge.

**Thread safety.** Lock-free reads of immutable records. Concurrent ADDs to
one set serialize in the writer lock: each is a replace.

**Cost (V).**

| Operation | Scalar | AVX2 | × bar |
|---|---|---|---|
| IS_MEMBER, 64 members | 15.5 ns branchless search | **2.1 ns** scan | 0.04 B |
| IS_MEMBER, 1,024 members | 32.5 ns | 32.8 ns (scan no longer wins) | 0.57 B |
| INTER_CARD, 1,024 × 1,024 | 1.64 µs branchy merge; 3.13 µs branchless | **552.8 ns** all-pairs (3.0×) | 1.27 D |

Costs are after the lookup, with the values in cache. The branchless
merge loses to the branchy one: its loop-carried chain (load, compare,
advance) is longer than the mispredicts it avoids.

**Memory.** 4 or 8 bytes per member.

**Pitfalls.** ADD copies the set: O(n) per write. Sets beyond a few
thousand members, or written often, belong in bitmaps (H3) or an indexed
structure.

### H5. Near-duplicate search (SimHash + multi-index hashing)

**Why.**
- **Deduplication** of listings, documents, tickets, product titles,
  images (a 64-bit perceptual hash).
- **Abuse:** spam and fraud campaigns that vary a few words.
- **Content reuse:** plagiarism and scraping detection.
- **Prior art:** Google deduplicated its crawl with 64-bit SimHash
  fingerprints and Hamming distance ≤ 3 (Manku et al., C).

**API.**
```proto
rpc FindNear(FindNearRequest) returns (FindNearResponse);
message FindNearRequest {
  bytes prefix = 1;                // an opted-in index (KVASM_NEAR_PREFIXES)
  fixed64 fingerprint = 2;         // a 64-bit SimHash / pHash, computed by the client
  uint32 max_distance = 3;         // 0 .. 3
  uint32 limit = 4;
}
message FindNearResponse { repeated NearMatch matches = 1; }
message NearMatch { bytes key = 1; uint32 distance = 2; }
```
- A key in an opted-in prefix carries its fingerprint, set with
  `SetFingerprint(key, fingerprint)` (a Mutate op), or as the value's
  first 8 bytes by convention.
  - [functions-plan.md](functions-plan.md) H5 proposes the convention, so
    that Put and Delete maintain the index and snapshots need no change.
  - It also adds `with_values` to the request and `truncated` to the
    response.

**Semantics.**
- **Exact:** every key whose fingerprint is within `max_distance` bits,
  and only those (no false negatives, no false positives).
- **Order:** results come back by distance, each key once.

**How it works.**
- **Index:** multi-index hashing (Norouzi, Punjani, Fleet, C):
  - split the 64-bit code into four 16-bit blocks;
  - keep four tables mapping each block value to the codes having it;
  - any code within distance 3 agrees with the query exactly on at least
    one block (pigeonhole). So the four posting lists of the query's
    blocks hold every answer: about 4 × n / 65,536 candidates (612 at
    10M).
- **Postings** hold the full 64-bit code and the key's id, so the filter
  needs no second lookup.
- **The filter, four codes per AVX2 step:**
  1. `VPXOR` with the query;
  2. nibble-lookup popcount (`VPSHUFB`, `VPSADBW`);
  3. `VPCMPGTQ` against the bound;
  4. `VMOVMSKPD`, then only the passing lanes are visited.
- **Each match is reported once:** a code also present in an earlier
  table, under the query's block there, was already reported, which is
  checked arithmetically with no set.

**Thread safety.**
- The probe's posting lists are static (built in 0.53 s).
- **A live index** appends to chunked lists: a release store of the chunk
  count publishes an entry, and readers scan lock-free.
- **A delete** marks the id with a tombstone; compaction is background
  work (RCU).
- **Inserts** cost about four cache-missing appends (E).

**Cost (V, 10M random fingerprints).**

| Method | Time | × bar |
|---|---|---|
| MIH, scalar `POPCNT` filter | 890.2 ns | 2.19 I |
| MIH, AVX2 filter | **695.0 ns** | 1.71 I |
| brute force, XOR + `POPCNT` over all 10M | 10.79 ms | 26,572 I |

Checked exact against brute force; every planted near-duplicate was found.

**Memory.** 48 B per fingerprint: 4 tables of (8-byte code, 4-byte id).
- **To 40 B:** keep only the 48 bits a list doesn't imply.
- **To 16 B:** keep ids only, at the price of a miss per candidate.

**Pitfalls.**
- **Real fingerprints cluster** (similar documents share blocks), so the
  posting lists of popular block values grow. Measure on real data first.
- **Wider distances need more blocks.** Distance d needs d + 1 blocks of
  64/(d + 1) bits. Distance 7 with 8 blocks of 8 bits scans n/256 per
  table: not golden at 10M.

### H6. Geo radius

**Why.**
- **Nearby search:** nearby drivers, stores, couriers, users.
- **Geofences** and delivery zones.
- **"What is around this point"** for maps and logistics.
- Redis offers the same over sorted sets of 52-bit geohashes (C).

**API.**
```proto
rpc GeoAdd(GeoAddRequest) returns (GeoAddResponse);       // key -> (lat, lon), batched
rpc GeoRadius(GeoRadiusRequest) returns (GeoRadiusResponse);
message GeoRadiusRequest {
  bytes index = 1;                 // the opted-in geo index
  double lat = 2; double lon = 3;
  double radius_m = 4;
  uint32 limit = 5;                // the synchronous budget (functions-plan §1.2)
}
message GeoRadiusResponse { repeated GeoHit hits = 1; bytes cursor = 2; }
message GeoHit { bytes key = 1; double distance_m = 2; }
```

**Semantics.**
- Every point within `radius_m` by the index's distance formula, which is
  equirectangular here (exact to well under 0.1% at city scale).
- Results are unordered unless `sorted` is asked. Beyond `limit`, a cursor
  continues.

**How it works.**
- **Codes:** coordinates quantized to 32 bits per axis, with the same
  meters per quantum on both axes (longitude scaled by the cosine of the
  region's mid latitude), so Morton cells are squares.
- **Morton code:** two `PDEP` (BMI2), one per axis; decoded with two
  `PEXT`. The 64-bit codes are kept sorted.
- **Query:**
  1. **Cover:** the radius's bounding box, quantized, fits in at most
     2 × 2 Morton cells of side 2^k, and each cell is one contiguous range
     of codes.
  2. **Seek** the four ranges at once, through a prefix table (P5): 2^20
     entries giving the first index of every top-20-bit prefix. That is
     two dependent misses per seek, all four seeks overlapped.
  3. **Scan** each range: `PEXT` both coordinates, test the box, then the
     exact distance.
- **Fallback:** Morton codes by shifts and masks ("magic bits") on CPUs
  without fast `PDEP`. AMD microcoded `PDEP` before Zen 3.

**Thread safety.**
- **Probe:** a sorted array, rebuilt in 0.6 s for 10M points.
- **Live updates** (E), either:
  - the ordered index (functions-plan §2.5) keyed by the Morton code;
    seeks there cost 71 ns batched (M);
  - or a static base with a small sorted delta merged in the background,
    readers RCU-safe.

**Cost (V, 10M uniform points over 445 × 190 km).**

| Radius | Results | Codes scanned | Binary-search seeks | Prefix-table seeks | × bar |
|---|---|---|---|---|---|
| 200 m | 15.0 | 82 | 1.07 µs | **697.5 ns** | 1.72 I |
| 1,000 m | 371.4 | 3,548 | 6.66 µs | 6.18 µs | 15.2 I (§6) |
| brute force | — | 10M | 8.18 ms | — | 20,143 I |

Checked exact against brute force for both seek methods.
- **Morton code:** `PDEP` 0.4 ns, shifts and masks 1.1 ns.

**Memory.** 28 B per point (code, id, lat, lon), plus the 4 MiB prefix
table.

**Pitfalls.**
- **Real points cluster.** In a city the 200 m cells hold far more points
  than uniform data does; the cost tracks the codes scanned.
- **Overscan.** The 2 × 2 cover scans 5.5× (200 m) to 9.6× (1,000 m)
  more codes than it returns. A 3 × 3 cover one level finer would scan
  about 3-5×, for nine seeks instead of four (E).

### H7. TopK (heavy hitters), measured

The functions plan's F3, priced there as an estimate; here measured.

**Why.**
- **Trending items** and searches.
- **Hot keys** for capacity planning.
- **The top abusers** by request volume.

**How it works.**
- **HeavyKeeper (Gong et al., C):** 2 rows × 4,096 buckets of
  (fingerprint, count), 64 KiB, so it stays in L2.
  - A bucket held by another key decays with probability 1.08^-count:
    heavy keys stay, light ones are evicted quickly.
- **The top 100** live in a min-heap. "Is this key in the heap" is 13
  AVX2 `VPCMPEQD` over the 100 ids, instead of the hash index a heap of
  keys usually needs.

**Cost (V).**
- **16.7 ns per update** (0.29 B), on a Zipf(1) stream of 1,048,576
  updates over 10M keys.
- **Accuracy:** all 100 reported keys truly are top-100, against exact
  counts. The count error is 0.1%.
- **Reading** is a copy of the 100-entry heap.

**Against the plan's estimate.** The plan priced Space-Saving at 50-100 ns
per add. HeavyKeeper is cheaper and keeps the same API.

**Thread safety.**
- **Per-worker sketches.** One sketch per worker is updated without
  atomics. A read merges the workers' heaps, re-estimating each candidate
  from every worker's sketch: ≤ 700 candidates × 7 workers × 2 buckets,
  tens of µs (E).
- **Alternatively, one sketch** under a one-byte spinlock, as the plan's
  F3 proposed.

### H8. Quantiles (DDSketch), measured

The functions plan's F4, priced there as O(buckets); here measured.

**How it works.**
- **Buckets:** 2,048 logarithmic buckets with γ = (1 + α)/(1 − α) and
  α = 1% (Masson, Rim, Lee, C), 16 KiB.
- **A record** is `ceil(log(x) / log γ)` and one increment, plus one more
  for the bucket's group of 64. That group count is kept on the write, so
  a quantile skips 64 buckets per step.

**Cost (V, 1,048,576 lognormal latencies).**

| Operation | Time | × bar |
|---|---|---|
| record a value (glibc `log` dominates) | **16.7 ns** | 0.29 B |
| quantile, walking all buckets | 102.2 ns | 0.23 D |
| quantile, through group counts | **16.2 ns** | 0.04 D |

- **Accuracy:** at p50, p90, p99 and p99.9 the worst error was 0.19%,
  against the 1% guarantee.
- **Both quantile methods return identical answers.**
- **Further speedup (C):** DDSketch's interpolated index mappings would
  replace `log` and cut the record cost further. Not measured.

**Thread safety.**
- Buckets and group counts are updated with `LOCK XADD`, so any number of
  writers is safe.
- A quantile is a snapshot within ± the in-flight updates.

### H9. Sliding-window counters

**Why.**
- **Smooth rate limits:** "at most 100 requests in any 60 seconds",
  without the burst at fixed-window edges (the sliding-window counter, C).
- **Live metrics:** "requests in the last minute", per key.

**API.** An Increment variant on a window-typed value:
`WindowAdd(key, delta)` returns the window's sum after the add.

**How it works.**
- **The value:** a ring of 64 buckets (u32 counts) and the epoch of the
  newest.
- **An add:**
  1. computes the current bucket from the clock;
  2. if that bucket's epoch is stale, zeroes it with one `LOCK CMPXCHG16B`
     on (epoch, count);
  3. `LOCK XADD`s the count.
- **The sum** widens and adds eight buckets per step (`VPMOVZXDQ` +
  `VPADDQ`).

**Cost (V for the kernel, E for the whole).**
- Summing 64 buckets: **4.5 ns** with AVX2 (17.2 ns scalar).
- An add costs about one in-place increment (58 ns batched, M) plus that
  sum.

**Thread safety.** In-place, lock-free (XADD, CMPXCHG16B). The sum is a
snapshot within ± in-flight adds.

### H10. Similarity scoring of stored vectors

**Why.**
- **Re-ranking:** score candidate items' small embeddings against a
  query.
- **Near-duplicate grading:** grade the near-duplicates H5 returns with
  MinHash signatures (Jaccard estimates).
- **Personalization:** a user vector · item vector.

**API.** `Score(query, keys[], kind)`: for each key, the dot product of its
stored vector with the query (int8 or fp32), or the MinHash agreement.
Keys are looked up with the batched pipeline.

**How it works, and its cost after the lookup (V).**

| Kernel | Scalar | AVX2 | AVX-VNNI |
|---|---|---|---|
| int8 dot, 256 dims (`VPMADDUBSW` + `VPMADDWD`; `VPDPBUSD`) | 61.3 ns | 3.2 ns | **1.5 ns** |
| fp32 dot, 256 dims (`VFMADD231PS`, 4 accumulators) | 71.9 ns | **5.2 ns** | — |
| MinHash agreement, 128 × u32 (`VPCMPEQD` + `VMOVMSKPS` + `POPCNT`) | 42.9 ns | **5.6 ns** | — |

- **VNNI needs accumulators.** One VNNI accumulator chains every
  `VPDPBUSD` behind the previous one's 5-cycle latency. Four make it 2.1×
  faster than the AVX2 pair.
- **The AVX2 int8 product saturates.** `VPMADDUBSW` saturates its 16-bit
  pairs, so it is exact only for inputs with |u·s| pair sums < 32,768;
  VNNI accumulates in 32 bits.
- **The lookup dominates.** Scoring a candidate costs a batched get plus
  ≤ 6 ns: about 1 B per candidate.

**Not this:** approximate nearest-neighbor search over all 10M vectors.
Index-based ANN (HNSW and kin) costs hundreds of µs and several times the
data in memory: not golden, and out of scope here as in the functions plan
(I.3).

---

## 3. Making the store itself faster

### P1. The compact tier

H1 is also the largest per-lookup speedup found for the store. For values
of ≤ 8 bytes it halves dependent-chain latency (0.54×). With P2 it brings
the batched get to 0.53× the store's.

The general form: **put small values in the line the lookup already
fetched.** A 128-byte bucket (two lines, which the adjacent-line
prefetcher fetches together) would hold four such entries, or two with
32-byte keys. Not measured.

### P2. SipHash-2-4 in AVX2 lanes

**What.** The batched GET hashes 32 independent keys per batch. SipHash
is serial within one message, but four messages fit in the four 64-bit
lanes of a ymm register:
- one round is `VPADDQ`, `VPXOR`;
- rotate by 13, 17 or 21: `VPSLLQ` + `VPSRLQ` + `VPOR` (AVX2 has no
  64-bit rotate);
- rotate by 32: one `VPSHUFD`;
- rotate by 16: one `VPSHUFB`.

**The tail without a library call.** Each key's last partial block is one
overlapping 8-byte load ending at the key's last byte, shifted down. It
stays inside the key, so it is safe at the end of any buffer.

**Two groups interleaved.** Eight keys run as two independent 4-lane
groups, instruction by instruction, so the vector ports stay busy while
one group waits on its rotate-xor chain.

**Cost (V, 20-21-byte keys).**

| Hash | ns per key | Batched get (paired, 5 alternations) |
|---|---|---|
| the store's assembly, scalar | 13.9 | 60.2 ns |
| 4 AVX2 lanes | 6.8 (0.49×) | 61.3 ns (1.018×, no gain) |
| **8 AVX2 lanes, two groups** | **5.6 (0.40×)** | **56.8 ns (0.943×)** |

- **Bit-exact** with the store's hash for all 1,048,576 query keys. No
  table, seed or protocol changes.
- **Where the pipeline is less memory-bound, the gain is larger:** the
  compact tier's batched get drops from 37.2 to 28.1 ns (−24%).
- **Fallback:** the scalar assembly, for CPUs without AVX2 and for groups
  whose keys differ in length class (len / 8).

**Where it goes.** The hash stage of the GET pipeline (ADR 0005), behind
the existing CPUID dispatch that picks the tag scan (ADR 0006).

### P3. Flat combining for writers

**The problem (M).** Concurrent writers today *lose* throughput: 3.35 M
puts/s from one thread, 2.25 M from seven. Every writer takes the store's
single writer mutex (ADR 0003), and the lock line and the store's lines
bounce between cores.

**The change (Hendler et al., C).**
- **Publish:** each writer publishes its request in a per-thread slot (one
  cache line) and tries the lock.
- **Combine:** whoever takes it becomes the combiner:
  1. it collects every pending request;
  2. prefetches all their buckets and old records (the write-batching
     gain, M);
  3. applies them;
  4. marks each slot done.
- **Wait:** other writers wait on their own slot.
- **Unchanged invariant:** only one thread ever writes the store, so ADR
  0003's correctness argument stands. What changes is who executes the
  writes, and that they arrive batched.

**Cost (V, 10M keys, replace-puts of 100-byte values, paired).**

| Threads | Mutex | Flat combining | Gain |
|---|---|---|---|
| 1 | 4.51 M puts/s | 4.44 M | 0.98× |
| 2 | 2.73 M | 3.86 M | 1.42× |
| 4 | 2.07 M | 3.43 M | 1.66× |
| 7 | 2.24 M | **4.29 M** | **1.91×** |

- **The collapse is gone:** seven writers reach the single-writer rate.
- **Combined batches are small here:** at most one request per thread,
  so ≤ 7. A server's connection batches (up to 32 writes per request)
  make each combined batch up to 7 × 32. The single-thread write
  batching measured 3.4× (M) is the gain left to collect (E).

**The waits** use P4.

**Alternatives.**
- **ffwd** (a dedicated server thread, C) delegates the same way, but
  pins a core.
- **Fine-grained bucket locks** were rejected in ADR 0003 (cuckoo
  displacement spans buckets).

### P4. UMONITOR / UMWAIT for waits

**What.** A waiting thread arms a monitor on the line it waits for
(`UMONITOR`) and sleeps in C0.1 (`UMWAIT`) until that line is written.
- No system call and no spinning.
- The wake-up is a cache-coherence event.

**Cost (V, one-way wake-up between two P-cores).**

| Wait | Wake-up | Waiting core |
|---|---|---|
| PAUSE spin | 114.4 ns | busy; on this core a PAUSE takes ~160 cycles (C) |
| **UMONITOR + UMWAIT** | **103.5 ns** (0.90×) | idle in C0.1 |
| futex wait / wake | 2.70 µs (23.6×) | asleep; two system calls |

**Where.**
- the flat-combining waiters (P3);
- writers waiting for the writer lock;
- any worker hand-off queue.

**Why it helps beyond latency.** An idle waiter gives its core's shared
resources to the sibling hyperthread. On this host that is often the
host's own work, or another vCPU.

**Fallback and limits.**
- **Detection:** CPUID WAITPKG, plus one trial `UMWAIT` at startup. A
  hypervisor may refuse it (#UD); the probe checks.
- **Without it,** waits use PAUSE.
- **Linux caps each sleep** (`/sys/devices/system/cpu/umwait_control`);
  the loop re-arms.

### P5. Prefix tables for seeks into sorted arrays

**What.** Over a sorted array of 64-bit keys, a table of 2^20 entries
gives, for every top-20-bit prefix, the index of the first key with it.
- A seek reads one entry (one miss), then searches a slice of about
  n / (prefixes used) keys: ~23 at 10M points, within 2-3 lines.
- That is two dependent misses instead of a binary search's ~10 deep
  levels.
- It is the simplest learned index (Kraska et al.; RadixSpline is the
  general form, C).

**Cost (V).** Four seeks per geo query, interleaved:
- 1.07 µs → 697.5 ns per query (1.53×);
- 19 ms to build at 10M;
- 4 MiB.

**Where else.**
- the sorted snapshot behind ordered queries (golden-operations §2.3,
  where the interleaved Eytzinger search measured 70.7 ns);
- timestamp-prefixed keys;
- any rank / seek on a sorted code array.

It is complementary to Eytzinger. It wins when keys are spread over their
prefixes, and loses on skewed prefixes (a slice can be long). A RadixSpline
bounds the slice length.

### P6. Keep aggregates on the write

**The rule.** Pay O(1) on the write to make an O(n) read O(1).

| Read | Kept on write | Write cost | Read: before → after |
|---|---|---|---|
| BITCOUNT of a range | per-4 KiB counts (H3) | +6.8 ns per SETBIT (10.1 vs 3.3) | 7.44 µs → 67.6 ns |
| BITCOUNT of all | the total | in the same +6.8 ns | 12.8 µs → one load |
| a quantile | group counts per 64 buckets (H8) | one more increment | 102.2 → 16.2 ns |
| an HLL estimate | the harmonic sum (M) | — | golden-operations §2.5 |
| Count / Rank | subtree counts | functions-plan §2.5 | O(n) → O(log n) |

---

## 4. The assembly toolbox, with measured fallbacks

Every SIMD path keeps a scalar or SSE2 path (ADR 0006); the scalar row is
measured in the same run.

| Technique | Instructions | Used in | SIMD | Fallback | Gain |
|---|---|---|---|---|---|
| lanes across independent messages | `VPADDQ VPXOR VPSLLQ VPSRLQ VPSHUFD VPSHUFB` | SipHash x8 (P2) | 5.6 ns | 13.9 ns (asm) | 2.5× |
| tag compare in one line | `PCMPEQB PMOVMSKB` (SSE2) | compact buckets (H1), struct directory (H2) | 2.6 ns field lookup | — (baseline x86-64) | — |
| nibble-lookup popcount | `VPSHUFB VPSADBW` | bitmaps, Hamming filter | 52 GB/s | `POPCNT` 44 GB/s | 1.2× |
| carry-save adder tree | `VPAND VPOR VPXOR` (Harley-Seal) | long BITCOUNT | 82 GB/s | 44 GB/s | 1.9× |
| zero-block skip | `VPTEST`, `TZCNT` | BITPOS | 92.1 ns | 221.3 ns | 2.4× |
| threshold mask | `VPCMPGTQ VMOVMSKPD` | MIH filter (H5) | 695 ns | 890 ns | 1.28× |
| all-pairs block compare | `VPCMPEQD VPERMD VMOVMSKPS POPCNT` | SINTERCARD (H4) | 552.8 ns | 1.64 µs | 3.0× |
| vector membership | `VPCMPEQD` OR-reduce, `VPTEST` | SISMEMBER ≤ 256, TopK heap (H7) | 2.1 ns | 15.5 ns | 7.4× |
| int8 dot | `VPDPBUSD` (AVX-VNNI), 4 accumulators | scoring (H10) | 1.5 ns | 61.3 ns; AVX2 3.2 | 41× |
| fp32 dot | `VFMADD231PS`, 4 accumulators | scoring (H10) | 5.2 ns | 71.9 ns | 14× |
| widen and add | `VPMOVZXDQ VPADDQ` | window sums (H9), count directories (H3) | 4.5 ns | 17.2 ns | 3.8× |
| bit deposit / extract | `PDEP PEXT` (BMI2) | Morton codes (H6) | 0.4 ns | 1.1 ns (magic bits) | 2.75× |
| test-and-set bit | `LOCK BTS` | SETBIT with counts (H3) | 10.1 ns | — | — |
| in-place add | `LOCK XADD` | increments (H1, H2), counts | 39.8 ns compact increment | — | — |
| field-name hash | `CRC32` (SSE4.2) | struct tags (H2) | — | table CRC | — |
| user-mode wait | `UMONITOR UMWAIT` (WAITPKG) | waits (P4) | 103.5 ns | PAUSE 114.4; futex 2.70 µs | 0.90× / 26× |
| prefetch for write | `PREFETCHW` | increments (H1, H2) | — | `PREFETCHT0` | — |
| staged prefetch pipelines | `PREFETCHT0` in 2-5 stages | every batched path; HGET (5 stages) | 59.6 ns HGET | — | — |
| branchless search | `CMOV` | seeks, set membership | — | branchy | — |

Notes:
- **AVX-512** would double the lanes of every row. This CPU has none, so
  nothing here is measured with it.
- **VAES, VPCLMULQDQ, GFNI and SHA-NI** are present. Nothing in Group H
  needed them:
  - VAES-based hashing was measured as an alternative to SipHash in the
    golden probe (M);
  - SHA-NI would suit content-addressed values (deduplicated blobs).
    Not measured.

---

## 5. Thread safety, collected

| Service | Readers | Writers | Linearizable? |
|---|---|---|---|
| H1 compact | seqlock per bucket, retry on odd or changed | XADD / 8-byte store in place; structure under the writer lock | yes, for a fixed value length |
| H2 struct fields | immutable directory and slots; fields never move | XADD / 8-byte store / tag byte in place; adds under the writer lock; slot reuse after a grace period | per field; a whole-value Put wins over racing field updates |
| H3 bitmaps | plain loads | `LOCK BTS/BTR` + count XADDs | per bit; counts are within the in-flight flips |
| H4 sets | immutable records | copy-on-write through the writer path | yes |
| H5 near-dup | lock-free scans of append-only chunks | appends with release stores; tombstones; background compaction | yes for single inserts (E) |
| H6 geo | the ordered index (OLC), or base + delta under RCU | the ordered index's writers | as the ordered index |
| H7 TopK | merge of per-worker sketches | each worker its own sketch | approximate by design |
| H8 quantiles | snapshot of counts | XADD | approximate by design (±1% values) |
| H9 windows | snapshot sum | CMPXCHG16B (epoch reset) + XADD | per add |
| H10 scoring | as Get | — | as Get |
| P3 combining | — | one combiner at a time; release/acquire on slot states | unchanged: one writer |

---

## 6. Not golden, and the way out

- **BITCOUNT of a whole bitmap by scan** costs 12.8 µs per MiB (29× D),
  memory-bandwidth-bound at 82 GB/s, so no instruction trick will make it
  golden. The kept total (P6) makes it one load.
- **AND-count of two bitmaps** costs 17.9 µs per MiB pair (41× D), and
  every word of both is read. The ways out:
  - for *registered* pairs, keep the intersection count on each SETBIT:
    test the other bitmap's bit, then one XADD;
  - for sparse sets, Roaring containers (C) intersect arrays at the
    speed of their members, not their range;
  - otherwise it is in the 1000× tier, with an honest synchronous budget.
- **Geo radius with hundreds of results** (1,000 m: 371 results, 6.18 µs
  = 15.2× I) is output-bound: 16.6 ns per result, including the 9.6×
  overscan. A `limit` with a cursor keeps every call golden. A finer
  cover cuts the overscan.
- **Near-duplicates at distance > 3, or on clustered fingerprints:**
  candidates grow quickly with the distance (H5 pitfalls). Measure on
  real data before promising a budget.
- **Set intersections of large sets** are O(n) by nature. Use bitmaps or
  Roaring.
- **ANN search over 10M vectors:** not golden, out of scope.

---

## 7. Recommendations, by value for cost

1. **Flat combining + UMWAIT waits (P3, P4).**
   - Fixes the writer collapse: 1.91× at seven threads.
   - Touches only the writer lock module (wlock.asm). The store's
     correctness argument is unchanged.
   - Then layer the measured write batching (3.4×, M) into the combiner.
2. **8-lane SipHash in the GET pipeline (P2).**
   - −5.7% on every batched get, −24% in the compact tier.
   - Bit-exact; a contained change behind CPUID dispatch.
3. **The compact key space (H1 / P1):** gets and increments of small
   values at 0.53-0.70× the store, with less memory. The cost is a second
   table and prefix routing.
4. **Sketches as typed values (H7, H8, H9).** All measured golden; they
   slot into the plan's Phase 5. TopK and quantiles now have measured
   prices.
5. **Struct fields (H2) and bitmaps with kept counts (H3).** Field-level
   updates remove read-modify-write round trips; bitmaps are a compact
   analytics primitive.
6. **Prefix tables (P5)** wherever the store keeps sorted snapshots.
7. **Opt-in indexes: near-duplicates (H5) and geo (H6).** Golden at 10M
   on uniform data. Measure on the customer's real data before the gate.
8. **Small sets (H4) and scoring (H10).** Cheap kernels; worth it when a
   customer needs them.

Each would land with its own ADR and measurement gate, in the functions
plan's phases ([functions-plan.md](functions-plan.md) §V.10, which also
gives every function its API names, usage and gates):

| Phase | Gets |
|---|---|
| S (store speedups, any time, no API) | P2, P3, P4 |
| 5 (analytics and typed values) | H2, H3, H4, H9, H10; H7 and H8 are the plan's F3 and F4 |
| 6 (opt-ins) | H1, H5, H6 |

---

## 8. Sources

| Topic | Source |
|---|---|
| AVX2 popcount, Harley-Seal | Muła, Kurz, Lemire, "Faster Population Counts Using AVX2 Instructions", The Computer Journal 61(1), 2018; arXiv [1611.07612](https://arxiv.org/abs/1611.07612) |
| Multi-index hashing | Norouzi, Punjani, Fleet, "Fast Exact Search in Hamming Space with Multi-Index Hashing", CVPR 2012; arXiv [1307.2982](https://arxiv.org/abs/1307.2982) |
| SimHash, Hamming ≤ 3 | Manku, Jain, Das Sarma, "Detecting Near-Duplicates for Web Crawling", WWW 2007, [research.google.com/pubs/archive/33026.pdf](https://research.google.com/pubs/archive/33026.pdf) |
| Flat combining | Hendler, Incze, Shavit, Tzafrir, SPAA 2010, [PDF](https://people.csail.mit.edu/shanir/publications/Flat%20Combining%20SPAA%2010.pdf) |
| Delegation (ffwd) | Roghanchi, Eriksson, Basu, SOSP 2017, [PDF](https://www.cs.uic.edu/~jakob/papers/sosp17_draft.pdf) |
| SIMD set intersection | Schlegel, Willhalm, Lehner, ADMS 2011, [PDF](https://www.adms-conf.org/p1-SCHLEGEL.pdf); Lemire, Boytsov, Kurz, arXiv [1401.6399](https://arxiv.org/abs/1401.6399) |
| Roaring bitmaps | Chambi, Lemire, Kaser, Godin, Software: Practice and Experience 46, 2016, arXiv [1402.6407](https://arxiv.org/abs/1402.6407); Lemire et al., arXiv [1709.07821](https://arxiv.org/abs/1709.07821) |
| HeavyKeeper | Gong et al., USENIX ATC 2018, [usenix.org/conference/atc18/presentation/gong](https://www.usenix.org/conference/atc18/presentation/gong) |
| Space-Saving | Metwally, Agrawal, El Abbadi, ICDT 2005, [UCSB TR 2005-23](https://www.cs.ucsb.edu/research/tech-reports/2005-23) |
| DDSketch | Masson, Rim, Lee, VLDB 2019, arXiv [1908.10693](https://arxiv.org/abs/1908.10693) |
| Learned indexes | Kraska et al., SIGMOD 2018, arXiv [1712.01208](https://arxiv.org/abs/1712.01208); Kipf et al., "RadixSpline", aiDM 2020, arXiv [2004.14541](https://arxiv.org/abs/2004.14541) |
| Morton codes with BMI2 | J. Baert, [forceflow.be, 2016](https://www.forceflow.be/2016/11/25/using-the-bmi2-instruction-set-to-encode-decode-morton-codes/) |
| Redis GEO (52-bit geohash on sorted sets) | [redis.io GEOADD](https://redis.io/docs/latest/commands/geoadd/) |
| UMWAIT | [felixcloutier.com/x86/umwait](https://www.felixcloutier.com/x86/umwait) |
| PAUSE latency on Golden Cove (140 → 160 cycles) | AnandTech, [i9-12900K review, p.5](https://www.anandtech.com/show/17047/the-intel-12th-gen-core-i912900k-review-hybrid-performance-brings-hybrid-complexity/5) |
| VPDPBUSD latency, accumulators | [uops.info](https://uops.info/table.html); [sora5801/avx512-vnni-int8-dot](https://github.com/sora5801/avx512-vnni-int8-dot) |
| Sliding-window counters | [Arcjet, rate-limiting algorithms compared](https://blog.arcjet.com/rate-limiting-algorithms-token-bucket-vs-sliding-window-vs-fixed-window/) |
| b-bit minwise hashing | Li, König, [NIPS 2010 PDF](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/NIPS10_hashing.pdf) |
| In-place updates, HybridLog | Chandramouli et al., "FASTER", SIGMOD 2018, [PDF](https://www.microsoft.com/en-us/research/uploads/prod/2018/03/faster-sigmod18.pdf) |
| Partitioned KV design | Lim et al., "MICA", NSDI 2014, [usenix.org](https://www.usenix.org/conference/nsdi14/technical-sessions/presentation/lim) |
| SIMD control-byte tables | [Abseil Swiss tables](https://abseil.io/about/design/swisstables); [Folly F14](https://github.com/facebook/folly/blob/main/folly/container/F14.md) |

---

## 9. Limits, and how the probe was kept honest

- **One machine:** i9-13900K under WSL2, P-cores, no PMU, so no
  cache-miss counters. Host load moves absolute latency (§1); ratios
  within a run are the claim.
- **Synthetic data:**
  - uniform keys (TopK uses Zipf(1));
  - uniform geo points;
  - random fingerprints.

  Real clustering makes H5 and H6 scan more. Each says so.
- **Prototypes:** C next to the store, not store code. Thread safety is
  designed and reasoned (§5), not stress-tested here; the combining rows
  are the only concurrent ones.
- **Four probe bugs, found and fixed before the certified run**
  (benchmarks README):
  1. **A variable-size value copy compiled to `rep movsq`.** It added
     about 25 ns to every batched get, which flattered every ratio to B by
     about 30%. A control run of the golden probe exposed it (B 56-64 vs
     83 ns).
  2. **Pure kernels hoisted out of timing loops** (a 1 MiB popcount read
     4.5 TB/s). Fixed with a compiler barrier per iteration.
  3. **Scalar references auto-vectorized and inlined while the SIMD ones
     were called.** Fixed with `noinline` on both sides and
     `no-tree-vectorize` on the scalar side.
  4. **Two avoidable SIMD slowdowns:** a per-key `memcpy` call for the
     hash tail, and a single VNNI accumulator.
- **Certification and the VM:** the run's window was certified with
  `docker events`.
  - The idle check now reads only `/proc/<pid>/stat`: `ps` hangs on
    processes stuck in D state in the VM since 2026-09-29.
  - Those stuck processes also hold the VM's load average at 6 or more,
    so it cannot serve as an idle signal.
  - **A VM restart (the user's call) would clear them.**

---

## 10. Open decisions

1. **Compact keys:** a typed key space by prefix (proposed), or a
   transparent tier with migration? Migration costs a second probe on
   misses, or a per-key move protocol.
2. **Struct capacity:** 16 slots (one SSE2 compare) or 32 (one AVX2
   compare). Growth only by whole-value Put (proposed), or a copy that
   coordinates with in-place updaters?
3. **Bitmap COUNT under concurrency:** within the in-flight flips
   (proposed), or exact via a per-bitmap sequence word (two more locked
   ops per flip)?
4. **TopK:** HeavyKeeper (measured) or Space-Saving (the plan's choice,
   with error bounds per item)?
5. **The geo index:** the ordered index keyed by Morton code, or a
   base + delta array with a prefix table? It depends on the write rate.
6. **Near-duplicate fingerprints:** a dedicated `SetFingerprint`, or a
   convention on the value's first 8 bytes? The functions plan proposes
   the convention, for geo points too (its §V.13 decision 11).
