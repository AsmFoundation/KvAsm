/*
 * test_store.c — single-threaded behaviour of the store (store.asm +
 * cuckoo.asm) against a reference model.
 *
 *  1. Random put/get/delete sequence (400k ops) vs a model array: return
 *     codes, values and counts must match exactly. Keys are 4..48 bytes.
 *  2. Short keys: the empty key and all 1- and 2-byte keys.
 *  3. Capacity: a table sized so the index is ~83% full at max_records must
 *     accept exactly max_records keys, then refuse new keys with -ENOSPC while
 *     still accepting replacements; every key stays retrievable.
 *  4. Memory limit: -ENOMEM at the limit; memory returns after delete+reclaim.
 *  5. Oversized lengths: -E2BIG without touching memory.
 *  6. QSBR grace period: a record held by an online reader is not freed until
 *     that reader announces a quiescent state (or goes offline).
 */
#include "kvnode_abi.h"
#include "test_common.h"

enum { NIDS = 20000, MAXV = 200 };

/** Model entry for one key id. */
typedef struct {
  int present;
  uint32_t vlen;
  uint8_t val[MAXV];
} ModelEntry;

static ModelEntry g_model[NIDS];

/** Deterministic key bytes for an id: 4-byte id prefix + 0..44 byte suffix. */
static size_t make_key(uint32_t id, uint8_t *out) {
  uint64_t s = 0x9E3779B97F4A7C15ull ^ (id * 0x100000001B3ull) ^ 1;
  size_t suffix = rng_next(&s) % 45;
  memcpy(out, &id, 4);
  for (size_t i = 0; i < suffix; i++) out[4 + i] = (uint8_t)rng_next(&s);
  return 4 + suffix;
}

/** Store size in bytes of a record, as accounted by kv_store_put. */
static uint64_t rec_size(size_t klen, size_t vlen) {
  return 24 + ((klen + 7) & ~(size_t)7) + vlen;
}

/** Looks up key and compares against expected presence/value. */
static void expect_get(KvTable *t, const uint8_t *key, size_t klen, int present,
                       const uint8_t *val, uint32_t vlen) {
  uint64_t h[2];
  kv_store_hash(t, key, klen, h);
  const KvRec *r = kv_store_get(t, key, klen, h[0], h[1]);
  CHECK((r != NULL) == (present != 0));
  if (r && present) {
    CHECK(r->key_len == klen);
    CHECK(memcmp(r->data, key, klen) == 0);
    CHECK(r->val_len == vlen);
    CHECK(memcmp(kv_rec_value(r), val, vlen) == 0);
  }
}

/** Test 1: random operations against the model. */
static void test_model(void) {
  uint8_t seed[16] = {1, 2, 3};
  KvTable *t = kv_store_create(NIDS * 2, 1, seed, 0);
  CHECK(t != NULL);
  kv_qsbr_online(t, 0);
  uint64_t s = 42, live = 0, bytes = 0;
  uint8_t key[64], val[MAXV];
  for (int op = 0; op < 400000; op++) {
    uint32_t id = (uint32_t)(rng_next(&s) % NIDS);
    size_t klen = make_key(id, key);
    uint64_t h[2];
    kv_store_hash(t, key, klen, h);
    ModelEntry *m = &g_model[id];
    uint32_t dice = (uint32_t)(rng_next(&s) % 10);
    if (dice < 5) {
      uint32_t vlen = (uint32_t)(rng_next(&s) % MAXV);
      rng_fill(&s, val, vlen);
      int64_t rc = kv_store_put(t, key, klen, val, vlen, h[0], h[1]);
      CHECK(rc == (m->present ? 0 : 1));
      if (!m->present) { live++; } else { bytes -= rec_size(klen, m->vlen); }
      bytes += rec_size(klen, vlen);
      m->present = 1;
      m->vlen = vlen;
      memcpy(m->val, val, vlen);
    } else if (dice < 8) {
      expect_get(t, key, klen, m->present, m->val, m->vlen);
    } else {
      int64_t rc = kv_store_delete(t, key, klen, h[0], h[1]);
      CHECK(rc == (m->present ? 1 : 0));
      if (m->present) { live--; bytes -= rec_size(klen, m->vlen); }
      m->present = 0;
    }
    if ((op & 1023) == 0) kv_qsbr_quiescent(t, 0);
  }
  for (uint32_t id = 0; id < NIDS; id++) {
    size_t klen = make_key(id, key);
    expect_get(t, key, klen, g_model[id].present, g_model[id].val, g_model[id].vlen);
  }
  kv_qsbr_offline(t, 0);
  kv_store_reclaim(t); /* reader offline => every retired record is free-able */
  KvStats st;
  kv_store_stats(t, &st);
  CHECK_EQ_U64(st.count, live);
  CHECK_EQ_U64(st.limbo_count, 0);
  CHECK_EQ_U64(st.mem_used, bytes);
  printf("  model: %llu live, %llu inserts, %llu replaces, %llu deletes, %llu bfs, %llu moves\n",
         (unsigned long long)st.count, (unsigned long long)st.inserts,
         (unsigned long long)st.replaces, (unsigned long long)st.deletes,
         (unsigned long long)st.bfs, (unsigned long long)st.moves);
  kv_store_destroy(t);
}

/** Test 2: empty and very short keys (exercise the 0..7-byte compare path). */
static void test_short_keys(void) {
  uint8_t seed[16] = {9};
  KvTable *t = kv_store_create(2000, 0, seed, 0);
  uint8_t key[2], val[1];
  uint64_t h[2];
  kv_store_hash(t, key, 0, h);
  CHECK(kv_store_put(t, key, 0, "E", 1, h[0], h[1]) == 1);
  for (int a = 0; a < 256; a++) {
    key[0] = (uint8_t)a; val[0] = (uint8_t)(a ^ 0x5A);
    kv_store_hash(t, key, 1, h);
    CHECK(kv_store_put(t, key, 1, val, 1, h[0], h[1]) == 1);
  }
  for (int a = 0; a < 1000; a++) {
    key[0] = (uint8_t)a; key[1] = (uint8_t)(a >> 8); val[0] = (uint8_t)a;
    kv_store_hash(t, key, 2, h);
    CHECK(kv_store_put(t, key, 2, val, 1, h[0], h[1]) == 1);
  }
  kv_store_hash(t, key, 0, h);
  expect_get(t, key, 0, 1, (const uint8_t *)"E", 1);
  for (int a = 0; a < 256; a++) {
    key[0] = (uint8_t)a; val[0] = (uint8_t)(a ^ 0x5A);
    expect_get(t, key, 1, 1, val, 1);
  }
  for (int a = 0; a < 1000; a++) {
    key[0] = (uint8_t)a; key[1] = (uint8_t)(a >> 8); val[0] = (uint8_t)a;
    expect_get(t, key, 2, 1, val, 1);
  }
  kv_store_destroy(t);
}

/** Test 3: exact capacity behaviour at ~83% index load. */
static void test_capacity(void) {
  const uint64_t cap = 40960; /* = 5 * 8192: exactly 83.3% of 8192*6 slots at capacity */
  uint8_t seed[16] = {7, 7};
  KvTable *t = kv_store_create(cap, 0, seed, 0);
  uint8_t key[16];
  uint64_t h[2], ok = 0;
  for (uint64_t i = 0; i < cap; i++) {
    memcpy(key, &i, 8); memcpy(key + 8, &i, 8);
    kv_store_hash(t, key, 16, h);
    if (kv_store_put(t, key, 16, key, 8, h[0], h[1]) == 1) ok++;
  }
  CHECK_EQ_U64(ok, cap);
  uint64_t extra = cap + 1;
  memcpy(key, &extra, 8); memcpy(key + 8, &extra, 8);
  kv_store_hash(t, key, 16, h);
  CHECK(kv_store_put(t, key, 16, key, 8, h[0], h[1]) == -ENOSPC);
  uint64_t zero = 0; /* replacing an existing key must still work when full */
  memcpy(key, &zero, 8); memcpy(key + 8, &zero, 8);
  kv_store_hash(t, key, 16, h);
  CHECK(kv_store_put(t, key, 16, "new", 3, h[0], h[1]) == 0);
  for (uint64_t i = 1; i < cap; i++) {
    memcpy(key, &i, 8); memcpy(key + 8, &i, 8);
    expect_get(t, key, 16, 1, key, 8);
  }
  KvStats st;
  kv_store_stats(t, &st);
  printf("  capacity: %llu/%llu keys in %llu buckets (load %.1f%%), %llu bfs inserts, %llu moves\n",
         (unsigned long long)st.count, (unsigned long long)cap, (unsigned long long)st.nbuckets,
         100.0 * (double)st.count / (double)(st.nbuckets * 6), (unsigned long long)st.bfs,
         (unsigned long long)st.moves);
  kv_store_destroy(t);
}

/** Test 4 and 5: memory limit and oversized lengths. */
static void test_limits(void) {
  uint8_t seed[16] = {3};
  KvTable *t = kv_store_create(1000, 0, seed, 4096);
  uint8_t key[8] = {0}, val[1000] = {0};
  uint64_t h[2];
  int accepted = 0;
  for (int i = 0; i < 20; i++) {
    key[0] = (uint8_t)i;
    kv_store_hash(t, key, 8, h);
    int64_t rc = kv_store_put(t, key, 8, val, sizeof val, h[0], h[1]);
    if (rc == 1) accepted++;
    else CHECK(rc == -ENOMEM);
  }
  CHECK(accepted == 3); /* each record is 24+8+1000 = 1032 bytes: 3 fit in 4096 */
  /* Deleting frees the budget once reclaimed (no readers registered online). */
  key[0] = 0;
  kv_store_hash(t, key, 8, h);
  CHECK(kv_store_delete(t, key, 8, h[0], h[1]) == 1);
  kv_store_reclaim(t);
  key[0] = 100;
  kv_store_hash(t, key, 8, h);
  CHECK(kv_store_put(t, key, 8, val, sizeof val, h[0], h[1]) == 1);
  kv_store_destroy(t);

  t = kv_store_create(10, 0, seed, 0);
  kv_store_hash(t, key, 8, h);
  CHECK(kv_store_put(t, key, 0x80000000ull, val, 1, h[0], h[1]) == -E2BIG);
  CHECK(kv_store_put(t, key, 8, val, 0x80000000ull, h[0], h[1]) == -E2BIG);
  kv_store_destroy(t);
}

/** Test 6: grace period semantics. */
static void test_qsbr(void) {
  uint8_t seed[16] = {5};
  KvTable *t = kv_store_create(100, 1, seed, 0);
  uint8_t key[8] = {'k'};
  uint64_t h[2];
  kv_store_hash(t, key, 8, h);
  CHECK(kv_store_put(t, key, 8, "value-1", 7, h[0], h[1]) == 1);
  kv_qsbr_online(t, 0);
  const KvRec *held = kv_store_get(t, key, 8, h[0], h[1]);
  CHECK(held != NULL);
  CHECK(kv_store_put(t, key, 8, "value-2", 7, h[0], h[1]) == 0); /* retires `held` */
  CHECK(kv_store_reclaim(t) == 1);         /* reader still in the old epoch */
  CHECK(memcmp(kv_rec_value(held), "value-1", 7) == 0);
  kv_qsbr_quiescent(t, 0);                 /* reader drops its reference */
  CHECK(kv_store_reclaim(t) == 0);
  const KvRec *now = kv_store_get(t, key, 8, h[0], h[1]);
  CHECK(now && memcmp(kv_rec_value(now), "value-2", 7) == 0);
  CHECK(kv_store_delete(t, key, 8, h[0], h[1]) == 1);
  CHECK(kv_store_reclaim(t) == 1);         /* `now` may still be in use */
  kv_qsbr_offline(t, 0);
  CHECK(kv_store_reclaim(t) == 0);         /* offline readers never block */
  KvStats st;
  kv_store_stats(t, &st);
  CHECK_EQ_U64(st.mem_used, 0);
  kv_store_destroy(t);
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  test_model();
  test_short_keys();
  test_capacity();
  test_limits();
  test_qsbr();
  return test_finish("test_store");
}
