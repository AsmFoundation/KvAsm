/*
 * bench_store.c — store-level lookup benchmark: memory-hierarchy levels,
 * SIMD vs SWAR tag scans, and private L1/L2 record caches (docs/adr/0006).
 *
 * Why a separate benchmark: over the network a GET costs about 1.4 µs on one
 * core, of which the store lookup is 0.1-0.2 µs, so store-level differences
 * drown in protocol and kernel noise. Here the store is called directly on
 * one pinned thread, with the server's exact code paths (kv_store_hash,
 * kv_store_prefetch, kv_store_prefetch_records, kv_store_get or
 * kv_hcache_get), so each variant's cost per lookup is visible.
 *
 * Working sets, sized to the levels of the memory hierarchy (records are
 * ~144 B with 16-byte keys and 100-byte values, plus ~13-26 B of index):
 *   L1d    256 keys     ~40 KiB   (a P-core's L1d: 48 KiB)
 *   L2     8 Ki keys    ~1.4 MiB  (L2: 2 MiB)
 *   L3     128 Ki keys  ~21 MiB   (L3: 36 MiB, shared)
 *   DRAM   1 Mi keys    ~170 MiB  (the network benchmark's key space)
 *   DRAM4  4 Mi keys    ~680 MiB  (TLB pressure; full runs only)
 * Access patterns (every one hashes each key, as the server does):
 *   lat    dependent chain: the next key comes from the value just read, so
 *          nothing overlaps — pure latency per lookup
 *   thr    independent uniform lookups; the CPU overlaps what it can
 *   b16    the server's batched GET pipeline, 16 per batch: hash and
 *          prefetch buckets, then prefetch records, then look up
 *   b64    the same with 64 per batch (ADR 0005's deferred option)
 *   zthr   Zipf 0.99 (YCSB scrambled), independent
 *   zb16   Zipf 0.99, batched 16
 *   zmix   Zipf 0.99, batched 16, 10% puts: cache invalidation at work
 * Variants: swar (production default), sse2 (one SIMD compare for both
 * buckets), and private caches in front of either (L1 = 2^10 entries =
 * 32 KiB, L2 = 2^16 entries = 2 MiB).
 *
 * Method: inside every repetition the variants run back to back in an order
 * rotated per repetition, so slow drift and position effects hit all alike.
 * Reported: median ns per operation over the repetitions, and each variant's
 * paired ratio to swar (same repetition) with its range and how many
 * repetitions it was faster in. Every lookup is verified (key index and,
 * under zmix, the exact latest version); any wrong answer fails the run.
 *
 * Build: make build/bench/bench_store
 * Run:   build/bench/bench_store [--quick] [--cpu N] [--reps R]
 * Caveat: WSL2 vCPUs on a shared host — absolute numbers are indicative,
 * paired ratios are the result.
 */
#include <math.h>
#include <sched.h>

#include "kvnode_abi.h"
#include "test_common.h"

enum { KEY_LEN = 16, VAL_LEN = 100, SEQ_LEN = 1 << 20, MAX_BATCH = 64 };

/** A lookup implementation under test. */
typedef struct {
  const char *name;
  int scan;               /* KV_SCAN_SWAR / KV_SCAN_SSE2 */
  uint32_t l1_bits, l2_bits;  /* private cache levels, 0 = off */
} Variant;

static const Variant kVariants[] = {
    {"swar", KV_SCAN_SWAR, 0, 0},
    {"sse2", KV_SCAN_SSE2, 0, 0},
    {"swar+L1", KV_SCAN_SWAR, 10, 0},
    {"swar+L1L2", KV_SCAN_SWAR, 10, 16},
    {"sse2+L1L2", KV_SCAN_SSE2, 10, 16},
};
enum { NVAR = sizeof kVariants / sizeof kVariants[0] };

/** An access pattern. */
typedef enum { M_LAT, M_THR, M_B16, M_B64, M_ZTHR, M_ZB16, M_ZMIX, NMODES } Mode;
static const char *const kModeNames[NMODES] = {"lat", "thr", "b16", "b64", "zthr", "zb16", "zmix"};

/** Everything one table size needs. */
typedef struct {
  KvTable *t;
  uint64_t n;             /* keys */
  uint32_t *uni, *zipf;   /* SEQ_LEN precomputed key indices per distribution */
  uint64_t *version;      /* latest version per key (zmix verification) */
} Store;

/* ---- keys and values ---- */

/**
 * Key i: "key:" + 12 lowercase hex digits (16 bytes, like the network
 * benchmark's "key:%012d"). Hex because formatting must cost next to
 * nothing: snprintf would add ~50 ns per lookup and hide the differences
 * being measured. The store hashes and compares bytes, so the digits' base
 * is irrelevant to it.
 */
static inline void key_of(uint64_t i, char key[KEY_LEN + 1]) {
  static const char hex[] = "0123456789abcdef";
  memcpy(key, "key:", 4);
  for (int d = 15; d >= 4; d--, i >>= 4) key[d] = hex[i & 15];
  key[KEY_LEN] = 0;
}

/**
 * Value of key i at a version: [0..8) next index of the latency chain,
 * [8..16) the key index, [16..24) the version, then filler.
 */
static void value_of(uint64_t i, uint64_t next, uint64_t version, uint8_t v[VAL_LEN]) {
  memcpy(v, &next, 8);
  memcpy(v + 8, &i, 8);
  memcpy(v + 16, &version, 8);
  for (int b = 24; b < VAL_LEN; b++) v[b] = (uint8_t)(i * 31 + (uint64_t)b);
}

/* ---- distributions ---- */

/** splitmix64 finalizer: scrambles Zipf ranks over the key space (YCSB). */
static inline uint64_t mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

/**
 * Fills seq with Zipf(theta) ranks over [0, n), scrambled (YCSB's
 * "scrambled Zipfian": the hottest keys are spread over the key space).
 * Uses Gray et al.'s generator, as YCSB and kvbench do.
 */
static void fill_zipf(uint32_t *seq, uint64_t n, double theta, uint64_t seed) {
  double zetan = 0;
  for (uint64_t i = 1; i <= n; i++) zetan += 1.0 / pow((double)i, theta);
  double zeta2 = 1.0 + 1.0 / pow(2.0, theta);
  double alpha = 1.0 / (1.0 - theta);
  double eta = (1.0 - pow(2.0 / (double)n, 1.0 - theta)) / (1.0 - zeta2 / zetan);
  uint64_t s = seed;
  for (uint64_t k = 0; k < SEQ_LEN; k++) {
    double u = (double)(rng_next(&s) >> 11) * (1.0 / 9007199254740992.0);
    double uz = u * zetan;
    uint64_t rank;
    if (uz < 1.0) rank = 0;
    else if (uz < zeta2) rank = 1;
    else rank = (uint64_t)((double)n * pow(eta * u - eta + 1.0, alpha));
    if (rank >= n) rank = n - 1;
    seq[k] = (uint32_t)(mix64(rank) % n);
  }
}

/** Fills seq with uniform indices over [0, n). */
static void fill_uniform(uint32_t *seq, uint64_t n, uint64_t seed) {
  uint64_t s = seed;
  for (uint64_t k = 0; k < SEQ_LEN; k++) seq[k] = (uint32_t)(rng_next(&s) % n);
}

/* ---- table setup ---- */

/**
 * Builds a store of n keys whose values form one random cycle (Sattolo), so
 * the latency chain visits every key.
 * @return the store; aborts the benchmark on setup failure.
 */
static Store build(uint64_t n) {
  static const uint8_t seed[16] = {7, 1, 8, 2, 8, 1, 8, 2, 8, 4, 5, 9, 0, 4, 5, 2};
  Store st = {.n = n};
  st.t = kv_store_create(n + n / 4, 1, seed, 0);
  if (!st.t || kv_store_enable_inval(st.t, 16) != 0) {
    fprintf(stderr, "store setup failed for n=%llu\n", (unsigned long long)n);
    exit(2);
  }
  uint64_t *cyc = malloc(n * sizeof *cyc);
  for (uint64_t i = 0; i < n; i++) cyc[i] = i;
  uint64_t rs = 0xBADC0DE;
  for (uint64_t i = n - 1; i > 0; i--) { /* Sattolo: a single cycle */
    uint64_t j = rng_next(&rs) % i, t = cyc[i];
    cyc[i] = cyc[j];
    cyc[j] = t;
  }
  char key[KEY_LEN + 1];
  uint8_t val[VAL_LEN];
  for (uint64_t i = 0; i < n; i++) {
    key_of(i, key);
    value_of(i, cyc[i], 0, val);
    uint64_t h[2];
    kv_store_hash(st.t, key, KEY_LEN, h);
    if (kv_store_put(st.t, key, KEY_LEN, val, VAL_LEN, h[0], h[1]) != 1) {
      fprintf(stderr, "insert failed at %llu\n", (unsigned long long)i);
      exit(2);
    }
  }
  free(cyc);
  st.version = calloc(n, sizeof *st.version);
  st.uni = malloc(SEQ_LEN * sizeof *st.uni);
  st.zipf = malloc(SEQ_LEN * sizeof *st.zipf);
  fill_uniform(st.uni, n, 0x5EED + n);
  fill_zipf(st.zipf, n, 0.99, 0xF00D + n);
  return st;
}

/** Frees a store (all readers are offline by then). */
static void destroy(Store *st) {
  kv_store_destroy(st->t);
  free(st->version);
  free(st->uni);
  free(st->zipf);
}

/* ---- one measured run ---- */

/** Lookup through the variant's path: private cache or the table. */
static inline const KvRec *get(KvTable *t, KvHCache *c, const char *key, uint64_t h1,
                               uint64_t h2) {
  return c ? kv_hcache_get(c, key, KEY_LEN, h1, h2) : kv_store_get(t, key, KEY_LEN, h1, h2);
}

/** @return 1 if r is key i's record at the expected version. */
static inline int verify(const KvRec *r, uint64_t i, uint64_t version) {
  if (!r || r->val_len != VAL_LEN) return 0;
  const uint8_t *v = kv_rec_value(r);
  uint64_t idx, ver;
  memcpy(&idx, v + 8, 8);
  memcpy(&ver, v + 16, 8);
  return idx == i && ver == version;
}

/**
 * Performs `ops` operations of one pattern.
 * @param st store. @param c private cache or NULL. @param mode pattern.
 * @param ops operations. @param pos in/out cursor into the index sequences.
 * @param bad out: count of wrong answers.
 */
static void run_ops(Store *st, KvHCache *c, Mode mode, uint64_t ops, uint64_t *pos,
                    uint64_t *bad) {
  KvTable *t = st->t;
  char keys[MAX_BATCH][KEY_LEN + 1];
  uint64_t h1[MAX_BATCH], h2[MAX_BATCH], idx[MAX_BATCH];
  uint8_t val[VAL_LEN];
  uint64_t p = *pos, errors = 0;
  if (mode == M_LAT) { /* dependent chain: the value names the next key */
    uint64_t i = st->uni[p++ & (SEQ_LEN - 1)];
    for (uint64_t k = 0; k < ops; k++) {
      uint64_t h[2];
      key_of(i, keys[0]);
      kv_store_hash(t, keys[0], KEY_LEN, h);
      const KvRec *r = get(t, c, keys[0], h[0], h[1]);
      if (!verify(r, i, st->version[i])) { errors++; break; }
      memcpy(&i, kv_rec_value(r), 8);
    }
  } else if (mode == M_THR || mode == M_ZTHR) {
    const uint32_t *seq = mode == M_THR ? st->uni : st->zipf;
    for (uint64_t k = 0; k < ops; k++) {
      uint64_t i = seq[p++ & (SEQ_LEN - 1)], h[2];
      key_of(i, keys[0]);
      kv_store_hash(t, keys[0], KEY_LEN, h);
      errors += !verify(get(t, c, keys[0], h[0], h[1]), i, st->version[i]);
    }
  } else { /* batched pipeline, as kv_grpc_run_batch does it */
    const uint32_t *seq = mode == M_B16 || mode == M_B64 ? st->uni : st->zipf;
    uint64_t batch = mode == M_B64 ? 64 : 16;
    uint64_t mix = mode == M_ZMIX;
    for (uint64_t done = 0; done < ops; done += batch) {
      for (uint64_t b = 0; b < batch; b++) { /* parse: hash, prefetch buckets */
        idx[b] = seq[p++ & (SEQ_LEN - 1)];
        key_of(idx[b], keys[b]);
        uint64_t h[2];
        kv_store_hash(t, keys[b], KEY_LEN, h);
        h1[b] = h[0];
        h2[b] = h[1];
        kv_store_prefetch(t, h1[b], h2[b]);
      }
      for (uint64_t b = 0; b < batch; b++) kv_store_prefetch_records(t, h1[b], h2[b]);
      for (uint64_t b = 0; b < batch; b++) {
        uint64_t i = idx[b];
        if (mix && (b % 10) == 9) { /* 10%: a put of a new version */
          uint64_t next;
          const KvRec *old = kv_store_get(t, keys[b], KEY_LEN, h1[b], h2[b]);
          memcpy(&next, kv_rec_value(old), 8);
          value_of(i, next, ++st->version[i], val);
          errors += kv_store_put(t, keys[b], KEY_LEN, val, VAL_LEN, h1[b], h2[b]) != 0;
        } else {
          errors += !verify(get(t, c, keys[b], h1[b], h2[b]), i, st->version[i]);
        }
      }
      kv_qsbr_quiescent(t, 0); /* between batches, as the event loop does */
    }
  }
  *pos = p;
  *bad += errors;
}

/**
 * Times one (variant, pattern) run: fresh private cache, warm-up, then
 * `ops` timed operations.
 * @return ns per operation.
 */
static double measure(Store *st, const Variant *v, Mode mode, uint64_t ops, uint64_t *bad) {
  kv_set_scan_mode(v->scan);
  KvHCache *c = NULL;
  if (v->l1_bits | v->l2_bits) {
    c = kv_hcache_create(st->t, v->l1_bits, v->l2_bits);
    if (!c) { fprintf(stderr, "cache setup failed\n"); exit(2); }
  }
  uint64_t pos = mode * 104729ull; /* different start per pattern, same per variant */
  run_ops(st, c, mode, ops / 4 + 64, &pos, bad); /* warm caches (CPU and private) */
  uint64_t t0 = now_ns();
  run_ops(st, c, mode, ops, &pos, bad);
  double ns = (double)(now_ns() - t0) / (double)ops;
  kv_hcache_destroy(c);
  kv_set_scan_mode(KV_SCAN_SWAR);
  return ns;
}

/* ---- reporting ---- */

/** qsort comparator for doubles. */
static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/** @return median of n doubles (sorts a copy). */
static double median(const double *x, int n) {
  double tmp[64];
  memcpy(tmp, x, (size_t)n * sizeof *x);
  qsort(tmp, (size_t)n, sizeof *tmp, cmp_d);
  return n % 2 ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2;
}

/** Prints the host facts that matter for reading the numbers. */
static void print_env(int cpu) {
  char line[256], model[128] = "?";
  FILE *f = fopen("/proc/cpuinfo", "r");
  while (f && fgets(line, sizeof line, f))
    if (strncmp(line, "model name", 10) == 0) {
      snprintf(model, sizeof model, "%s", strchr(line, ':') + 2);
      model[strcspn(model, "\n")] = 0;
      break;
    }
  if (f) fclose(f);
  printf("CPU: %s, pinned to vCPU %d. Caches of cpu%d:", model, cpu, cpu);
  for (int i = 0; i < 5; i++) {
    char path[128], size[32] = "", lvl[8] = "", type[16] = "";
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/size", cpu, i);
    if ((f = fopen(path, "r"))) { if (!fgets(size, sizeof size, f)) size[0] = 0; fclose(f); }
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, i);
    if ((f = fopen(path, "r"))) { if (!fgets(lvl, sizeof lvl, f)) lvl[0] = 0; fclose(f); }
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cache/index%d/type", cpu, i);
    if ((f = fopen(path, "r"))) { if (!fgets(type, sizeof type, f)) type[0] = 0; fclose(f); }
    size[strcspn(size, "\n")] = lvl[strcspn(lvl, "\n")] = type[strcspn(type, "\n")] = 0;
    if (size[0]) printf(" L%s %s %s;", lvl, type, size);
  }
  printf("\n\n");
}

/**
 * Entry point.
 * @param argc/argv --quick (small sizes, 1 repetition: `make test`),
 *        --cpu N (default 2), --reps R (default 7, max 64).
 * @return 0 when every lookup was verified correct.
 */
int main(int argc, char **argv) {
  int quick = 0, cpu = 2, reps = 7;
  for (int a = 1; a < argc; a++) {
    if (strcmp(argv[a], "--quick") == 0) quick = 1;
    else if (strcmp(argv[a], "--cpu") == 0 && a + 1 < argc) cpu = atoi(argv[++a]);
    else if (strcmp(argv[a], "--reps") == 0 && a + 1 < argc) reps = atoi(argv[++a]);
    else { fprintf(stderr, "usage: %s [--quick] [--cpu N] [--reps R]\n", argv[0]); return 2; }
  }
  if (reps < 1 || reps > 64) { fprintf(stderr, "--reps must be 1..64\n"); return 2; }
  if (quick) reps = 1;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) perror("sched_setaffinity (continuing unpinned)");
  print_env(cpu);

  static const struct { const char *label; uint64_t n; } kSizes[] = {
      {"L1d", 256}, {"L2", 8192}, {"L3", 131072}, {"DRAM", 1048576}, {"DRAM4", 4194304}};
  const int nsizes = quick ? 2 : 5;
  const uint64_t ops = quick ? 50000 : 2000000;
  uint64_t bad = 0;
  static double ns[5][NMODES][NVAR][64];

  for (int si = 0; si < nsizes; si++) {
    Store st = build(kSizes[si].n);
    kv_qsbr_online(st.t, 0);
    for (int r = 0; r < reps; r++)
      for (int m = 0; m < NMODES; m++)
        for (int k = 0; k < NVAR; k++) { /* rotated order: position effects cancel */
          int vi = (k + r + m) % NVAR;
          ns[si][m][vi][r] = measure(&st, &kVariants[vi], (Mode)m, ops, &bad);
        }
    kv_qsbr_offline(st.t, 0);

    printf("## %s working set: %llu keys\n\n", kSizes[si].label, (unsigned long long)kSizes[si].n);
    printf("Median ns per operation (%d repetitions):\n\n| pattern |", reps);
    for (int v = 0; v < NVAR; v++) printf(" %s |", kVariants[v].name);
    printf("\n|---|");
    for (int v = 0; v < NVAR; v++) printf("---|");
    printf("\n");
    for (int m = 0; m < NMODES; m++) {
      printf("| %s |", kModeNames[m]);
      for (int v = 0; v < NVAR; v++) printf(" %.1f |", median(ns[si][m][v], reps));
      printf("\n");
    }
    printf("\nPaired ratio to swar, same repetition (< 1 = faster): median [min–max], faster in k/%d:\n\n| pattern |",
           reps);
    for (int v = 1; v < NVAR; v++) printf(" %s |", kVariants[v].name);
    printf("\n|---|");
    for (int v = 1; v < NVAR; v++) printf("---|");
    printf("\n");
    for (int m = 0; m < NMODES; m++) {
      printf("| %s |", kModeNames[m]);
      for (int v = 1; v < NVAR; v++) {
        double ratio[64], lo = 1e9, hi = 0;
        int faster = 0;
        for (int r = 0; r < reps; r++) {
          ratio[r] = ns[si][m][v][r] / ns[si][m][0][r];
          if (ratio[r] < lo) lo = ratio[r];
          if (ratio[r] > hi) hi = ratio[r];
          faster += ratio[r] < 1.0;
        }
        printf(" %.3f [%.2f–%.2f] %d/%d |", median(ratio, reps), lo, hi, faster, reps);
      }
      printf("\n");
    }
    printf("\n");
    fflush(stdout);
    destroy(&st);
  }
  CHECK_EQ_U64(bad, 0);
  return test_finish("bench_store");
}
