/*
 * endpoints_probe.c — endpoints derived from how KvNode hashes and stores,
 * and the SIMD work they let the server do on values. Evidence for
 * docs/proposals/functions-plan.md: Part II §3 (the store as built, and
 * where SIMD pays) and Group J (the functions measured here).
 *
 * Why it exists: a re-read of the store (siphash.asm, cuckoo.asm,
 * store.asm, grpc.asm) showed that a GET is memory-bound (two dependent
 * misses: bucket line, then record), that SIMD inside the lookup can only
 * shave the hash and the compares, and that values are opaque bytes the
 * server never computes on. So the next endpoints come from (a) what the
 * bucket and record layout makes O(1) and (b) value-level work where SIMD
 * runs at GB/s and returns a few bytes instead of whole values. This probe
 * measures the ones whose cost is not obvious, on the store's own code at
 * 10M keys:
 *
 *   bar        exact get in the three regimes (D dependent, I independent,
 *              B the server's batched pipeline), plus the cheap layout
 *              endpoints: Stat (lengths, no value) and GetRange (16 bytes)
 *   etag       CRC32C of values with the server's own assembly (folding
 *              VPCLMULQDQ vs the SSE4.2 instruction), and a conditional get
 *              that answers "not modified" from an on-the-fly CRC
 *   snapshot   a consistent multi-key read from bucket versions (double
 *              collect): cost against plain batched gets, and a writer race
 *              that counts torn reads of both
 *   scan       unordered iteration over the whole table by bucket cursor
 *              (keys, keys + values, 7 threads by hash shard)
 *   cells      max registers (LOCK CMPXCHG), 64-flag words (LOCK OR) and
 *              counter arrays (LOCK XADD at an index; AVX2 sum and max)
 *   json       field extraction from JSON values: a simdjson-style AVX2 +
 *              PCLMULQDQ structural scan against a scalar tokenizer, with
 *              the same path walker; then JsonGet on a store of documents
 *   sha        SHA-256 with the SHA-NI instructions against a scalar
 *              reference (content addressing: PutBlob, FindMissingBlobs)
 *   grep       substring search over values: an AVX2 first/last-byte filter
 *              against glibc memmem
 *
 * Checks: every section verifies against a reference before its numbers
 * print: every scanned key against the key set, every snapshot against the
 * writer's ordering, every structural position and every extracted field
 * against the scalar tokenizer and the generator's expected values, SHA-256
 * against FIPS 180-4 vectors and the scalar code, CRC32C against a bitwise
 * reference, every substring count against memmem, every aggregate and
 * register against scalar code. A mismatch exits 1.
 *
 * Usage: endpoints_probe [--keys N] [--cpu C] [--reps R]
 * Build: run.sh next to this file.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* MAP_ANONYMOUS, affinity, memmem */
#endif
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>          /* __rdtsc */

#include "kvnode_abi.h"

/* The server's CRC-32C (crc32c.asm), as declared in tests/test_crc32c.c. */
uint32_t kv_crc32c(uint32_t crc, const void *buf, size_t len);
int kv_crc32c_best_mode(void);
int kv_crc32c_set_mode(int mode);

enum {
  KEYMAX = 32,           /* key buffer; the longest key is 21 bytes */
  VAL_LEN = 100,         /* the benchmark suite's value size */
  FIELDS = 4,            /* keys per "user" */
  BATCH = 32,            /* the server's GET batch */
  MAX_REPS = 16,
  MAX_THREADS = 7,       /* CPUs 1-7: distinct P-cores on this host */
  NQ = 1 << 20,          /* lookups per timed pass */
  SLOTS = 6,             /* record slots per bucket (kvnode.inc BK_SLOTS) */
};

static const char *const kSyl[20] = {"ka", "lo", "mi", "nu", "ra", "se", "ti", "vo", "ze", "ba",
                                     "de", "fi", "go", "hu", "ja", "ke", "li", "mo", "na", "pe"};
static const char *const kFields[FIELDS] = {"name", "email", "plan", "prefs"};
static const uint8_t kSeed[16] __attribute__((aligned(8))) = {9, 7, 5, 3, 1, 2, 4, 6,
                                                              8, 10, 12, 14, 16, 18, 20, 22};
/* Thread i runs on CPU g_tcpu[i]: vCPU 0 carries VM interrupt work, 8-15 are HT siblings. */
static const int g_tcpu[MAX_THREADS] = {1, 2, 3, 4, 5, 6, 7};

static uint32_t g_n;             /* keys in the main store */
static KvTable *g_store;         /* the main store: 20-21 byte keys, 100-byte values */
static uint8_t *g_arena;         /* every main-store key once, back to back */
static uint32_t *g_start;        /* key i = g_arena[g_start[i] .. g_start[i + 1]) */
static uint8_t g_sink[1 << 16];  /* value copies land here */
static volatile uint64_t g_vsink;   /* timed results are folded here so no loop is dead code */

/* ========================================================================= */
/* Utilities                                                                  */
/* ========================================================================= */

/**
 * Monotonic clock in nanoseconds.
 * @returns CLOCK_MONOTONIC as ns.
 */
static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/**
 * Prints a message and exits with status 1.
 * @param what  the failed condition
 * @remarks Every check guards printed numbers: a failure aborts the run so
 *          a wrong structure can never report a fast time.
 */
static void die(const char *what) {
  fprintf(stderr, "endpoints_probe: %s\n", what);
  exit(1);
}

/**
 * Zeroed heap memory that cannot fail.
 * @param n  bytes
 * @returns the memory; exits on allocation failure.
 */
static void *xcalloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) die("out of memory");
  return p;
}

/**
 * Zeroed memory for a large structure, marked for transparent huge pages.
 * @param n  bytes
 * @returns page-aligned memory; exits on failure. Release with big_free.
 */
static void *big_alloc(size_t n) {
  void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("out of memory (mmap)");
  madvise(p, n, MADV_HUGEPAGE);
  return p;
}

/**
 * Releases memory from big_alloc.
 * @param p  the memory
 * @param n  its size in bytes (as passed to big_alloc)
 */
static void big_free(void *p, size_t n) { munmap(p, n); }

/**
 * Pins the calling thread to one CPU.
 * @param cpu  CPU number
 * @remarks Exits if the kernel refuses: an unpinned run would mix P-cores,
 *          E-cores and HT siblings.
 */
static void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) die("pthread_setaffinity_np");
}

/**
 * Compiler barrier: makes the compiler assume all memory may have changed.
 * @remarks Timing loops that repeat a pure call with the same arguments
 *          call it once per iteration, or GCC computes the call once (the
 *          services probe's lesson). Emits no instruction.
 */
static inline void clobber(void) { __asm__ volatile("" ::: "memory"); }

/**
 * xorshift64 step: the probe's deterministic random source.
 * @param s  state (never 0)
 * @returns the next 64 random bits.
 */
static inline uint64_t rnd(uint64_t *s) {
  *s ^= *s << 13;
  *s ^= *s >> 7;
  *s ^= *s << 17;
  return *s;
}

/**
 * splitmix64 finalizer: a well-mixed 64-bit function of 64 bits.
 * @param x  input
 * @returns 64 mixed bits.
 */
static inline uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

/**
 * qsort comparator for uint64_t.
 * @param a, b  the two elements
 * @returns negative, zero or positive as *a is below, equal to or above *b.
 */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/**
 * Median of per-repetition totals, per operation.
 * @param t    totals in ns (sorted in place)
 * @param reps how many
 * @param ops  operations per repetition
 * @returns the median ns per operation.
 */
static double med(uint64_t *t, int reps, double ops) {
  qsort(t, (size_t)reps, sizeof *t, cmp_u64);
  return (double)t[reps / 2] / ops;
}

/**
 * Writes main-store key number i: "user:<5 syllables of i/4>:<field i%4>".
 * @param i    key index (< 4 * 20^5)
 * @param out  at least KEYMAX bytes
 * @returns the key length (20 or 21).
 */
static int make_key(uint32_t i, uint8_t *out) {
  uint32_t u = i / FIELDS;
  for (int s = 4; s >= 0; s--) {
    memcpy(out + 5 + 2 * s, kSyl[u % 20], 2);
    u /= 20;
  }
  memcpy(out, "user:", 5);
  out[15] = ':';
  size_t fl = strlen(kFields[i % FIELDS]);
  memcpy(out + 16, kFields[i % FIELDS], fl);
  return 16 + (int)fl;
}

/**
 * Main-store key i's bytes in the arena.
 * @param i  key index (< g_n)
 * @returns a pointer to its first byte (not NUL-terminated).
 */
static inline const uint8_t *akey(uint32_t i) { return g_arena + g_start[i]; }

/**
 * Main-store key i's length.
 * @param i  key index (< g_n)
 * @returns 20 or 21.
 */
static inline uint32_t alen(uint32_t i) { return g_start[i + 1] - g_start[i]; }

/**
 * A key's order-independent signature, so a scan's keys can be checked
 * against the key set by summing.
 * @param k    key bytes (at least 16)
 * @param len  key length
 * @returns 64 mixed bits of the key.
 */
static inline uint64_t key_sig(const uint8_t *k, uint32_t len) {
  uint64_t a, b, c;
  memcpy(&a, k, 8);
  memcpy(&b, k + 8, 8);
  memcpy(&c, k + len - 8, 8);
  return mix64(a ^ (b << 21 | b >> 43) ^ (c << 42 | c >> 22) ^ len);
}

/* The exact-get bars (ns): D = dependent chain, I = independent loop, B = batched. */
enum { BAR_NONE = -1, BAR_DEP = 0, BAR_IND = 1, BAR_BAT = 2 };
static double g_bar[3];

/**
 * Prints one result row with its ratio to the main store's bar of the same
 * regime.
 * @param what  operation (blank continues the previous one)
 * @param how   technique
 * @param ns    latency in ns
 * @param bar   BAR_DEP / BAR_IND / BAR_BAT, or BAR_NONE for no ratio
 * @param note  remark (may be "")
 * @remarks "GOLD" marks a ratio of at most 10: the bar of this work.
 */
static void row(const char *what, const char *how, double ns, int bar, const char *note) {
  char v[48], r[48];
  if (ns < 1e3) snprintf(v, sizeof v, "%9.1f ns", ns);
  else if (ns < 1e6) snprintf(v, sizeof v, "%9.2f us", ns / 1e3);
  else snprintf(v, sizeof v, "%9.2f ms", ns / 1e6);
  if (bar >= 0) {
    double x = ns / g_bar[bar];
    snprintf(r, sizeof r, "%7.2fx%c %s", x, "DIB"[bar], x <= 10.0 ? "GOLD" : "    ");
  } else {
    snprintf(r, sizeof r, "%-14s", "");
  }
  printf("  %-32s %-46s %s  %s  %s\n", what, how, v, r, note);
}

/**
 * Prints this process's resident and huge-page-backed anonymous memory.
 * @remarks Only this process's own maps are read: the VM holds processes
 *          stuck in D state whose /proc files hang a reader.
 */
static void print_memory(void) {
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  char line[256];
  if (!f) return;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "Rss:", 4) || !strncmp(line, "AnonHugePages:", 14)) printf("  %s", line);
  fclose(f);
}

/*
 * Timed kernels are never inlined, so a scalar row and its SIMD row pay the
 * same call; scalar references are never auto-vectorized, so a "scalar" row
 * is scalar (the services probe's lessons).
 */
#define KERNEL __attribute__((noinline))
#define SCALAR __attribute__((noinline, optimize("no-tree-vectorize")))

/* ========================================================================= */
/* The table's layout, read directly (kvnode.inc)                             */
/* ========================================================================= */

/** kvnode.inc Bucket: version, 8 tag bytes (6 used), 6 record slots: one 64-byte line. */
typedef struct {
  uint64_t ver, tags;
  const KvRec *slot[SLOTS];
} Bucket;

/** The first fields of kvnode.inc Table (line 0, immutable after creation). */
typedef struct {
  Bucket *buckets;
  uint64_t mask;
} TableHead;

/**
 * A key's two candidate buckets: LOCATE's arithmetic (cuckoo.asm).
 * @param t   the table
 * @param h1  the key's hash, low word (selects the primary bucket)
 * @param h2  high word (its top byte is the tag, which selects the other)
 * @param b1  receives the primary bucket
 * @param b2  receives the alternate bucket
 * @remarks tag = h2 >> 56 (0 becomes 1); b2 = b1 ^ ((((tag * K) >> 32) | 1)
 *          & mask) with K = 0x9E3779B97F4A7C15: the alternate depends only
 *          on (bucket, tag), so relocation never needs the key.
 */
static inline void buckets_of(const TableHead *t, uint64_t h1, uint64_t h2, const Bucket **b1, const Bucket **b2) {
  uint64_t tag = h2 >> 56;
  if (!tag) tag = 1;
  uint64_t i1 = h1 & t->mask;
  uint64_t off = (((tag * 0x9E3779B97F4A7C15ull) >> 32) | 1) & t->mask;
  *b1 = &t->buckets[i1];
  *b2 = &t->buckets[i1 ^ off];
}

/* ========================================================================= */
/* Key streams and the bar                                                    */
/* ========================================================================= */

/** A stream of query keys, one after another (a request stream). */
typedef struct {
  uint8_t (*key)[KEYMAX];
  uint32_t *len;
} Stream;

static Stream g_q;               /* main-store query stream */

/**
 * NQ random main-store keys copied into a sequential stream.
 * @param n     key-space size (ids are drawn from [0, n))
 * @param seed  random seed (non-zero)
 * @param ids   receives the NQ ids, may be NULL
 * @returns the stream (heap memory, kept for the whole run).
 * @remarks A server reads keys from its request buffers, which are in
 *          cache; a random arena access per lookup would add a miss.
 */
static Stream make_stream(uint32_t n, uint64_t seed, uint32_t *ids) {
  Stream s = {xcalloc((size_t)NQ * KEYMAX), xcalloc((size_t)NQ * sizeof(uint32_t))};
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t id = (uint32_t)(rnd(&seed) % n);
    if (ids) ids[i] = id;
    s.len[i] = alen(id);
    memcpy(s.key[i], akey(id), s.len[i]);
  }
  return s;
}

/**
 * Hashes a batch of 32 stream keys and prefetches both buckets of each,
 * key by key, as the server's parse path does.
 * @param t  store
 * @param s  stream
 * @param b  index of the batch's first key
 * @param h  receives the 32 hash pairs
 */
static inline void hash_prefetch_batch(KvTable *t, const Stream *s, uint32_t b, uint64_t h[BATCH][2]) {
  for (int k = 0; k < BATCH; k++) {
    kv_store_hash(t, s->key[b + (uint32_t)k], s->len[b + (uint32_t)k], h[k]);
    kv_store_prefetch(t, h[k][0], h[k][1]);
  }
}

/**
 * NQ exact gets where each key is chosen by the previous value (pure
 * latency), copying the 100-byte value.
 * @returns total ns; exits if a key is missing.
 * @remarks The next key is i or i + NQ/2, never a neighbour, so no lookup
 *          is answered from cache (the golden probe's lesson).
 */
static uint64_t bar_dependent(void) {
  uint64_t t0 = now_ns(), x = 0;
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t q = (i + (uint32_t)x * (NQ / 2)) & (NQ - 1);
    uint64_t h[2];
    kv_store_hash(g_store, g_q.key[q], g_q.len[q], h);
    const KvRec *r = kv_store_get(g_store, g_q.key[q], g_q.len[q], h[0], h[1]);
    if (!r) die("dependent get missed");
    memcpy(g_sink, kv_rec_value(r), VAL_LEN);
    x = g_sink[VAL_LEN - 1] & 1;
  }
  return now_ns() - t0;
}

/**
 * NQ exact gets in a plain loop (independent iterations).
 * @returns total ns; exits if a key is missing.
 */
static uint64_t bar_independent(void) {
  uint64_t t0 = now_ns();
  for (uint32_t i = 0; i < NQ; i++) {
    uint64_t h[2];
    kv_store_hash(g_store, g_q.key[i], g_q.len[i], h);
    const KvRec *r = kv_store_get(g_store, g_q.key[i], g_q.len[i], h[0], h[1]);
    if (!r) die("independent get missed");
    memcpy(g_sink, kv_rec_value(r), VAL_LEN);
  }
  return now_ns() - t0;
}

/* What a batched pass does with each record found. */
enum { DO_GET = 0, DO_STAT = 1, DO_RANGE = 2, DO_ETAG = 3 };

/**
 * The server's batched GET pipeline over NQ keys, with a choice of what to
 * answer: the whole value, its metadata, a slice, or "not modified".
 * @param what  DO_GET: copy the 100-byte value (constant size: inlined
 *              moves, as the golden probe); DO_STAT: read only the header
 *              (key and value lengths); DO_RANGE: copy 16 bytes at offset
 *              40; DO_ETAG: CRC32C of the value (the server's assembly),
 *              compared with a client tag, the value copied only on change
 * @param etags  DO_ETAG: the client's tag per stream position (NULL else)
 * @param changed  DO_ETAG: incremented per value that differed (may be NULL)
 * @returns total ns; exits if a key is missing.
 */
static uint64_t bar_batched(int what, const uint32_t *etags, uint64_t *changed) {
  uint64_t t0 = now_ns(), h[BATCH][2], acc = 0, ch = 0;
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    hash_prefetch_batch(g_store, &g_q, b, h);
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const KvRec *r = kv_store_get(g_store, g_q.key[b + (uint32_t)k], g_q.len[b + (uint32_t)k], h[k][0], h[k][1]);
      if (!r) die("batched get missed");
      if (what == DO_GET) {
        memcpy(g_sink, kv_rec_value(r), VAL_LEN);
      } else if (what == DO_STAT) {
        acc += r->val_len + r->key_len;
      } else if (what == DO_RANGE) {
        memcpy(g_sink, kv_rec_value(r) + 40, 16);
      } else {
        uint32_t c = kv_crc32c(0, kv_rec_value(r), r->val_len);
        if (c != etags[b + (uint32_t)k]) {   /* changed: send the value */
          memcpy(g_sink, kv_rec_value(r), VAL_LEN);
          ch++;
        }
      }
    }
  }
  g_vsink += acc;
  if (changed) *changed += ch;
  return now_ns() - t0;
}

/* ========================================================================= */
/* Consistent multi-key reads (double collect over bucket versions)           */
/* ========================================================================= */

enum { GROUP = 16 };

/**
 * Busy-waits for a number of TSC cycles.
 * @param cycles  how long (0 returns at once)
 * @remarks The race test spends this per key read, standing in for the cache
 *          miss a cold key costs at 10M keys (the bar D, 100-400 ns): with
 *          16 hot keys in cache a reader is so much faster than a writer
 *          that the writer can never overtake it, and no read could tear.
 */
static inline void spin_cycles(uint64_t cycles) {
  uint64_t t0 = __rdtsc();
  while (__rdtsc() - t0 < cycles) _mm_pause();
}

/**
 * Reads a group of keys as one consistent snapshot: collect every key's
 * bucket versions and value, then re-read the versions; any change means a
 * write landed in one of those buckets meanwhile, so try again.
 * @param t      store (its head is read for bucket addresses)
 * @param keys   GROUP keys
 * @param lens   their lengths
 * @param h      their hashes
 * @param seqs   receives each value's first 8 bytes
 * @param tries  incremented per attempt (1 when nothing raced)
 * @param delay  TSC cycles spent after each key (0 = none; see spin_cycles)
 * @remarks Why it is a snapshot: a bucket's version is odd while a writer
 *          changes it and +2 after, so unchanged even versions across the
 *          whole window mean no write touched any of the keys' buckets
 *          during it. Every value read was therefore current throughout,
 *          all at the same instants. Loads stay in program order on x86
 *          (TSO); the acquire loads keep the compiler from moving them.
 *          A writer that never pauses would starve it (the seqlock
 *          livelock); the endpoint's fallback is to read under the writer
 *          lock after a few attempts. Exits if a key is missing.
 */
static void read_snapshot(KvTable *t, uint8_t keys[GROUP][KEYMAX], const uint32_t *lens, uint64_t h[GROUP][2],
                          uint64_t *seqs, uint64_t *tries, uint64_t delay) {
  const TableHead *th = (const TableHead *)(const void *)t;
  const Bucket *b1[GROUP], *b2[GROUP];
  uint64_t v1[GROUP], v2[GROUP];
  for (int k = 0; k < GROUP; k++) buckets_of(th, h[k][0], h[k][1], &b1[k], &b2[k]);
  for (;;) {
    int odd = 0;
    ++*tries;
    for (int k = 0; k < GROUP; k++) {
      v1[k] = __atomic_load_n(&b1[k]->ver, __ATOMIC_ACQUIRE);
      v2[k] = __atomic_load_n(&b2[k]->ver, __ATOMIC_ACQUIRE);
      odd |= (int)((v1[k] | v2[k]) & 1);
      const KvRec *r = kv_store_get(t, keys[k], lens[k], h[k][0], h[k][1]);
      if (!r) die("snapshot get missed");
      memcpy(&seqs[k], kv_rec_value(r), 8);
      if (delay) spin_cycles(delay);
    }
    if (odd) {
      _mm_pause();
      continue;
    }
    int same = 1;
    for (int k = 0; k < GROUP; k++)
      same &= __atomic_load_n(&b1[k]->ver, __ATOMIC_ACQUIRE) == v1[k] &&
              __atomic_load_n(&b2[k]->ver, __ATOMIC_ACQUIRE) == v2[k];
    if (same) return;
  }
}

/**
 * The same group read without validation: what GetMany returns today.
 * @param t      store
 * @param keys   GROUP keys
 * @param lens   their lengths
 * @param h      their hashes
 * @param seqs   receives each value's first 8 bytes
 * @param delay  TSC cycles spent after each key (0 = none; see spin_cycles)
 */
static void read_plain(KvTable *t, uint8_t keys[GROUP][KEYMAX], const uint32_t *lens, uint64_t h[GROUP][2],
                       uint64_t *seqs, uint64_t delay) {
  for (int k = 0; k < GROUP; k++) {
    const KvRec *r = kv_store_get(t, keys[k], lens[k], h[k][0], h[k][1]);
    if (!r) die("plain group get missed");
    memcpy(&seqs[k], kv_rec_value(r), 8);
    if (delay) spin_cycles(delay);
  }
}

/**
 * Whether a group read is a state the writer actually passed through: it
 * writes keys 0..15 in order with sequence s, then all with s + 1, so every
 * instant shows a prefix at s + 1 and the rest at s.
 * @param v  the 16 sequences read, in key order
 * @returns 1 when non-increasing with a spread of at most 1.
 */
static int is_snapshot(const uint64_t *v) {
  for (int i = 1; i < GROUP; i++)
    if (v[i] > v[i - 1]) return 0;
  return v[0] - v[GROUP - 1] <= 1;
}

static KvTable *g_grp;           /* the race test's 16-key store */
static uint8_t g_gkeys[GROUP][KEYMAX];
static uint32_t g_glens[GROUP];
static uint64_t g_gh[GROUP][2];
static _Atomic int g_stop;

enum { RACE_READ_DELAY = 400, RACE_WRITER_PAUSE = 30000 };   /* TSC cycles: ~0.1 us per key, ~10 us per round */

/**
 * The race's writer: rewrites the 16 keys in order with a rising sequence,
 * pausing between rounds, until told to stop.
 * @param arg  unused
 * @returns NULL; exits if a put does not replace.
 * @remarks The pause stands for a group that is written in bursts (a
 *          transfer touching several balances), not continuously: a writer
 *          that never rests starves every validated read (see read_snapshot).
 */
static void *race_writer(void *arg) {
  (void)arg;
  pin(2);
  uint8_t val[VAL_LEN];
  memset(val, 0, sizeof val);
  for (uint64_t s = 2; !atomic_load_explicit(&g_stop, memory_order_relaxed); s++) {
    for (int k = 0; k < GROUP; k++) {
      memcpy(val, &s, 8);
      if (kv_store_put(g_grp, g_gkeys[k], g_glens[k], val, VAL_LEN, g_gh[k][0], g_gh[k][1]) != 0)
        die("race put did not replace");
    }
    spin_cycles(RACE_WRITER_PAUSE);
  }
  return NULL;
}

/* ========================================================================= */
/* Unordered scan by bucket cursor                                            */
/* ========================================================================= */

enum { SCAN_AHEAD = 8 };         /* buckets whose records are prefetched ahead of the reader */

/** One scan's result: keys seen and the sum of their signatures. */
typedef struct {
  uint64_t count, sig, bytes;
} ScanOut;

/**
 * The occupied slots of a bucket.
 * @param b  bucket
 * @returns bit i set when slot i holds a record (tag byte non-zero), from
 *          one SSE2 compare of the tag word against zero.
 */
static inline uint32_t occupied(const Bucket *b) {
  __m128i t = _mm_cvtsi64_si128((long long)b->tags);
  uint32_t empty = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(t, _mm_setzero_si128()));
  return ~empty & 0x3F;
}

/**
 * Visits every key in buckets [from, to): what an unordered Iterate or
 * ScanShard endpoint does, with no index at all.
 * @param t      store head
 * @param from   first bucket
 * @param to     end bucket
 * @param vals   1 = also copy each value (100 bytes)
 * @param out    receives the count, signature sum and value bytes
 * @remarks Buckets are read in order, so their lines stream in by the
 *          hardware prefetcher; records are random, so the records of the
 *          bucket SCAN_AHEAD positions ahead are prefetched while this one
 *          is read. A cursor is just a bucket index: the table never
 *          rehashes (its size is fixed at creation), so a cursor stays
 *          valid across calls. No writer runs here; under writes a key
 *          moved by a cuckoo displacement can be missed or seen twice, which
 *          the endpoint detects with the table's move counter.
 */
static void scan_range(const TableHead *t, uint64_t from, uint64_t to, int vals, ScanOut *out) {
  uint64_t count = 0, sig = 0, bytes = 0;
  for (uint64_t b = from; b < to; b++) {
    if (b + SCAN_AHEAD < to) {
      const Bucket *pb = &t->buckets[b + SCAN_AHEAD];
      for (uint32_t m = occupied(pb); m; m &= m - 1) __builtin_prefetch(pb->slot[__builtin_ctz(m)]);
    }
    const Bucket *B = &t->buckets[b];
    for (uint32_t m = occupied(B); m; m &= m - 1) {
      const KvRec *r = B->slot[__builtin_ctz(m)];
      if (!r) continue;
      count++;
      sig += key_sig(r->data, r->key_len);
      if (vals) {
        memcpy(g_sink, kv_rec_value(r), VAL_LEN);
        bytes += r->val_len;
      }
    }
  }
  out->count = count;
  out->sig = sig;
  out->bytes = bytes;
}

/** One scan thread's assignment. */
typedef struct {
  int cpu, slot;
  uint64_t from, to;
  ScanOut out;
  pthread_barrier_t *start;
} ScanJob;

/**
 * Scan thread: one hash shard (a bucket range), online as a QSBR reader.
 * @param arg  ScanJob*
 * @returns NULL.
 */
static void *scan_thread(void *arg) {
  ScanJob *j = arg;
  pin(j->cpu);
  kv_qsbr_online(g_store, (uint32_t)j->slot);
  pthread_barrier_wait(j->start);
  scan_range((const TableHead *)(const void *)g_store, j->from, j->to, 0, &j->out);
  kv_qsbr_offline(g_store, (uint32_t)j->slot);
  return NULL;
}

/* ========================================================================= */
/* In-place cells: max registers, flag words, counter arrays                  */
/* ========================================================================= */

/**
 * Raises a cell to v if v is larger (a max register), lock-free.
 * @param c  8-aligned int64 cell
 * @param v  candidate
 * @returns the value before.
 * @remarks LOCK CMPXCHG loop: only a larger candidate writes, so a register
 *          that already holds the maximum is a plain load.
 */
static inline int64_t cell_max(_Atomic int64_t *c, int64_t v) {
  int64_t cur = atomic_load_explicit(c, memory_order_relaxed);
  while (cur < v && !atomic_compare_exchange_weak_explicit(c, &cur, v, memory_order_relaxed, memory_order_relaxed)) {
  }
  return cur;
}

enum { ARR = 168 };              /* a week of hourly counters in one value */

/**
 * Sum of an int64 array, scalar.
 * @param a  elements
 * @param n  count
 * @returns the sum.
 */
SCALAR static int64_t arr_sum_scalar(const int64_t *a, size_t n) {
  int64_t s = 0;
  for (size_t i = 0; i < n; i++) s += a[i];
  return s;
}

/**
 * Sum of an int64 array with AVX2: four accumulators of four lanes.
 * @param a  elements
 * @param n  count (any; the tail is scalar)
 * @returns the sum.
 */
__attribute__((target("avx2"))) KERNEL static int64_t arr_sum_avx2(const int64_t *a, size_t n) {
  __m256i s0 = _mm256_setzero_si256(), s1 = s0, s2 = s0, s3 = s0;
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    s0 = _mm256_add_epi64(s0, _mm256_loadu_si256((const __m256i *)(const void *)(a + i)));
    s1 = _mm256_add_epi64(s1, _mm256_loadu_si256((const __m256i *)(const void *)(a + i + 4)));
    s2 = _mm256_add_epi64(s2, _mm256_loadu_si256((const __m256i *)(const void *)(a + i + 8)));
    s3 = _mm256_add_epi64(s3, _mm256_loadu_si256((const __m256i *)(const void *)(a + i + 12)));
  }
  __m256i s = _mm256_add_epi64(_mm256_add_epi64(s0, s1), _mm256_add_epi64(s2, s3));
  __m128i x = _mm_add_epi64(_mm256_castsi256_si128(s), _mm256_extracti128_si256(s, 1));
  int64_t r = _mm_cvtsi128_si64(x) + _mm_extract_epi64(x, 1);
  for (; i < n; i++) r += a[i];
  return r;
}

/**
 * Maximum of an int64 array, scalar.
 * @param a  elements
 * @param n  count (>= 1)
 * @returns the maximum.
 */
SCALAR static int64_t arr_max_scalar(const int64_t *a, size_t n) {
  int64_t m = a[0];
  for (size_t i = 1; i < n; i++) m = a[i] > m ? a[i] : m;
  return m;
}

/**
 * Maximum of an int64 array with AVX2: VPCMPGTQ + VPBLENDVB (AVX2 has no
 * 64-bit max instruction; AVX-512's VPMAXSQ would be one).
 * @param a  elements
 * @param n  count (>= 1)
 * @returns the maximum.
 */
__attribute__((target("avx2"))) KERNEL static int64_t arr_max_avx2(const int64_t *a, size_t n) {
  size_t i = 0;
  int64_t r = a[0];
  if (n >= 8) {
    __m256i m0 = _mm256_loadu_si256((const __m256i *)(const void *)a), m1 = _mm256_loadu_si256((const __m256i *)(const void *)(a + 4));
    for (i = 8; i + 8 <= n; i += 8) {
      __m256i x0 = _mm256_loadu_si256((const __m256i *)(const void *)(a + i));
      __m256i x1 = _mm256_loadu_si256((const __m256i *)(const void *)(a + i + 4));
      m0 = _mm256_blendv_epi8(m0, x0, _mm256_cmpgt_epi64(x0, m0));
      m1 = _mm256_blendv_epi8(m1, x1, _mm256_cmpgt_epi64(x1, m1));
    }
    m0 = _mm256_blendv_epi8(m0, m1, _mm256_cmpgt_epi64(m1, m0));
    int64_t l[4];
    _mm256_storeu_si256((__m256i *)(void *)l, m0);
    r = l[0];
    for (int k = 1; k < 4; k++) r = l[k] > r ? l[k] : r;
  }
  for (; i < n; i++) r = a[i] > r ? a[i] : r;
  return r;
}

/* ========================================================================= */
/* JSON: structural scan (scalar and AVX2 + PCLMULQDQ) and one path walker    */
/* ========================================================================= */

/*
 * A structural is a position the path walker needs: { } [ ] : , outside
 * strings, and every unescaped quote (opening and closing). Scalars
 * (numbers, true, false, null) produce none: their span is found between
 * the ':' and the next structural.
 */

/** The scalar tokenizer: one byte at a time, a string/escape state machine. */
typedef struct {
  const uint8_t *s;
  size_t n, i;
  int in_str;
} ScalarIter;

/**
 * Starts a scalar scan.
 * @param it  iterator
 * @param s   document
 * @param n   its length
 */
static inline void sc_init(ScalarIter *it, const uint8_t *s, size_t n) {
  it->s = s;
  it->n = n;
  it->i = 0;
  it->in_str = 0;
}

/**
 * The next structural, scalar.
 * @param it  iterator
 * @returns its position, or -1 at the end of the document.
 * @remarks Inside a string a backslash skips the byte after it, and only a
 *          quote ends the string; outside, the six structural bytes and a
 *          quote are reported. One branchy step per byte: what a fast
 *          non-SIMD tokenizer (jsmn-like) does.
 */
static inline long sc_next(ScalarIter *it) {
  while (it->i < it->n) {
    uint8_t c = it->s[it->i++];
    if (it->in_str) {
      if (c == '\\') it->i++;
      else if (c == '"') {
        it->in_str = 0;
        return (long)it->i - 1;
      }
      continue;
    }
    switch (c) {
      case '"': it->in_str = 1; return (long)it->i - 1;
      case '{': case '}': case '[': case ']': case ':': case ',': return (long)it->i - 1;
      default: break;
    }
  }
  return -1;
}

/** The SIMD tokenizer: 64 bytes per step, structurals as a bit mask. */
typedef struct {
  const uint8_t *s;
  size_t n, base;
  uint64_t mask;          /* structurals of the current block not yet returned */
  uint64_t prev_in_str;   /* all ones when the previous block ended inside a string */
  uint64_t prev_escaped;  /* 1 when the previous block's last backslash escapes this block's first byte */
} SimdIter;

/**
 * Starts a SIMD scan.
 * @param it  iterator
 * @param s   document (only its n bytes are read; the last partial block
 *            is copied to a space-padded buffer first)
 * @param n   its length
 */
static inline void si_init(SimdIter *it, const uint8_t *s, size_t n) {
  it->s = s;
  it->n = n;
  it->base = (size_t)-64;
  it->mask = 0;
  it->prev_in_str = 0;
  it->prev_escaped = 0;
}

/**
 * Bytes of 64 equal to c, as a bit mask.
 * @param lo  bytes 0-31   @param hi  bytes 32-63   @param c  the byte
 * @returns bit i set when byte i equals c.
 */
__attribute__((target("avx2"))) static inline uint64_t eq64(__m256i lo, __m256i hi, char c) {
  __m256i v = _mm256_set1_epi8(c);
  uint64_t a = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(lo, v));
  uint64_t b = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hi, v));
  return a | b << 32;
}

/**
 * Which bytes of a block a backslash escapes (simdjson's branchless
 * find_escaped, Langdale and Lemire 2019).
 * @param backslash     the block's backslash mask
 * @param prev_escaped  in: 1 when the first byte is escaped by the previous
 *                      block; out: the same for the next block
 * @returns the mask of escaped bytes.
 * @remarks A run of backslashes escapes every second byte from its start:
 *          runs starting on even bits flip an alternating mask; the carry
 *          out of the add says an odd run reaches past the block's end.
 */
static inline uint64_t find_escaped(uint64_t backslash, uint64_t *prev_escaped) {
  backslash &= ~*prev_escaped;
  uint64_t follows_escape = backslash << 1 | *prev_escaped;
  const uint64_t even_bits = 0x5555555555555555ull;
  uint64_t odd_sequence_starts = backslash & ~even_bits & ~follows_escape;
  uint64_t seq_even;
  *prev_escaped = __builtin_add_overflow(odd_sequence_starts, backslash, &seq_even);
  uint64_t invert_mask = seq_even << 1;
  return (even_bits ^ invert_mask) & follows_escape;
}

/**
 * The structurals of one 64-byte block.
 * @param it  iterator (its string and escape carries are updated)
 * @param p   the block (64 readable bytes)
 * @returns the structural mask.
 * @remarks Six byte compares per 32 bytes: quote, backslash, ':' and ','
 *          directly, and the four brackets as two compares after OR 0x20
 *          ('[' | 0x20 = '{', ']' | 0x20 = '}'). Inside-string bytes come
 *          from a prefix XOR of the unescaped quotes, one carry-less
 *          multiply by all ones (PCLMULQDQ); that is simdjson's trick.
 */
__attribute__((target("avx2,pclmul"))) static inline uint64_t block_structurals(SimdIter *it, const uint8_t *p) {
  __m256i lo = _mm256_loadu_si256((const __m256i *)(const void *)p);
  __m256i hi = _mm256_loadu_si256((const __m256i *)(const void *)(p + 32));
  uint64_t bs = eq64(lo, hi, '\\'), qt = eq64(lo, hi, '"');
  __m256i lo20 = _mm256_or_si256(lo, _mm256_set1_epi8(0x20)), hi20 = _mm256_or_si256(hi, _mm256_set1_epi8(0x20));
  uint64_t op = eq64(lo20, hi20, '{') | eq64(lo20, hi20, '}') | eq64(lo, hi, ':') | eq64(lo, hi, ',');
  if (bs | it->prev_escaped) qt &= ~find_escaped(bs, &it->prev_escaped);
  uint64_t in_str = (uint64_t)_mm_cvtsi128_si64(
                        _mm_clmulepi64_si128(_mm_set_epi64x(0, (long long)qt), _mm_set1_epi8(-1), 0)) ^
                    it->prev_in_str;
  it->prev_in_str = (uint64_t)((int64_t)in_str >> 63);
  return (op & ~in_str) | qt;
}

/**
 * The next structural, SIMD.
 * @param it  iterator
 * @returns its position, or -1 at the end of the document.
 * @remarks Pulls 64-byte blocks only as the walker needs them, so a field
 *          near the start costs only the blocks up to it (early exit). The
 *          last partial block goes through a space-padded copy: never a read
 *          past the value, which may end at a page boundary.
 */
__attribute__((target("avx2,pclmul,bmi"))) static inline long si_next(SimdIter *it) {
  while (!it->mask) {
    it->base += 64;
    if (it->base >= it->n) return -1;
    if (it->n - it->base >= 64) {
      it->mask = block_structurals(it, it->s + it->base);
    } else {
      uint8_t tmp[64];
      memset(tmp, ' ', sizeof tmp);
      memcpy(tmp, it->s + it->base, it->n - it->base);
      it->mask = block_structurals(it, tmp);
    }
  }
  long p = (long)(it->base + _tzcnt_u64(it->mask));
  it->mask = _blsr_u64(it->mask);
  return p;
}

/** A path query: object keys from the root, e.g. {"stats", "likes"}. */
typedef struct {
  const char *key[4];
  size_t len[4];
  int depth;
} PathQ;

/** A value's span in the document: [start, end), start = -1 when absent. */
typedef struct {
  long start, end;
} Span;

/**
 * Whether a byte is JSON whitespace.
 * @param c  the byte
 * @returns 1 for space, tab, CR or LF.
 */
static inline int jws(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/*
 * The path walker, generated for each tokenizer so its NEXT call inlines.
 * It follows object members from the root: a key string (two quote
 * structurals), ':', then the value: an object or array (skipped by depth
 * counting over structurals, or descended into when the key matches and the
 * path goes deeper), a string (two quotes), or a scalar (no structurals: its
 * span lies between ':' and the next ',' or '}'). It returns as soon as the
 * target value is complete; an absent key reads the whole object.
 */
#define DEFINE_WALK(NAME, ITER, INIT, NEXT, ATTR)                                                  \
  ATTR static Span NAME(const uint8_t *s, size_t n, const PathQ *q) {                               \
    ITER it;                                                                                       \
    INIT(&it, s, n);                                                                               \
    Span none = {-1, -1};                                                                          \
    long p = NEXT(&it);                                                                            \
    if (p < 0 || s[p] != '{') return none;                                                         \
    int level = 0;                                                                                 \
    for (;;) {                                                                                     \
      p = NEXT(&it);                                                                               \
      if (p < 0 || s[p] != '"') return none;                                                       \
      long ks = p + 1, ke = NEXT(&it), colon = NEXT(&it);                                          \
      if (ke < 0 || colon < 0 || s[colon] != ':') return none;                                     \
      int match = (size_t)(ke - ks) == q->len[level] && !memcmp(s + ks, q->key[level], q->len[level]); \
      long v = NEXT(&it), after;                                                                   \
      if (v < 0) return none;                                                                      \
      if (match && level + 1 < q->depth) {                                                         \
        if (s[v] != '{') return none;                                                              \
        level++;                                                                                   \
        continue;                                                                                  \
      }                                                                                            \
      if (s[v] == '{' || s[v] == '[') {                                                            \
        int d = 1;                                                                                 \
        long x = v;                                                                                \
        while (d > 0) {                                                                            \
          x = NEXT(&it);                                                                           \
          if (x < 0) return none;                                                                  \
          d += (s[x] == '{' || s[x] == '[') - (s[x] == '}' || s[x] == ']');                         \
        }                                                                                          \
        if (match) return (Span){v, x + 1};                                                        \
        after = NEXT(&it);                                                                         \
      } else if (s[v] == '"') {                                                                    \
        long close = NEXT(&it);                                                                    \
        if (close < 0) return none;                                                                \
        if (match) return (Span){v, close + 1};                                                    \
        after = NEXT(&it);                                                                         \
      } else {                                                                                     \
        after = v;                                                                                 \
        if (match) {                                                                               \
          long a = colon + 1, b = v;                                                               \
          while (a < b && jws(s[a])) a++;                                                          \
          while (b > a && jws(s[b - 1])) b--;                                                      \
          return (Span){a, b};                                                                     \
        }                                                                                          \
      }                                                                                            \
      if (after < 0 || s[after] != ',') return none;                                               \
    }                                                                                              \
  }

DEFINE_WALK(walk_scalar, ScalarIter, sc_init, sc_next, KERNEL)
DEFINE_WALK(walk_simd, SimdIter, si_init, si_next, __attribute__((target("avx2,pclmul,bmi"))) KERNEL)

/**
 * Counts every structural of a document, scalar (stage 1 alone).
 * @param s  document   @param n  its length
 * @returns the count.
 */
KERNEL static uint64_t count_scalar(const uint8_t *s, size_t n) {
  ScalarIter it;
  sc_init(&it, s, n);
  uint64_t c = 0;
  while (sc_next(&it) >= 0) c++;
  return c;
}

/**
 * Counts every structural of a document, SIMD (stage 1 alone).
 * @param s  document   @param n  its length
 * @returns the count.
 */
__attribute__((target("avx2,pclmul,bmi"))) KERNEL static uint64_t count_simd(const uint8_t *s, size_t n) {
  SimdIter it;
  si_init(&it, s, n);
  uint64_t c = 0;
  while (si_next(&it) >= 0) c++;
  return c;
}

/** A growing output buffer for the document generator. */
typedef struct {
  char *p;
  size_t n, cap;
} Buf;

/**
 * Appends bytes.
 * @param b  buffer   @param s  bytes   @param n  count
 */
static void bput(Buf *b, const char *s, size_t n) {
  if (b->n + n > b->cap) die("json generator buffer");
  memcpy(b->p + b->n, s, n);
  b->n += n;
}

/**
 * Appends a NUL-terminated string.
 * @param b  buffer   @param s  string
 */
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }

/**
 * Appends an unsigned decimal.
 * @param b  buffer   @param v  value
 */
static void bputu(Buf *b, uint64_t v) {
  char t[24];
  int k = snprintf(t, sizeof t, "%llu", (unsigned long long)v);
  bput(b, t, (size_t)k);
}

/**
 * Appends a separator with random whitespace after it (JSON allows any).
 * @param b    buffer
 * @param sep  ":" or ","
 * @param r    random state
 */
static void bsep(Buf *b, const char *sep, uint64_t *r) {
  bputs(b, sep);
  if (rnd(r) & 1) bputs(b, " ");
}

/** What the generator put at the queried paths, to check extractions against. */
typedef struct {
  char plan[16], zip[16], likes[24], score[24];
} JsonTruth;

/* Text fragments for strings: escapes, escaped backslashes, structural bytes inside strings. */
static const char *const kFrag[] = {
    "word ", "He said \\\"hi\\\" ", "C:\\\\temp\\\\ ", "\\\\\\\"x\\\\\\\" ", "caf\\u00e9 ", "a{b}[c]:d,e ",
    "tab\\tnew\\nline ", "x ",
};

/**
 * Generates one user document: nested objects, arrays, numbers, booleans
 * and strings full of escapes and bracket bytes, with random whitespace.
 * @param b      output (reset by the caller)
 * @param id     document id
 * @param tags   tags in the "tags" array
 * @param hist   entries in the "history" array (each a nested object)
 * @param bio    fragments in the "bio" string
 * @param r      random state
 * @param truth  receives the values at the four queried paths
 * @remarks "bio" ends in an escaped backslash, so its closing quote follows
 *          a backslash: the case escape handling most often gets wrong.
 */
static void gen_doc(Buf *b, uint32_t id, int tags, int hist, int bio, uint64_t *r, JsonTruth *truth) {
  static const char *const plans[3] = {"free", "pro", "team"};
  snprintf(truth->plan, sizeof truth->plan, "\"%s\"", plans[id % 3]);
  snprintf(truth->zip, sizeof truth->zip, "\"%05u\"", (unsigned)(rnd(r) % 100000));
  snprintf(truth->likes, sizeof truth->likes, "%llu", (unsigned long long)(rnd(r) % 1000000));
  snprintf(truth->score, sizeof truth->score, "%u.%02u", (unsigned)(rnd(r) % 100), (unsigned)(rnd(r) % 100));
  bputs(b, "{\"id\"");
  bsep(b, ":", r);
  bputu(b, id);
  bsep(b, ",", r);
  bputs(b, "\"name\":\"");
  for (int i = 0; i < 3; i++) bputs(b, kSyl[rnd(r) % 20]);
  bputs(b, "\"");
  bsep(b, ",", r);
  bputs(b, "\"email\":\"u");
  bputu(b, id);
  bputs(b, "@example.com\",\"plan\"");
  bsep(b, ":", r);
  bputs(b, truth->plan);
  bsep(b, ",", r);
  bputs(b, "\"verified\":true,\"address\":{\"street\":\"");
  bputu(b, rnd(r) % 999);
  bputs(b, " Main St\",\"city\":\"Haifa\",\"zip\"");
  bsep(b, ":", r);
  bputs(b, truth->zip);
  bputs(b, ",\"geo\":[32.79,34.98]},\"tags\":[");
  for (int i = 0; i < tags; i++) {
    if (i) bsep(b, ",", r);
    bputs(b, "\"t");
    bputu(b, rnd(r) % 1000);
    bputs(b, "\"");
  }
  bputs(b, "],\"bio\":\"");
  for (int i = 0; i < bio; i++) bputs(b, kFrag[rnd(r) % (sizeof kFrag / sizeof kFrag[0])]);
  bputs(b, "end\\\\\",\"history\":[");
  for (int i = 0; i < hist; i++) {
    if (i) bsep(b, ",", r);
    bputs(b, "{\"t\":");
    bputu(b, 1700000000 + rnd(r) % 100000000);
    bputs(b, ",\"e\":\"login\",\"meta\":{\"ip\":\"10.0.");
    bputu(b, rnd(r) % 256);
    bputs(b, ".");
    bputu(b, rnd(r) % 256);
    bputs(b, "\",\"ok\":true,\"note\":\"}{][\"}}");
  }
  bputs(b, "],\"stats\":{\"views\":");
  bputu(b, rnd(r) % 10000000);
  bputs(b, ",\"likes\"");
  bsep(b, ":", r);
  bputs(b, truth->likes);
  bputs(b, " ,\"followers\":");
  bputu(b, rnd(r) % 100000);
  bputs(b, "},\"score\"");
  bsep(b, ":", r);
  bputs(b, truth->score);
  bputs(b, "}");
}

/* ========================================================================= */
/* SHA-256: scalar reference and SHA-NI                                       */
/* ========================================================================= */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/**
 * Rotate a 32-bit word right.
 * @param x  word   @param n  bits (1..31)
 * @returns the rotated word.
 */
static inline uint32_t ror32(uint32_t x, int n) { return x >> n | x << (32 - n); }

/**
 * SHA-256 compression of whole blocks, scalar (FIPS 180-4).
 * @param st  the eight state words, updated
 * @param p   blocks
 * @param nb  block count (64 bytes each)
 */
SCALAR static void sha256_blocks_ref(uint32_t st[8], const uint8_t *p, size_t nb) {
  for (; nb; nb--, p += 64) {
    uint32_t w[64], a = st[0], b = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
      uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
    st[4] += e;
    st[5] += f;
    st[6] += g;
    st[7] += h;
  }
}

/**
 * SHA-256 compression of whole blocks with the SHA-NI instructions.
 * @param st  the eight state words, updated
 * @param p   blocks
 * @param nb  block count (64 bytes each)
 * @remarks SHA256RNDS2 runs two rounds on the state held as (ABEF, CDGH);
 *          SHA256MSG1/MSG2 extend the message schedule four words at a
 *          time from a ring of the last four groups. The state shuffles in
 *          and out follow Intel's reference. Verified against FIPS 180-4
 *          vectors and the scalar code.
 */
__attribute__((target("sha,sse4.1,ssse3"))) KERNEL static void sha256_blocks_ni(uint32_t st[8], const uint8_t *p,
                                                                              size_t nb) {
  const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bll, 0x0405060700010203ll);
  __m128i TMP = _mm_loadu_si128((const __m128i *)(const void *)&st[0]);
  __m128i STATE1 = _mm_loadu_si128((const __m128i *)(const void *)&st[4]);
  TMP = _mm_shuffle_epi32(TMP, 0xB1);
  STATE1 = _mm_shuffle_epi32(STATE1, 0x1B);
  __m128i STATE0 = _mm_alignr_epi8(TMP, STATE1, 8);
  STATE1 = _mm_blend_epi16(STATE1, TMP, 0xF0);
  for (; nb; nb--, p += 64) {
    __m128i ABEF = STATE0, CDGH = STATE1, M[4], msg;
    for (int g = 0; g < 16; g++) {
      if (g < 4) {
        M[g] = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(const void *)(p + 16 * g)), MASK);
      } else {   /* W[g] from W[g-4] (M[g&3]), W[g-3], W[g-2], W[g-1] */
        __m128i t = _mm_sha256msg1_epu32(M[g & 3], M[(g + 1) & 3]);
        t = _mm_add_epi32(t, _mm_alignr_epi8(M[(g + 3) & 3], M[(g + 2) & 3], 4));
        M[g & 3] = _mm_sha256msg2_epu32(t, M[(g + 3) & 3]);
      }
      msg = _mm_add_epi32(M[g & 3], _mm_loadu_si128((const __m128i *)(const void *)&K256[4 * g]));
      STATE1 = _mm_sha256rnds2_epu32(STATE1, STATE0, msg);
      msg = _mm_shuffle_epi32(msg, 0x0E);
      STATE0 = _mm_sha256rnds2_epu32(STATE0, STATE1, msg);
    }
    STATE0 = _mm_add_epi32(STATE0, ABEF);
    STATE1 = _mm_add_epi32(STATE1, CDGH);
  }
  TMP = _mm_shuffle_epi32(STATE0, 0x1B);
  STATE1 = _mm_shuffle_epi32(STATE1, 0xB1);
  STATE0 = _mm_blend_epi16(TMP, STATE1, 0xF0);
  STATE1 = _mm_alignr_epi8(STATE1, TMP, 8);
  _mm_storeu_si128((__m128i *)(void *)&st[0], STATE0);
  _mm_storeu_si128((__m128i *)(void *)&st[4], STATE1);
}

/**
 * SHA-256 of a message.
 * @param m    message
 * @param len  length
 * @param out  receives the 32-byte digest
 * @param ni   1 = SHA-NI blocks, 0 = scalar blocks
 * @remarks Padding: 0x80, zeros, then the bit length big-endian, in one or
 *          two final blocks.
 */
static void sha256(const uint8_t *m, size_t len, uint8_t out[32], int ni) {
  uint32_t st[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t full = len / 64;
  if (ni) sha256_blocks_ni(st, m, full);
  else sha256_blocks_ref(st, m, full);
  uint8_t tail[128];
  size_t r = len - full * 64, tl = r + 9 <= 64 ? 64 : 128;
  memset(tail, 0, sizeof tail);
  memcpy(tail, m + full * 64, r);
  tail[r] = 0x80;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
  if (ni) sha256_blocks_ni(st, tail, tl / 64);
  else sha256_blocks_ref(st, tail, tl / 64);
  for (int i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(st[i] >> 24);
    out[4 * i + 1] = (uint8_t)(st[i] >> 16);
    out[4 * i + 2] = (uint8_t)(st[i] >> 8);
    out[4 * i + 3] = (uint8_t)st[i];
  }
}

/**
 * Formats a digest as lowercase hex.
 * @param d    32 bytes
 * @param hex  receives 64 characters and a NUL
 */
static void hex32(const uint8_t d[32], char hex[65]) {
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
}

/* ========================================================================= */
/* Substring search over values                                               */
/* ========================================================================= */

/**
 * Occurrences of a needle, with glibc memmem (the reference).
 * @param s  haystack   @param n  its length
 * @param k  needle     @param kn its length (>= 2)
 * @returns the number of start positions where the needle occurs.
 */
KERNEL static uint64_t count_memmem(const uint8_t *s, size_t n, const uint8_t *k, size_t kn) {
  uint64_t c = 0;
  const uint8_t *p = s, *end = s + n;
  while (p < end) {
    const uint8_t *q = memmem(p, (size_t)(end - p), k, kn);
    if (!q) break;
    c++;
    p = q + 1;
  }
  return c;
}

/**
 * Occurrences of a needle with AVX2 (Muła's first/last-byte filter): 32
 * candidate positions per step where both the first and the last byte of
 * the needle match, then an exact compare of each candidate.
 * @param s  haystack   @param n  its length
 * @param k  needle     @param kn its length (>= 2)
 * @returns the number of start positions where the needle occurs.
 * @remarks Two loads, two compares and an AND per 32 positions; the last
 *          positions are checked one by one, so nothing is read past n.
 */
__attribute__((target("avx2,bmi"))) KERNEL static uint64_t count_avx2(const uint8_t *s, size_t n, const uint8_t *k,
                                                                      size_t kn) {
  const __m256i f = _mm256_set1_epi8((char)k[0]), l = _mm256_set1_epi8((char)k[kn - 1]);
  uint64_t c = 0;
  size_t i = 0;
  for (; i + kn - 1 + 32 <= n; i += 32) {
    __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
    __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(s + i + kn - 1));
    uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(a, f), _mm256_cmpeq_epi8(b, l)));
    for (; m; m = _blsr_u32(m)) c += !memcmp(s + i + _tzcnt_u32(m) + 1, k + 1, kn - 2);
  }
  for (; i + kn <= n; i++) c += s[i] == k[0] && !memcmp(s + i + 1, k + 1, kn - 1);
  return c;
}

/* ========================================================================= */
/* Driver                                                                     */
/* ========================================================================= */

/**
 * Builds every structure, verifies each against its reference, and prints
 * every measurement against the main store's exact-get bars.
 * @param argc  argument count
 * @param argv  [--keys N] [--cpu C] [--reps R]
 * @returns 0; exits 1 on a failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  int cpu = 4, reps = 5;
  g_n = 10000000;
  for (int i = 1; i < argc; i++) {
    if (i + 1 < argc && !strcmp(argv[i], "--keys")) g_n = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (i + 1 < argc && !strcmp(argv[i], "--cpu")) cpu = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--reps")) reps = atoi(argv[++i]);
    else g_n = 0;
  }
  if (g_n < 1000000 || g_n % FIELDS || g_n > 12000000 || reps < 1 || reps > MAX_REPS) {
    fprintf(stderr, "usage: endpoints_probe [--keys N (multiple of 4, 1000000..12000000)] [--cpu C] [--reps 1..%d]\n",
            MAX_REPS);
    return 2;
  }
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("bmi") || !__builtin_cpu_supports("sse4.2") ||
      !__builtin_cpu_supports("pclmul") || !__builtin_cpu_supports("sha"))
    die("this probe needs AVX2, BMI1, SSE4.2, PCLMULQDQ and SHA-NI");
  pin(cpu);
  uint64_t t[MAX_REPS], ta[MAX_REPS], tb[MAX_REPS];
  char note[192];

  /* ---- the main store: n keys, the server's defaults ---- */
  if (kv_set_scan_mode(KV_SCAN_SSE2) < 0) die("kv_set_scan_mode");
  if (kv_crc32c_set_mode(kv_crc32c_best_mode()) < 0) die("kv_crc32c_set_mode");
  g_store = kv_store_create(g_n, MAX_THREADS + 1, kSeed, 0);
  if (!g_store) die("kv_store_create");
  g_start = xcalloc(((size_t)g_n + 1) * sizeof *g_start);
  g_arena = xcalloc((size_t)g_n * 21 + 64);
  uint64_t t0 = now_ns(), expect_sig = 0;
  {
    uint8_t val[VAL_LEN];
    uint32_t off = 0;
    for (uint32_t i = 0; i < g_n; i++) {
      g_start[i] = off;
      int len = make_key(i, g_arena + off);
      uint64_t h[2];
      for (int j = 0; j < VAL_LEN; j++) val[j] = (uint8_t)(i * 31u + (uint32_t)j);
      kv_store_hash(g_store, g_arena + off, (size_t)len, h);
      if (kv_store_put(g_store, g_arena + off, (size_t)len, val, VAL_LEN, h[0], h[1]) != 1) die("fill put");
      expect_sig += key_sig(g_arena + off, (uint32_t)len);
      off += (uint32_t)len;
    }
    g_start[g_n] = off;
  }
  printf("main store: %u keys of 20-21 bytes, 100-byte values; fill %.1f s; cpu %d, reps %d; crc32c mode %d\n", g_n,
         (double)(now_ns() - t0) / 1e9, cpu, reps, kv_crc32c_best_mode());
  kv_qsbr_online(g_store, 0);
  g_q = make_stream(g_n, 0x243F6A8885A308D3ull, NULL);

  /* ================= the bar, and the layout endpoints ================= */
  printf("\nTHE BAR: exact gets on the main store (x = ratio to the bar of the same regime; GOLD = within 10x)\n");
  for (int r = 0; r < reps; r++) t[r] = bar_dependent();
  g_bar[BAR_DEP] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = bar_independent();
  g_bar[BAR_IND] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = bar_batched(DO_GET, NULL, NULL);
  g_bar[BAR_BAT] = med(t, reps, NQ);
  row("get (100-byte value)", "dependent chain", g_bar[BAR_DEP], BAR_DEP, "bar D");
  row("", "independent loop", g_bar[BAR_IND], BAR_IND, "bar I");
  row("", "batched pipeline (the server's GET path)", g_bar[BAR_BAT], BAR_BAT, "bar B");
  printf("BARS dep=%.1f ind=%.1f bat=%.1f\n", g_bar[BAR_DEP], g_bar[BAR_IND], g_bar[BAR_BAT]);
  for (int r = 0; r < reps; r++) {   /* get / stat / range alternate: paired */
    ta[r] = bar_batched(DO_STAT, NULL, NULL);
    tb[r] = bar_batched(DO_RANGE, NULL, NULL);
    t[r] = bar_batched(DO_GET, NULL, NULL);
  }
  double g0 = med(t, reps, NQ);
  snprintf(note, sizeof note, "%.2fx the get beside it (%.1f ns)", med(ta, reps, NQ) / g0, g0);
  row("Stat (lengths, no value)", "batched; reads the record header only", med(ta, reps, NQ), BAR_BAT, note);
  snprintf(note, sizeof note, "%.2fx the get beside it", med(tb, reps, NQ) / g0);
  row("GetRange (16 bytes at offset 40)", "batched; copies the slice only", med(tb, reps, NQ), BAR_BAT, note);

  /* ================= ETag: CRC32C on the fly ================= */
  printf("\nETAG: CRC32C of values with the server's crc32c.asm (folding VPCLMULQDQ vs the SSE4.2 instruction)\n");
  {
    uint8_t *buf = xcalloc(1 << 16);
    uint64_t seed = 1234;
    for (int i = 0; i < 1 << 16; i++) buf[i] = (uint8_t)rnd(&seed);
    for (int i = 0; i < 2000; i++) {   /* the assembly == a bitwise reference */
      size_t len = (size_t)(rnd(&seed) % 5000), off = (size_t)(rnd(&seed) % 1000);
      uint32_t c = 0xFFFFFFFFu;
      for (size_t j = 0; j < len; j++) {
        c ^= buf[off + j];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1)));
      }
      if (kv_crc32c(0, buf + off, len) != ~c) die("kv_crc32c disagrees with the bitwise reference");
    }
    static const size_t sizes[4] = {100, 1024, 4096, 65536};
    for (int mode = 0; mode < 2; mode++) {
      int m = mode == 0 ? kv_crc32c_best_mode() : 1;
      if (kv_crc32c_set_mode(m) < 0) die("kv_crc32c_set_mode");
      for (int si = 0; si < 4; si++) {
        uint32_t iters = (uint32_t)(4000000 / (sizes[si] / 64 + 1));
        uint64_t acc = 0;
        for (int r = 0; r < reps; r++) {
          uint64_t s = now_ns();
          for (uint32_t i = 0; i < iters; i++, clobber()) acc += kv_crc32c(0, buf, sizes[si]);
          t[r] = now_ns() - s;
        }
        g_vsink += acc;
        double ns = med(t, reps, iters);
        char what[48];
        snprintf(what, sizeof what, "CRC32C of %zu bytes", sizes[si]);
        snprintf(note, sizeof note, "%.1f GB/s", (double)sizes[si] / ns);
        row(what, m == 1 ? "SSE4.2 crc32 chain" : "best mode (VPCLMULQDQ folding)", ns, BAR_BAT,
            note);
      }
    }
    if (kv_crc32c_set_mode(kv_crc32c_best_mode()) < 0) die("kv_crc32c_set_mode");
    /* GetIfChanged: the client sends the CRC it holds; equal => "not modified", no value bytes. */
    uint32_t *etag_same = xcalloc((size_t)NQ * 4), *etag_old = xcalloc((size_t)NQ * 4);
    for (uint32_t i = 0; i < NQ; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, g_q.key[i], g_q.len[i], h);
      const KvRec *r = kv_store_get(g_store, g_q.key[i], g_q.len[i], h[0], h[1]);
      etag_same[i] = kv_crc32c(0, kv_rec_value(r), r->val_len);
      etag_old[i] = etag_same[i] ^ 1;
    }
    uint64_t changed = 0;
    for (int r = 0; r < reps; r++) {   /* unchanged / changed / plain get alternate: paired */
      ta[r] = bar_batched(DO_ETAG, etag_same, &changed);
      tb[r] = bar_batched(DO_ETAG, etag_old, NULL);
      t[r] = bar_batched(DO_GET, NULL, NULL);
    }
    if (changed != 0) die("an unchanged value reported a different CRC");
    g0 = med(t, reps, NQ);
    snprintf(note, sizeof note, "%.2fx the get beside it; 0 value bytes sent", med(ta, reps, NQ) / g0);
    row("GetIfChanged, value unchanged", "batched + CRC32C of the 100-byte value", med(ta, reps, NQ), BAR_BAT, note);
    snprintf(note, sizeof note, "%.2fx the get beside it", med(tb, reps, NQ) / g0);
    row("GetIfChanged, value changed", "batched + CRC32C + the value copy", med(tb, reps, NQ), BAR_BAT, note);
    free(etag_same);
    free(etag_old);
    free(buf);
  }

  /* ================= consistent multi-key reads ================= */
  printf("\nSNAPSHOT: %d keys read as one consistent snapshot (double collect over bucket versions)\n", GROUP);
  {
    /* cost: groups of 16 random main-store keys, plain vs validated (no writer) */
    enum { NG = NQ / GROUP };
    uint64_t tries = 0, sum = 0, seqs[GROUP];
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t g = 0; g < NG; g++) {
        uint64_t h[GROUP][2];
        uint8_t keys[GROUP][KEYMAX];
        uint32_t lens[GROUP];
        for (int k = 0; k < GROUP; k++) {
          uint32_t qi = g * GROUP + (uint32_t)k;
          lens[k] = g_q.len[qi];
          memcpy(keys[k], g_q.key[qi], KEYMAX);
          kv_store_hash(g_store, keys[k], lens[k], h[k]);
          kv_store_prefetch(g_store, h[k][0], h[k][1]);
        }
        for (int k = 0; k < GROUP; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
        read_plain(g_store, keys, lens, h, seqs, 0);
        sum += seqs[0];
      }
      ta[r] = now_ns() - s;
      s = now_ns();
      for (uint32_t g = 0; g < NG; g++) {
        uint64_t h[GROUP][2];
        uint8_t keys[GROUP][KEYMAX];
        uint32_t lens[GROUP];
        for (int k = 0; k < GROUP; k++) {
          uint32_t qi = g * GROUP + (uint32_t)k;
          lens[k] = g_q.len[qi];
          memcpy(keys[k], g_q.key[qi], KEYMAX);
          kv_store_hash(g_store, keys[k], lens[k], h[k]);
          kv_store_prefetch(g_store, h[k][0], h[k][1]);
        }
        for (int k = 0; k < GROUP; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
        read_snapshot(g_store, keys, lens, h, seqs, &tries, 0);
        sum += seqs[0];
      }
      tb[r] = now_ns() - s;
    }
    g_vsink += sum;
    double plain = med(ta, reps, NQ), snap = med(tb, reps, NQ);
    row("GetMany of 16 keys, per key", "plain (today's GetMany)", plain, BAR_BAT, "batched pipeline");
    snprintf(note, sizeof note, "%.2fx plain; %.3f attempts per snapshot", snap / plain,
             (double)tries / (double)(NG * (uint32_t)reps));
    row("", "consistent snapshot (versions re-checked)", snap, BAR_BAT, note);

    /* the race: a writer rewrites 16 keys in order; count torn reads */
    g_grp = kv_store_create(64, 4, kSeed, 0);
    if (!g_grp) die("kv_store_create (group)");
    uint8_t val[VAL_LEN];
    memset(val, 0, sizeof val);
    uint64_t one = 1;
    memcpy(val, &one, 8);
    for (int k = 0; k < GROUP; k++) {
      g_glens[k] = (uint32_t)snprintf((char *)g_gkeys[k], KEYMAX, "acct:%02d:balance", k);
      kv_store_hash(g_grp, g_gkeys[k], g_glens[k], g_gh[k]);
      if (kv_store_put(g_grp, g_gkeys[k], g_glens[k], val, VAL_LEN, g_gh[k][0], g_gh[k][1]) != 1) die("group fill");
    }
    pin(3);
    kv_qsbr_online(g_grp, 1);
    atomic_store(&g_stop, 0);
    pthread_t w;
    if (pthread_create(&w, NULL, race_writer, NULL) != 0) die("pthread_create");
    enum { RACE = 200000 };
    uint64_t torn_plain = 0, torn_snap = 0, race_tries = 0, s0 = now_ns();
    for (int i = 0; i < RACE; i++) {
      read_plain(g_grp, g_gkeys, g_glens, g_gh, seqs, RACE_READ_DELAY);
      torn_plain += !is_snapshot(seqs);
      kv_qsbr_quiescent(g_grp, 1);
    }
    double tplain = (double)(now_ns() - s0) / RACE;
    s0 = now_ns();
    for (int i = 0; i < RACE; i++) {
      read_snapshot(g_grp, g_gkeys, g_glens, g_gh, seqs, &race_tries, RACE_READ_DELAY);
      torn_snap += !is_snapshot(seqs);
      kv_qsbr_quiescent(g_grp, 1);
    }
    double tsnap = (double)(now_ns() - s0) / RACE;
    atomic_store(&g_stop, 1);
    pthread_join(w, NULL);
    kv_qsbr_offline(g_grp, 1);
    if (torn_snap != 0) die("a validated snapshot was torn");
    if (torn_plain == 0) die("the race never tore a plain read: the test proves nothing");
    snprintf(note, sizeof note, "%llu of %d reads torn", (unsigned long long)torn_plain, RACE);
    row("16 keys under a racing writer", "plain reads (~0.1 us per key, as cold keys)", tplain, BAR_NONE, note);
    snprintf(note, sizeof note, "0 of %d torn; %.2f attempts per read", RACE, (double)race_tries / RACE);
    row("", "validated snapshots", tsnap, BAR_NONE, note);
    kv_store_destroy(g_grp);
    pin(cpu);
  }

  /* ================= unordered scan ================= */
  printf("\nSCAN: every key by bucket cursor, no index (the table is %llu buckets of 64 bytes)\n",
         (unsigned long long)(((const TableHead *)(const void *)g_store)->mask + 1));
  {
    const TableHead *th = (const TableHead *)(const void *)g_store;
    uint64_t nb = th->mask + 1;
    ScanOut o;
    scan_range(th, 0, nb, 0, &o);
    if (o.count != g_n || o.sig != expect_sig) die("the scan did not see every key exactly once");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      scan_range(th, 0, nb, 0, &o);
      t[r] = now_ns() - s;
    }
    double per = med(t, reps, g_n);
    snprintf(note, sizeof note, "%.1f M keys/s; checked: all %u keys once", 1e3 / per, g_n);
    row("Iterate keys (1 thread)", "bucket order, records prefetched 8 ahead", per, BAR_BAT, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      scan_range(th, 0, nb, 1, &o);
      t[r] = now_ns() - s;
    }
    per = med(t, reps, g_n);
    snprintf(note, sizeof note, "%.1f M keys/s, %.2f GB/s of values", 1e3 / per, (double)o.bytes / (per * g_n));
    row("Iterate keys + values (1 thread)", "the same, copying each 100-byte value", per, BAR_BAT, note);
    pthread_barrier_t start;
    pthread_t th7[MAX_THREADS];
    ScanJob jobs[MAX_THREADS];
    kv_qsbr_offline(g_store, 0);
    for (int r = 0; r < reps; r++) {
      pthread_barrier_init(&start, NULL, MAX_THREADS + 1);
      for (int i = 0; i < MAX_THREADS; i++) {
        jobs[i] = (ScanJob){g_tcpu[i], i + 1, nb * (uint64_t)i / MAX_THREADS, nb * (uint64_t)(i + 1) / MAX_THREADS,
                            {0, 0, 0}, &start};
        if (pthread_create(&th7[i], NULL, scan_thread, &jobs[i]) != 0) die("pthread_create");
      }
      pthread_barrier_wait(&start);
      uint64_t s = now_ns();
      uint64_t cnt = 0, sig = 0;
      for (int i = 0; i < MAX_THREADS; i++) {
        pthread_join(th7[i], NULL);
        cnt += jobs[i].out.count;
        sig += jobs[i].out.sig;
      }
      t[r] = now_ns() - s;
      pthread_barrier_destroy(&start);
      if (cnt != g_n || sig != expect_sig) die("the sharded scan did not see every key exactly once");
    }
    kv_qsbr_online(g_store, 0);
    per = med(t, reps, g_n);
    snprintf(note, sizeof note, "%.1f M keys/s; the whole table in %.0f ms", 1e3 / per, per * g_n / 1e6);
    row("ScanShard keys (7 shards, 7 threads)", "one bucket range per thread", per, BAR_BAT, note);
  }

  /* ================= in-place cells ================= */
  printf("\nCELLS: max registers, 64-flag words and counter arrays, in place\n");
  {
    enum { CN = 1000000, AN = 200000 };
    KvTable *cs = kv_store_create(CN, MAX_THREADS + 1, kSeed, 0);
    if (!cs) die("kv_store_create (cells)");
    int64_t zero = 0;
    for (uint32_t i = 0; i < CN; i++) {
      uint64_t h[2];
      kv_store_hash(cs, akey(i), alen(i), h);
      if (kv_store_put(cs, akey(i), alen(i), &zero, 8, h[0], h[1]) != 1) die("cells fill");
    }
    uint32_t *ids = xcalloc((size_t)NQ * 4);
    Stream cq = make_stream(CN, 0x5151ull, ids);
    int64_t *arg = xcalloc((size_t)NQ * 8);
    uint64_t seed = 777;
    for (uint32_t i = 0; i < NQ; i++) arg[i] = (int64_t)(rnd(&seed) % 1000000);
    /* three passes per rep, alternating: XADD (increment), CMPXCHG max, LOCK OR flags */
    double res[3];
    const char *names[3] = {"increment (LOCK XADD), baseline", "max register (LOCK CMPXCHG loop)",
                            "flag word (LOCK OR of a 64-bit mask)"};
    for (int op = 0; op < 3; op++) {
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns(), h[BATCH][2];
        for (uint32_t b = 0; b < NQ; b += BATCH) {
          hash_prefetch_batch(cs, &cq, b, h);
          for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(cs, h[k][0], h[k][1]);
          for (int k = 0; k < BATCH; k++) {
            uint32_t qi = b + (uint32_t)k;
            const KvRec *rec = kv_store_get(cs, cq.key[qi], cq.len[qi], h[k][0], h[k][1]);
            _Atomic int64_t *c = (_Atomic int64_t *)(uintptr_t)kv_rec_value(rec);
            if (op == 0) atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
            else if (op == 1) cell_max(c, arg[qi]);
            else atomic_fetch_or_explicit(c, (int64_t)(1ull << (arg[qi] & 63)), memory_order_relaxed);
          }
        }
        t[r] = now_ns() - s;
      }
      res[op] = med(t, reps, NQ);
      /* exact: rebuild the expected cell of 1,000 sampled keys from the stream */
      if (op == 1) {
        for (uint32_t i = 0; i < 1000; i++) {
          int64_t want = 0;   /* the increments of op 0 ran first: reps x occurrences */
          for (uint32_t j = 0; j < NQ; j++) want += ids[j] == ids[i];
          want *= reps;
          for (uint32_t j = 0; j < NQ; j++)
            if (ids[j] == ids[i] && arg[j] > want) want = arg[j];
          uint64_t h[2];
          kv_store_hash(cs, cq.key[i], cq.len[i], h);
          int64_t got;
          memcpy(&got, kv_rec_value(kv_store_get(cs, cq.key[i], cq.len[i], h[0], h[1])), 8);
          if (got != want) die("max register is not exact");
        }
      }
      snprintf(note, sizeof note, "%.2fx the increment", res[op] / res[0]);
      row(op == 0 ? "1M 8-byte cells, batched" : "", names[op], res[op], BAR_BAT, op ? note : "exact (checked)");
    }
    kv_store_destroy(cs);
    free(arg);
    free(ids);
    free(cq.key);
    free(cq.len);

    /* counter arrays: 168 hourly counters per key */
    KvTable *as = kv_store_create(AN, MAX_THREADS + 1, kSeed, 0);
    if (!as) die("kv_store_create (arrays)");
    int64_t *arr = xcalloc(ARR * 8);
    for (uint32_t i = 0; i < AN; i++) {
      uint64_t h[2];
      for (int j = 0; j < ARR; j++) arr[j] = (int64_t)((i * 7919u + (uint32_t)j * 104729u) % 1000);
      kv_store_hash(as, akey(i), alen(i), h);
      if (kv_store_put(as, akey(i), alen(i), arr, ARR * 8, h[0], h[1]) != 1) die("arrays fill");
    }
    for (int i = 0; i < 1000; i++) {   /* kernels == scalar */
      for (int j = 0; j < ARR; j++) arr[j] = (int64_t)(rnd(&seed) % 2000001) - 1000000;
      if (arr_sum_avx2(arr, ARR) != arr_sum_scalar(arr, ARR) || arr_max_avx2(arr, ARR) != arr_max_scalar(arr, ARR))
        die("array kernels disagree");
    }
    uint64_t acc = 0;
    for (int kind = 0; kind < 4; kind++) {
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns();
        for (int i = 0; i < 1000000; i++, clobber()) {
          switch (kind) {
            case 0: acc += (uint64_t)arr_sum_scalar(arr, ARR); break;
            case 1: acc += (uint64_t)arr_sum_avx2(arr, ARR); break;
            case 2: acc += (uint64_t)arr_max_scalar(arr, ARR); break;
            default: acc += (uint64_t)arr_max_avx2(arr, ARR); break;
          }
        }
        t[r] = now_ns() - s;
      }
      static const char *const kn[4] = {"scalar", "AVX2 (4 accumulators)", "scalar", "AVX2 (VPCMPGTQ + VPBLENDVB)"};
      row(kind == 0 ? "sum of 168 int64 (in cache)" : kind == 2 ? "max of 168 int64 (in cache)" : "", kn[kind],
          med(t, reps, 1000000), BAR_BAT, "");
    }
    uint32_t *aids = xcalloc((size_t)NQ * 4);
    Stream aq = make_stream(AN, 0xA77Aull, aids);
    for (int r = 0; r < reps; r++) {   /* ArrayIncrement and ArraySum alternate: paired */
      uint64_t s = now_ns(), h[BATCH][2];
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        hash_prefetch_batch(as, &aq, b, h);
        for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(as, h[k][0], h[k][1]);
        for (int k = 0; k < BATCH; k++) {
          uint32_t qi = b + (uint32_t)k;
          const KvRec *rec = kv_store_get(as, aq.key[qi], aq.len[qi], h[k][0], h[k][1]);
          _Atomic int64_t *cell = (_Atomic int64_t *)(uintptr_t)kv_rec_value(rec) + (qi % ARR);
          __builtin_prefetch((const void *)cell, 1);
          atomic_fetch_add_explicit(cell, 1, memory_order_relaxed);
        }
      }
      ta[r] = now_ns() - s;
      s = now_ns();
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        hash_prefetch_batch(as, &aq, b, h);
        for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(as, h[k][0], h[k][1]);
        const int64_t *vals[BATCH];
        for (int k = 0; k < BATCH; k++) {   /* a third stage: the whole 1,344-byte array */
          uint32_t qi = b + (uint32_t)k;
          vals[k] = (const int64_t *)(const void *)kv_rec_value(
              kv_store_get(as, aq.key[qi], aq.len[qi], h[k][0], h[k][1]));
          for (int o = 0; o < ARR * 8; o += 64) __builtin_prefetch((const uint8_t *)vals[k] + o);
        }
        for (int k = 0; k < BATCH; k++) acc += (uint64_t)arr_sum_avx2(vals[k], ARR);
      }
      tb[r] = now_ns() - s;
    }
    g_vsink += acc;
    row("ArrayIncrement (one of 168)", "batched + LOCK XADD at the index", med(ta, reps, NQ), BAR_BAT,
        "200k keys of 1,344 bytes");
    row("ArraySum (all 168)", "batched + array prefetch stage + AVX2 sum", med(tb, reps, NQ), BAR_BAT,
        "8 bytes on the wire instead of 1,344");
    kv_store_destroy(as);
    free(arr);
    free(aids);
    free(aq.key);
    free(aq.len);
  }

  /* ================= JSON ================= */
  printf("\nJSON: one field out of a JSON value (simdjson-style AVX2 + PCLMULQDQ scan vs a scalar tokenizer)\n");
  {
    enum { ND = 2000 };
    static const int shapes[2][3] = {{6, 3, 10}, {30, 40, 120}};   /* tags, history, bio fragments */
    PathQ qs[5] = {
        {{"plan"}, {4}, 1},
        {{"address", "zip"}, {7, 3}, 2},
        {{"stats", "likes"}, {5, 5}, 2},
        {{"score"}, {5}, 1},
        {{"nope"}, {4}, 1},
    };
    static const char *const qn[5] = {"plan (early, top level)", "address.zip (early, nested)",
                                      "stats.likes (late, nested)", "score (last)", "absent key (whole doc)"};
    for (int sh = 0; sh < 2; sh++) {
      char **doc = xcalloc(ND * sizeof *doc);
      size_t *dl = xcalloc(ND * sizeof *dl), total = 0;
      JsonTruth *truth = xcalloc(ND * sizeof *truth);
      uint64_t seed = 0x15011ull + (uint64_t)sh;
      Buf b = {xcalloc(1 << 20), 0, 1 << 20};
      for (int d = 0; d < ND; d++) {
        b.n = 0;
        gen_doc(&b, (uint32_t)d, shapes[sh][0], shapes[sh][1], shapes[sh][2], &seed, &truth[d]);
        doc[d] = xcalloc(b.n);
        memcpy(doc[d], b.p, b.n);
        dl[d] = b.n;
        total += b.n;
      }
      free(b.p);
      for (int d = 0; d < ND; d++) {   /* every structural position equal; every field as generated */
        ScalarIter a;
        SimdIter c;
        sc_init(&a, (const uint8_t *)doc[d], dl[d]);
        si_init(&c, (const uint8_t *)doc[d], dl[d]);
        for (;;) {
          long x = sc_next(&a), y = si_next(&c);
          if (x != y) die("SIMD structurals differ from the scalar tokenizer");
          if (x < 0) break;
        }
        const char *want[5] = {truth[d].plan, truth[d].zip, truth[d].likes, truth[d].score, NULL};
        for (int qi = 0; qi < 5; qi++) {
          Span u = walk_scalar((const uint8_t *)doc[d], dl[d], &qs[qi]), v = walk_simd((const uint8_t *)doc[d], dl[d], &qs[qi]);
          if (u.start != v.start || u.end != v.end) die("SIMD and scalar walkers disagree");
          if (!want[qi]) {
            if (u.start >= 0) die("an absent key was found");
          } else if (u.start < 0 || (size_t)(u.end - u.start) != strlen(want[qi]) ||
                     memcmp(doc[d] + u.start, want[qi], strlen(want[qi]))) {
            die("an extracted field differs from the generated value");
          }
        }
      }
      printf("  %d documents of %.0f bytes on average: every structural and every field checked\n", ND,
             (double)total / ND);
      uint64_t acc = 0;
      for (int r = 0; r < reps; r++) {   /* stage 1 alone, both ways, alternating */
        uint64_t s = now_ns();
        for (int d = 0; d < ND; d++) acc += count_scalar((const uint8_t *)doc[d], dl[d]);
        ta[r] = now_ns() - s;
        s = now_ns();
        for (int d = 0; d < ND; d++) acc += count_simd((const uint8_t *)doc[d], dl[d]);
        tb[r] = now_ns() - s;
      }
      double sa = med(ta, reps, ND), sb = med(tb, reps, ND);
      char what[64];
      snprintf(what, sizeof what, "index a %.0f-byte document", (double)total / ND);
      snprintf(note, sizeof note, "%.2f GB/s", (double)total / ND / sa);
      row(what, "scalar tokenizer, every structural", sa, BAR_IND, note);
      snprintf(note, sizeof note, "%.2f GB/s; %.1fx the scalar", (double)total / ND / sb, sa / sb);
      row("", "AVX2 + PCLMULQDQ, every structural", sb, BAR_IND, note);
      for (int qi = 0; qi < 5; qi++) {
        for (int r = 0; r < reps; r++) {
          uint64_t s = now_ns();
          for (int d = 0; d < ND; d++) acc += (uint64_t)walk_scalar((const uint8_t *)doc[d], dl[d], &qs[qi]).end;
          ta[r] = now_ns() - s;
          s = now_ns();
          for (int d = 0; d < ND; d++) acc += (uint64_t)walk_simd((const uint8_t *)doc[d], dl[d], &qs[qi]).end;
          tb[r] = now_ns() - s;
        }
        sa = med(ta, reps, ND);
        sb = med(tb, reps, ND);
        snprintf(what, sizeof what, "JsonGet %s", qn[qi]);
        row(what, "scalar tokenizer + walker", sa, BAR_IND, "");
        snprintf(note, sizeof note, "%.1fx the scalar", sa / sb);
        row("", "SIMD tokenizer + the same walker", sb, BAR_IND, note);
      }
      g_vsink += acc;
      if (sh == 0) {   /* JsonGet end to end: 1M documents of this shape in a store */
        enum { JN = 1000000 };
        KvTable *js = kv_store_create(JN, MAX_THREADS + 1, kSeed, 0);
        if (!js) die("kv_store_create (json)");
        Buf jb = {xcalloc(1 << 16), 0, 1 << 16};
        uint64_t s2 = 0x99ull;
        JsonTruth tr;
        for (uint32_t i = 0; i < JN; i++) {
          jb.n = 0;
          gen_doc(&jb, i, shapes[0][0], shapes[0][1], shapes[0][2], &s2, &tr);
          uint64_t h[2];
          kv_store_hash(js, akey(i), alen(i), h);
          if (kv_store_put(js, akey(i), alen(i), jb.p, jb.n, h[0], h[1]) != 1) die("json store fill");
        }
        free(jb.p);
        uint32_t *jids = xcalloc((size_t)NQ * 4);
        Stream jq = make_stream(JN, 0x4A50ull, jids);
        for (int r = 0; r < reps; r++) {   /* whole document vs one field, alternating */
          uint64_t s = now_ns(), h[BATCH][2];
          for (uint32_t bb = 0; bb < NQ; bb += BATCH) {
            hash_prefetch_batch(js, &jq, bb, h);
            for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(js, h[k][0], h[k][1]);
            for (int k = 0; k < BATCH; k++) {
              const KvRec *rec = kv_store_get(js, jq.key[bb + (uint32_t)k], jq.len[bb + (uint32_t)k], h[k][0], h[k][1]);
              memcpy(g_sink, kv_rec_value(rec), rec->val_len);
            }
          }
          ta[r] = now_ns() - s;
          s = now_ns();
          for (uint32_t bb = 0; bb < NQ; bb += BATCH) {
            hash_prefetch_batch(js, &jq, bb, h);
            for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(js, h[k][0], h[k][1]);
            for (int k = 0; k < BATCH; k++) {
              const KvRec *rec = kv_store_get(js, jq.key[bb + (uint32_t)k], jq.len[bb + (uint32_t)k], h[k][0], h[k][1]);
              Span sp = walk_simd(kv_rec_value(rec), rec->val_len, &qs[2]);
              if (sp.start < 0) die("JsonGet missed stats.likes");
              memcpy(g_sink, kv_rec_value(rec) + sp.start, (size_t)(sp.end - sp.start));
            }
          }
          tb[r] = now_ns() - s;
        }
        double whole = med(ta, reps, NQ), one = med(tb, reps, NQ);
        row("get a whole ~1 KB JSON document", "batched pipeline (glibc memcpy)", whole, BAR_BAT, "1M documents");
        snprintf(note, sizeof note, "%.2fx the whole get; a few bytes on the wire", one / whole);
        row("JsonGet stats.likes (late field)", "batched + SIMD tokenizer + walker", one, BAR_BAT, note);
        kv_store_destroy(js);
        free(jids);
        free(jq.key);
        free(jq.len);
      }
      for (int d = 0; d < ND; d++) free(doc[d]);
      free(doc);
      free(dl);
      free(truth);
    }
  }

  /* ================= SHA-256 ================= */
  printf("\nSHA-256: content addressing (SHA-NI instructions vs scalar FIPS 180-4 code)\n");
  {
    static const struct {
      const char *msg, *hex;
    } vec[3] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for (int v = 0; v < 3; v++)
      for (int ni = 0; ni < 2; ni++) {
        uint8_t d[32];
        char hx[65];
        sha256((const uint8_t *)vec[v].msg, strlen(vec[v].msg), d, ni);
        hex32(d, hx);
        if (strcmp(hx, vec[v].hex)) die(ni ? "SHA-NI fails a FIPS 180-4 vector" : "scalar SHA-256 fails a FIPS vector");
      }
    uint8_t *buf = xcalloc(1 << 20);
    uint64_t seed = 31;
    for (int i = 0; i < 1 << 20; i++) buf[i] = (uint8_t)rnd(&seed);
    for (int i = 0; i < 500; i++) {   /* SHA-NI == scalar on random lengths */
      size_t len = (size_t)(rnd(&seed) % 20000);
      uint8_t a[32], c[32];
      sha256(buf, len, a, 0);
      sha256(buf, len, c, 1);
      if (memcmp(a, c, 32)) die("SHA-NI disagrees with the scalar SHA-256");
    }
    static const size_t sz[4] = {100, 4096, 65536, 1 << 20};
    for (int si = 0; si < 4; si++) {
      uint32_t iters = (uint32_t)(64000000 / (sz[si] + 1000));
      double ns[2];
      for (int ni = 0; ni < 2; ni++) {
        uint8_t d[32];
        uint64_t acc = 0;
        for (int r = 0; r < reps; r++) {
          uint64_t s = now_ns();
          for (uint32_t i = 0; i < iters; i++, clobber()) {
            sha256(buf, sz[si], d, ni);
            acc += d[0];
          }
          t[r] = now_ns() - s;
        }
        g_vsink += acc;
        ns[ni] = med(t, reps, iters);
      }
      char what[48];
      snprintf(what, sizeof what, "SHA-256 of %zu bytes", sz[si]);
      snprintf(note, sizeof note, "%.2f GB/s", (double)sz[si] / ns[0]);
      row(what, "scalar", ns[0], BAR_BAT, note);
      snprintf(note, sizeof note, "%.2f GB/s; %.1fx the scalar", (double)sz[si] / ns[1], ns[0] / ns[1]);
      row("", "SHA-NI (SHA256RNDS2, MSG1, MSG2)", ns[1], BAR_BAT, note);
    }
    /* PutBlob (4 KiB): digest, then put under the digest (skipped when present) */
    enum { BN = 100000 };
    KvTable *bs = kv_store_create(BN * 2, MAX_THREADS + 1, kSeed, 0);
    if (!bs) die("kv_store_create (blobs)");
    uint64_t created = 0, s = now_ns();
    for (uint32_t i = 0; i < BN; i++) {
      const uint8_t *blob = buf + (i % 64) * 4096 + (i / 64) % 64;   /* 4,096 distinct blobs, then repeats */
      uint8_t d[32];
      uint64_t h[2];
      sha256(blob, 4096, d, 1);
      kv_store_hash(bs, d, 32, h);
      if (kv_store_get(bs, d, 32, h[0], h[1])) continue;   /* already stored: deduplicated */
      if (kv_store_put(bs, d, 32, blob, 4096, h[0], h[1]) != 1) die("blob put");
      created++;
    }
    double per = (double)(now_ns() - s) / BN;
    snprintf(note, sizeof note, "%llu stored, %llu deduplicated", (unsigned long long)created,
             (unsigned long long)(BN - created));
    row("PutBlob, 4 KiB", "SHA-NI digest + exists + put", per, BAR_IND, note);
    /* FindMissingBlobs: batched exists of 32-byte digests, half present */
    uint8_t (*dg)[KEYMAX] = xcalloc((size_t)NQ * KEYMAX);
    uint32_t *dlen = xcalloc((size_t)NQ * 4);
    for (uint32_t i = 0; i < NQ; i++) {
      uint32_t which = (uint32_t)(rnd(&seed) % 8192);   /* < 4096: stored blob; else unknown */
      if (which < 4096) sha256(buf + (which % 64) * 4096 + (which / 64) % 64, 4096, dg[i], 1);
      else
        for (int j = 0; j < 32; j++) dg[i][j] = (uint8_t)rnd(&seed);
      dlen[i] = 32;
    }
    Stream dq = {dg, dlen};
    uint64_t found = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s1 = now_ns(), h[BATCH][2];
      found = 0;
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        hash_prefetch_batch(bs, &dq, b, h);
        for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(bs, h[k][0], h[k][1]);
        for (int k = 0; k < BATCH; k++) found += kv_store_get(bs, dq.key[b + (uint32_t)k], 32, h[k][0], h[k][1]) != NULL;
      }
      t[r] = now_ns() - s1;
    }
    snprintf(note, sizeof note, "%.0f%% present", 100.0 * (double)found / NQ);
    row("FindMissingBlobs (32-byte digests)", "batched exists", med(t, reps, NQ), BAR_BAT, note);
    kv_store_destroy(bs);
    free(dg);
    free(dlen);
    free(buf);
  }

  /* ================= substring search ================= */
  printf("\nGREP: substring search over 256 MiB of text values (a value filter for Scan / Export)\n");
  {
    size_t n = (size_t)256 << 20;
    uint8_t *text = big_alloc(n + 64);
    static const char *const words[12] = {"the ", "cat ", "sat ", "on ", "a ", "mat ", "with ", "orange ",
                                          "juice ", "and ", "the cat ", "tea "};
    uint64_t seed = 4242;
    size_t p = 0;
    while (p < n) {
      const char *w = words[rnd(&seed) % 12];
      size_t l = strlen(w);
      if (p + l > n) break;
      memcpy(text + p, w, l);
      p += l;
    }
    memset(text + p, ' ', n - p);
    for (int i = 0; i < 4096; i++) {   /* plant a rare needle */
      size_t at = (size_t)(rnd(&seed) % (n - 16));
      memcpy(text + at, "orange7", 7);
    }
    static const char *const needles[2] = {"orange7", "the cat sat"};
    for (int ni = 0; ni < 2; ni++) {
      const uint8_t *k = (const uint8_t *)needles[ni];
      size_t kn = strlen(needles[ni]);
      uint64_t c1 = count_memmem(text, n, k, kn), c2 = count_avx2(text, n, k, kn);
      if (c1 != c2) die("AVX2 substring count disagrees with memmem");
      for (int r = 0; r < reps; r++) {   /* alternating: paired */
        uint64_t s = now_ns();
        g_vsink += count_memmem(text, n, k, kn);
        ta[r] = now_ns() - s;
        s = now_ns();
        g_vsink += count_avx2(text, n, k, kn);
        tb[r] = now_ns() - s;
      }
      double a = med(ta, reps, 1), b = med(tb, reps, 1);
      char what[64];
      snprintf(what, sizeof what, "find \"%s\" in 256 MiB", needles[ni]);
      snprintf(note, sizeof note, "%.2f GB/s; %llu matches", (double)n / a, (unsigned long long)c1);
      row(what, "glibc memmem", a, BAR_NONE, note);
      snprintf(note, sizeof note, "%.2f GB/s; %.1fx memmem", (double)n / b, a / b);
      row("", "AVX2 first/last-byte filter", b, BAR_NONE, note);
    }
    big_free(text, n + 64);
  }

  printf("\n");
  print_memory();
  printf("all checks passed\n");
  return 0;
}
