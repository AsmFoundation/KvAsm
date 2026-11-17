/*
 * lookup_decomp.c — where do the ~90 ns of a batched GET lookup go?
 *
 * Runs the server's batched GET pipeline (kv_grpc_run_batch's order: hash +
 * prefetch buckets, prefetch records, look up + copy the 100-byte value)
 * against the real store objects, in four variants per working set:
 *   hash / prehash   SipHash per lookup (as the server) or hashes read from
 *                    a precomputed sequential array (isolates SipHash)
 *   4k   / thp       records from glibc malloc (4 KiB pages, as the server)
 *                    or from a prefaulted 2 MiB-page slab with glibc's exact
 *                    chunk spacing (isolates page-walk cost)
 * The allocator switch uses the linker's --wrap=malloc/--wrap=free, so only
 * the store's own calls are redirected, never glibc's internals.
 *
 * Working sets: 8 Ki keys (records + buckets fit in L2: the compute floor)
 * and 1 Mi keys (~170 MiB: the network benchmark's key space).
 * Method: REPS repetitions; inside each, the four variants run back to back
 * in an order rotated per repetition; median ns/lookup and each variant's
 * per-repetition ratio to "4k hash" (the server's configuration).
 *
 * Build (dev container, from server/): cc -O2 -D_GNU_SOURCE -I tests
 *   -Wl,--wrap=malloc -Wl,--wrap=free -o /tmp/lookup_decomp lookup_decomp.c
 *   build/obj/{siphash,util,cuckoo,wlock,store,...}.o -pthread
 * Usage: lookup_decomp [cpu] [reps]
 */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "kvasm_abi.h"

enum { KEY_LEN = 16, VAL_LEN = 100, SEQ = 1 << 20, BATCH = 16, NV = 4 };
#define HUGE (2ull << 20)

/* ---- the record allocator switch ---------------------------------------- */
void *__real_malloc(size_t n);
void __real_free(void *p);
static unsigned char *slab_base, *slab_cur, *slab_end;
static int slab_on;

/** malloc for the linked objects: the THP slab while slab_on, else glibc. */
void *__wrap_malloc(size_t n) {
  if (slab_on && n <= 4096) {
    size_t chunk = (n + 8 + 15) & ~(size_t)15; /* glibc's chunk size for n */
    if (chunk < 32) chunk = 32;
    if (slab_cur + chunk <= slab_end) {
      void *p = slab_cur;                      /* 16-byte aligned, like glibc */
      slab_cur += chunk;
      return p;
    }
  }
  return __real_malloc(n);
}

/** free for the linked objects: slab blocks are never returned. */
void __wrap_free(void *p) {
  if ((unsigned char *)p >= slab_base && (unsigned char *)p < slab_end) return;
  __real_free(p);
}

/** Maps a 2 MiB-aligned slab, asks for huge pages and faults them in with writes. */
static void slab_init(size_t bytes) {
  bytes = (bytes + HUGE - 1) & ~(HUGE - 1);
  unsigned char *m = mmap(NULL, bytes + HUGE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) { perror("mmap"); exit(2); }
  slab_base = (unsigned char *)(((uintptr_t)m + HUGE - 1) & ~(HUGE - 1));
  slab_end = slab_base + bytes;
  slab_cur = slab_base;
  madvise(slab_base, bytes, MADV_HUGEPAGE);
  for (unsigned char *q = slab_base; q < slab_end; q += HUGE) *q = 0;
}

/* bench_store's per-key version array (calloc'd, all zero) when non-NULL. */
static uint64_t *g_version;

/* ---- helpers ---------------------------------------------------------------- */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t rng(uint64_t *s) { /* xorshift64* */
  *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
  return *s * 0x2545F4914F6CDD1Dull;
}

static void key_of(uint64_t i, char *k) { /* "key:" + 12 digits, like kvbench */
  char tmp[32];
  snprintf(tmp, sizeof tmp, "key:%012llu", (unsigned long long)i);
  memcpy(k, tmp, KEY_LEN);
}

/** AnonHugePages (KiB) of the mapping that contains addr, from /proc/self/smaps. */
static long vma_huge_kib(const void *addr) {
  FILE *f = fopen("/proc/self/smaps", "r");
  char line[512];
  int in = 0;
  long v = -1;
  while (f && fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && strchr(line, '-') < strchr(line, ' ')) {
      in = (uintptr_t)addr >= lo && (uintptr_t)addr < hi;
    } else if (in && sscanf(line, "AnonHugePages: %ld kB", &v) == 1) {
      break;
    }
  }
  if (f) fclose(f);
  return v;
}

/** Builds a store of n keys; records come from the slab when thp is set. */
static KvTable *build(uint64_t n, int thp) {
  static const uint8_t seed[16] = {7, 1, 8, 2, 8, 1, 8, 2, 8, 4, 5, 9, 0, 4, 5, 2};
  KvTable *t = kv_store_create(n + n / 4, 1, seed, 0);
  if (!t) { fprintf(stderr, "store\n"); exit(2); }
  char key[KEY_LEN];
  uint8_t val[VAL_LEN];
  memset(val, 0xA5, sizeof val);
  slab_on = thp;
  for (uint64_t i = 0; i < n; i++) {
    key_of(i, key);
    memcpy(val, &i, 8);
    uint64_t h[2];
    kv_store_hash(t, key, KEY_LEN, h);
    if (kv_store_put(t, key, KEY_LEN, val, VAL_LEN, h[0], h[1]) != 1) { fprintf(stderr, "put\n"); exit(2); }
  }
  slab_on = 0;
  kv_qsbr_online(t, 0);
  return t;
}

/**
 * One timed run of the batched pipeline; returns ns per lookup.
 * Keys come from K, precomputed per sequence position (read sequentially,
 * like the server's keys sitting in the request buffer): no formatting cost
 * inside the timed loop.
 */
static double run(KvTable *t, const uint32_t *seq, const char (*K)[KEY_LEN], const uint64_t *H1,
                  const uint64_t *H2, int prehash, uint64_t ops, uint64_t *pos, uint64_t *bad) {
  const char *keys[BATCH];
  uint64_t h1[BATCH], h2[BATCH];
  uint32_t idx[BATCH];
  uint8_t buf[VAL_LEN];
  uint64_t p = *pos, errors = 0, t0 = now_ns();
  for (uint64_t done = 0; done < ops; done += BATCH) {
    for (int b = 0; b < BATCH; b++) { /* parse: hash, prefetch buckets */
      uint64_t q = p++ & (SEQ - 1);
      idx[b] = seq[q];
      keys[b] = K[q];
      if (prehash) {
        h1[b] = H1[q];
        h2[b] = H2[q];
      } else {
        uint64_t h[2];
        kv_store_hash(t, keys[b], KEY_LEN, h);
        h1[b] = h[0];
        h2[b] = h[1];
      }
      kv_store_prefetch(t, h1[b], h2[b]);
    }
    for (int b = 0; b < BATCH; b++) kv_store_prefetch_records(t, h1[b], h2[b]);
    for (int b = 0; b < BATCH; b++) { /* look up, copy the value as the reply does */
      const KvRec *r = kv_store_get(t, keys[b], KEY_LEN, h1[b], h2[b]);
      memcpy(buf, kv_rec_value(r), VAL_LEN);
      uint64_t v;
      memcpy(&v, buf, 8);
      errors += v != idx[b];
      /* bench_store's verify() also reads version[i] (8 bytes per key, a
       * random, unprefetched access); g_version != NULL reproduces it. */
      if (g_version) errors += g_version[idx[b]] != 0;
    }
    kv_qsbr_quiescent(t, 0);
  }
  double ns = (double)(now_ns() - t0) / (double)ops;
  *pos = p;
  *bad += errors;
  return ns;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
static double median(double *x, int n) {
  double tmp[64];
  memcpy(tmp, x, (size_t)n * sizeof *x);
  qsort(tmp, (size_t)n, sizeof *tmp, cmp_d);
  return n % 2 ? tmp[n / 2] : (tmp[n / 2 - 1] + tmp[n / 2]) / 2;
}

int main(int argc, char **argv) {
  int cpu = argc > 1 ? atoi(argv[1]) : 2, reps = argc > 2 ? atoi(argv[2]) : 9;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
  kv_set_scan_mode(KV_SCAN_SSE2); /* the server's default */
  static const uint64_t sizes[] = {8192, 1048576};
  static const char *names[NV] = {"4k hash", "4k prehash", "thp hash", "thp prehash"};
  for (int si = 0; si < 2; si++) {
    uint64_t n = sizes[si];
    slab_init(n * 176 + (8u << 20));
    KvTable *t4k = build(n, 0);
    KvTable *thp = build(n, 1);
    long slab_huge = vma_huge_kib(slab_base);
    uint32_t *seq = __real_malloc(SEQ * sizeof *seq);
    uint64_t *H1 = __real_malloc(SEQ * sizeof *H1), *H2 = __real_malloc(SEQ * sizeof *H2);
    char (*K)[KEY_LEN] = __real_malloc((size_t)SEQ * KEY_LEN);
    uint64_t s = 0x5EED + n;
    for (uint64_t q = 0; q < SEQ; q++) {
      uint64_t h[2];
      seq[q] = (uint32_t)(rng(&s) % n);
      key_of(seq[q], K[q]);
      kv_store_hash(t4k, K[q], KEY_LEN, h); /* same seed: same hashes in both stores */
      H1[q] = h[0];
      H2[q] = h[1];
    }
    printf("n=%llu keys: slab mapping AnonHugePages %ld KiB (records ~%llu KiB)\n",
           (unsigned long long)n, slab_huge, (unsigned long long)(n * 160 / 1024));
    double res[NV][64];
    uint64_t bad = 0, ops = 4u << 20;
    for (int r = 0; r < reps; r++) {
      for (int k = 0; k < NV; k++) {
        int v = (k + r) % NV; /* rotated order */
        KvTable *t = v < 2 ? t4k : thp;
        int pre = v % 2;
        uint64_t pos = (uint64_t)r * 7919u;
        run(t, seq, (const char (*)[KEY_LEN])K, H1, H2, pre, ops / 4, &pos, &bad); /* warm */
        res[v][r] = run(t, seq, (const char (*)[KEY_LEN])K, H1, H2, pre, ops, &pos, &bad);
      }
    }
    for (int v = 0; v < NV; v++) {
      double ratio[64];
      int faster = 0;
      for (int r = 0; r < reps; r++) {
        ratio[r] = res[v][r] / res[0][r];
        faster += ratio[r] < 1.0;
      }
      printf("  %-12s %6.1f ns/lookup   vs 4k hash: %.3f (faster in %d/%d)\n", names[v],
             median(res[v], reps), median(ratio, reps), faster, reps);
    }
    printf("  wrong answers: %llu\n", (unsigned long long)bad);
    /* The same "4k hash" pipeline plus bench_store's version[i] read. */
    g_version = calloc(n, sizeof *g_version);
    double with_ver[64];
    for (int r = 0; r < reps; r++) {
      uint64_t pos = (uint64_t)r * 7919u;
      run(t4k, seq, (const char (*)[KEY_LEN])K, H1, H2, 0, ops / 4, &pos, &bad);
      with_ver[r] = run(t4k, seq, (const char (*)[KEY_LEN])K, H1, H2, 0, ops, &pos, &bad);
    }
    printf("  4k hash + bench_store's version[i] read: %.1f ns/lookup\n\n", median(with_ver, reps));
    free(g_version);
    g_version = NULL;
    if (bad) return 1;
  }
  return 0;
}
