/*
 * test_store_mt.c — concurrency torture test for the store.
 *
 * Writers (each owning a disjoint set of key ids) put new versions and delete
 * keys while readers run lock-free lookups. Every value is self-describing:
 *   [0..8) key id, [8..16) version, [16..24) checksum, then filler bytes
 *   derived from (id, version).
 * Reader checks (any failure is counted and fails the test):
 *   * the value belongs to the requested id (never another key's value),
 *   * id/version/checksum/filler are consistent (never a torn or freed
 *     record: freed memory would be overwritten by malloc/free metadata),
 *   * per reader, the version of a key never goes backwards (a single writer
 *     owns each id and only increases versions),
 *   * "stable" ids — inserted before the threads start and never deleted —
 *     are never reported missing, even while cuckoo relocations move them.
 * The table runs near its maximum load so relocations happen constantly.
 * A second phase uses 16 hot keys to maximize version retries and
 * reclamation churn. At the end, with all readers offline, reclamation must
 * drain limbo and the memory accounting must equal the live records exactly.
 *
 * Cached phases repeat the workloads with readers looking up through private
 * L1/L2 caches (hcache.asm, deliberately tiny so entries conflict and are
 * reused) and with the SSE2 tag scan. They add a freshness check that a
 * stale cache would fail: writers publish (version, present) per id after
 * each write returns; a reader that loaded the published state before its
 * lookup must then see at least that version, and never a version at or
 * below one whose delete had already completed.
 *
 * Pinned phases model Download: half the readers pin some records they find
 * (kv_store_pin) and keep them for up to 64 batches — across quiescent
 * announcements and offline periods, when QSBR alone would let the writers'
 * replacements free them — re-validating every held record's bytes after
 * each batch, then unpin. A pin that failed to protect its record would
 * show as a checksum failure (freed memory is reused by malloc) or a crash.
 * At the end no pin may remain (pinned == 0, deferred == 0) and the memory
 * accounting must still equal the live records exactly, so a deferred
 * record the last unpin forgot to free fails the phase.
 *
 * Batched phases (ADR 0011) make the odd writers use the server's batch
 * path — records allocated outside the mutex, then up to 48 puts and
 * deletes applied under one kv_store_write_begin/_end hold — beside the
 * per-write ones, under every reader check above (torn values, monotonic
 * versions, freshness through caches, pins), including -ENOSPC refusals
 * inside a batch and keys written several times in one batch.
 */
#include <pthread.h>
#include <stdatomic.h>

#include "kvasm_abi.h"
#include "test_common.h"

enum { MAX_THREADS = 32 };

typedef struct {
  KvTable *t;
  uint32_t nids, nstable, nwriters, nreaders, seconds;
  uint32_t l1_bits, l2_bits;   /* reader caches; both 0 = readers use kv_store_get */
  uint32_t pin;                /* 1: even readers pin records they read (Download model) */
  _Atomic uint64_t *state;     /* per id: (version << 1) | present, after the write returned */
  atomic_int stop;
  atomic_ullong errors, reads, hits, writes, deletes, full, missing_stable, stale;
  atomic_ullong cache_hits;
  atomic_ullong pins, pin_checks, pin_errors;
} Shared;

/** A record a pinning reader holds across quiescent states. */
typedef struct {
  const KvRec *r;
  uint32_t id;
  uint64_t version;  /**< the version validated when it was pinned */
  uint32_t left;     /**< batches to keep it before unpinning */
} Held;

typedef struct {
  Shared *sh;
  uint32_t idx;
} ThreadArg;

/** Fixed-size key for an id (16 bytes). */
static void key_of(uint32_t id, uint8_t key[16]) {
  uint64_t a = id, b = (uint64_t)id * 0x9E3779B97F4A7C15ull;
  memcpy(key, &a, 8);
  memcpy(key + 8, &b, 8);
}

/** Builds the self-checking value; returns its length (24..120). */
static size_t value_of(uint32_t id, uint64_t version, uint8_t *out) {
  uint64_t s = ((uint64_t)id << 32) ^ version ^ 0xD1B54A32D192ED03ull;
  size_t filler = rng_next(&s) % 97;
  uint64_t idv = id, sum = (uint64_t)id * 31 + version * 131 + filler;
  memcpy(out, &idv, 8);
  memcpy(out + 8, &version, 8);
  memcpy(out + 16, &sum, 8);
  for (size_t i = 0; i < filler; i++) out[24 + i] = (uint8_t)rng_next(&s);
  return 24 + filler;
}

/** @return 1 if (buf,len) is a consistent value for id; sets *version. */
static int value_ok(uint32_t id, const uint8_t *buf, size_t len, uint64_t *version) {
  uint8_t expect[128];
  if (len < 24 || len > 128) return 0;
  uint64_t idv, ver;
  memcpy(&idv, buf, 8);
  memcpy(&ver, buf + 8, 8);
  if (idv != id) return 0;
  size_t n = value_of(id, ver, expect);
  if (n != len || memcmp(expect, buf, len) != 0) return 0;
  *version = ver;
  return 1;
}

/**
 * Re-validates a held record's bytes and identity.
 * @param h  the held record
 * @returns 1 when it is still exactly the version that was pinned
 */
static int held_ok(const Held *h) {
  uint8_t key[16];
  uint64_t v;
  key_of(h->id, key);
  return h->r->key_len == 16 && memcmp(h->r->data, key, 16) == 0 &&
         value_ok(h->id, kv_rec_value(h->r), h->r->val_len, &v) && v == h->version;
}

/** Writer thread: puts (85%) and deletes (15%, never stable ids) of owned ids. */
static void *writer_main(void *p) {
  ThreadArg *a = p;
  Shared *sh = a->sh;
  uint64_t s = 0xABCDEF + a->idx, *ver = calloc(sh->nids, sizeof(uint64_t));
  uint8_t key[16], val[128];
  while (!atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
    uint32_t id = (uint32_t)(rng_next(&s) % sh->nids);
    id -= id % sh->nwriters;
    id += a->idx;
    if (id >= sh->nids) continue;
    key_of(id, key);
    uint64_t h[2];
    kv_store_hash(sh->t, key, 16, h);
    if (id >= sh->nstable && rng_next(&s) % 100 < 15) {
      kv_store_delete(sh->t, key, 16, h[0], h[1]);
      /* Published only after the delete returned: from here on, no reader
       * may see a version <= ver[id] (the next put uses ver[id] + 1). */
      atomic_store_explicit(&sh->state[id], ver[id] << 1, memory_order_release);
      atomic_fetch_add_explicit(&sh->deletes, 1, memory_order_relaxed);
    } else {
      size_t n = value_of(id, ++ver[id], val);
      int64_t rc = kv_store_put(sh->t, key, 16, val, n, h[0], h[1]);
      if (rc == -ENOSPC) atomic_fetch_add_explicit(&sh->full, 1, memory_order_relaxed);
      else if (rc < 0) atomic_fetch_add_explicit(&sh->errors, 1, memory_order_relaxed);
      else /* the put completed: readers that see this must see >= ver[id] */
        atomic_store_explicit(&sh->state[id], (ver[id] << 1) | 1, memory_order_release);
      atomic_fetch_add_explicit(&sh->writes, 1, memory_order_relaxed);
    }
  }
  free(ver);
  return NULL;
}

/** One write of a batched writer's batch. */
typedef struct {
  uint32_t id;
  uint64_t version;  /**< Put: the new version; Delete: the version it ends */
  KvRec *rec;        /**< Put: the filled record; NULL for a Delete */
  uint8_t key[16];
  uint64_t h[2];
  int64_t rc;
} BatchOp;

/**
 * Writer thread using the server's batch path (ADR 0011): batches of 1..48
 * puts and deletes of its owned ids — records allocated and filled with
 * kv_store_rec_alloc outside the mutex — applied in order between
 * kv_store_write_begin and kv_store_write_end with the *_locked functions,
 * against the same readers and invariants as writer_main. A batch may touch
 * one id several times. The published per-id state is updated only after
 * write_end returned, op by op in batch order, which is when the server
 * answers: a reader that sees a state must find at least that version.
 * @param p  ThreadArg*
 * @returns NULL
 */
static void *writer_batch_main(void *p) {
  ThreadArg *a = p;
  Shared *sh = a->sh;
  uint64_t s = 0xBA7C4ED + a->idx, *ver = calloc(sh->nids, sizeof(uint64_t));
  uint8_t val[128];
  BatchOp ops[48];
  /* Its own QSBR slot, after the readers' (run_phase creates nreaders +
   * nwriters slots). It never reads, so the slot stays offline and holds no
   * grace period back; it only collects and frees this writer's retirements. */
  const uint32_t rid = sh->nreaders + a->idx;
  while (!atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
    uint32_t n = 0, want = 1 + (uint32_t)(rng_next(&s) % 48);
    while (n < want) {
      uint32_t id = (uint32_t)(rng_next(&s) % sh->nids);
      id -= id % sh->nwriters;
      id += a->idx;
      if (id >= sh->nids) continue;
      BatchOp *o = &ops[n];
      o->id = id;
      key_of(id, o->key);
      kv_store_hash(sh->t, o->key, 16, o->h);
      o->rec = NULL;
      if (id >= sh->nstable && rng_next(&s) % 100 < 15) {
        o->version = ver[id];
      } else {
        o->version = ++ver[id];
        size_t len = value_of(id, o->version, val);
        KvRec *r = kv_store_rec_alloc_local(sh->t, rid, o->key, 16, len);
        if (kv_is_err(r)) {
          atomic_fetch_add_explicit(&sh->errors, 1, memory_order_relaxed);
          continue;
        }
        memcpy((uint8_t *)kv_rec_value(r), val, len);
        o->rec = r;
      }
      n++;
    }
    kv_store_write_begin(sh->t);
    for (uint32_t i = 0; i < n; i++) {
      BatchOp *o = &ops[i];
      o->rc = o->rec ? kv_store_put_locked(sh->t, rid, o->rec, o->h[0], o->h[1])
                     : kv_store_delete_locked(sh->t, rid, o->key, 16, o->h[0], o->h[1]);
    }
    kv_store_write_end(sh->t, rid);
    for (uint32_t i = 0; i < n; i++) {
      BatchOp *o = &ops[i];
      if (!o->rec) {
        atomic_store_explicit(&sh->state[o->id], o->version << 1, memory_order_release);
        atomic_fetch_add_explicit(&sh->deletes, 1, memory_order_relaxed);
      } else if (o->rc == -ENOSPC) {
        kv_store_rec_free(sh->t, o->rec);   /* refused: still ours */
        atomic_fetch_add_explicit(&sh->full, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&sh->writes, 1, memory_order_relaxed);
      } else {
        if (o->rc < 0) atomic_fetch_add_explicit(&sh->errors, 1, memory_order_relaxed);
        else atomic_store_explicit(&sh->state[o->id], (o->version << 1) | 1, memory_order_release);
        atomic_fetch_add_explicit(&sh->writes, 1, memory_order_relaxed);
      }
    }
  }
  free(ver);
  return NULL;
}

/** Reader thread: lock-free gets with full validation; QSBR slot = idx. */
static void *reader_main(void *p) {
  ThreadArg *a = p;
  Shared *sh = a->sh;
  uint64_t s = 0x12345 + a->idx, *seen = calloc(sh->nids, sizeof(uint64_t));
  uint64_t errors = 0, reads = 0, hits = 0, missing = 0, stale = 0;
  uint8_t key[16];
  /* Odd readers keep a private cache in cached phases, even ones read the
   * table directly, so cached and uncached readers race on the same keys. */
  KvHCache *cache = NULL;
  if ((sh->l1_bits | sh->l2_bits) && (a->idx & 1)) {
    cache = kv_hcache_create(sh->t, sh->l1_bits, sh->l2_bits);
    CHECK(cache != NULL);
  }
  /* Pinned phases: even readers hold up to 16 pinned records at a time. */
  enum { HOLD = 16 };
  Held held[HOLD];
  int nheld = 0, pinning = sh->pin && (a->idx & 1) == 0;
  uint64_t pins = 0, pin_checks = 0, pin_errors = 0;
  kv_qsbr_online(sh->t, a->idx);
  while (!atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
    for (int i = 0; i < 64; i++) {
      uint32_t id = (uint32_t)(rng_next(&s) % sh->nids);
      key_of(id, key);
      uint64_t h[2];
      kv_store_hash(sh->t, key, 16, h);
      /* Loaded before the lookup: every write it describes has returned. */
      uint64_t done = atomic_load_explicit(&sh->state[id], memory_order_acquire);
      const KvRec *r = cache ? kv_hcache_get(cache, key, 16, h[0], h[1])
                             : kv_store_get(sh->t, key, 16, h[0], h[1]);
      reads++;
      if (!r) {
        if (id < sh->nstable) missing++;
        continue;
      }
      hits++;
      uint64_t v;
      if (r->key_len != 16 || memcmp(r->data, key, 16) != 0 ||
          !value_ok(id, kv_rec_value(r), r->val_len, &v)) {
        errors++;
        continue;
      }
      if (v < seen[id]) errors++; /* time went backwards for this key */
      seen[id] = v;
      /* Freshness: at least the last completed put; above a completed delete. */
      if ((done & 1) ? v < (done >> 1) : (done && v <= (done >> 1))) stale++;
      /* Download model: pin it while this online period still protects it. */
      if (pinning && nheld < HOLD && (rng_next(&s) & 7) == 0) {
        if (kv_store_pin(sh->t, r) == 0) {
          held[nheld++] = (Held){r, id, v, 1 + (uint32_t)(rng_next(&s) % 64)};
          pins++;
        } else {
          pin_errors++;               /* -ENOMEM: never expected here */
        }
      }
    }
    kv_qsbr_quiescent(sh->t, a->idx); /* drop references between batches */
    if ((rng_next(&s) & 255) == 0) {  /* occasionally idle, like epoll_wait */
      kv_qsbr_offline(sh->t, a->idx);
      kv_qsbr_online(sh->t, a->idx);
    }
    /* Held records past a quiescent state: only their pins protect them now.
     * Their bytes must still be exactly the pinned version. */
    for (int i = nheld - 1; i >= 0; i--) {
      pin_checks++;
      if (!held_ok(&held[i])) pin_errors++;
      if (--held[i].left == 0) {
        kv_store_unpin(sh->t, held[i].r);
        held[i] = held[--nheld];
      }
    }
  }
  for (int i = 0; i < nheld; i++) {   /* release what is still held, checked once more */
    pin_checks++;
    if (!held_ok(&held[i])) pin_errors++;
    kv_store_unpin(sh->t, held[i].r);
  }
  atomic_fetch_add(&sh->pins, pins);
  atomic_fetch_add(&sh->pin_checks, pin_checks);
  atomic_fetch_add(&sh->pin_errors, pin_errors);
  kv_qsbr_offline(sh->t, a->idx);
  if (cache) {
    uint64_t st[4];
    kv_hcache_stats(cache, st);
    atomic_fetch_add(&sh->cache_hits, st[0] + st[1]);
    kv_hcache_destroy(cache);
  }
  atomic_fetch_add(&sh->errors, errors);
  atomic_fetch_add(&sh->reads, reads);
  atomic_fetch_add(&sh->hits, hits);
  atomic_fetch_add(&sh->missing_stable, missing);
  atomic_fetch_add(&sh->stale, stale);
  free(seen);
  return NULL;
}

/**
 * Runs one phase and checks all invariants.
 * @param name label. @param capacity store capacity. @param nids key space.
 * @param nstable never-deleted ids. @param nw writers. @param nr readers.
 * @param seconds duration.
 * @param l1_bits,l2_bits private cache sizes of the odd readers (0,0 = none).
 * @param scan KV_SCAN_SWAR or KV_SCAN_SSE2, set while no thread runs.
 * @param pin 1: even readers pin and hold records (the Download model).
 * @param batch 1: odd writers use the batched write API (writer_batch_main),
 *        even writers the per-write one, against the same keys' readers.
 */
static void run_phase(const char *name, uint64_t capacity, uint32_t nids, uint32_t nstable,
                      uint32_t nw, uint32_t nr, uint32_t seconds, uint32_t l1_bits,
                      uint32_t l2_bits, int scan, int pin, int batch) {
  uint8_t seed[16];
  uint64_t s = 99;
  rng_fill(&s, seed, 16);
  Shared sh = {.nids = nids, .nstable = nstable, .nwriters = nw, .nreaders = nr, .seconds = seconds,
               .l1_bits = l1_bits, .l2_bits = l2_bits, .pin = (uint32_t)pin};
  sh.state = calloc(nids, sizeof *sh.state);
  CHECK(sh.state != NULL);
  CHECK(kv_set_scan_mode(scan) >= 0);
  sh.t = kv_store_create(capacity, nr + nw, seed, 0); /* + a slot per (batched) writer */
  CHECK(sh.t != NULL);
  /* Few generation slots on purpose: unrelated keys share them, which must
   * only cost misses. Enabled for every phase (writers bump either way). */
  CHECK(kv_store_enable_inval(sh.t, 10) == 0);
  CHECK(kv_store_enable_inval(sh.t, 10) == -EINVAL); /* once only */
  uint8_t key[16], val[128];
  for (uint32_t id = 0; id < nstable; id++) {
    key_of(id, key);
    uint64_t h[2];
    kv_store_hash(sh.t, key, 16, h);
    size_t n = value_of(id, 0, val);
    CHECK(kv_store_put(sh.t, key, 16, val, n, h[0], h[1]) == 1);
  }
  pthread_t th[MAX_THREADS];
  ThreadArg args[MAX_THREADS];
  for (uint32_t i = 0; i < nr; i++) {
    args[i] = (ThreadArg){&sh, i};
    pthread_create(&th[i], NULL, reader_main, &args[i]);
  }
  for (uint32_t i = 0; i < nw; i++) {
    args[nr + i] = (ThreadArg){&sh, i};
    pthread_create(&th[nr + i], NULL, (batch && (i & 1)) ? writer_batch_main : writer_main,
                   &args[nr + i]);
  }
  sleep(seconds);
  atomic_store(&sh.stop, 1);
  for (uint32_t i = 0; i < nr + nw; i++) pthread_join(th[i], NULL);

  /* Quiescent end state: all readers offline => everything retired is free-able.
   * kv_store_reclaim frees at most RECLAIM_BATCH_FORCE (4096) records per call
   * (bounded work for the server's housekeeping tick) and returns how many
   * still wait, so drain in a loop: a busy host can end a phase with tens of
   * thousands in limbo. With every reader offline each call makes progress;
   * the cap only guards against a store that stopped freeing. */
  for (int round = 0; round < 1000; round++) {
    /* The global limbo, then every slot's own (batched writers retired into
     * theirs; their owners have exited, so draining them from here is safe). */
    uint64_t left = kv_store_reclaim(sh.t);
    for (uint32_t r = 0; r < nr + nw; r++) left += kv_store_reclaim_local(sh.t, r, 4096);
    if (left == 0) break;
  }
  KvStats st;
  kv_store_stats(sh.t, &st);
  uint64_t live_bytes = 0, live = 0;
  kv_qsbr_online(sh.t, 0);
  for (uint32_t id = 0; id < nids; id++) {
    key_of(id, key);
    uint64_t h[2];
    kv_store_hash(sh.t, key, 16, h);
    const KvRec *r = kv_store_get(sh.t, key, 16, h[0], h[1]);
    if (r) {
      live++;
      live_bytes += 24 + 16 + r->val_len;
      uint64_t v;
      CHECK(value_ok(id, kv_rec_value(r), r->val_len, &v));
    } else {
      CHECK(id >= nstable);
    }
  }
  kv_qsbr_offline(sh.t, 0);
  CHECK_EQ_U64(st.limbo_count, 0);
  CHECK_EQ_U64(st.count, live);
  CHECK_EQ_U64(st.mem_used, live_bytes);
  CHECK_EQ_U64(atomic_load(&sh.errors), 0);
  CHECK_EQ_U64(atomic_load(&sh.missing_stable), 0);
  CHECK_EQ_U64(atomic_load(&sh.stale), 0);
  if (l1_bits | l2_bits) CHECK(atomic_load(&sh.cache_hits) > 0); /* the cache was exercised */
  /* Every pin released: nothing pinned, nothing deferred (a deferred record
   * the last unpin did not free would also break mem_used == live_bytes). */
  CHECK_EQ_U64(st.pinned, 0);
  CHECK_EQ_U64(st.deferred, 0);
  CHECK_EQ_U64(atomic_load(&sh.pin_errors), 0);
  if (pin) {
    CHECK(atomic_load(&sh.pins) > 0);
    unsigned long long bad = (unsigned long long)atomic_load(&sh.pin_errors);
    printf("  %-12s %llu pins held across quiescent states, %llu re-validations, %llu %s\n",
           name, (unsigned long long)atomic_load(&sh.pins),
           (unsigned long long)atomic_load(&sh.pin_checks), bad,
           bad ? "CORRUPTED (a pinned record was freed or changed)" : "corrupted");
  }
  printf("  %-12s %2u readers %2u writers %us: %llu reads (%llu hits, %llu from caches), %llu writes,"
         " %llu deletes, %llu full; live %llu (load %.0f%%), bfs %llu, moves %llu, freed %llu\n",
         name, nr, nw, seconds, (unsigned long long)atomic_load(&sh.reads),
         (unsigned long long)atomic_load(&sh.hits), (unsigned long long)atomic_load(&sh.cache_hits),
         (unsigned long long)atomic_load(&sh.writes), (unsigned long long)atomic_load(&sh.deletes),
         (unsigned long long)atomic_load(&sh.full), (unsigned long long)live,
         100.0 * (double)live / (double)(st.nbuckets * 6), (unsigned long long)st.bfs,
         (unsigned long long)st.moves, (unsigned long long)st.freed);
  kv_store_destroy(sh.t);
  free(sh.state);
  kv_set_scan_mode(KV_SCAN_SWAR);
}

/** Entry point. @return 0 when no invariant was violated. */
int main(void) {
  /* capacity = 5 * 32768 => up to 83% index load; ids > capacity => ENOSPC churn */
  run_phase("near-full", 163840, 180000, 20000, 4, 8, 4, 0, 0, KV_SCAN_SWAR, 0, 0);
  run_phase("hot-keys", 1024, 16, 4, 4, 8, 2, 0, 0, KV_SCAN_SWAR, 0, 0);
  /* Same workloads through private caches (L1 256, L2 4096 entries) and SSE2. */
  run_phase("near-full/c", 163840, 180000, 20000, 4, 8, 3, 8, 12, KV_SCAN_SSE2, 0, 0);
  run_phase("hot-keys/c", 1024, 16, 4, 4, 8, 2, 8, 12, KV_SCAN_SSE2, 0, 0);
  run_phase("hot-keys/l2", 1024, 16, 4, 4, 8, 1, 0, 6, KV_SCAN_SWAR, 0, 0); /* L2 only */
  /* Download model: pinned records outlive constant replacement (hot keys:
   * nearly every pinned record is retired while held, so most pins end in a
   * deferral) and relocation churn near full load. */
  run_phase("hot-keys/pin", 1024, 16, 4, 4, 8, 2, 0, 0, KV_SCAN_SSE2, 1, 0);
  run_phase("near-full/pin", 163840, 180000, 20000, 4, 8, 2, 0, 0, KV_SCAN_SWAR, 1, 0);
  /* Batched writers (ADR 0011) beside per-write ones: relocation churn near
   * full load and -ENOSPC, hot keys hit several times per batch, then the
   * same through private caches (freshness) and with pinning readers. */
  run_phase("near-full/b", 163840, 180000, 20000, 4, 8, 3, 0, 0, KV_SCAN_SSE2, 0, 1);
  run_phase("hot-keys/b", 1024, 16, 4, 4, 8, 2, 0, 0, KV_SCAN_SWAR, 0, 1);
  run_phase("hot-keys/bc", 1024, 16, 4, 4, 8, 2, 8, 12, KV_SCAN_SSE2, 0, 1);
  run_phase("hot-keys/bp", 1024, 16, 4, 4, 8, 2, 0, 0, KV_SCAN_SSE2, 1, 1);
  return test_finish("test_store_mt");
}
