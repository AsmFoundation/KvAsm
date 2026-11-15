# ADR 0011: Batched writes, with retirement, reclamation and record reuse per worker

- **Status:** accepted
- **Date:** 2026-11-15
- **Amends:** [ADR 0003](0003-concurrency-model.md) (writers and QSBR),
  [ADR 0005](0005-batched-get-prefetch-and-worker-pool.md) (the batch)
- **Evidence:** `benchmarks/results/2026-11-15-write-batching` (A/B runs,
  profiles, scripts), `server/bench/bench_write.c` (store level)

## Context

The question was "why are our writes so slow?". The published comparison
(2026-09-27) had 8.84M GET/s at 4 cores but only 2.23M SET/s: 0.87× Redis,
0.60× Garnet. The 0.2.0 server measured alone on today's (busier) host
(`solo.sh`, 100% SET):

- **One worker:** 0.65M SET/s, at 1,587 ns of server CPU per SET.
- **Four workers:** 1.21M SET/s at 3,071 ns per SET. Four cores did not
  reach twice one core, because each SET's CPU cost doubled.

**Where the time went.** These measurements located it:

- **Kernel time is the same for GET and SET** in the same setup:
  1,108 vs 1,278 ns per request (`faults_set.sh`, READ_RATIO 1 vs 0). That
  is loopback TCP and waking the co-located client. The SET-specific cost
  was user time: **1,105 ns per SET against 394 per GET**.
- **No page faults:** 0.000 minor faults per SET. glibc's heap trimming is
  not the cause.
- **A profile of one worker under 100% SET** (`profile_set.sh`, perf
  cpu-clock, `profile-original-1w.out`):
  - `cuckoo_scan_bucket`: 38.9% of user time;
  - `store_reclaim_step`, reading and freeing retired records: 15.2%;
  - glibc `malloc`/`free`: 20.9%.

**Why, in the code.** Every Put did all of this, one write at a time:

1. a LOCK XADD on the shared memory counter, `malloc`, and copies;
2. then, under the ONE writer mutex:
   - the index update, whose bucket and old record were not prefetched (a
     GET's are, ADR 0005), so their DRAM misses were taken while holding
     the mutex;
   - an XCHG of the global epoch per retired record, which invalidated the
     line every worker reads at each quiescent announcement;
   - a reclamation step that read up to 8 cold retired records and called
     `free()` on each, still under the mutex.

Every other worker waited through those misses and frees, and the mutex's
cache line changed cores on every write. A store probe had already measured
the result: 4.51M puts/s with one writer, 2.07M with four
(golden-services P3).

## Decision

### 1. Writes join the read's batch (`grpc.asm`)

- **At dispatch** a Put's record is allocated and filled, outside the mutex,
  while the value is hot from parsing. Its key is hashed and its buckets
  are prefetched, exactly as for a Get. A Delete keeps its key and hashes.
  Gets, Puts and Deletes are linked into the connection's batch in arrival
  order.
- **`kv_grpc_run_batch`** runs three passes:
  1. prefetch every call's candidate records (for a write, the old record
     whose key the index update compares);
  2. apply every Put and Delete, in arrival order, under one hold of the
     writer mutex (`batch_apply_writes`, at most 64 writes per hold);
  3. look up the Gets and answer every call, in arrival order.
- **Writes take effect before the Gets of the same read,** as they did
  when Puts ran at dispatch. Every call of one read was in flight at once
  (none was answered before the read ended), so this order is a valid
  linearization (the argument of ADR 0005).
- **A write the peer resets within the same read is not applied.** Its
  record is freed with the stream. gRPC cancellation promises nothing
  either way; before, such a Put had already run at dispatch.

### 2. The store's batch API (`store.asm`)

- **`kv_store_write_begin`, `kv_store_put_locked`,
  `kv_store_delete_locked`, `kv_store_write_end`.** The mutex covers only
  the index update and the invalidation bump (hcache.asm's order is kept:
  publish, bump, only then retire).
- **The records a hold unlinks go to the writer's own QSBR slot**
  (`QsbrSlot.pending`), not to the global limbo.
- **`kv_store_write_end` unlocks first, then:**
  - advances the epoch E → E+1 with **one** LOCK XADD for the whole hold;
  - stamps every pending record E and appends them to the slot's own
    limbo;
  - frees what is due there (`slot_reclaim`, budget 2n + 8).

  Why this is safe: the unlinks happened before the unlock, and the locked
  increment is a full fence after them. A reader that loads an epoch > E
  therefore loads it after the unlinks and cannot reach any of the records
  — the QSBR argument of `store.asm`'s header, with the stamp taken after
  the unlinks instead of under the mutex.
- **The global limbo's epoch bump became a LOCK XADD as well.** Slots now
  advance the epoch outside the mutex, and the old load + XCHG pair could
  move it backwards in between.
- **Reclamation per slot runs outside the mutex, in parallel across
  workers.**
  - The bound is min(epoch at scan time, observed epoch of every online
    reader).
  - Scans are rate-limited after a hold. They are forced on each worker's
    once-a-second housekeeping tick (`kv_store_reclaim_local`), so an idle
    worker's last records come back within a tick. The first version
    rate-limited the tick too, and held an idle worker's records for 16 s;
    `itest`'s stalled-download memory checks caught it.
- **Pins:** a record a Download pinned must be deferred, not freed, and the
  pin table is writer-mutex state.
  - `pin_used` is read after the reader scan. A Download pins under the
    mutex before its worker's next quiescent announcement. x86-TSO makes
    that worker's stores visible in order, so once the scan saw the
    announcement that ended a record's grace period, that pin is visible
    too.
  - With no pin anywhere, records are freed without the mutex. Otherwise
    the slot takes the mutex and checks each record.

### 3. A record cache per worker (`kv_store_rec_alloc_local`)

- **The records a slot's reclamation frees stay in the slot,** LIFO per
  glibc chunk-size class (records up to 1,032 bytes, 64 classes), for that
  worker's next Puts.
- **It is bounded:** 256 records per class and 256 KiB per worker. The
  excess goes back to glibc.
- **Classes are glibc's chunk sizes,** so any block of a class fits any
  request of the class. The store asks malloc for the whole chunk anyway.

Why: a record is allocated by the worker that wrote it and freed by the
worker whose write replaced it. With several workers, nearly every `free()`
crossed threads and took another worker's arena lock. After steps 1–2 that
was 44% of a worker's user time at 4 workers, `__lll_lock_wait_private`
included. A 1,024-entry glibc tcache (`GLIBC_TUNABLES`) proved the cause
first: 1.74× SET/s at 4 workers, paired 6/6. That fix is fragile, though:
any user-set `GLIBC_TUNABLES` replaces it, and it caches every size class
of every thread without bound. The slot cache is the same mechanism, owned
and bounded by the store.

### Memory and layout

- **`QsbrSlot` grows from 64 bytes to 1,472:**
  - line 0 is still the shared observed epoch;
  - the other lines are owner-private (limbo, scan bound, pending records,
    the cache heads and counts), so the owner's bookkeeping never
    invalidates the line other workers' scans read.
- **A worker's limbo** holds about one to two holds of records.
- **Its cache holds up to 256 KiB of freed records.** These are not
  counted by `KVNODE_MAX_MEMORY_MB`, like glibc's free memory.

## Measurements

- **Each server alone** (`solo.sh`; 4 workers on their own cores, the
  client on 16 vCPUs, 1M keys, 100-byte values, 64 × 16 in flight;
  original and new alternated, 3 pairs):

| Workload | 0.2.0 | ADR 0011 | Per pair | Server CPU per request |
|---|---|---|---|---|
| 100% SET, 4 workers | 1.21M/s | **3.21M/s** | **2.61×** (2.57, 2.67, 2.59) | 3,071 → 1,254 ns |
| 50/50, 4 workers | 2.05M/s | **3.51M/s** | **1.77×** (1.87, 1.74, 1.72) | 1,884 → 1,123 ns |
| 100% SET, 1 worker | 0.65M/s | **0.78M/s** | **1.19×** (1.20, 1.20, 1.18) | 1,587 → 1,219 ns |

  - **Writes now scale end to end.** From 1 to 4 workers, SET/s grows
    4.1× at nearly constant CPU per SET; 0.2.0 grew 1.9×.
  - **A SET costs about what a GET costs.** At 4 workers, server CPU per
    SET ÷ per GET fell from 3.3× to 1.16× (paired run, `ab-final.out`).

- **End to end, paired:** A/B (`abw.py`), 6 blocks per configuration, both
  servers at once.
  - Each server's threads share their physical cores with the other's
    (SMT siblings), so the hardware is identical by construction.
  - The first slot layout was discarded. It gave each server its own
    vCPUs, and in this VM those were sibling pairs. At 4 workers the
    per-block ratio then followed the slot (0.35–1.97).
  - Ratios are paired per block; sign test, 6/6 = p 0.031.
  - Absolute rates are half a core's worth per variant.

| Configuration (workers : read ratio) | Batching only (step 1) | + slot reclamation (step 2) | + record cache (final) |
|---|---|---|---|
| 1 : 100% SET | 1.19× (6/6) | 1.22× (6/6) | **1.25×** (6/6); p50 0.79×, CPU per SET 0.80× |
| 4 : 100% SET | 1.01× (n.s.), p99 1.24× worse | 1.45× (6/6) | **2.76×** (6/6; the new server's client saturates, so a floor); p50 0.26×, CPU per SET 0.35× |
| 4 : 50/50 | **0.85× (0/6): regressed** | 1.35× (6/6) | **1.65×** (6/6); p50 0.46×, CPU per request 0.57× |
| 1 : 90% GET | 1.02× (6/6) | 1.05× (6/6) | 1.02× (6/6) |
| 1 : 100% GET | 1.00 | 1.00 | 1.00 |
| 4 : 100% GET | — | 1.00 | 0.99 (n.s.; client-bound in both) |
| 1 : 100% SET, 256 connections × 1 (unpipelined) | 1.01 (n.s.) | 1.04 (n.s.) | 0.97 (1/6, p 0.22, n.s.) |

The step columns compare each step with the original server; the final
column is the final server against the original
(`ab-batching.out`, `ab-step2.out`, `ab-final.out`). Unpipelined writes
have nothing to batch within a read (one request per read), so they gain
nothing; the final 0.97 is within this configuration's noise (its blocks
range 0.90–1.07), but it is not a win either.

- **Step 1 alone was rejected by its own measurement.** Holds of up to 64
  writes still contained reclamation's cold misses and `free()` calls, so
  other workers spun out and slept on the futex. The Gets of a batch now
  waited behind their writes' mutex, and the mixed load lost 15%. Step 2
  removed reclamation from the mutex and turned the loss into +35%.
- **Store level** (`bench_write`, 1M keys, 100-byte values, uniform
  replacements, 3 repetitions × 2 s per cell, threads pinned one per core):

| Writer threads | Per-write `kv_store_put` | Batched, final | Batched / per-write |
|---|---|---|---|
| 1 | 1.66M/s | 5.42M/s | 3.1× |
| 2 | 1.30M/s | 9.19M/s | 6.4× |
| 4 | 1.27M/s | 12.58M/s | 10.0× |
| 7 | 1.05M/s | 6.89M/s | 7.2× (5.7–9.1× over repetitions) |

  - **Per-write locking got slower with every thread added**
    (1.66 → 1.05M/s).
  - **Batched writes now scale: 2.32× from 1 to 4 threads.**
  - **Seven threads fall back** on this host, whose other programs leave
    no spare physical cores. That cell's ratio also varies most.
  - **Before the record cache** (step 2), the same benchmark measured 3.76,
    2.51, 2.76 and 3.37M/s: no scaling at all.

- **Against the other stores** (W1, `run.py --profile writes`,
  `benchmarks/results/2026-11-15-writes-w1`): indicative only.
  - Every run window overlapped other work in the VM, and a median 10.3
    CPUs were busy outside it.
  - KvNode's writes came out ahead of all five on 100% SET, from 1.72×
    Garnet (5/6 blocks) to 4.17× Memcached. On 50/50 it led all five,
    1.76–3.42×.
  - The quiet-host ratios of 2026-09-27 (0.87× Redis, 0.60× Garnet), times
    this change's paired gain, predict the same picture.
  - A quiet-host re-run must confirm it before docs/CLAIMS.md changes.
- **What the final server still spends** (`profile-final-4w.out`, 4
  workers, 100% SET):
  - 3.54M SET/s at 1,185 ns of server CPU per SET: 450 ns user, 735 ns
    kernel.
  - glibc's allocator is down to ~1% of user time (it was ≥ 43.8% after
    step 2).
  - The largest user-space items:
    - the batch's prefetched misses landing (14.8%);
    - cold retired-record headers in `slot_reclaim` (12.0%);
    - the memory counter's LOCK XADD (7.2%);
    - the cold link of a popped cache block (4.3%).
  - Even all of those together come to less than a sixth of a SET's
    server CPU; the rest is the kernel's socket work, the same as a GET's.

## Alternatives considered

- **Flat combining** (golden-services P3, 1.91× at 7 writers in a
  prototype): one combiner applies every thread's writes.
  - It removes handoffs but keeps every write on one core.
  - Once batching amortized the handoffs, the remaining limits were cold
    reclamation and cross-thread `free()`, which combining does not
    touch.
  - Not needed now. It stays the next step if the mutex itself becomes
    the limit.
- **Lock striping** (libcuckoo-style): writes on disjoint buckets in
  parallel.
  - It is the way to scale writes beyond one mutex.
  - It is invasive: cuckoo paths cross buckets, and the shared counters
    would need atomics.
  - Deferred until measurements show the mutex as the limit.
- **A ring-buffer limbo** instead of the list through `Rec.next`.
  - Reclamation could then prefetch deeper and never touch a record to
    decide.
  - Cold record headers remain 12.0% of user time in the final profile.
  - A candidate, not done.
- **Shipping the glibc tunable** (`GLIBC_TUNABLES` in the image): measured
  and replaced by the record cache, for the reasons in Decision §3.
- **Per-slot memory accounting.** The LOCK XADD on the shared memory
  counter remains: 7.2% of user time at 4 workers. In the final profile,
  58% of `kv_store_rec_alloc_local`'s samples land on the instruction
  after it.
  - Exact `KVNODE_MAX_MEMORY_MB` checks need it.
  - A per-slot credit scheme is a candidate: one LOCK XADD per many
    records, at the price of a limit that is exact only to the credit.
- **Prefetching the next cache block.** Popping a block reads its link, a
  cold line: 4.3% of user time. Prefetching the new head on each pop would
  hide it for the next Put. A candidate, too small to resolve end to end
  on this host.

## Verification

- **`test_store`:**
  - **Test 9:** the random workload in batches of 1–64 with keys repeated
    within a batch, checking return codes in order, the final state, a
    short limbo, and exact memory accounting once the slot is drained.
  - **Test 10:** the record cache — the same block comes back, only for
    its class, the per-class cap holds (exactly 256 of 300 retired blocks
    return), and accounting stays exact.
- **`test_store_mt`:** four new phases with batched writers beside
  per-write ones, under every reader check: torn values, monotonic
  versions, freshness through private caches, pins (0 corrupted), and
  `-ENOSPC` inside batches.
- **`itest_grpc`:**
  - `test_batch_order`: Put, Put, Get, Delete, Get, Put, Get sent together
    on one key, 50 rounds. The writes answer exactly as sequential writes
    would, and each Get answers from a state at or after the writes sent
    before it.
  - Every phase as before: h2spec 146/146, fuzzing, streaming limits, and
    stalled downloads, where a pinned record's memory comes back 20 ms
    after its call ends.
- **`bench_write --quick`** in `make test`: values verified and accounting
  exact after draining every slot.
- **The .NET suite** against the image.
