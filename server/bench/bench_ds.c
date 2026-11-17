/*
 * bench_ds.c — KvAsm data-structure bake-off (see docs/adr/0001).
 *
 * Question: can a linked structure — a linked-list hash table, or a binary
 * tree with assembly tricks — beat the bucketized cuckoo hash for KvAsm's
 * exact-key get/put/delete, given the requirement of O(1) operations?
 *
 * Candidates (all x86-64 assembly, same intrusive API, same records, same
 * SipHash-2-4-128 key hash computed inside every timed operation):
 *   cuckoo  src/cuckoo.asm       2 candidate buckets x 6 slots, SWAR tags,
 *                                version words (production code, including
 *                                its concurrency bookkeeping)
 *   chain   bench/ds/chain.asm   linked lists, cached hash per node
 *   trie    bench/ds/trie.asm    binary crit-bit trie on the hash, branchless
 *                                descent, tagged 16-byte nodes
 *
 * Measurements per key count N (single thread, pinned):
 *   build       insert N keys in random order                  (ns/insert)
 *   get-lat     dependent chain: each key comes from the previous value
 *               (nothing overlaps: pure per-request latency)     (ns/op)
 *   get-thr     independent random hits (CPU may overlap misses) (ns/op)
 *   miss-thr    independent random absent keys                   (ns/op)
 *   update      upsert of existing keys                          (ns/op)
 *   churn       delete + re-insert of a random key               (ns/pair)
 *   p50/p99/p99.9  per-operation latency of independent hits (rdtscp)
 * Every lookup result is verified; any wrong answer fails the run.
 *
 * Build: make bench   Run: build/bench/bench_ds [--quick]
 * Caveat: WSL2 vCPUs — absolute numbers are indicative, ratios are the result.
 */
#include <sched.h>

#include "kvasm_abi.h"
#include "test_common.h"

/* chain.asm / trie.asm */
void *bd_chain_create(uint64_t cap);
void bd_chain_destroy(void *c);
const KvRec *bd_chain_get(void *c, const void *key, size_t len, uint64_t h1, uint64_t h2);
int64_t bd_chain_upsert(void *c, KvRec *rec, uint64_t h1, uint64_t h2);
KvRec *bd_chain_remove(void *c, const void *key, size_t len, uint64_t h1, uint64_t h2);
KvRec **bd_chain_heads(void *c, uint64_t *nb);
void *bd_trie_create(uint64_t cap);
void bd_trie_destroy(void *t);
const KvRec *bd_trie_get(void *t, const void *key, size_t len, uint64_t h1, uint64_t h2);
int64_t bd_trie_upsert(void *t, KvRec *rec, uint64_t h1, uint64_t h2);
KvRec *bd_trie_remove(void *t, const void *key, size_t len, uint64_t h1, uint64_t h2);
uint64_t bd_trie_root(void *t);

enum { REC_STRIDE = 64, KEY_LEN = 16 };

static const uint64_t g_hkey[2] = {0x0706050403020100ull, 0x0f0e0d0c0b0a0908ull};
static uint8_t *g_recs;   /* N records, 64 bytes each */
static uint64_t g_n;      /* keys inserted */

/** splitmix64 finalizer (bijective), used to derive distinct keys. */
static inline uint64_t mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

/** Key i: two words of a bijection => all keys distinct; no memory access. */
static inline void key_of(uint64_t i, uint64_t k[2]) {
  k[0] = mix64(2 * i + 1);
  k[1] = mix64(2 * i + 2);
}

/** @return record i of the pre-built record array. */
static inline KvRec *rec_of(uint64_t i) { return (KvRec *)(g_recs + i * REC_STRIDE); }

/** One candidate structure behind a uniform intrusive API. */
typedef struct {
  const char *name;
  void *(*create)(uint64_t cap);
  void (*destroy)(void *self);
  const KvRec *(*get)(void *self, const void *key, size_t len, uint64_t h1, uint64_t h2);
  int64_t (*upsert)(void *self, KvRec *rec, uint64_t h1, uint64_t h2);
  KvRec *(*remove)(void *self, const void *key, size_t len, uint64_t h1, uint64_t h2);
  void (*describe)(void *self, char *buf, size_t n);
} Ds;

/* ---- cuckoo adapter (store.asm table used through the intrusive API) ---- */

/** Creates a cuckoo table; the store's own seed is unused (we pass hashes). */
static void *cuckoo_create(uint64_t cap) {
  static const uint8_t seed[16] = {1};
  return kv_store_create(cap, 0, seed, 0);
}

/** Unlinks every key first: records belong to the harness, not the store. */
static void cuckoo_destroy(void *self) {
  for (uint64_t i = 0; i < g_n; i++) {
    uint64_t k[2], h[2];
    key_of(i, k);
    kv_siphash128(k, KEY_LEN, g_hkey, h);
    kv_cuckoo_remove(self, k, KEY_LEN, h[0], h[1]);
  }
  kv_store_destroy(self);
}

static const KvRec *cuckoo_get(void *s, const void *k, size_t n, uint64_t a, uint64_t b) {
  return kv_cuckoo_get(s, k, n, a, b);
}
static int64_t cuckoo_upsert(void *s, KvRec *r, uint64_t a, uint64_t b) {
  return kv_cuckoo_upsert(s, r, a, b);
}
static KvRec *cuckoo_remove(void *s, const void *k, size_t n, uint64_t a, uint64_t b) {
  return kv_cuckoo_remove(s, k, n, a, b);
}

/** Load factor, relocations and index size of a cuckoo table. */
static void cuckoo_describe(void *self, char *buf, size_t n) {
  KvStats st;
  kv_store_stats(self, &st);
  snprintf(buf, n, "load %.0f%%, %llu path inserts, %llu moves, index %.1f B/key",
           100.0 * (double)st.count / (double)(st.nbuckets * 6), (unsigned long long)st.bfs,
           (unsigned long long)st.moves, (double)(st.nbuckets * 64) / (double)st.count);
}

/* ---- chain adapter ---- */

/** Average/max chain length over non-empty buckets and index size. */
static void chain_describe(void *self, char *buf, size_t n) {
  uint64_t nb, nonempty = 0, total = 0, maxlen = 0;
  KvRec **heads = bd_chain_heads(self, &nb);
  for (uint64_t b = 0; b < nb; b++) {
    uint64_t len = 0;
    for (KvRec *r = heads[b]; r; r = r->next) len++;
    if (len) nonempty++;
    total += len;
    if (len > maxlen) maxlen = len;
  }
  snprintf(buf, n, "avg chain %.2f, max chain %llu, index %.1f B/key", (double)total / (double)nonempty,
           (unsigned long long)maxlen, (double)(nb * 8 + total * 8) / (double)total);
}

/* ---- trie adapter ---- */

/** Accumulates leaf depths of a tagged-pointer subtree. */
static void trie_walk(uint64_t p, uint64_t depth, uint64_t *sum, uint64_t *max, uint64_t *leaves) {
  if (p & 1) {
    *sum += depth;
    (*leaves)++;
    if (depth > *max) *max = depth;
    return;
  }
  const uint64_t *node = (const uint64_t *)(p & 0x00FFFFFFFFFFFFFFull);
  trie_walk(node[0], depth + 1, sum, max, leaves);
  trie_walk(node[1], depth + 1, sum, max, leaves);
}

/** Average/max depth of the trie and index size. */
static void trie_describe(void *self, char *buf, size_t n) {
  uint64_t sum = 0, max = 0, leaves = 0, root = bd_trie_root(self);
  if (root) trie_walk(root, 0, &sum, &max, &leaves);
  snprintf(buf, n, "avg depth %.1f, max depth %llu, index %.1f B/key", (double)sum / (double)leaves,
           (unsigned long long)max, (double)((leaves - 1) * 16) / (double)leaves);
}

static const Ds kDs[] = {
    {"cuckoo", cuckoo_create, cuckoo_destroy, cuckoo_get, cuckoo_upsert, cuckoo_remove, cuckoo_describe},
    {"chain", bd_chain_create, bd_chain_destroy, bd_chain_get, bd_chain_upsert, bd_chain_remove,
     chain_describe},
    {"trie", bd_trie_create, bd_trie_destroy, bd_trie_get, bd_trie_upsert, bd_trie_remove, trie_describe},
};

/* ---- timing ---- */

/** Serialized TSC read before a timed region. */
static inline uint64_t tsc_start(void) {
  uint32_t lo, hi;
  __asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
  return ((uint64_t)hi << 32) | lo;
}

/** Serialized TSC read after a timed region (RDTSCP waits for prior work). */
static inline uint64_t tsc_stop(void) {
  uint32_t lo, hi, aux;
  __asm__ volatile("rdtscp\n\tlfence" : "=a"(lo), "=d"(hi), "=c"(aux)::"memory");
  return ((uint64_t)hi << 32) | lo;
}

/** @return TSC ticks per nanosecond, measured against CLOCK_MONOTONIC. */
static double tsc_per_ns(void) {
  uint64_t t0 = now_ns(), c0 = tsc_start();
  while (now_ns() - t0 < 200000000ull) {}
  uint64_t t1 = now_ns(), c1 = tsc_stop();
  return (double)(c1 - c0) / (double)(t1 - t0);
}

/** qsort comparator for u64. */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

/** One hashed lookup of key i, verified against the expected record. */
static inline const KvRec *lookup(const Ds *d, void *s, uint64_t i) {
  uint64_t k[2], h[2];
  key_of(i, k);
  kv_siphash128(k, KEY_LEN, g_hkey, h);
  return d->get(s, k, KEY_LEN, h[0], h[1]);
}

/** Result row of one structure at one size. */
typedef struct {
  double build, lat, thr, miss, upd, churn, p50, p99, p999;
  char desc[160];
} Row;

/**
 * Runs every measurement for one structure.
 * @param d candidate. @param n key count. @param m lookups per measurement.
 * @param ticks TSC ticks per ns. @param row results.
 */
static void run_ds(const Ds *d, uint64_t n, uint64_t m, double ticks, Row *row) {
  void *s = d->create(n);
  CHECK(s != NULL);
  uint64_t *order = malloc(n * sizeof *order);
  for (uint64_t i = 0; i < n; i++) order[i] = i;
  uint64_t rs = 0xC0FFEE;
  for (uint64_t i = n - 1; i > 0; i--) { /* random insertion order */
    uint64_t j = rng_next(&rs) % (i + 1), t = order[i];
    order[i] = order[j];
    order[j] = t;
  }

  uint64_t t0 = now_ns();
  for (uint64_t x = 0; x < n; x++) {
    uint64_t i = order[x], k[2], h[2];
    key_of(i, k);
    kv_siphash128(k, KEY_LEN, g_hkey, h);
    int64_t rc = d->upsert(s, rec_of(i), h[0], h[1]);
    if (rc != 0) CHECK(rc == 0);
  }
  row->build = (double)(now_ns() - t0) / (double)n;
  free(order);

  for (uint64_t i = 0; i < n; i++) /* full verification */
    if (lookup(d, s, i) != rec_of(i)) { CHECK(!"lookup returned wrong record"); break; }

  /* get-lat: value word 0 holds the next key of a single random cycle */
  uint64_t idx = 0;
  t0 = now_ns();
  for (uint64_t x = 0; x < m; x++) {
    const KvRec *r = lookup(d, s, idx);
    if (r != rec_of(idx)) { CHECK(!"chase lookup failed"); break; }
    memcpy(&idx, kv_rec_value(r), 8);
  }
  row->lat = (double)(now_ns() - t0) / (double)m;

  uint64_t xs = 0x9E3779B97F4A7C15ull, bad = 0;
  t0 = now_ns();
  for (uint64_t x = 0; x < m; x++) {
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    uint64_t i = xs % n;
    bad += lookup(d, s, i) != rec_of(i);
  }
  row->thr = (double)(now_ns() - t0) / (double)m;
  CHECK(bad == 0);

  t0 = now_ns();
  for (uint64_t x = 0; x < m; x++) {
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    bad += lookup(d, s, n + xs % n) != NULL; /* keys n..2n-1 were never inserted */
  }
  row->miss = (double)(now_ns() - t0) / (double)m;
  CHECK(bad == 0);

  t0 = now_ns();
  for (uint64_t x = 0; x < m; x++) {
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    uint64_t i = xs % n, k[2], h[2];
    key_of(i, k);
    kv_siphash128(k, KEY_LEN, g_hkey, h);
    bad += d->upsert(s, rec_of(i), h[0], h[1]) != (int64_t)(uintptr_t)rec_of(i);
  }
  row->upd = (double)(now_ns() - t0) / (double)m;
  CHECK(bad == 0);

  uint64_t pairs = m / 2;
  t0 = now_ns();
  for (uint64_t x = 0; x < pairs; x++) {
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    uint64_t i = xs % n, k[2], h[2];
    key_of(i, k);
    kv_siphash128(k, KEY_LEN, g_hkey, h);
    bad += d->remove(s, k, KEY_LEN, h[0], h[1]) != rec_of(i);
    bad += d->upsert(s, rec_of(i), h[0], h[1]) != 0;
  }
  row->churn = (double)(now_ns() - t0) / (double)pairs;
  CHECK(bad == 0);

  enum { SAMPLES = 400000 };
  uint64_t *lat = malloc(SAMPLES * sizeof *lat);
  for (uint64_t x = 0; x < SAMPLES; x++) {
    xs ^= xs << 13; xs ^= xs >> 7; xs ^= xs << 17;
    uint64_t i = xs % n;
    uint64_t c0 = tsc_start();
    const KvRec *r = lookup(d, s, i);
    uint64_t c1 = tsc_stop();
    bad += r != rec_of(i);
    lat[x] = c1 - c0;
  }
  CHECK(bad == 0);
  qsort(lat, SAMPLES, sizeof *lat, cmp_u64);
  row->p50 = (double)lat[SAMPLES / 2] / ticks;
  row->p99 = (double)lat[SAMPLES * 99 / 100] / ticks;
  row->p999 = (double)lat[SAMPLES * 999 / 1000] / ticks;
  free(lat);

  for (uint64_t i = 0; i < n; i++) /* still consistent after churn */
    if (lookup(d, s, i) != rec_of(i)) { CHECK(!"post-churn lookup failed"); break; }
  d->describe(s, row->desc, sizeof row->desc);
  g_n = n;
  d->destroy(s);
}

/**
 * Entry point: pins to one CPU and runs all candidates at several sizes.
 * @param argc/argv "--quick" runs only the small size (used by `make test`).
 * @return 0 when every verification passed.
 */
int main(int argc, char **argv) {
  int quick = argc > 1 && strcmp(argv[1], "--quick") == 0;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(2, &set);
  sched_setaffinity(0, sizeof set, &set);
  double ticks = tsc_per_ns();

  const uint64_t sizes[] = {65536, 1048576, 4194304};
  const size_t nsizes = quick ? 1 : 3;
  const uint64_t maxn = sizes[nsizes - 1];
  g_recs = aligned_alloc(64, maxn * REC_STRIDE);
  for (uint64_t i = 0; i < maxn; i++) { /* key, then next-in-cycle as the value */
    KvRec *r = rec_of(i);
    uint64_t k[2];
    key_of(i, k);
    r->next = NULL;
    r->aux = 0;
    r->key_len = KEY_LEN;
    r->val_len = 16;
    memcpy(r->data, k, 16);
  }

  uint64_t m = quick ? 200000 : 2000000;
  uint64_t t0 = now_ns(), k[2], h[2], acc = 0;
  for (uint64_t i = 0; i < m; i++) {
    key_of(i, k);
    acc ^= kv_siphash128(k, KEY_LEN, g_hkey, h);
  }
  printf("SipHash-2-4-128 of a 16-byte key (included in every op below): %.1f ns [%llx]\n\n",
         (double)(now_ns() - t0) / (double)m, (unsigned long long)(acc & 0xF));

  for (size_t si = 0; si < nsizes; si++) {
    uint64_t n = sizes[si];
    uint64_t *cyc = malloc(n * sizeof *cyc);
    for (uint64_t i = 0; i < n; i++) cyc[i] = i;
    uint64_t rs = 0xBADC0DE;
    for (uint64_t i = n - 1; i > 0; i--) { /* Sattolo: one cycle through all keys */
      uint64_t j = rng_next(&rs) % i, t = cyc[i];
      cyc[i] = cyc[j];
      cyc[j] = t;
    }
    for (uint64_t i = 0; i < n; i++) /* value starts after the 16-byte key */
      memcpy(rec_of(i)->data + KEY_LEN, &cyc[i], 8);
    free(cyc);

    printf("N = %llu keys (records %llu MiB)\n", (unsigned long long)n,
           (unsigned long long)(n * REC_STRIDE >> 20));
    printf("| structure | build | get-lat | get-thr | miss-thr | update | churn | p50 | p99 | p99.9 | structure stats |\n");
    printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
    for (size_t d = 0; d < sizeof kDs / sizeof kDs[0]; d++) {
      Row row;
      g_n = n;
      run_ds(&kDs[d], n, m, ticks, &row);
      printf("| %s | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %.0f | %s |\n", kDs[d].name,
             row.build, row.lat, row.thr, row.miss, row.upd, row.churn, row.p50, row.p99, row.p999,
             row.desc);
      fflush(stdout);
    }
    printf("(all times in ns per operation)\n\n");
  }
  free(g_recs);
  return test_finish("bench_ds");
}
