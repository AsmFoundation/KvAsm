/*
 * seq_probe.c — what a lock-free revision sequence costs, and which way of
 * publishing "everything up to revision R is done" survives concurrency.
 * (docs/proposals/export-watch.md, §3.1 and §5.2)
 *
 * Why it exists: the proposal first numbered writes inside ADR 0003's writer
 * mutex (a plain increment). The review asked why a lock is involved at all
 * when an atomic increment would do. It would — for the NUMBER. But an
 * atomic fetch-add alone does not tell a reader which revisions are
 * finished: writer A can take 7, writer B 8, and B's write can land first.
 * Watchers and cuts need a monotonic "stable" watermark such that every
 * revision at or below it is done. This probe measures the counter and three
 * ways to maintain that watermark, and verifies each under concurrency.
 *
 * Variants (each thread does --ops operations; threads pinned to --cpus):
 *   mutex+inc     kv_mutex_lock; counter++; kv_mutex_unlock — the original
 *                 proposal: correct only because writers are serialized
 *   mutex+lazy    today's serialized writers running the fixed protocol
 *                 (the "seq+lazy" writer side inside the mutex): what the
 *                 fix costs the current server
 *   xadd          LOCK XADD on a shared counter, nothing else: the floor of
 *                 any atomic sequence
 *   seq+cas       xadd; mark done[r]; every writer then advances one shared
 *                 watermark by CAS over contiguous done revisions
 *   seq+inflight  each writer announces a lower bound of its revision in its
 *                 own cache line (re-announcing until the counter did not
 *                 move, so the bound is tight and the watermark monotonic),
 *                 then xadd, publish, clear; a reader computes
 *                 stable = min(next, min(announced) - 1)
 *   seq+lazy      xadd; publish done[r] — nothing else. Readers advance their
 *                 own watermark by scanning published slots (the history
 *                 ring already is such an array). A writer about to reuse a
 *                 slot waits until the slot's previous revision was published.
 * Checked in every trial: the counter ends at threads x ops, and each
 * watermark ends at exactly that revision. With --verify CPU, a reader
 * thread follows the watermark of seq+cas, seq+inflight and seq+lazy while
 * the writers run. It fails the run, printing the values, if the watermark
 * ever covers a revision that is not yet published (unsafe), and it counts
 * readings that fall below an earlier one (safe, but every consumer would
 * then have to clamp). The first version of seq+inflight announced its
 * bound once; this counter is how its regressions were found, and they
 * remain with the re-announcing version, because a stale announcement can
 * become visible before the re-read replaces it.
 *
 * Usage: seq_probe [--ops N] [--cpus 1,2,3,...] [--reps R] [--verify CPU]
 *        runs 1, 2, 4 and all listed CPUs (threads = a prefix of the list).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* pthread_setaffinity_np, CPU_SET */
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* wlock.asm: the server's writer mutex (futex, bounded spin). */
void kv_mutex_lock(uint32_t *lock);
void kv_mutex_unlock(uint32_t *lock);

enum { MAX_THREADS = 32, MAX_REPS = 32, RING = 1 << 20 };
#define NONE UINT64_MAX             /* "no revision" in done[] (cas, inflight) and in announcements */

/** The variants under test (see the file header). */
typedef enum { V_MUTEX_INC, V_MUTEX_LAZY, V_XADD, V_SEQ_CAS, V_SEQ_INFLIGHT, V_SEQ_LAZY, V_COUNT } Variant;

static const char *const kNames[V_COUNT] = {"mutex+inc", "mutex+lazy", "xadd",
                                            "seq+cas",   "seq+inflight", "seq+lazy"};

/** One writer's announcement (seq+inflight), alone on its cache line. */
typedef struct {
  _Alignas(64) _Atomic uint64_t v;
} Inflight;

/* Shared state, each word on its own cache line: the variants must measure
 * their own traffic, not false sharing between unrelated words. */
static _Alignas(64) uint32_t g_lock;               /* wlock.asm mutex word */
static _Alignas(64) uint64_t g_plain;              /* mutex+inc counter */
static _Alignas(64) _Atomic uint64_t g_next;       /* last revision handed out */
static _Alignas(64) _Atomic uint64_t g_stable;     /* seq+cas watermark */
static _Alignas(64) _Atomic uint64_t g_done[RING]; /* published revisions, by slot */
static Inflight g_inflight[MAX_THREADS];
static _Atomic int g_run_verifier;                 /* 1 while a verified trial runs */
static _Atomic int g_threads_now;                  /* writers in the current trial */

/** One thread's assignment. */
typedef struct {
  Variant v;
  int id;                 /* index into g_inflight */
  int cpu;
  uint64_t ops;
  pthread_barrier_t *start;
} Job;

/**
 * seq+cas: marks revision r done, then advances the shared watermark over
 * every contiguous done revision (CAS keeps it monotonic, by one at a time).
 * @param r  the revision this thread just completed
 * @remarks Slot reuse: r shares its slot with r - RING, so r first waits
 *          until the watermark covers r - RING (the previous occupant is
 *          done). It only waits if some reservation lags RING behind.
 */
static void cas_complete(uint64_t r) {
  while (r - atomic_load_explicit(&g_stable, memory_order_acquire) > RING)
    __builtin_ia32_pause();
  atomic_store_explicit(&g_done[r & (RING - 1)], r, memory_order_release);
  uint64_t s = atomic_load_explicit(&g_stable, memory_order_acquire);
  for (;;) {
    uint64_t want = s + 1;
    if (atomic_load_explicit(&g_done[want & (RING - 1)], memory_order_acquire) != want) return;
    if (atomic_compare_exchange_weak_explicit(&g_stable, &s, want, memory_order_acq_rel,
                                              memory_order_acquire))
      s = want;
    /* on failure s holds the newer watermark: keep going from there */
  }
}

/**
 * seq+lazy writer side: publish revision r in its slot.
 * @param r  the revision this thread reserved and completed
 * @remarks The slot's previous occupant is r - RING; the slot holds exactly
 *          that value once it was published (slots start as i - RING, see
 *          trial()). Waiting for it keeps a lapping writer from overwriting an
 *          unpublished revision, without any shared watermark.
 */
static void lazy_publish(uint64_t r) {
  _Atomic uint64_t *slot = &g_done[r & (RING - 1)];
  while (atomic_load_explicit(slot, memory_order_acquire) != r - RING) __builtin_ia32_pause();
  atomic_store_explicit(slot, r, memory_order_release);
}

/**
 * seq+lazy reader side: advances a watermark over contiguous published slots.
 * @param w  the reader's current watermark
 * @returns the new watermark (>= w).
 */
static uint64_t lazy_advance(uint64_t w) {
  while (atomic_load_explicit(&g_done[(w + 1) & (RING - 1)], memory_order_acquire) == w + 1) w++;
  return w;
}

/**
 * seq+inflight: the stable revision computed from the announcements.
 * @param threads  writers taking part
 * @returns min(next, min over writers of (announced bound - 1)).
 * @remarks Safety (C11 seq_cst, compiled to XCHG / LOCK XADD / MOV on x86):
 *          a writer stores its bound (<= its future revision) before its
 *          XADD; if this read of `next` covers revision r, r's XADD precedes
 *          it in the single total order, so the announcement does too and the
 *          loads below see it (or the clear stored after r was published).
 *          Monotonicity needs the bound to be tight, which the writer
 *          ensures by re-announcing until `next` did not move in between.
 *          The counter is read FIRST: reading announcements first could miss
 *          a writer that announces and reserves in between.
 */
static uint64_t stable_inflight(int threads) {
  uint64_t s = atomic_load_explicit(&g_next, memory_order_seq_cst);
  for (int t = 0; t < threads; t++) {
    uint64_t b = atomic_load_explicit(&g_inflight[t].v, memory_order_seq_cst);
    if (b != NONE && b - 1 < s) s = b - 1;
  }
  return s;
}

/**
 * seq+inflight writer side: announce, reserve, publish, clear.
 * @param id  this writer's announcement slot
 * @remarks The announce loop: a bound read before other writers reserved
 *          would be stale, and a reader computing after the announcement
 *          would report a watermark below one it already reported. Re-reading
 *          `next` after the (fenced) store and re-announcing until it held
 *          still makes the bound equal to the counter at announcement time,
 *          so no later reading can fall below an earlier one.
 */
static void inflight_write(int id) {
  uint64_t bound = atomic_load_explicit(&g_next, memory_order_seq_cst) + 1;
  for (;;) {
    atomic_store_explicit(&g_inflight[id].v, bound, memory_order_seq_cst); /* XCHG: full fence */
    uint64_t now = atomic_load_explicit(&g_next, memory_order_seq_cst) + 1;
    if (now == bound) break;
    bound = now;
  }
  uint64_t r = atomic_fetch_add_explicit(&g_next, 1, memory_order_seq_cst) + 1;
  /* The write's effects and its history slot would be published here. */
  atomic_store_explicit(&g_done[r & (RING - 1)], r, memory_order_release);
  atomic_store_explicit(&g_inflight[id].v, NONE, memory_order_release);
}

/**
 * Thread body: pin, wait for the start barrier, run the variant.
 * @param arg  Job*
 * @returns NULL.
 */
static void *run(void *arg) {
  Job *j = arg;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(j->cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
  pthread_barrier_wait(j->start);
  for (uint64_t i = 0; i < j->ops; i++) {
    switch (j->v) {
      case V_MUTEX_INC:
        kv_mutex_lock(&g_lock);
        g_plain++;
        kv_mutex_unlock(&g_lock);
        break;
      case V_MUTEX_LAZY:
        kv_mutex_lock(&g_lock);
        lazy_publish(atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed) + 1);
        kv_mutex_unlock(&g_lock);
        break;
      case V_XADD:
        atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed);
        break;
      case V_SEQ_CAS:
        cas_complete(atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed) + 1);
        break;
      case V_SEQ_INFLIGHT:
        inflight_write(j->id);
        break;
      default: /* V_SEQ_LAZY */
        lazy_publish(atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed) + 1);
        break;
    }
  }
  return NULL;
}

/**
 * Monotonic clock in nanoseconds.
 * @returns CLOCK_MONOTONIC as ns.
 */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/** What the verifier watches and what it found. */
typedef struct {
  Variant v;
  int cpu;
  uint64_t checked;       /* revisions confirmed published below the watermark */
  uint64_t samples;       /* watermark readings */
  uint64_t regressions;   /* readings below an earlier one (not monotonic) */
  int failed;             /* 1: covered an unpublished revision (unsafe) */
  uint64_t f_prev, f_s, f_r, f_d;   /* the values at the failure */
} Verify;

/**
 * Verifier thread: follows the watermark while the writers run. It checks the
 * safety property — every revision at or below the watermark is published —
 * and fails the run at once if it is broken. It also counts readings that
 * fall below an earlier one: safe (they claim less), but a watermark that
 * goes back must be clamped by every consumer, which is a design cost.
 * @param arg  Verify*
 * @returns NULL; the verdict, counts and offending values are in the Verify.
 * @remarks A slot holding a larger revision was legitimately reused after
 *          the older one was published (every variant here waits for that,
 *          or needs a writer stalled for about a million revisions).
 */
static void *verifier(void *arg) {
  Verify *vf = arg;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(vf->cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
  uint64_t prev = 0;
  while (atomic_load(&g_run_verifier)) {
    uint64_t s;
    if (vf->v == V_SEQ_CAS) s = atomic_load_explicit(&g_stable, memory_order_acquire);
    else if (vf->v == V_SEQ_INFLIGHT) s = stable_inflight(atomic_load(&g_threads_now));
    else s = lazy_advance(prev);
    vf->samples++;
    if (s < prev) {
      vf->regressions++;
      continue;                     /* a consumer would keep its larger, still-valid reading */
    }
    for (uint64_t r = prev + 1; r <= s; r++) {
      uint64_t d = atomic_load_explicit(&g_done[r & (RING - 1)], memory_order_acquire);
      if (d < r || (vf->v != V_SEQ_LAZY && d == NONE)) {
        vf->failed = 1;
        vf->f_prev = prev;
        vf->f_s = s;
        vf->f_r = r;
        vf->f_d = d;
        return NULL;
      }
      vf->checked++;
    }
    prev = s;
  }
  return NULL;
}

/**
 * Runs one variant with the first `threads` CPUs and checks its result.
 * @param v          variant
 * @param threads    thread count
 * @param cpus       CPU list
 * @param ops        operations per thread
 * @param verify_cpu CPU for the verifier, or -1 (timing runs)
 * @param vf         receives the verifier's findings (when it runs)
 * @returns elapsed ns; exits 1 if a counter, watermark or verification fails.
 */
static uint64_t trial(Variant v, int threads, const int *cpus, uint64_t ops, int verify_cpu,
                      Verify *vf) {
  pthread_t th[MAX_THREADS], vt;
  Job jobs[MAX_THREADS];
  pthread_barrier_t start;
  int lazy = v == V_SEQ_LAZY || v == V_MUTEX_LAZY;
  int verify = verify_cpu >= 0 && (v == V_SEQ_CAS || v == V_SEQ_INFLIGHT || v == V_SEQ_LAZY);
  g_lock = 0;
  g_plain = 0;
  atomic_store(&g_next, 0);
  atomic_store(&g_stable, 0);
  /* Lazy slots start as "the previous occupant i - RING was published"
   * (slot 0's first revision is RING, whose predecessor is 0); the others
   * start empty. */
  for (uint64_t i = 0; i < RING; i++)
    atomic_store(&g_done[i], lazy ? (i == 0 ? 0 : i - RING) : NONE);
  for (int i = 0; i < MAX_THREADS; i++) atomic_store(&g_inflight[i].v, NONE);
  atomic_store(&g_threads_now, threads);
  if (verify) {
    *vf = (Verify){v, verify_cpu, 0, 0, 0, 0, 0, 0, 0, 0};
    atomic_store(&g_run_verifier, 1);
    if (pthread_create(&vt, NULL, verifier, vf) != 0) {
      fprintf(stderr, "seq_probe: pthread_create failed\n");
      exit(1);
    }
  }
  pthread_barrier_init(&start, NULL, (unsigned)threads + 1);
  for (int i = 0; i < threads; i++) {
    jobs[i] = (Job){v, i, cpus[i], ops, &start};
    if (pthread_create(&th[i], NULL, run, &jobs[i]) != 0) {
      fprintf(stderr, "seq_probe: pthread_create failed\n");
      exit(1);
    }
  }
  pthread_barrier_wait(&start);
  uint64_t t0 = now_ns();
  for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
  uint64_t dt = now_ns() - t0;
  pthread_barrier_destroy(&start);
  if (verify) {
    atomic_store(&g_run_verifier, 0);
    pthread_join(vt, NULL);
    if (vf->failed) {
      fprintf(stderr, "seq_probe: %s, %d threads: UNSAFE: the watermark covered an unpublished revision "
                      "(prev %llu, now %llu, revision %llu, slot holds %llu)\n",
              kNames[v], threads, (unsigned long long)vf->f_prev, (unsigned long long)vf->f_s,
              (unsigned long long)vf->f_r, (unsigned long long)vf->f_d);
      exit(1);
    }
  }

  uint64_t expect = (uint64_t)threads * ops;
  uint64_t got = v == V_MUTEX_INC ? g_plain : atomic_load(&g_next);
  if (got != expect) {
    fprintf(stderr, "seq_probe: %s counter %llu, expected %llu\n", kNames[v], (unsigned long long)got,
            (unsigned long long)expect);
    exit(1);
  }
  uint64_t stable = expect;
  if (v == V_SEQ_CAS) stable = atomic_load(&g_stable);
  else if (v == V_SEQ_INFLIGHT) stable = stable_inflight(threads);
  else if (lazy) stable = lazy_advance(expect > RING ? expect - RING : 0);
  if (stable != expect) {
    fprintf(stderr, "seq_probe: %s watermark %llu, expected %llu\n", kNames[v],
            (unsigned long long)stable, (unsigned long long)expect);
    exit(1);
  }
  return dt;
}

/** qsort comparator for uint64_t. */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/**
 * Parses options and prints, per thread count and variant, the median ns per
 * operation (per thread and system-wide) and the aggregate rate; with
 * --verify, how many revisions the verifier confirmed.
 * @param argc  argument count
 * @param argv  [--ops N] [--cpus a,b,c] [--reps R] [--verify CPU]
 * @returns 0; exits 1 on a failed check, 2 on bad usage.
 * @remarks Verified trials include the verifier's cache traffic, so their
 *          times are not comparable with unverified ones; run.sh times
 *          without it and verifies in a separate run.
 */
int main(int argc, char **argv) {
  uint64_t ops = 2000000;
  int reps = 5, ncpu = 0, verify_cpu = -1;
  int cpus[MAX_THREADS];
  const char *list = "1,2,3,4,5,6,7";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--ops")) ops = strtoull(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--cpus")) list = argv[i + 1];
    else if (!strcmp(argv[i], "--reps")) reps = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--verify")) verify_cpu = atoi(argv[i + 1]);
  }
  for (const char *p = list; *p && ncpu < MAX_THREADS;) {
    cpus[ncpu++] = atoi(p);
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
  }
  if (ops < 1000 || reps < 1 || reps > MAX_REPS || ncpu < 1) {
    fprintf(stderr, "usage: seq_probe [--ops N>=1000] [--cpus a,b,...] [--reps 1..%d] [--verify CPU]\n",
            MAX_REPS);
    return 2;
  }
  int counts[4] = {1, 2, 4, ncpu};
  printf("ops/thread %llu, reps %d, cpus %s%s\n", (unsigned long long)ops, reps, list,
         verify_cpu >= 0 ? " (VERIFY run: times include the verifier's traffic)" : "");
  printf("%-8s %-13s %12s %15s %10s%s\n", "threads", "variant", "ns/op/thread", "ns/op (system)",
         "M ops/s", verify_cpu >= 0 ? "  verified" : "");
  for (int c = 0; c < 4; c++) {
    int threads = counts[c];
    if (threads > ncpu || (c == 3 && threads <= 4)) continue;
    for (int v = 0; v < V_COUNT; v++) {
      uint64_t t[MAX_REPS], checked = 0, samples = 0, regressions = 0;
      Verify vf = {0};
      for (int r = 0; r < reps; r++) {
        t[r] = trial((Variant)v, threads, cpus, ops, verify_cpu, &vf);
        checked += vf.checked;
        samples += vf.samples;
        regressions += vf.regressions;
      }
      qsort(t, (size_t)reps, sizeof *t, cmp_u64);
      double med = (double)t[reps / 2];
      double total = (double)ops * threads;
      printf("%-8d %-13s %12.2f %15.2f %10.2f", threads, kNames[v], med / (double)ops, med / total,
             total / med * 1e3);
      if (verify_cpu >= 0 && (v == V_SEQ_CAS || v == V_SEQ_INFLIGHT || v == V_SEQ_LAZY))
        printf("  %llu revisions safe in %llu readings; went back %llu times", (unsigned long long)checked,
               (unsigned long long)samples, (unsigned long long)regressions);
      printf("\n");
    }
  }
  return 0;
}
