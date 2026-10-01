# Proposal: endpoints from the storage design — how KvNode hashes and stores, where SIMD pays, and what to offer next

- **Status:** proposed, 2026-10-01. Nothing here is implemented or decided.
- **The ask:** revisit how KvNode's hashing and storage work and how SIMD
  is usable, then figure out more endpoints.
- **What this adds:**
  - **Hashing and storage**, re-read from the code with file references
    (§1, §2), and **where a request's time goes** (§3).
  - **Where SIMD is used today and where it can pay**, stage by stage, with
    measurements (§4). The finding: a GET is memory-bound, so SIMD inside
    the lookup shaves little. Its leverage is on values, which the server
    never computes on today.
  - **Twelve endpoints, Group J (§5):** what the bucket and record layout
    makes cheap, and what SIMD makes cheap on values.
    [functions-plan.md](functions-plan.md) Group J has their API,
    semantics, usage, gates and phases.
  - **Why the letter J:** the plan's Part I numbers its sections I.1-I.3,
    so this group skips the letter I.
- **Companions:**
  - [golden-operations.md](golden-operations.md) (M);
  - [golden-services.md](golden-services.md) (V: Group H, Track S);
  - [functions-plan.md](functions-plan.md) (the functions, phases and
    gates).
- **Evidence:**
  [benchmarks/results/2026-10-01-endpoints-probe](../../benchmarks/results/2026-10-01-endpoints-probe/README.md).
  The run is certified, and every answer is checked against a reference.

  | Label | Source |
  |---|---|
  | **N** | measured by this endpoints probe, 10M keys |
  | **V** | the services probe |
  | **M** | the golden probe |
  | **E** | estimated from measured parts |
  | **C** | cited (§8) |

## Summary

**The bars of this run (N):** D 411.4 ns, I 369.5 ns, B 57.5 ns. They
are in line with the services probe the same day.

**How a GET runs today:**
1. **Parse** the request.
2. **Hash** the key (SipHash-2-4-128, scalar, 12.7-13.9 ns).
3. **Prefetch** both candidate buckets.
4. Once the read's batch is parsed, **prefetch** the candidate records.
5. **Probe:** one SSE2 compare of 12 tags, then an exact key compare.
6. **Copy** the value once into the HTTP/2 output buffer.

That is two dependent cache misses (bucket, record) whose latency the batch
overlaps.

**SIMD today:** the SSE2 tag compare (ADR 0006), CRC-32C folding with
VPCLMULQDQ for Upload and Download, and HPACK literal copies. Everything
else is scalar, and it does not matter much: compute is a small slice of a
GET.
- The measured lever, 8-lane AVX2 hashing, gains 5.7% (V).
- **SIMD's real leverage is on values:**
  - JSON fields at 2.4-2.8 GB/s;
  - SHA-256 at 2.5 GB/s;
  - substring filters at 3.9-19.6 GB/s;
  - array aggregates 2.7-4.6× scalar.

  Values are opaque today, so every such need ships the whole value to a
  client.

**Twelve new endpoints (Group J).**

| # | Endpoint | Where | Why the design makes it cheap | Cost (N unless noted) | × bar |
|---|---|---|---|---|---|
| J1 | Value metadata (length, type) | GetMany `with_length` | lengths sit in the record header | 58.8 ns batched (1.00× a get) | 1.02 B |
| J2 | Slices of values | GetMany `range`; Download for one large value | the value is contiguous in the record | 58.7 ns for 16 bytes | 1.02 B |
| J3 | Conditional reads (ETags) | GetMany `if_tags` | CRC-32C folding at 88-91 GB/s, or the version in O(1) | 67.5 ns when unchanged, no value bytes | 1.17 B |
| J4 | Snapshot reads | GetMany `consistent` | bucket versions are seqlocks: a double collect validates a multi-key read | 1.01× plain GetMany; 0 torn reads in 200,000 under a racing writer | 1.19 B |
| J5 | Iterate by hash shard | `Iterate` | the table never rehashes: a bucket index is a stable cursor, a bucket range a hash shard | 54 M keys/s per thread; 340 M keys/s on 7 threads | 0.05-0.32 B |
| J6 | Max / min registers | Increment `op` | values are 8-aligned in-place cells | 43.1 ns, mostly unchanged registers (0.91× an increment); ≈ 1.0× when raising (E) | 0.75 B |
| J7 | Flag words | Increment `op` | the same cells, 64 flags per word | 47.1 ns (1.00×) | 0.82 B |
| J8 | Counter arrays | `Arrays` | in-place `LOCK XADD` at an index; AVX2 aggregates | add 71.7 ns; sum of 168 cells 116.4 ns | 1.25 / 2.02 B |
| J9 | JsonGet | `JsonGet` | a SIMD structural scan of the record's value | late field of 668 B 253 ns; of 5 KB 1.75 µs; early fields 31-80 ns | 0.08-4.73 I |
| J10 | Content-addressed blobs (and Bazel's CAS) | `PutBlob`; Exists; an opt-in REAPI service | SHA-NI hashing at 2.5 GB/s; existence is the cheapest lookup | PutBlob 4 KiB 1.77 µs; 33.8 ns per digest checked | 4.79 I; 0.59 B |
| J11 | Value filters | Iterate and Export | SIMD substring and JSON predicates, streamed | 3.9-19.6 GB/s (2.5-3.9× memmem) | paged |
| J12 | BulkLoad | the client, over pipelined Mutate | write batching and flat combining (M, V) | ≈ 126 ns per key (M) | — |

**Four things the measurements teach:**
1. **Metadata and slices cost a get** (1.00×). Their value is on the wire
   and at the client, not in the store.
2. **A consistent multi-key read is nearly free** (1.01×), because the
   bucket versions the readers already check are seqlocks. Under a racing
   writer, 11% of plain 16-key reads were torn and none of the validated
   ones.
3. **Iterating without an index is fast:** 340 M keys/s on 7 threads, all
   10M keys in 29 ms. Buckets stream in order; only the records are
   random.
4. **Value work fits the golden budget.** Extracting a late field from a
   ~1 KB JSON document costs 3.34× sending the whole document, on the
   server, and saves the client the bytes and the parse.

---

## 1. How hashing works

- **The function:** SipHash-2-4 with a 128-bit output
  ([server/src/siphash.asm](../../server/src/siphash.asm), `kv_siphash128`
  at line 61).
  - It is keyed with a per-process secret seed from `getrandom`
    (`Table.seed0/1`, used by `kv_store_hash` in store.asm line 380).
  - **Why keyed:** clients choose keys, so a keyed pseudo-random function
    keeps bucket occupancy random whatever arrives (no hash flooding, ADR
    0004).
  - **Why scalar:** it is pure 64-bit add, rotate and XOR. The file keeps
    the server's "no SIMD requirement" rule.
  - **Safe at the end of a buffer:** reads never pass the key; the tail is
    an overlapping 8-byte load, or 4/2/1-byte loads.
- **One pass, two words:**
  - **h1** picks the primary bucket: `b1 = h1 & mask`.
  - **h2** gives a one-byte tag: `tag = h2 >> 56`, with 0 reserved for
    empty slots.
  - **The alternate bucket** comes from the bucket and the tag alone:
    `b2 = b1 ^ ((((tag × 0x9E3779B97F4A7C15) >> 32) | 1) & mask)`
    (cuckoo.asm `LOCATE`, line 102).
  - This is partial-key cuckoo hashing, as in MemC3 (C). A relocation
    needs only bucket lines, never the key. The offset is odd, so
    `b2 ≠ b1` and `alt(alt(b)) = b`.
- **When:** at parse time, for every request (grpc.asm line 670), before
  the GET batch runs. A GET's two buckets are prefetched right away (line
  730).
- **Cost:** 12.7-13.9 ns for a 20-21-byte key (M, V): 23% of a batched get
  (M) and 18% of a whole GET's user-space instructions (§3).
  - Eight keys hash in 5.6 ns each in AVX2 lanes, bit-exact (V, Track S).
- **Exactness:** keys are compared byte for byte (`kv_keyeq`, util.asm line
  33), never by hash (ADR 0004). Clients send a key longer than 64 bytes as
  `0xFF ‖ SHA-512(key)`, so no key the server sees exceeds 65 bytes.
- **What it means for endpoints:**
  - **The secret seed keeps placement private.** Clients cannot compute
    where a key lives; a cluster must route with its own hash.
  - **A bucket range is a hash range.** That is a free shard key for
    parallel iteration (J5).
  - **Every request already carries its 128-bit hash** from parse time, so
    anything keyed by the hash costs nothing extra.

## 2. How storage works

- **The table** ([server/include/kvnode.inc](../../server/include/kvnode.inc),
  `struc Table`, line 100):
  - a 64-byte-aligned bucket array in an anonymous NORESERVE mapping, with
    transparent huge pages, prefaulted at creation (store.asm line 117);
  - `nbuckets = next_pow2(max(2, ceil(capacity / 5)))` (store.asm line
    159), so at most ~83% of the 6-slot buckets are ever used;
  - **the capacity is fixed at creation:** the table never rehashes.
  - The header is grouped by writer, so readers never share a line with
    state that writers update on every operation. Immutable fields, the
    writer mutex, the QSBR epoch, memory accounting, the writer's
    statistics and the Download pins each sit on their own cache lines.
- **A bucket** (`struc Bucket`, line 76) is one 64-byte line:
  - an 8-byte version (a seqlock: odd while the writer changes it);
  - 8 tag bytes, 6 used;
  - 6 record pointers.
- **A record** (`struc Rec`, line 61):
  - `next` and `aux` (writer-only), `key_len`, `val_len`;
  - the key, zero-padded to 8, then the value.
  - It is malloc'd and immutable once published.
  - Memory is reserved with `LOCK XADD` on `Table.mem_used` before the
    malloc (store.asm line 463), so concurrent writers cannot overshoot the
    limit.
  - The value is 8-aligned, which is what makes in-place cells possible.
- **A read** (`kv_cuckoo_get_sse2`, cuckoo.asm line 358) is lock-free:
  1. both versions must be even (PAUSE while odd);
  2. both tag words go in one XMM register, and one `PCMPEQB` + `PMOVMSKB`
     matches all 12 slots;
  3. each candidate: slot non-zero, same `key_len`, `kv_keyeq`;
  4. a hit is final;
  5. a miss re-checks both versions and retries if either moved, because a
     relocation may have been moving the key.
  - QSBR keeps every record a reader can hold alive until the reader's next
    quiescent state.
- **A write** (`kv_store_put`, store.asm line 432):
  1. allocate and fill the record **outside** the lock;
  2. take the futex mutex (wlock.asm);
  3. `kv_cuckoo_upsert` (line 449). Records are published only inside an
     odd-version window, so readers never accept a half-updated bucket:
     - **replace:** one slot store;
     - **insert:** into a free slot of either bucket;
     - **otherwise:** a breadth-first relocation search bounded by
       `MAX_BFS = 256` buckets (`cuckoo_bfs_insert`, line 710). Each move
       makes both buckets odd, copies the record into the hole, clears the
       old slot, makes both even, and counts itself in `Table.st_moves`, so
       a key is never in neither bucket.
  4. retire the replaced record to limbo, stamped with the epoch;
  5. free up to `RECLAIM_BATCH = 8` records whose grace period has ended.
- **Downloads pin a record** across event-loop passes (a pin table), so
  reclamation defers it instead of freeing it under the transfer.
- **The server's GET pipeline** (grpc.asm):
  1. hash and prefetch at parse;
  2. after the read, `kv_grpc_run_batch` (line 936) prefetches every
     batched GET's candidate records, three lines each;
  3. `get_execute` (line 1004) looks up and frames each response.

  The value is copied once, by `memcpy`, into the output buffer. A GET
  allocates nothing.
- **Per-worker L1/L2 record caches** (`hcache.asm`) sit in front of the
  table when configured. They were built, measured slower, and are off by
  default (ADR 0006).
- **PUTs and DELETEs are not batched:** each takes the mutex at parse time.
  That is why write batching (M) and flat combining (V) are the write-side
  levers.

## 3. Where a request's time goes

| Measure | Value | Source |
|---|---|---|
| one GET on one core (quiet host, busy-polling client) | 269 ns of server time: 139 ns user, 130 ns kernel | [store-lookup](../../benchmarks/results/2026-09-27-store-lookup/README.md) |
| the store's part of it | 69 ns inside the server; 43.5 ns alone (SipHash ~12, compute ~9, memory ~23) | the same |
| user-space instructions per GET | 1,425: HTTP/2 parsing 20%, SipHash 18%, response framing 16%, stream lifecycle 15%, store lookup 11%, gRPC and protobuf 11% | [second-pass](../../benchmarks/results/2026-09-27-second-pass/README.md) (callgrind) |
| an exact get at 10M keys | dependent 411.4 ns; plain loop 369.5 ns; batched pipeline 57.5 ns | N |
| the hash | 12.7-13.9 ns scalar; 5.6 ns in 8 AVX2 lanes, which makes a batched get 0.943× | M, V |
| the 100-byte value copy | ≈ 0: Stat, which skips it, costs 1.00× a get | N |

**Instructions are not time.**
- SipHash runs at a high IPC. The store's ~160 instructions cost ~60 ns in
  stalls: two dependent misses, which batching overlaps.
- The rest of a GET is protocol and kernel: HTTP/2, gRPC framing, system
  calls.

So:
- **Inside the lookup,** only the hash is compute worth vectorizing, and it
  is (Track S).
- **Faster GETs** come from fewer misses and less protocol work:
  - the compact tier (V);
  - huge-page records (M);
  - fewer wake-ups and system calls per request (ADR 0009).

  Wider registers do not help.
- **SIMD's leverage is in work on values.** Today that work happens on
  clients, after the whole value has crossed the network.

## 4. SIMD: where it is used, and where it pays

| Stage | Work | Today | SIMD option | Effect |
|---|---|---|---|---|
| HTTP/2 and HPACK | frame and header parsing | scalar; 16-byte `MOVDQU` literal copies | Huffman decode tables | small headers, little to win (E) |
| protobuf decode | varints, fields | scalar | SIMD varint decoding for big batches | a few ns per key in a 1,024-key GetMany (E) |
| hash | SipHash-2-4-128 | scalar ARX | 8 AVX2 lanes, two interleaved groups, bit-exact | 0.40× the hash; batched get −5.7% (V) |
| bucket probe | 12 tags | SSE2 `PCMPEQB` + `PMOVMSKB` | AVX2 over two keys at once | nothing: the probe waits on memory |
| key compare | ≤ 65 bytes after compaction | 8-byte words | one 32-byte `VPCMPEQB` | nothing measurable |
| value copy | 0.1-1024 KiB | glibc `memcpy` (AVX2, ERMS) | already SIMD | — |
| integrity | CRC-32C | folding with `VPCLMULQDQ`: 88-91 GB/s (N); SSE4.2: 15-25 GB/s | — | an ETag on the fly costs 2.8 ns per 100 B (N) |
| writes | malloc, copy, mutex, upsert | scalar | flat combining (not SIMD) | 1.91× at 7 writers (V) |
| iteration (new) | buckets in order, records random | — | an SSE2 occupancy mask per bucket | memory-bound: 54 M keys/s per thread (N) |
| JSON (new) | find a field | — | structural scan: `VPCMPEQB` ×6 per 32 bytes, `PCLMULQDQ` prefix XOR for strings, `TZCNT`/`BLSR` | 1.6-2.3× a scalar tokenizer; 2.4-2.8 GB/s (N) |
| content hashes (new) | SHA-256 | — | `SHA256RNDS2`, `SHA256MSG1/2` | 2.5 GB/s, 6.3-6.5× scalar (N) |
| value filters (new) | find bytes | — | first/last-byte candidates, 32 per step | 3.9-19.6 GB/s, 2.5-3.9× memmem (N) |
| aggregates (new) | sum / max of int64 | — | `VPADDQ`; `VPCMPGTQ` + `VPBLENDVB` | 4.6× / 2.7× scalar (N) |

**Fallbacks (ADR 0006).** Every row keeps a scalar path, measured next to
it:
- SipHash: the scalar assembly;
- CRC-32C: four modes, chosen at startup;
- JSON: the scalar tokenizer, with the same walker;
- SHA-256: the FIPS code;
- substring search: memmem;
- aggregates: scalar loops.

**AVX-512** (absent on this CPU) would widen every lane count. It would
also add `VPMAXSQ` for the max, `VPCOMPRESSB` to collect JSON structurals,
and `VPOPCNTQ` for popcounts.

## 5. The endpoints (Group J)

The plan's Group J has each function's API, semantics, usage and gates.
Here: why the storage design makes each cheap, what was measured, and the
design points the measurements raised.

### J1. Value metadata: lengths and types without the value

- **Where:** GetMany with `with_length`, alone or with `keys_only`.
- **Why the design makes it cheap:**
  - `key_len` and `val_len` live in the record header;
  - a typed value's 4-byte magic leads its value, inside the three record
    lines the pipeline already prefetches (keys never exceed 65 bytes);
  - with revisions (functions-plan §1.5) the version lives in `Rec.aux`.
- **Cost (N):** 58.8 ns batched, 1.00× a get of a 100-byte value. The
  lookup is the cost.
- **Saves:** for a 1 MiB value, the whole transfer. In Redis terms:
  `STRLEN`, `TYPE`.

### J2. Slices of values

- **Where:**
  - **one large value:** Download already takes `offset` and `length`
    (ADR 0010), streamed and CRC-checked;
  - **many small values:** GetMany with `range` brings the same slice to
    batches.
- **Why the design makes it cheap:** the value is contiguous in its record,
  so a slice is an offset and a copy.
- **Cost (N):** 58.7 ns for 16 bytes at offset 40, batched.

### J3. Conditional reads with ETags

- **Where:** GetMany with `if_tags`. A key whose tag still matches comes
  back `unchanged`, with no value bytes. The client gets tags from an
  earlier read (`with_tags`).
- **Two kinds of tag, because versions miss in-place updates:**

  | Tag | Computed | Cost | Sees |
  |---|---|---|---|
  | CRC-32C of the value, with its length (default) | on the fly, with the folding assembly | 2.8 ns per 100 B; 720 ns per 64 KiB (N) | every change, including in-place counters, fields and bits |
  | the version (Phase 0) | read from the header | O(1) | only Put-class writes: in-place updates keep the version (functions-plan §2.3 rule 5) |

- **Cost (N):** 67.5 ns when unchanged, 1.12× a get, with 0 value bytes
  sent.
- **Collisions:**
  - A changed value of the same length keeps its CRC with probability
    2^-32.
  - CRC-32C detects every change confined to 32 consecutive bits, so a
    counter's small change is always seen.
- **Storing the CRC at write time** would make the default tag O(1) too. It
  is the change ADR 0010 already names as the next lever for downloads: 4
  bytes per record, and Upload computes the CRC anyway.

### J4. Snapshot reads: a consistent multi-key read

- **Where:** GetMany with `consistent`.
- **How (a double collect, Afek et al., C):**
  1. Read the table's **atomic-batch sequence word**, then every key's two
     bucket versions, all even.
  2. Read the records.
  3. Re-read the versions and the sequence word.
  4. **All unchanged:** no write touched these buckets during the window,
     and no atomic batch ran, so the values coexisted. Otherwise retry.
- **Why the sequence word.** An atomic batch (A8) applies its mutations one
  by one under the writer lock, so each bucket's version alone could
  validate a window that falls between two of its mutations. Atomic
  batches make the word odd for their whole duration. They are rare, so
  readers almost never retry on it.
- **Why the design makes it cheap:** the bucket versions are the seqlocks
  the readers already check, and the second collect reads lines that are
  still in cache.
- **Cost (N):**
  - **At rest:** 68.2 ns per key, 1.01× plain GetMany, 1.000 attempts.
  - **Under a racing writer** (16 keys rewritten in order, readers at ~0.1
    µs per key): plain reads were torn 21,904 times in 200,000; validated
    reads 0 times, at 1.52 attempts each.
- **Limits of the guarantee:**
  - **Starvation:** a writer that never pauses would starve validation (the
    seqlock livelock). After a few attempts the read runs under the writer
    lock, a bounded wait of about a microsecond for 16 keys.
  - **In-place cells:** counter, field and bit updates do not touch bucket
    versions. Each such cell is read at one instant inside the window, but
    two cells are not read at the same instant. Use Export for an exact cut
    of counters.
  - **Private caches** (ADR 0006, off by default) are bypassed: a snapshot
    reads the table.
  - **One response:** a snapshot must fit in one response (the 1 MiB cap).
    It cannot continue across calls.

### J5. Iterate: every key, by hash shard, no index

- **Why the design makes it cheap:**
  - **A stable cursor:** the table never rehashes, so a bucket index stays
    valid across calls.
  - **Free shards:** a bucket range is a hash range, so n consumers take n
    shards.
  - **Cheap occupancy:** one SSE2 compare per bucket finds the occupied
    slots, and the records of the bucket 8 ahead are prefetched.
- **Cost (N):**
  - 18.5 ns per key (54 M keys/s) on one thread;
  - 19.5 ns with the 100-byte value copied (5.1 GB/s);
  - **340 M keys/s on 7 shards: all 10M keys in 29 ms.**
- **Under writes, only a cuckoo relocation moves an existing key.** A
  replace is in place, an insert takes a free slot, and a delete clears
  one. A relocation can move a key:
  - **from a bucket not yet scanned into one already scanned:** a miss;
  - **the other way round:** a repeat.
- **The fix: a relocation log.**
  - Each move appends its destination (bucket, slot) to a small ring. The
    ring holds 65,536 entries (512 KiB), indexed by `Table.st_moves`,
    which every move already increments.
  - A cursor carries its bucket and the move count it has seen.
  - **Each page, after its scan, re-reads every slot that a move since the
    cursor filled inside the region scanned so far.** Whatever key sits
    there now is emitted. Doing it after the scan also covers the moves
    made during the page, the last page's included.
  - **Proof sketch:** a key present for the whole pass ends in some bucket
    f.
    - If f was scanned after the key's last move into it, the scan emitted
      it.
    - Otherwise that move filled a slot in an already-scanned region, and
      the log emits it.
  - **The guarantee:** every key present for the whole pass appears at
    least once. With no relocation during the pass, exactly once.
  - A cursor older than the ring's reach answers `restart` for its shard.
- **The cost is negligible:** one 8-byte store per relocation, which
  already costs a breadth-first search.
- **Relocations are rare.** They happen only on inserts that find both
  candidate buckets full. The table's sizing keeps it at most ~83% full.

### J6. Max and min registers

- **Where:** Increment with `op = MAX` or `MIN`.
- **How:** an 8-aligned cell, `LOCK CMPXCHG` until the cell is at least the
  candidate. A candidate below the maximum is one load.
- **Cost (N, E):**
  - 43.1 ns batched, 0.91× an increment (exact, checked);
  - mostly the no-write path: the probe replays its candidates in each
    repetition, so after the first one they rarely raise a register;
  - a raising update adds one `LOCK CMPXCHG`: ≈ 1.0× an increment (E).
- **Idempotent:** applying a max twice leaves the same state, so retries
  are safe.

### J7. Flag words: 64 flags per key

- **Where:** Increment with `op = SET_BITS` or `CLEAR_BITS` and a mask.
- **How:** a `LOCK CMPXCHG` loop returns the previous word, so one call
  says which flags were already set.
- **Cost (N, E):**
  - 47.1 ns batched, 1.00× an increment, measured with `LOCK OR`, which
    always writes but returns nothing;
  - the `LOCK CMPXCHG` loop costs the same uncontended (E).
- **On a compact key** (golden-services H1) a flag check is one cache miss.
- **Bitmaps (H3) are the large form:** millions of bits by user number.

### J8. Counter arrays

- **The value:** a typed value of N int64 cells.
  - **ADD** at an index is `LOCK XADD`.
  - **SUM, MIN and MAX** over a range use AVX2.
- **Uses:** a week of hourly counters in one key (168 cells), latency
  histograms with fixed buckets, per-resource quotas.
- **Cost (N), 200k keys of 168 cells:**
  - ADD 71.7 ns;
  - SUM of all 168 cells 116.4 ns, sending 8 bytes instead of 1,344.
- **The in-cache kernels:**

  | Kernel | AVX2 | Scalar |
  |---|---|---|
  | sum | 6.3 ns | 29.2 ns |
  | max | 18.2 ns | 48.5 ns |

### J9. JsonGet

- **How:** a simdjson-style structural scan (Langdale and Lemire, C). Per 64
  bytes:
  - six byte compares find quotes, backslashes and `{ } [ ] : ,` (the
    brackets via `| 0x20`);
  - branchless escape detection;
  - the in-string mask is a carry-less multiply of the quote mask by all
    ones (`PCLMULQDQ`).

  A path walker pulls structurals as needed and stops when the target value
  is complete. The answer is the field's raw JSON text: no tree is built.
- **Cost (N):**

  | Document | Field | SIMD | Scalar |
  |---|---|---|---|
  | 668 B | top level / nested, early | 49.5 / 79.7 ns | 80.6 / 155.6 ns |
  | 668 B | late, nested | 253 ns | 491 ns |
  | 4,969 B | top level / nested, early | 31.3 / 52.3 ns | 52.9 / 96.2 ns |
  | 4,969 B | late, nested | 1.75 µs | 3.36 µs |
  | indexing a whole document | 668 / 4,969 B | 2.44 / 2.83 GB/s | 1.04 / 1.60 GB/s |

- **End to end on 1M stored documents:** 369.2 ns, against 110.7 ns for
  the whole document. The client receives a few bytes instead of ~1 KB and
  parses nothing.
- **Lazy validation:** the walker validates what it reads. A document
  broken after the requested field still answers, as simdjson's On Demand
  front end does.
- **Headroom:** this prototype pulls one structural per call. simdjson
  flattens each block's structurals into an array first and parses at
  several GB/s (C).

### J10. Content-addressed blobs (and Bazel's CAS)

- **The endpoints:**
  - **PutBlob** stores a blob under the SHA-256 of its content, computed by
    the server (SHA-NI). It skips the write when the blob is present
    (deduplication).
  - **Exists** on the digest keys finds missing blobs; **Get** reads them.
- **Why the server computes the digest:** a client cannot store content
  under a wrong address, by bug or on purpose. The prefix is configured
  (unset by default, so no existing key is refused after an upgrade), and
  other writes under it are refused.
- **Keys:** a prefix plus the raw 32-byte digest, 36 bytes with `cas:`.
  The 64-character hex form would cross 64 bytes and be compacted.
- **Cost (N):**
  - SHA-NI: 2.47-2.57 GB/s (6.3-6.5× scalar); 97.7 ns for 100 bytes.
  - PutBlob 4 KiB: 1.77 µs, mostly the digest.
  - Checking a digest: 33.8 ns, batched.
- **Bazel compatibility (C).** Bazel's remote cache speaks four gRPC
  services:
  - Capabilities;
  - ActionCache (action digest → result: a plain key);
  - ContentAddressableStorage (FindMissingBlobs, BatchUpdateBlobs,
    BatchReadBlobs);
  - ByteStream, for large blobs: Upload and Download with a SHA-256
    computed on the way.

  Served on the same port as an opt-in, they would let Bazel, Buck2 and
  Pants use KvNode as a remote build cache.

### J11. Value filters on Iterate and Export

- **The filters:**
  - the value contains given bytes;
  - a JSON field compared with a literal;
  - an int64 at an offset within a range.

  They are evaluated per record during the scan, so only matches cross the
  network.
- **Cost (N):** AVX2 first/last-byte candidates run at 19.6 GB/s for a
  needle with a rare last byte and 3.9 GB/s for a common one: 3.9× and 2.5×
  glibc memmem. JSON predicates run at JsonGet's speed.
- **Paged, not golden:** a page stops after a bounded amount of work, so a
  selective filter returns sparse pages instead of one long call.

### J12. BulkLoad

- **A client helper, not an RPC.** The plan keeps one request per call
  (functions-plan §1.4). BulkLoad pipelines PutMany calls (Mutate, 256 puts
  each) over a few connections, as the client's 4-connection Upload pool
  does.
- **Cost (M, E):**
  - batched puts cost 126 ns per key against 433 ns one at a time (M);
  - flat combining keeps that rate when other writers run (V).
- **Uses:** initial loads, restores and migrations (J5's other half).

## 6. What does not pay, or not yet

- **SIMD in the probe and key compare:** they wait on memory. Two keys per
  AVX2 compare, or 32-byte key compares, would not move a GET.
- **A pull-model JSON tokenizer** reaches 1.6-2.3× scalar. simdjson's
  flattened index, and full parses that build a tape, are the next step
  (C).
- **Substring search slows with common bytes.** It depends on how rare the
  needle's first and last bytes are (19.6 vs 3.9 GB/s). Picking the needle's
  two rarest bytes by a static frequency table is Muła's refinement (C).
- **Snapshot reads under constant writes need the lock fallback** (J4).
- **SHA-256 of large blobs costs 2.5 GB/s per core.** A 1 GiB blob hashes
  in about 0.4 s: stream it, with the digest computed on the way.
- **A server-side BulkLoad stream:** pipelined batches already reach the
  batched write rate (J12).

## 7. Recommendations, by value for cost

1. **Metadata, slices and ETags (J1-J3).** Trivial in the store; they save
   bandwidth for every cache and large-value client.
2. **Snapshot reads (J4).** One flag on GetMany and one sequence word,
   nearly free. It closes a correctness gap: today a reader can see an
   atomic batch half-applied (functions-plan A8).
3. **Iterate (J5).** Backups, migrations and index rebuilds at 340 M
   keys/s from Phase 1, before Export's cuts and without the ordered index.
4. **Max registers and flag words (J6, J7)** at the cost of an increment;
   **counter arrays (J8)** with the typed values.
5. **Content-addressed blobs (J10)**, ideally with Bazel's cache services:
   a large, existing client base.
6. **JsonGet and value filters (J9, J11):** the most SIMD per byte, and the
   most bandwidth saved.
7. **BulkLoad (J12)** in the client, any time after PutMany.

Phases are in [functions-plan.md](functions-plan.md) §V.10.

## 8. Sources

| Topic | Source |
|---|---|
| simdjson | Langdale, Lemire, "Parsing Gigabytes of JSON per Second", VLDB Journal 28(6), 2019; arXiv [1902.08318](https://arxiv.org/abs/1902.08318); [github.com/simdjson/simdjson](https://github.com/simdjson/simdjson) |
| SIMD substring search | W. Muła, "SIMD-friendly algorithms for substring searching", [0x80.pl, 2016](http://0x80.pl/notesen/2016-11-28-simd-strfind.html) |
| SHA-NI | Intel, "Intel SHA Extensions" ([article](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sha-extensions.html), [white paper](https://www.intel.com/content/dam/develop/external/us/en/documents/intel-sha-extensions-white-paper-402097.pdf)); [noloader/SHA-Intrinsics](https://github.com/noloader/SHA-Intrinsics) |
| Bazel's remote cache and CAS | [bazelbuild/remote-apis, remote_execution.proto](https://github.com/bazelbuild/remote-apis/blob/main/build/bazel/remote/execution/v2/remote_execution.proto) (Capabilities, ActionCache, ContentAddressableStorage; ByteStream for large blobs) |
| Redis SCAN guarantees | [redis.io/docs/latest/commands/scan](https://redis.io/docs/latest/commands/scan/) |
| Conditional requests (ETag, If-None-Match, 304) | RFC 9110 §13; [MDN If-None-Match](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/If-None-Match) |
| Atomic snapshots (double collect) | Afek, Attiya, Dolev, Gafni, Merritt, Shavit, "Atomic Snapshots of Shared Memory", JACM 40(4), 1993; [PDF](https://people.csail.mit.edu/shanir/publications/AADGMS.pdf) |
| Partial-key cuckoo hashing, optimistic reads | Fan, Andersen, Kaminsky, "MemC3", NSDI 2013, [usenix.org](https://www.usenix.org/conference/nsdi13/technical-sessions/presentation/fan) |
| CRC folding with PCLMULQDQ | Intel, "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ", 2009 (cited in crc32c.asm) |

## 9. Open decisions

1. **Store the CRC-32C at write time** (+4 bytes per record; ADR 0010's
   next lever)? Then the default ETag (J3) costs O(1), and downloads skip a
   pass.
2. **The relocation log's size** (J5): 65,536 entries (512 KiB) proposed.
   A cursor older than the ring's reach restarts its shard.
3. **Snapshot reads' fallback** (J4): how many optimistic attempts before
   reading under the writer lock? Four proposed.
4. **Bazel's cache services** (J10): serve them directly (a second gRPC
   service on the same port), or offer PutBlob and leave the protocol to a
   proxy?
5. **JSON:** extraction only (JsonGet, filters), or also in-place edits
   (JsonSet, JsonIncr: copy-on-write with a compare-and-set on the
   record)?
6. **Export by hash shard:** n parallel streams of one cut would need the
   cut to outlive its first stream (export-watch.md §5). Iterate shards
   from Phase 1; Export shards only if a customer needs parallel
   point-in-time copies.
