/*
 * bench_write.c — store-level write throughput and writer scaling: the
 * per-write path (kv_store_put, the server's before ADR 0011) against the
 * batched path the server uses since: records prepared outside the mutex,
 * buckets and old records prefetched, one mutex hold per batch, and the
 * replaced records retired and freed in each writer's own QSBR slot,
 * outside the mutex.
 *
 * Why a store-level benchmark: over the network a write's server CPU is
 * dominated by the kernel on this host (about 1.2 µs of a 1.6 µs SET at one
 * worker), and running A and B side by side on a shared desktop halves each
 * core, so the end-to-end A/B (benchmarks/results/2026-11-15-write-batching)
 * shows relative gains but cannot show how writes scale with threads. Here
 * the store is driven directly with the server's exact calls, and only the
 * write path is measured.
 *
 * Workload: N keys preloaded (16-byte keys, 100-byte values, capacity N *
 * 1.25 as the A/B servers use), then T threads replace uniformly random
 * keys — the steady state of a write load, where every write allocates a
 * record and retires the old one. Each thread is a QSBR reader too, as a
 * worker is, and announces a quiescent state after every 16 writes (one
 * batch). Threads are pinned to the even vCPUs 2, 4, ..., 14: one
 * hyperthread per physical core.
 *   single  16 x kv_store_put (lock, index update, retire and reclaim under
 *           the mutex, per write)
 *   batch   hash + prefetch buckets + kv_store_rec_alloc_local and copy for 16
 *           writes, prefetch their old records, kv_store_write_begin,
 *           16 x kv_store_put_locked, kv_store_write_end (the server's
 *           kv_grpc_run_batch with a 16-request read)
 * Method: per repetition, every thread count runs both modes back to back,
 * in an order rotated per repetition. Reported: the median writes/s over
 * the repetitions, batch/single as a paired ratio (geomean, range, wins),
 * and each mode's scaling against its own one-thread rate.
 * Verification (every run, --quick in `make test`): every key's value names
 * its key, and after draining the global limbo and every slot, limbo is
 * empty and the memory accounting equals the live records exactly.
 *
 * Build: make build/bench/bench_write
 * Run:   build/bench/bench_write [--quick] [--reps R] [--seconds S]
 * Caveat: WSL2 vCPUs on a shared host — absolute numbers are indicative,
 * paired ratios and the shape of the scaling are the result.
 */
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

#include "kvnode_abi.h"
#include "test_common.h"

enum { KEY_LEN = 16, VAL_LEN = 100, BATCH = 16, MAX_THREADS = 7, MAX_REPS = 16 };

/** vCPUs the writer threads are pinned to: one hyperthread per core. */
static const int kCpus[MAX_THREADS] = {2, 4, 6, 8, 10, 12, 14};

/** One writer thread's parameters and results. */
typedef struct {
  KvTable *t;
  uint64_t nkeys;
  int batch;              /**< 1: the batched path, 0: kv_store_put per write */
  uint32_t rid;           /**< its QSBR slot (= thread index) */
  atomic_int *go, *stop;
  uint64_t writes;        /**< writes completed (output) */
  uint64_t errors;        /**< refused or failed writes (output; must stay 0) */
} Writer;

/**
 * Key i: "key:" + 12 lowercase hex digits (16 bytes), formatted without
 * snprintf, whose cost would hide what is measured.
 * @param i    key index
 * @param key  out: 16 bytes
 */
static inline void key_of(uint64_t i, char key[KEY_LEN]) {
  static const char hex[] = "0123456789abcdef";
  memcpy(key, "key:", 4);
  for (int d = 15; d >= 4; d--, i >>= 4) key[d] = hex[i & 15];
}

/**
 * Value of key i at a version: [0..8) the key index, [8..16) the version,
 * then filler derived from i, so verification can tell whose value it is.
 * @param i        key index
 * @param version  version written
 * @param v        out: VAL_LEN bytes
 */
static void value_of(uint64_t i, uint64_t version, uint8_t v[VAL_LEN]) {
  memcpy(v, &i, 8);
  memcpy(v + 8, &version, 8);
  for (int b = 16; b < VAL_LEN; b++) v[b] = (uint8_t)(i * 31 + (uint64_t)b);
}

/**
 * Pins the calling thread to one vCPU. Failure (fewer vCPUs, a restricted
 * container) is ignored: the run is then unpinned, not wrong.
 * @param cpu  vCPU number
 */
static void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  (void)sched_setaffinity(0, sizeof set, &set);
}

/**
 * Writer thread: replaces random keys in batches of 16 until told to stop.
 * @param p  Writer*
 * @returns NULL
 */
static void *writer_main(void *p) {
  Writer *w = p;
  pin(kCpus[w->rid % MAX_THREADS]);
  uint64_t s = 0x5EED0000ull + w->rid, ver = (uint64_t)w->rid << 40;
  char keys[BATCH][KEY_LEN];
  uint64_t h[BATCH][2];
  KvRec *recs[BATCH];
  uint8_t val[VAL_LEN];
  kv_qsbr_online(w->t, w->rid);
  while (!atomic_load(w->go)) {
  }
  while (!atomic_load_explicit(w->stop, memory_order_relaxed)) {
    if (w->batch) {
      /* Prepare outside the mutex, as kv_grpc_dispatch does per request. */
      for (int b = 0; b < BATCH; b++) {
        uint64_t i = rng_next(&s) % w->nkeys;
        key_of(i, keys[b]);
        kv_store_hash(w->t, keys[b], KEY_LEN, h[b]);
        kv_store_prefetch(w->t, h[b][0], h[b][1]);
        value_of(i, ++ver, val);
        KvRec *r = kv_store_rec_alloc_local(w->t, w->rid, keys[b], KEY_LEN, VAL_LEN);
        if (kv_is_err(r)) {
          w->errors++;
          recs[b] = NULL;
          continue;
        }
        memcpy((uint8_t *)kv_rec_value(r), val, VAL_LEN);
        recs[b] = r;
      }
      /* kv_grpc_run_batch: old records, then one hold for the batch. */
      for (int b = 0; b < BATCH; b++) kv_store_prefetch_records(w->t, h[b][0], h[b][1]);
      kv_store_write_begin(w->t);
      for (int b = 0; b < BATCH; b++) {
        if (recs[b] && kv_store_put_locked(w->t, w->rid, recs[b], h[b][0], h[b][1]) < 0) {
          w->errors++;   /* -ENOSPC cannot happen on replacements */
        }
      }
      kv_store_write_end(w->t, w->rid);
    } else {
      for (int b = 0; b < BATCH; b++) {
        uint64_t i = rng_next(&s) % w->nkeys;
        key_of(i, keys[0]);
        uint64_t hh[2];
        kv_store_hash(w->t, keys[0], KEY_LEN, hh);
        value_of(i, ++ver, val);
        if (kv_store_put(w->t, keys[0], KEY_LEN, val, VAL_LEN, hh[0], hh[1]) < 0) w->errors++;
      }
    }
    w->writes += BATCH;
    kv_qsbr_quiescent(w->t, w->rid);   /* one event-loop iteration's worth */
  }
  kv_qsbr_offline(w->t, w->rid);
  return NULL;
}

/**
 * Runs one cell: T threads in one mode for a fixed time.
 * @param t        store
 * @param nkeys    key space
 * @param threads  writer threads (1..MAX_THREADS)
 * @param batch    1: batched path, 0: per-write
 * @param seconds  measured duration
 * @param errors   out: refused or failed writes, added up over the threads
 * @returns writes per second over all threads
 */
static double run_cell(KvTable *t, uint64_t nkeys, int threads, int batch, double seconds,
                       uint64_t *errors) {
  atomic_int go = 0, stop = 0;
  Writer w[MAX_THREADS];
  pthread_t th[MAX_THREADS];
  for (int i = 0; i < threads; i++) {
    w[i] = (Writer){.t = t, .nkeys = nkeys, .batch = batch, .rid = (uint32_t)i,
                    .go = &go, .stop = &stop};
    CHECK(pthread_create(&th[i], NULL, writer_main, &w[i]) == 0);
  }
  uint64_t t0 = now_ns();
  atomic_store(&go, 1);
  while ((double)(now_ns() - t0) < seconds * 1e9) {
    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
  }
  atomic_store(&stop, 1);
  uint64_t total = 0;
  for (int i = 0; i < threads; i++) {
    pthread_join(th[i], NULL);
    total += w[i].writes;
    *errors += w[i].errors;
  }
  return (double)total / ((double)(now_ns() - t0) / 1e9);
}

/** qsort comparator for doubles. @param a,b elements @returns -1, 0 or 1 */
static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/** @param v values @param n count (>= 1) @returns their median (v is sorted) */
static double median(double *v, int n) {
  qsort(v, (size_t)n, sizeof *v, cmp_double);
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/**
 * Entry point.
 * @param argc  argument count
 * @param argv  --quick (small and short, for `make test`), --reps R,
 *              --seconds S
 * @returns 0 when verification passed and no write failed
 */
int main(int argc, char **argv) {
  int quick = 0, reps = 3;
  double seconds = 2.0;
  for (int a = 1; a < argc; a++) {
    if (!strcmp(argv[a], "--quick")) quick = 1;
    else if (!strcmp(argv[a], "--reps") && a + 1 < argc) reps = atoi(argv[++a]);
    else if (!strcmp(argv[a], "--seconds") && a + 1 < argc) seconds = atof(argv[++a]);
    else {
      fprintf(stderr, "usage: %s [--quick] [--reps R] [--seconds S]\n", argv[0]);
      return 2;
    }
  }
  if (reps < 1) reps = 1;
  if (reps > MAX_REPS) reps = MAX_REPS;
  const uint64_t nkeys = quick ? 50000 : 1000000;
  if (quick) {
    reps = 1;
    seconds = 0.2;
  }
  static const int kThreads[] = {1, 2, 4, 7};
  const int ncounts = quick ? 2 : 4;

  uint8_t seed[16];
  uint64_t rs = 0xB3A7;
  rng_fill(&rs, seed, sizeof seed);
  KvTable *t = kv_store_create(nkeys + nkeys / 4, MAX_THREADS, seed, 0);
  CHECK(t != NULL);
  if (!t) return test_finish("bench_write");
  char key[KEY_LEN];
  uint8_t val[VAL_LEN];
  for (uint64_t i = 0; i < nkeys; i++) {
    key_of(i, key);
    uint64_t h[2];
    kv_store_hash(t, key, KEY_LEN, h);
    value_of(i, 0, val);
    CHECK(kv_store_put(t, key, KEY_LEN, val, VAL_LEN, h[0], h[1]) == 1);
  }

  double single[4][MAX_REPS], batch[4][MAX_REPS];
  uint64_t errors = 0;
  for (int r = 0; r < reps; r++) {
    for (int c = 0; c < ncounts; c++) {
      int first = r % 2;   /* rotate which mode runs first */
      for (int k = 0; k < 2; k++) {
        int mode = k ^ first;
        double rate = run_cell(t, nkeys, kThreads[c], mode, seconds, &errors);
        if (mode) batch[c][r] = rate;
        else single[c][r] = rate;
      }
    }
  }
  CHECK_EQ_U64(errors, 0);

  if (!quick) {
    printf("bench_write: %llu keys, 16-byte keys, %d-byte values, uniform replacements,"
           " %d reps x %.1f s per cell\n", (unsigned long long)nkeys, VAL_LEN, reps, seconds);
    printf("  threads   single (put)   batch (ADR 0011)   batch/single  [range]  wins"
           "   scaling single  batch\n");
    double base_s = 0, base_b = 0;
    for (int c = 0; c < ncounts; c++) {
      double ratio_log = 0, lo = 1e9, hi = 0, s_m, b_m, tmp_s[MAX_REPS], tmp_b[MAX_REPS];
      int wins = 0;
      for (int r = 0; r < reps; r++) {
        double q = batch[c][r] / single[c][r];
        ratio_log += log(q);
        lo = q < lo ? q : lo;
        hi = q > hi ? q : hi;
        wins += q > 1;
        tmp_s[r] = single[c][r];
        tmp_b[r] = batch[c][r];
      }
      s_m = median(tmp_s, reps);
      b_m = median(tmp_b, reps);
      if (c == 0) {
        base_s = s_m;
        base_b = b_m;
      }
      printf("  %7d   %9.2f M/s    %9.2f M/s       %6.2fx  [%.2f-%.2f]  %d/%d   %10.2fx %6.2fx\n",
             kThreads[c], s_m / 1e6, b_m / 1e6, exp(ratio_log / reps), lo, hi, wins, reps,
             s_m / base_s, b_m / base_b);
    }
  }

  /* Verification: every key holds a value that names it; after draining
   * the global limbo and every slot, nothing waits and the accounting is
   * exact (every key is live: replacements only). */
  for (uint64_t i = 0; i < nkeys; i++) {
    key_of(i, key);
    uint64_t h[2], id;
    kv_store_hash(t, key, KEY_LEN, h);
    const KvRec *rec = kv_store_get(t, key, KEY_LEN, h[0], h[1]);
    CHECK(rec != NULL && rec->val_len == VAL_LEN);
    if (!rec) continue;
    memcpy(&id, kv_rec_value(rec), 8);
    CHECK_EQ_U64(id, i);
  }
  for (int round = 0; round < 10000; round++) {
    uint64_t left = kv_store_reclaim(t);
    for (uint32_t s2 = 0; s2 < MAX_THREADS; s2++) left += kv_store_reclaim_local(t, s2, 4096);
    if (left == 0) break;
  }
  KvStats st;
  kv_store_stats(t, &st);
  CHECK_EQ_U64(st.limbo_count, 0);
  CHECK_EQ_U64(st.count, nkeys);
  CHECK_EQ_U64(st.mem_used, nkeys * (24 + KEY_LEN + VAL_LEN));
  kv_store_destroy(t);
  return test_finish("bench_write");
}
