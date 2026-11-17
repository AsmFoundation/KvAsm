# Proposal: Export and Watch with key patterns

- **Status:** proposed, 2026-09-29. Nothing here is implemented or decided.
  Once accepted it becomes an ADR under the next free number.
- **Scope:** two server-streaming RPCs. `Export` streams a consistent view
  of the keys that match a filter. `Watch` streams every later write to
  matching keys. Filters combine exact keys, prefixes, globs and regular
  expressions.
- **Evidence:** two probes on the store's real code.
  - [2026-09-29-export-probe](../../benchmarks/results/2026-09-29-export-probe/README.md):
    the costs the cut and scan choices depend on.
  - [2026-09-29-index-probe](../../benchmarks/results/2026-09-29-index-probe/README.md):
    the lock-free revision sequence (§3.1) and the key indexes of the
    companion proposal [key-indexes.md](key-indexes.md).
- **Work in parallel:** Upload/Download (client and server streaming) is
  being built in the worktree `worktree-upload-download`. It needs the same
  HTTP/2 streaming layer (§7). Both features must land on one mechanism, so
  §7 lists what Export and Watch need from it rather than designing a second
  one.

## Summary

- **Revisions, without a lock.** Each applied write takes the next 64-bit
  **revision** with one atomic fetch-and-add (`LOCK XADD`, 3 ns).
  - A write publishes its revision in its own history slot. Readers advance
    a **stable revision** over the published slots, so a write whose number
    is taken but not yet visible is never announced.
  - No step of Export or Watch takes the writer mutex, and writers never
    coordinate with each other to publish.
  - Measured: +3.3 ns per write for today's serialized writers, and 41.6M
    writes/s across 7 cores where a mutex tops out at 15.1M/s (§3.1).
  - A random **incarnation** id per process start tells clients when
    revisions restarted.
- **Export** streams the matching keys and values **exactly as they were at
  one revision R**, while writes continue.
  - The cut is taken by **copy-on-write of 4 KiB pages of the bucket
    array**, and a held QSBR epoch keeps the cut's records alive.
  - Measured on the store: the obvious alternative, collecting pointers
    under the writer lock, stalls every writer for **2.3 ms at 1M keys and
    37-73 ms at 16M**.
  - Copy-on-write costs one writer **0.2-1.4 µs, once per page per export**.
- **Watch** delivers every matching write **in revision order, exactly once,
  resumably**.
  - Writers append 32-byte events to a **shared history ring**. An event
    points at the immutable record, so nothing is copied.
  - Each watcher is a cursor into that ring, filtered on its own worker.
    Writers never wait for watchers.
  - A watcher that falls outside the retained history gets `OUT_OF_RANGE`
    and re-synchronizes.
  - `initial_state` gives Kubernetes-style list-then-watch in one stream.
- **Filters** are exact, prefix, glob (Redis syntax) and regex (an RE2
  subset, whole-key, byte-oriented). They compile to one DFA per request:
  - matching is **linear time, with no backtracking**;
  - compilation is bounded;
  - measured cost is 23-41 ns per key during a scan.
- **What does not change:** Get is untouched and readers stay lock-free.
  Put and Delete gain one atomic increment, one slot store and one
  predictable branch, plus the history append when Watch is enabled
  (budget: ≤ 5%, gate G1).
- **The main limit, and its answer:** a hash table has no key order, so
  without an index **a filtered Export scans the whole table**: about 1 ns
  per key to walk the table, 11-41 ns with each key read and matched.
  [key-indexes.md](key-indexes.md) proposes opt-in indexes that make
  selective prefix, suffix and anchored-pattern queries O(1) + O(k).
  Measured at 16M keys: 447 ms → 0.2 µs for a 4-key prefix.

## 1. Goals and non-goals

**Goals**

- Export: every key (optionally with its value) matching a filter, as one
  consistent point-in-time view, while the store keeps serving writes.
- Watch: every Put and Delete of matching keys, in order, no gaps, no
  duplicates, resumable after a disconnect.
- One filter language for both: exact keys, prefixes, globs, regular
  expressions, combined with include and exclude lists.
- Keep KvAsm's properties:
  - Get unchanged and readers lock-free;
  - Put and Delete O(1) with a small constant;
  - no new libraries (ADR 0008);
  - no untrusted pattern can stall the server.

**Non-goals**

- Ordered or range iteration. That needs an ordered index: ADR 0001's
  "revisit if range or prefix queries become a requirement".
- Prefix, suffix and pattern queries faster than a full scan. These are
  covered separately, by the key indexes of
  [key-indexes.md](key-indexes.md), which this proposal's Export uses when
  one exists.
- Matching on values.
- Durability. Export feeds snapshot files that the client writes; the
  server still never writes to disk.
- Multi-key transactions.
- Authentication. KvAsm has none; §10 covers what enumeration exposes.

## 2. API

New methods in the existing service, which is backward compatible: old
clients never call them.

```proto
service KvAsm {
  // ... Get, Put, Delete unchanged ...

  // Streams the keys (and values) that match a filter, as they were at one
  // revision. The first message says which revision.
  rpc Export(ExportRequest) returns (stream ExportResponse);

  // Streams every applied write to matching keys, in revision order.
  rpc Watch(WatchRequest) returns (stream WatchResponse);
}

// A key matches when at least one include selector matches it (or include
// is empty) and no exclude selector does.
message KeyFilter {
  repeated KeySelector include = 1;
  repeated KeySelector exclude = 2;
}

message KeySelector {
  oneof kind {
    bytes exact = 1;   // one key; clients compact long keys as for Get
    bytes prefix = 2;  // keys that start with these bytes
    bytes glob = 3;    // Redis-style glob (§4.2), whole key
    bytes regex = 4;   // RE2 subset (§4.3), whole key, byte semantics
  }
  bool ignore_case = 5;  // ASCII letters match either case (not for exact)
}

// The point a stream's data refers to.
message Cut {
  fixed64 incarnation = 1;  // random per server process start
  uint64 revision = 2;
}

message Entry { bytes key = 1; bytes value = 2; }
message EntryBatch { repeated Entry entries = 1; }

message ExportRequest {
  KeyFilter filter = 1;
  bool keys_only = 2;    // omit values
  uint64 limit = 3;      // stop after this many matches; 0 = no limit
  bool count_only = 4;   // no entries, only the summary
}

message ExportResponse {
  oneof item {
    Cut cut = 1;                // always the first message
    EntryBatch entries = 2;     // up to ~64 KiB each (one large entry may exceed it)
    ExportSummary summary = 3;  // always the last message before OK
  }
}
message ExportSummary { uint64 count = 1; bool limited = 2; }

message WatchRequest {
  KeyFilter filter = 1;
  uint64 start_revision = 2;       // 0: from the next write; N: writes with revision >= N
  fixed64 incarnation = 3;         // with start_revision: the issuing server; 0 = any
  bool keys_only = 4;              // PUT events without values
  bool initial_state = 5;          // stream the matching keys first (a cut), then events
  uint32 progress_interval_ms = 6; // 0 = 1000; values below 100 become 100
}

message WatchResponse {
  oneof item {
    Cut started = 1;            // first message: events follow from started.revision + 1
    EntryBatch initial = 2;     // initial_state: the matching keys as of started.revision
    InitialDone initial_done = 3;
    EventBatch events = 4;
    Progress progress = 5;      // every matching write <= revision has been delivered
  }
}
message InitialDone { uint64 count = 1; }
message Progress { uint64 revision = 1; }
message EventBatch { repeated Event events = 1; }
message Event {
  uint64 revision = 1;
  EventType type = 2;
  bytes key = 3;
  bytes value = 4;   // PUT only, and not with keys_only
  bool created = 5;  // PUT: the key did not exist before
}
enum EventType {
  EVENT_TYPE_UNSPECIFIED = 0;
  EVENT_TYPE_PUT = 1;
  EVENT_TYPE_DELETE = 2;
}
```

**Status codes.** They arrive as trailers after the response headers. A
failure before the first message is a Trailers-Only response.

| Code | Export | Watch |
|---|---|---|
| `OK` | complete, after `summary` | never: a watch ends by cancel, error or drain |
| `INVALID_ARGUMENT` | malformed filter, pattern syntax, over a filter limit ("filter too complex") | the same |
| `OUT_OF_RANGE` | — | `start_revision` older than the retained history, the watcher fell behind it, or `incarnation` names another process |
| `RESOURCE_EXHAUSTED` | too many concurrent exports, or aborted because the churn during it exceeded `KVASM_EXPORT_RETAIN_MB` | too many watchers; an `initial_state` cut hits the export limits |
| `DEADLINE_EXCEEDED` | ran longer than `KVASM_EXPORT_TIMEOUT_SEC` | — |
| `UNAVAILABLE` | the server is draining (SIGTERM) | the server is draining |
| `UNIMPLEMENTED` | Export is disabled (`KVASM_EXPORT=off`) | Watch is disabled (`KVASM_WATCH_HISTORY=0`) |
| `CANCELLED` | the client reset the stream | the same |

## 3. Semantics

### 3.1 Revisions and incarnations

- **Numbering: an atomic increment, no lock.**
  - Revisions start at 0. A write takes its revision with one `LOCK XADD`
    on the store's counter.
  - Who gets one: a Put that created or replaced a value, a Delete that
    removed a key, and an Upload's publish (Upload/Download work). A refused
    Put and a Delete of an absent key change nothing and get none.
  - Cuts also take a number (§5.2), so revisions are unique and increasing
    but not every number is a write.
  - A write takes its number while it owns its key. Today that is inside
    the critical section ADR 0003 already has for the index update, which
    is why the original proposal could use a plain increment. In a future
    design where writes run concurrently, it would be while the write holds
    its key's buckets. Either way, two writes to one key carry revisions in
    the order they took effect.
- **Visibility: the stable revision.**
  - A number is taken before its write is visible, and **published** after:
    the writer stores the revision in the write's own history slot (§6.1),
    or in a small completion ring when Watch is off.
  - The **stable revision** is the highest R such that every revision ≤ R
    is published. Readers compute it by advancing over contiguous published
    slots, so it never goes backwards and writers never coordinate.
  - Watchers deliver, and cuts start, only up to the stable revision, never
    up to the raw counter.
  - A writer about to reuse a slot first waits until the slot's previous
    revision was published. That happens only if some write stalls for a
    whole ring of revisions.
- **Why the counter alone is not enough.**
  - Writer A takes 7, writer B takes 8, and B's write lands first. A reader
    of the counter would announce 8 while 7 is still being written: a
    watcher would deliver them out of order, and a cut "at 8" could miss 7.
  - Measured, 7 threads
    ([index probe](../../benchmarks/results/2026-09-29-index-probe/README.md)):

    | Way to publish "done up to R" | M writes/s | Monotonic |
    |---|---|---|
    | every writer CAS-advances one shared watermark | 13.4 | yes, but no faster than the mutex (15.1): all writers serialize on one cache line |
    | each writer announces a bound in its own line; readers take the minimum | 30.2 | **no**: a stale announcement moved it back 312-3,070 times per verified run |
    | **each writer publishes only its own slot; readers advance (proposed)** | **41.6** | **yes, by construction** |

  - A concurrent verifier checked every configuration: no watermark ever
    covered an unpublished revision (4M-28M revisions each).
  - The bare counter hands out 137M revisions per second, so the sequencer
    itself never limits writes.
  - With today's serialized writers the protocol costs +3.3 ns per write.
- **One global order.** Revisions give every write one place in a total
  order, and the stable revision makes that order safe to read.
  - Today the index update itself is serialized (ADR 0003), so effects
    also become visible to Gets in revision order: the order is
    linearizable.
  - If writes to different keys ever run concurrently, two of them may
    become visible to Gets in the opposite order of their revisions. Within
    one key the order stays exact, and cuts and watches stay consistent
    with the revision order. Making cross-key visibility follow revisions
    too would need effects published in revision order: a commit sequencer,
    which is exactly the serialization a lock provides. It is a property to
    decide then, not a reason to keep the lock now.
- **Incarnation.** A 64-bit random id drawn from `getrandom` at startup
  (next to the SipHash seed). A restart loses every write and restarts the
  revisions, so revisions compare only within one incarnation. Every
  stream reports it in `Cut`.

### 3.2 What Export guarantees

- **Content.** The entries are exactly the keys that existed at revision
  `cut.revision` and match the filter, each once, with the value it had
  at that revision.
- **No torn values**, as for Get: records are immutable.
- **Order:** unspecified. It is hash (bucket) order, stable within one
  export.
- **With `limit`:** the first `limit` matches in that order, with
  `summary.limited = true` if more existed.
- **Writes during the export are not in it.** They belong after the cut:
  watch from `cut.revision + 1` to follow them (§3.5).

### 3.3 What Watch guarantees

- **Delivery.** For every applied write whose revision is greater than
  `started.revision` and whose key matches, the stream delivers exactly
  one event, in revision order, for as long as the stream lives.
- **Progress.** `Progress{X}` promises that every matching write up to
  revision X has been delivered. It is sent after `progress_interval_ms`
  without a matching event, so a client with rare matches still learns an
  up-to-date resume point.
- **Resume.** A client that applied everything up to revision X (from events
  or progress) reconnects with `start_revision = X + 1` and the stream's
  `incarnation`. It then receives exactly the matching writes after X, or
  `OUT_OF_RANGE` if the server no longer retains revision X + 1.
- **Falling behind.** If the server stops retaining what a live watcher still
  needs (it read too slowly, or its client stopped reading), the stream ends
  with `OUT_OF_RANGE`. Writers never wait for watchers. The price is that a
  slow watcher must re-synchronize.
- **A future start** (`start_revision` beyond the current revision) is
  allowed: events begin when that revision is reached.

### 3.4 Long keys

Official clients send keys longer than 64 bytes as `0xFF ‖ SHA-512(key)`
(ADR 0004). The server never sees their text, so:

- **exact** selectors work for long keys: the client compacts the selector
  like any key;
- **prefix, glob and regex** selectors match the *stored* bytes, so they can
  never match the text of a long key. `*` does match compacted keys (as
  65-byte blobs), which is what a full export for backup needs. Clients
  mark such entries with `KeyCompaction.IsCompacted`.

A keyspace that needs patterns over keys longer than 64 bytes must run all
its clients in raw mode, with `KVASM_MAX_KEY_BYTES` raised to fit (open
decision 2).

### 3.5 List-then-watch

A cache or replica needs the current state plus every later change, with
nothing missed. Two ways:

1. **`Watch(initial_state = true)`**, one stream.
   - It sends the matching keys as of `started.revision`, then
     `initial_done`, then events from `started.revision + 1`.
   - The cut takes a revision number and the watcher starts right after it
     (§6.6), so no write can fall between them. No lock is needed: every
     write the cut lacks has a larger number.
   - The history must hold everything written while the initial state
     streams. On a busy server with a large result, that may end in
     `OUT_OF_RANGE`: raise the history or use way 2.
2. **Watch first, then Export.**
   - Open `Watch` from now and buffer its events client-side.
   - Run `Export`, apply its entries, drop buffered events with
     revision ≤ `cut.revision`, apply the rest.
   - This needs no server history beyond the watcher's own lag, at the cost
     of client memory.

## 4. Filters

### 4.1 Selectors

- `exact` and `prefix` are byte strings. `glob` and `regex` are pattern
  text, sent as `bytes` so patterns may contain any byte (protobuf
  `string` would require valid UTF-8).
- A key matches a filter when some include selector matches (or there are
  none) and no exclude selector does.
- `ignore_case` folds ASCII letters only.

### 4.2 Glob syntax (Redis `KEYS` / `SCAN MATCH`)

| Syntax | Matches |
|---|---|
| `*` | any sequence of bytes, including none (it crosses `:` and `/`) |
| `?` | exactly one byte |
| `[abc]`, `[a-z]` | one byte in the set; ranges are byte values |
| `[^abc]`, `[!abc]` | one byte not in the set |
| `\c` | the byte `c` itself, also inside a set |
| anything else | itself |

- **Strictness.** An unterminated `[` or a trailing `\` is
  `INVALID_ARGUMENT`. Redis tolerates both; KvAsm rejects a pattern whose
  meaning is unclear.
- **Compilation.** A glob compiles to the same automaton as a regex:
  `*` → `(?s:.*)`, `?` → `(?s:.)`, sets → classes.

### 4.3 Regex syntax (an RE2 subset, whole-key match, bytes)

| Supported | Notes |
|---|---|
| literals; `\` before a metacharacter; `\xHH`, `\x{HH}`; `\a \f \t \n \r \v` | a literal is a byte, so a UTF-8 character is a sequence of literal bytes |
| `.` | one byte except `\n`; every byte under `(?s)` |
| `[...]`, ranges, `[^...]`, ASCII classes such as `[[:alpha:]]` | byte ranges |
| `\d \D \w \W \s \S` | ASCII, as RE2: `\s` is `[\t\n\f\r ]` |
| `( )`, `(?: )`, `(?P<name> )` | all non-capturing: only match or no match is reported |
| `\|` | alternation |
| `* + ?`, `{n}`, `{n,}`, `{n,m}` with n, m ≤ 1000 | lazy forms (`*?`) accepted; laziness cannot change a whole-key yes/no |
| `(?i)`, `(?s)`, `(?is)`, and scoped `(?i: )` | ASCII case folding; dot matches `\n` |
| `^` at the very start, `$` at the very end, `\A`, `\z` | accepted as no-ops: every match is whole-key |

- **Rejected with `INVALID_ARGUMENT`:** backreferences, lookaround,
  `\b` and `\B`, Unicode classes (`\p{…}`), `\C`, possessive or atomic
  forms, `(?m)` and `(?U)`, and `^`/`$` anywhere but the ends.
- **Why whole-key.** A key either matches or it does not; nothing is
  searched inside it. `.*foo.*` means "contains foo".
- **Why bytes.** Keys are arbitrary bytes. UTF-8-aware matching (`.` = one
  code point) would enlarge every automaton. It is open decision 3.

### 4.4 Why a DFA, and never backtracking

- **The risk.** Every pattern comes from an untrusted client.
  - A backtracking engine (PCRE, .NET, Java) can take exponential time on
    one short pattern and one short key. That is ReDoS.
  - Redis shipped exactly this for globs: **CVE-2022-36021**, where a
    crafted `KEYS`/`SCAN` pattern hangs the server at 100% CPU.
- **The choice.** KvAsm compiles a filter into a deterministic automaton.
  Matching a key then costs one table step per key byte, stops at the
  first byte from which no match is possible, and has no worst case beyond
  the key's length.

### 4.5 Compilation (on the request's worker, bounded)

1. **Parse.** Recursive descent into an AST, with at most 64 levels of
   nesting and 1,024 bytes per pattern.
2. **Thompson NFA.** Counted repetition is expanded as it is built, and
   the NFA-state limit is enforced during that expansion. `exact` and
   `prefix` selectors become literal pieces of the same NFA.
3. **Byte classes.** The 256 byte values are partitioned into classes that
   every transition treats alike (typically 2-40). The DFA table is then
   `states × classes`, not `states × 256`.
4. **Subset construction** into a DFA, with two limits:
   - `KVASM_FILTER_MAX_STATES` (default 2,048);
   - at most 131,072 transitions, so a table of 16-bit state ids is at
     most 256 KiB, sized to fit in L2.

   A dead state (no path to acceptance) ends matching early.
5. **Two automata per filter:** includes and excludes. The exclude
   automaton runs only on keys the include automaton accepted.

Anything over a limit is `INVALID_ARGUMENT` with the limit's name. The
worst-case compile time under these limits is estimated at 1-2 ms of one
worker; gate G6 measures it. Concurrent exports and watchers are capped
(§9), so compile work cannot be multiplied without bound.

### 4.6 Matching costs and fast paths

- **Measured scan costs** (store probe, 16-byte keys, records prefetched a
  chunk ahead):
  - reading the key and comparing a 10-byte prefix: 11-23 ns per key;
  - a byte-class DFA over the whole key: 23-41 ns per key (1M → 16M keys).
- **Exact-only filters** use a hash set of the selectors' `(h1, key)`,
  computed with the server's SipHash at compile time. Watch compares
  an event's stored `h1` first (§6.1), so a non-matching event costs one
  compare and never touches the record.
- **Prefix-only filters** compare bytes directly, with no DFA.
- **Mixed filters** run the DFA.

### 4.7 How correctness is checked

- **Regex oracle: PCRE2** (`libpcre2-dev` 10.46 from Debian trixie,
  added to the toolchain image).
  - Settings: anchored at both ends, `PCRE2_DOLLAR_ENDONLY`, no UTF.
  - Classes are spelled explicitly where PCRE2 and RE2 differ (PCRE2's
    `\s` includes `\v`).
  - Inside this subset, PCRE2 and RE2 agree on match versus no match.
- **Glob oracle:** a memoized reference matcher in C with Redis's rules.
- **Scale:** at least one million random (pattern, key) pairs from a
  grammar generator, plus mutated patterns for the error paths.

## 5. Export: a point-in-time cut by page copy-on-write

### 5.1 The obvious ways, measured

| Way | Cost | Verdict |
|---|---|---|
| Hold the writer lock and copy out every live record pointer | 2.3 ns per key: **2.3 ms** at 1M keys, **37-73 ms** at 16M, during which no write proceeds | rejected: a Put holds the lock ≤ ~450 ns today (2.23M SET/s through one lock at 4 cores), so this is ≥ 5,000× longer |
| Hold the lock and `memcpy` the bucket array | 1.2 ms at 1M keys, 16 ms at 16M | rejected for the same reason |
| Scan without a cut (Redis `SCAN` style) | none | rejected: a cuckoo displacement can move a key behind the scan, so a key present throughout could be **missed**, not just repeated |
| Fuzzy scan, then repair from the change history | none for writers | rejected: needs displacements logged, history covering the whole scan, and tombstone merging in every client |
| MVCC version chains | per-record versions and garbage collection | rejected: rewrites the core for one feature |
| **Copy-on-write of 4 KiB bucket pages** | **0.2 µs per page copy (0.84-1.4 µs if it also faults in its destination page), paid once per page by the first writer that modifies it** | **proposed** |

### 5.2 The cut, as one more operation in the revision sequence

A cut takes a revision number like a write does. That places it exactly
between the writes it contains and the writes it does not, with no lock.

**Prepared first**, touching nothing shared:

- **Bitmaps:** `preserved` (written by writers) and `consumed` (written by
  the exporter), one bit per 4 KiB page. That is 512 bytes for the default
  table and 8 KiB at 16M keys.
- **A copy area** the size of the bucket array, `mmap`'ed `MAP_NORESERVE`
  with `MADV_NOHUGEPAGE`. It is untouched, so it costs no memory yet.
  `MADV_NOHUGEPAGE` matters: a first write into a transparent huge page
  would zero 2 MiB inside a writer's critical section.

**Then, in order:**

1. **Hold.** Set one of the `KVASM_MAX_EXPORTS` reserved QSBR reader slots
   to the current epoch E (§5.5).
2. **Announce.** Set the cut's bit in `Table.cut_mask`, with its revision
   `c.R` marked pending.
3. **Take a number.** `r = LOCK XADD(counter) + 1`, then `c.R = r − 1`.
   Publish slot r as a CUT marker that watchers skip, so the stable
   revision can pass it.
4. **Wait for R.** The exporter starts walking pages once the stable
   revision reaches R. Every write numbered ≤ R has then published. With
   today's serialized writers it already has.

**Why the split is exact:**

- **After the cut:** steps 1 and 2 precede the `LOCK XADD`, a full barrier.
  A write numbered above r reserved after it, so its load of `cut_mask`
  (made after its own reservation) sees the bit, and its later retirements
  see the held slot.
- **Before the cut:** a write that sees the bit compares its revision with
  `c.R`. It waits the few nanoseconds `c.R` may still be pending, right
  after a cut starts. Above R, it preserves pages (§5.3). At or below R, it
  is part of the cut and must not.

**Release** reverses the order:

1. Clear the bit.
2. Take a number `r_e` (and publish its marker). Writes numbered above
   `r_e` cannot see the bit.
3. Once the stable revision reaches `r_e`, no write that might have seen
   the bit is still copying. Only then unmap the copy area and free the
   QSBR slot.

The bucket array never resizes (ADR 0001), so a page index is stable for
the life of the process. That is what makes page-level copy-on-write
simple.

### 5.3 Writers

Before its first store to bucket `b`, every routine that modifies one
checks the active cuts:

- publish (insert);
- replace;
- remove;
- each move of a displacement path.

```
if (cut_mask != 0)                       // predictable branch; nothing else when idle
    for each active cut c:
        wait while c.R is pending        // nanoseconds, only right after a cut starts
        if (my_revision > c.R):          // this write comes after the cut
            p = page(b)
            if (!c.preserved[p] && !c.consumed[p]):
                copy page p -> c.copy[p] // 176-199 ns measured, cold source
                sfence                   // string stores (rep movsb) are not ordered
                c.preserved[p] = 1
```

- A write modifies one bucket, and a displacement path a few, so a write
  copies at most a few pages, and each page at most once per export.
- For the default table the whole cost across one export is bounded by its
  4,096 pages.
- **Today** every bucket store happens inside ADR 0003's index critical
  section, so `preserved` needs only plain stores.
- **If writes to different buckets ever run concurrently:**
  - a writer claims a page with an atomic bit test-and-set, and one that
    loses the race waits for the winner's bit before modifying;
  - a write after the cut first waits until the stable revision reaches R,
    so that it never copies a page an older write is still changing.

  Neither changes anything above.

### 5.4 The exporter (lock-free)

The exporter walks the pages in order. It never takes the writer mutex
between the cut and the release.

```
for page p:
    if preserved[p]:
        src = c.copy[p]                    // stable: written before the bit
    else:
        copy page p from the live table into a private 4 KiB buffer
            with ordinary 8/16-byte loads (not rep movs)
        if preserved[p] (re-read):  src = c.copy[p]
        else:                       src = private buffer
    for each record pointer in src:        // alive: see 5.5
        match key; emit entry
    consumed[p] = 1                        // later writers skip this page
    if preserved[p]: madvise(c.copy[p], MADV_DONTNEED) (batched)
```

**Why a private copy that re-reads `preserved[p]` as 0 is the page as of R**
(x86-TSO):

- The first writer to modify page p after R stores the copy, then
  `preserved[p] = 1`, then the bucket. Under TSO, other cores see those
  stores in that order.
- The exporter's loads (the page, then `preserved[p]`) are not reordered
  with each other.
- If any page load saw a modification, the later load of `preserved[p]`
  must see the 1 stored before it.
- So a 0 means none of the page loads saw a change after R.

**Why ordinary loads.** Fast string operations (`rep movs`, which glibc's
`memcpy` uses for large sizes) do not guarantee program order among their
own accesses (Intel SDM §8.2.4). The exporter therefore copies with an
explicit loop of ordinary loads, whose order TSO does guarantee. Writers
may use `memcpy` because the `sfence` before the bit orders all of their
earlier stores, string stores included.

### 5.5 Keeping the cut's records alive

The cut holds pointers to records that writers may replace or delete after R.

- **Protection.** The reserved QSBR slot stays at epoch E until the export
  ends. Records unlinked after the cut are retired with epoch ≥ E, and
  QSBR frees them only once every slot has moved past their retire epoch,
  so they stay.
- **What is not held.** Records retired before the cut were not in the
  table at R, so the cut never references them and they are freed as
  usual.
- **Why the worker's own slot isn't enough.** The exporter's worker still
  announces quiescent states each pass for its own slot. Only the reserved
  slot spans passes.
- **Records created after R** are not in the cut, but if they are retired
  while it is held, they wait too. Retention is therefore the churn during
  the export, bounded by `KVASM_EXPORT_RETAIN_MB` (§5.7).

### 5.6 Memory

- **Bitmaps:** 2 bits per 4 KiB of bucket array.
- **Copy area:** only the pages writers modified before the exporter
  reached them. They are released as the exporter passes them. The worst
  case is the bucket array's size (16 MiB per million keys of capacity).
- **Retained records:** the write churn during the export (§5.5).

### 5.7 Pacing, fairness, limits, abort

- **Pacing.** The exporter produces one entries message (~64 KiB) at a time,
  only when flow control and the connection's output buffer have room.
  It keeps its place as (page, slot).
- **Fairness.** Production per event-loop pass is capped (§7), so one export
  cannot starve the worker's other connections.
  - Measured production rate: 3.0-4.9 GB/s on one core, far above one
    gRPC stream.
  - A 1M-key export is ~25 ms of CPU, spread over passes.
- **Limits:**
  - `KVASM_MAX_EXPORTS` (default 2) concurrent cuts, `initial_state`
    watches included; past it, `RESOURCE_EXHAUSTED`;
  - `KVASM_EXPORT_TIMEOUT_SEC` (default 600).
- **Abort is cooperative.**
  - If retained records exceed `KVASM_EXPORT_RETAIN_MB` (checked by
    writers in the reclaim step), the writer flags the oldest cut for abort
    and rings its worker's doorbell.
  - That worker ends the stream with `RESOURCE_EXHAUSTED` and releases the
    reserved slot at the end of its current pass, when it holds no record
    pointer from the cut.
  - A writer never releases the slot itself: the exporter may be reading
    those records at that moment.
- **Any stream end** releases the cut: success, cancel, deadline, drain or
  connection loss. The worker follows §5.2's release order: clear the bit,
  take a number, and unmap the copy area and free the QSBR slot once the
  stable revision passes that number.

### 5.8 Later, if measurements ask for it

- **A pool of prefaulted pages for writers.** Removes the 0.84-1.4 µs
  first-touch fault, leaving ~0.2 µs. Gate: write p99.9 during exports
  (G2).
- **Zero-copy framing.** Entries go from records straight into the output
  buffer, one copy instead of two, as the GET fast path does.
- **Exact-only exports without a scan.** Look each key up in the cut: the
  copied page if preserved, else the live page. That is a consistent
  multi-get.

## 6. Watch: a shared, revision-ordered history

### 6.1 The history ring

The ring holds `KVASM_WATCH_HISTORY` 32-byte events (a power of two, two
per cache line), allocated once at startup:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `rev` | the event's revision once published; 0 while the slot is being rewritten |
| 8 | `kind` | PUT, DELETE, or CUT (a cut's number, §5.2; watchers skip it); the `created` flag |
| 16 | `rec` | the record: the new one for a PUT, the removed one for a DELETE |
| 24 | `h1` | the key's SipHash h1, so exact-key watchers can filter without touching the record |

- **Newest end:** the newest *published* revision is §3.1's stable
  revision. Watchers compute it from the slots themselves, so the ring
  needs no head pointer.
- **Oldest end:** the table keeps `hist_tail`, the oldest revision still
  retained.

### 6.2 The writer's append (after the index update)

```
seq  = its revision (LOCK XADD, taken while the write owns its key, §3.1)
slot = ring[seq & mask]
wait while slot.rev < seq - capacity      // previous occupant unpublished: only after a long stall
slot.rev = 0                              // readers of the old event now fail validation
slot.kind, slot.rec, slot.h1 = ...
slot.rev = seq                            // publish: the stable revision may now pass seq
rec.aux = seq                             // "newest event referencing me" (writer-only field)
hist_tail = max(hist_tail, seq - capacity + 1)
release held records whose aux < hist_tail   // bounded per write (see 6.3)
```

- **No cold memory in the append.** It touches only the slot (sequential)
  and records the writer just wrote or looked up. Releasing held records
  reads each one's header once, as today's reclamation already does for
  every retired record.
- **Who owns what.**
  - The slot belongs to its revision, so publishing needs no lock.
  - `hist_tail`, the held FIFO and `rec.aux` belong to reclamation, which
    runs inside ADR 0003's critical section today, as the limbo list and
    Download's pin table do.
  - Concurrent writers would give reclamation its own synchronization, for
    all of them at once.
- **When Watch is disabled** (`KVASM_WATCH_HISTORY=0`):
  - the fetch-and-add remains;
  - so does a store into a small completion ring, which keeps the stable
    revision exact for cuts (§3.1);
  - nothing else.

### 6.3 Records the history still points to

A record in the history must outlive its removal from the table until its
event leaves the window. **Records are held by revision, not by reference
count.**

- **Retire.** When a writer retires a record (replaced or deleted) and
  `rec.aux ≥ hist_tail`, the record goes to a **held FIFO** instead of the
  QSBR limbo list. `aux` holds the revision of the newest event that
  references the record; at most two do, its PUT and its DELETE, and the
  DELETE is newer. `aux` is writer-only today and unused while a record is
  live (`kvasm.inc`), so the record header does not grow.
- **Release.** Each write moves up to 8 FIFO heads whose `aux < hist_tail`
  into limbo, stamped with the current epoch. From there QSBR frees them
  after a grace period as usual.
- **The resulting rule.** A record is freed only when all of these hold:
  1. it is out of the table;
  2. its newest event has left the history window;
  3. a grace period has passed since the later of the two;
  4. no Download pin holds it (once Download lands; it pins by a separate
     field that readers update atomically).
- **Why not a reference count in the record:** evicting an old event would
  then touch a record written thousands of events earlier, which is a DRAM
  miss inside the critical section on nearly every write.
- **Byte budget.** Held records are counted in `hist_held_bytes`. Above
  `KVASM_WATCH_HISTORY_MB` a writer also advances `hist_tail` by up to 8
  events per write. That is a soft limit enforced amortized, so each write
  stays O(1). Large values thus shorten the history window instead of
  exhausting memory.

### 6.4 Watchers

A watcher is a stream owned by one worker. It carries:

- its compiled filter;
- a **cursor** (the next revision to deliver);
- `keys_only`;
- a progress deadline;
- the staged entries message it is filling.

Each worker keeps a list of its watchers, and the minimum of their cursors.

**Reading an event** (while the worker is QSBR-online within a pass):

```
r1 = slot.rev
if r1 < want: stop for this pass — not published yet (its write is still in
              flight), or being rewritten; the next pass tells which
if r1 > want: overflow (overwritten)
kind, rec, h1 = slot fields
if slot.rev != r1: overflow (overwritten while reading)
if want < hist_tail: overflow (released early by the byte budget); do not touch rec
now rec is safe to read until this worker's next quiescent announcement
```

Stopping at the first unpublished slot is exactly "delivering up to the
stable revision" (§3.1): a watcher never gets revision 8 before 7, even if
8's write landed first.

The last check is what makes early release safe:

- A writer advances `hist_tail`, then retires the record with a full fence
  (the epoch `xchg`).
- If this worker read the old tail, the release came after its pass began,
  so the grace period protects the record.
- If the release came earlier, the worker sees the new tail and never
  dereferences `rec`.

**Delivery, in each pass:**

- For each watcher whose stream has room, read events from the cursor, up
  to a per-pass budget, filter them, and append matches to its staged
  message.
- Frame the message when it is full or the events run out.
- Watchers on one worker share the reading: events are the outer loop,
  watchers the inner, so a burst is read once per worker, not once per
  watcher.

**Overflow.** A cursor below `hist_tail` ends the stream with `OUT_OF_RANGE`
("revision N is no longer retained; oldest is M").

### 6.5 Waking an idle watcher's worker

Writers publish events with no knowledge of watchers. An idle watcher's
worker sleeps in `epoll_wait` for up to 1 s. Three pieces wake it:

- A global bitmask `watch_workers` marks the workers that own at least one
  watcher.
- Each worker has two flags, on a line of its own:
  - `sleeping`, set with `xchg` (a full fence) before it blocks;
  - `doorbell_pending`, which makes writers skip redundant wake-ups.
- `Worker.doorbell_reasons`: today every doorbell means "drain"
  (`worker.asm`). This becomes a bitmask set with `lock or`: DRAIN, WATCH,
  ABORT. The worker `xchg`-clears it after reading its `eventfd`.

**The writer, after publishing its slot:**

```
if watch_workers != 0:
    mfence                           // the slot store before the flag loads; no fence without watchers
    for each worker w in watch_workers, w != me:
        if w.sleeping and xchg(w.doorbell_pending, 1) == 0:
            doorbell_reasons |= WATCH; write(w.event_fd, 1)
```

Today the writer mutex's unlock (`lock xadd`) already orders the stores
before it, so a writer that checks after unlocking gets this fence for
free. The explicit one keeps Watch independent of that lock.

**The worker, before blocking:**

```
doorbell_pending = 0                 // first: a writer that saw 1 skipped its doorbell
xchg(sleeping, 1)                    // full fence
if slot(min_cursor).rev == min_cursor: sleeping = 0 and process now
```

Both sides fence before they read the other's flag, so a wake-up cannot be
lost:

- **A writer that finds `sleeping == 1`** rings, unless `doorbell_pending`
  shows a ring is already under way.
- **A writer that finds `sleeping == 0`** published its slot before
  checking, so the worker's re-check after its `xchg` sees it.
- **A writer that skipped because `doorbell_pending` was still 1** is caught
  by the same re-check, because the worker clears the flag first.
- **A write published out of order** (8 before 7) wakes the worker, which
  finds the event at its cursor (7) not yet published and sleeps again.
  7's own publication then wakes it.

**Costs:**

- Nothing without watchers.
- A few loads per write while watcher workers are busy: they never sleep,
  so there are no syscalls.
- One `eventfd` write per sleep cycle of an idle watcher's worker.
- The expensive middle ground is events arriving just slower than the
  worker drains them. Each can then cost a wake-up, and ADR 0009 measured
  that wake-ups are not free (~4.5 µs per cross-vCPU wake on this host).
  Gate G4 measures CPU per event across event rates. If the middle ground
  is costly, coalescing follows: at most one wake per
  `KVASM_WATCH_COALESCE_US`.

### 6.6 initial_state

`Watch(initial_state = true)` works as follows:

- It takes a cut exactly as Export does (§5).
- It sets the watcher's cursor to `R + 1`, R being the cut's number
  (§5.2). Every write the cut does not contain has a larger revision, so
  nothing falls between the initial state and the first event, with no
  lock.
- It streams the cut's matching keys as `initial` messages, then
  `initial_done`, then events from `R + 1`.
- Events written meanwhile wait in the shared ring, not in per-watcher
  buffers. If the ring drops them before the initial state is out, the
  stream ends with `OUT_OF_RANGE` (§3.5).

### 6.7 Progress

When a watcher is caught up, has sent nothing for `progress_interval_ms`,
and the revision has moved past its last report, it sends
`Progress{revision}`.

The worker's `epoll_wait` timeout becomes the minimum of 1 s and its
watchers' next progress deadline. Idle watchers therefore cost one wake
per interval per worker, not per watcher.

### 6.8 Costs

- **Per write, Watch enabled:** one 32-byte slot write, one header store to
  a hot record, one compare, and amortized FIFO releases. **Estimated
  5-15 ns; gate G1 accepts at most a 5% loss of SET throughput.**
- **Per event, per watcher**, on the watcher's own worker:
  - exact-key filters: ~2 ns (the `h1` compare);
  - prefix filters: ~10-20 ns;
  - DFA filters: ~20-40 ns;
  - plus one cross-core fetch of the record (~100 ns) per event per
    worker, when any of its watchers needs the key.
- **Fan-out.** At 1M writes per second, 100 regex watchers on one worker
  would cost that worker ~2-4 s of CPU per second. Watch therefore caps
  watchers (`KVASM_MAX_WATCHERS`). Per-worker indexes are the scaling
  step: an exact `h1` table, a prefix table, and one union DFA with
  per-watcher accept sets. They are gated by measurement (G5).

## 7. The streaming layer (shared with Download)

Export and Watch need the following from the HTTP/2 engine. The
Upload/Download work builds the first version of this layer. This list is
what it must also cover, so that there is one mechanism.

1. **Producers.** A stream can carry a producer with three entry points:
   - `produce(conn, stream, budget)` stages the next message(s) and
     reports MORE, WAIT (nothing now), DONE(status) or an error;
   - `abort(conn, stream)` releases whatever the stream holds (cut,
     watcher, pins). It is called from `stream_close` whatever the cause:
     RST_STREAM, connection loss or drain.

   Today `emit_progress` sends the trailers once `resp_off == resp_len`
   (`h2n.asm`). With a producer it asks for more instead.
2. **Trailers with a status after the headers.** The engine currently
   writes only `grpc-status: 0` after a body. Streams need any
   `grpc-status` and `grpc-message` there.
3. **A worker ready list.** Connections whose producers can progress
   without an I/O event: new history events, an unfinished export, a
   pending abort.
   - They run in a pass between ADR 0009's input phase and output phase.
   - They are flushed in the output phase with the rest.
   - While the list is non-empty, the loop does not block in
     `epoll_wait`.
4. **Budgets.** `conn_flush` currently repeats "send, pump, send" until the
   socket blocks. That is fine for bounded unary replies, but a producer
   on fast loopback could keep one worker on one connection for its whole
   export.
   - Production per stream per pass is capped (e.g. `OUT_HIGH_WATER`).
   - The work per worker per pass is capped too.
   - Leftover work waits on the ready list.
5. **Idle timeout.** `housekeeping` counts only received bytes as activity.
   A connection with a live producer stream is not idle. Its liveness
   comes from client keepalive PINGs, which grpc-dotnet sends every 60 s
   with KvAsm's options. Server-sent PINGs are a later option.
6. **Drain.** At `begin_drain`, watch streams end at once with
   `UNAVAILABLE`: they would never finish. Exports continue until the grace
   deadline.
7. **Routes.** `hpack.asm` classifies `:path` into route ids (`M_*`). Add
   `Export` and `Watch` (`M_EXPORT`, `M_WATCH`).
8. **Request decoding.** `proto.asm` decodes only flat key/value requests.
   Filters are nested and repeated (`KeyFilter` → `KeySelector` → `oneof`),
   so it needs a bounded structured decoder.

## 8. Changes by file

| File | Change |
|---|---|
| `proto/kvasm/v1/kvasm.proto` | the messages and methods of §2 |
| `server/include/kvasm.inc` | `Table`: `revision` and the history head/tail next to `epoch` (both polled by readers once per pass); `incarnation` (immutable line); `cut_mask`, cut and history pointers; the `HistEvent` and `Cut` structs; `Rec.aux` documented as "newest history revision while live" |
| `server/src/store.asm` | revision fetch-and-add and slot publication (every publishing path: Put, Delete, Upload's publish, snapshot load); history append and held FIFO; retire deferral; reserved QSBR slots for cuts; retention accounting and abort flags |
| `server/src/cuckoo.asm` | the page-preserve hook before bucket stores (publish, replace, remove, displacement moves) |
| `server/src/filter.asm` (new) | filter parser, NFA, byte classes, DFA construction, executor, exact/prefix fast paths |
| `server/src/export.asm` (new) | cut start and release, the lock-free page walk, the Export producer |
| `server/src/watch.asm` (new) | watcher registry, delivery, progress, overflow, wake-ups |
| `server/src/proto.asm` | bounded decoder for `ExportRequest` / `WatchRequest` |
| `server/src/grpc.asm` | dispatch for `M_EXPORT` / `M_WATCH` |
| `server/src/hpack.asm`, `server/include/server.inc` | the two routes |
| `server/src/h2n.asm` | producer streams and status trailers (§7), with Download |
| `server/src/worker.asm` | ready list, doorbell reasons, `sleeping` flag, budgets, idle exemption, drain policy, progress timeouts |
| `server/src/config.asm`, `main.asm` | the settings of §9; incarnation; extra QSBR slots; history allocation |
| `server/tests/*` | `test_filter` (PCRE2 and glob oracles), `test_cut` and `test_history` (store-level model checks), itest phases, fuzzer extension |
| `client/dotnet` | `KeyFilter`, `ExportAsync`, snapshot export helper, `WatchAsync`, an auto-resuming watcher |
| `docs/*` | PROTOCOL (methods, filters, resume rules, codes), OPERATIONS (settings, memory, exposure), ARCHITECTURE; the ADR once accepted |

**.NET client sketch:**

```csharp
var filter = KeyFilter.Glob("user:*").Except(KeyFilter.Prefix("user:tmp:"));

await using KvExport export = await kv.ExportAsync(filter);
Console.WriteLine($"as of revision {export.Revision}");
await foreach (KvEntry e in export) { /* e.Key, e.Value */ }

// Live data survives a restart: write it as a KVASNAP1 snapshot. Keys are
// already in wire form, so the writer must not compact them again.
await SnapshotFile.WriteExportAsync(kv, "/data/seed.kvs", KeyFilter.All);

await using KvWatch watch = await kv.WatchAsync(filter, new WatchOptions { InitialState = true });
await foreach (WatchEvent ev in watch) { /* Initial, InitialDone, Put, Delete, Progress */ }
```

## 9. Configuration

Proposed settings follow `config.asm`'s rules: strict parsing, and an
invalid value stops startup.

| Variable | Range | Default | Meaning |
|---|---|---|---|
| `KVASM_EXPORT` | `off` \| `on` | `off` (open decision 1) | enables Export |
| `KVASM_MAX_EXPORTS` | 1-16 | 2 | concurrent cuts, `initial_state` watches included |
| `KVASM_EXPORT_RETAIN_MB` | 1-1048576 | 256 | records kept alive by cuts before the oldest is aborted |
| `KVASM_EXPORT_TIMEOUT_SEC` | 0-86400 | 600 | longest export; 0 = none |
| `KVASM_WATCH_HISTORY` | 0, or a power of two 1024-16777216 | 0 (open decision 1) | events retained; 0 disables Watch and costs nothing |
| `KVASM_WATCH_HISTORY_MB` | 1-1048576 | 64 | replaced/deleted records kept for the history (soft) |
| `KVASM_MAX_WATCHERS` | 1-1000000 | 1024 | watchers per process |
| `KVASM_FILTER_MAX_STATES` | 16-65536 | 2048 | DFA states per filter |

- **Naming.** "Cut" is used for the in-memory point-in-time view so it
  cannot be confused with `KVASM_SNAPSHOT`, the snapshot *file*.
- **Memory.** The history costs 32 bytes per event at startup (2 MiB for
  65,536 events), plus the held records within its budget.

## 10. Security

- **Enumeration is new exposure.**
  - Today a client that can reach the port can read any key *it can name*.
    With Export it can read every key, and with Watch every future write.
  - KvAsm has no authentication (OPERATIONS.md).
  - Hence the proposed default: both **off** until an operator enables
    them (open decision 1).
- **Pattern DoS** is excluded by construction:
  - DFA matching, linear in the key's length;
  - bounded parse, NFA, DFA and compile work;
  - no recursion without a depth limit;
  - the parser fuzzed.
- **Resource DoS is bounded:**
  - concurrent cuts, watchers and streams per connection (existing);
  - retained-record budgets for cuts and for history;
  - an export deadline;
  - per-pass production budgets;
  - progress intervals of 100 ms or more.
- **Writers never wait on a client.** No reader, exporter or watcher can
  make a writer block. The worst a slow client causes is its own
  `OUT_OF_RANGE`, or its export being aborted.

## 11. Verification plan

1. **Filters (`test_filter`):**
   - syntax, errors and limits;
   - differential against PCRE2 (regex) and a reference glob matcher, on
     ≥ 1M generated pairs;
   - `ignore_case`;
   - the fast paths against the DFA on the same inputs.
2. **Cuts (`test_cut`, store level, threads):**
   - Writers apply random Puts and Deletes. A model records every write
     with its revision.
   - Exporters take cuts at random moments. Each export must equal the
     model's state at its revision, key for key and value for value.
   - Run near a full table (displacement paths), with reclamation active,
     with several concurrent cuts, and with aborts.
   - Freed records are poisoned in the test build, and a poisoned read
     fails the test.
3. **History (`test_history`):**
   - ring wraparound, byte-budget eviction and the held FIFO;
   - conservation: records created = live + limbo + held + freed;
   - slot validation under concurrent writes (torn reads caught);
   - `OUT_OF_RANGE` at the exact boundary.
4. **End to end (`make itest`, new phases):**
   - Export: full, filtered, `keys_only`, `limit`, `count_only`.
   - A client that stops reading: the export pauses and server memory stays
     bounded.
   - RST mid-export: the cut is released, its slot freed, limbo drains.
   - Concurrent writers during an export: consistency against a revision
     model.
   - Watch: with N writer connections, each watcher receives every matching
     event exactly once, in revision order.
   - Resume from a revision, and `OUT_OF_RANGE` with a tiny history and a
     slow watcher.
   - `initial_state` + events == the final state.
   - Progress on sparse matches; drain; every limit.
   - The HTTP/2 fuzzer extended with these streams and random resets.
5. **.NET (Testcontainers):**
   - the API;
   - network interruptions: the watcher resumes without gaps or
     duplicates;
   - `OUT_OF_RANGE` re-sync;
   - a server restart: a new incarnation leads to a re-sync.

## 12. Measurement gates

Each gate is a paired, concurrent A/B in the manner of ADR 0009 unless
stated otherwise.

| Gate | Measures | Pass |
|---|---|---|
| G0 | the revision sequence and its stable-revision protocol, alone | **done** (index probe): +3.3 ns per write with today's serialized writers; 41.6M/s lock-free at 7 threads; verified safe and monotonic |
| G1 | SET throughput and p99 with the history on vs off | ≤ 5% loss |
| G2 | write p99 / p99.9 while an export runs vs idle | p99.9 ≤ 2× (otherwise add the page pool, §5.8) |
| G3 | export throughput (keys/s, MB/s) over loopback, .NET and kvbench | network-bound: server CPU per exported byte below the client's |
| G4 | watch delivery latency p50/p99 and CPU per event at 1k / 10k / 100k events/s | decides the coalescing default (§6.5) |
| G5 | watcher fan-out: CPU per event per watcher for exact, prefix and regex | decides the per-worker indexes (§6.8) |
| G6 | filter compile time (typical and worst under the limits); match ns/key per selector kind | worst case ≤ 2 ms; otherwise tighten the limits |
| G7 | GET throughput with the new server | unchanged within noise (the GET path is untouched) |

## 13. Phases

Each phase ends committed with its tests, docs and gates.

1. **Filters** (`filter.asm`, `test_filter`, `bench_filter`). Independent of
   the server: it can start now, in parallel with Upload/Download. Gate G6.
2. **Streaming layer** (§7). Taken from the Upload/Download work if it has
   landed, else built here to the same interface.
3. **Revisions and Export:** revision and incarnation, cuts, the Export RPC,
   the .NET `ExportAsync` and snapshot export, `test_cut`, itest phases.
   Gates G2, G3, G7.
4. **Watch:** history, held FIFO, watchers, wake-ups, progress,
   `initial_state`, resume, the .NET `WatchAsync` and auto-resuming watcher,
   `test_history`, itest phases. Gates G1, G4, G5.
5. **Only if a gate asks:** page pool, zero-copy export framing, per-worker
   watch indexes, wake coalescing, exact-key exports without a scan.

## 14. Alternatives considered

| Alternative | Why not |
|---|---|
| Backtracking regex (PCRE-like) in the server | ReDoS; Redis CVE-2022-36021 is the glob version of the same failure |
| Linking RE2 or Hyperscan | C++ or very large C libraries in a server that links only glibc (ADR 0008); Hyperscan also needs SSSE3+ |
| Clients compile patterns and send DFA tables | puts a regex compiler into every client language (three are planned) and a table format into the wire contract |
| Any lock-held or scan-only Export (§5.1) | stalls writers for milliseconds, or misses keys that a displacement moves behind the scan |
| `fork()` snapshots (Redis `BGSAVE`) | copies the page tables of a multi-GB process (and 2 MiB pages under THP) inside a multi-threaded server; only the forking thread survives, and the data would have to be piped back to the worker that owns the gRPC stream |
| Writers match patterns and push to each watcher | O(watchers) of work inside the critical section that every write shares |
| Per-watcher event queues | memory multiplied by watchers; the shared ring stores each event once |
| Reference-counted history | a cold record-header miss inside the critical section on nearly every write (§6.3) |
| Revisions numbered inside the writer mutex (this proposal's first version) | correct today, but it ties Export and Watch to ADR 0003's lock; the lock-free sequence costs +3.3 ns per write and removes that tie (§3.1) |
| A stable revision that every writer advances by CAS | measured no faster than the mutex: 13.4M vs 15.1M writes/s at 7 threads, all writers on one cache line |
| Per-writer announcements of in-flight revisions | measured 30.2M/s but not monotonic: a stale announcement moved the watermark back 312-3,070 times per verified run, so every consumer would have to clamp |
| An ordered secondary index for prefix queries | O(log n) or O(k) writes and a second copy of every key; against O(1)-first (ADR 0001) |
| Keyspace notifications (Redis Pub/Sub style) | no order guarantee against a snapshot, no resume, silent loss |

## 15. Open decisions

1. **Default exposure.** Keep Export and Watch **off** until enabled
   (proposed, because there is no authentication), or on by default.
2. **Long keys.** Patterns cannot see the text of keys over 64 bytes
   (§3.4). The options:
   - accept it and recommend raw mode for keyspaces that need patterns
     (proposed);
   - or design a prefix-preserving compaction (a contract change that
     amends ADR 0004).
3. **Regex semantics:** bytes (proposed), or UTF-8-aware, where `.` matches
   one code point and automata are larger.
4. **Revisions in replies.** Should Put, Delete and Get replies carry the
   revision (a few bytes each)?
   - It would give read-your-writes together with Watch.
   - A per-record revision (+8 bytes per record) would also give the
     special-API track's versions and compare-and-set a basis.
5. **History default** once Watch is enabled: 65,536 events and 64 MiB are
   the proposed starting point, to confirm with G1 and G4.
6. **Wake coalescing** default, decided by G4.
