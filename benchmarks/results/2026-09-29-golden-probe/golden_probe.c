/*
 * golden_probe.c — which operations stay within 10x of an exact lookup at
 * 10M keys, thread-safe, using SIMD and other assembly-level techniques?
 * Evidence for docs/proposals/golden-operations.md.
 *
 * Why it exists: the bar set for this work is "anything within 10x of an
 * exact get / exists on a 10M-key collection is golden". This probe
 * measures that bar on the server's own store (store.asm, cuckoo.asm and
 * siphash.asm: SSE2 tag scan, the batched two-stage-prefetch GET pipeline),
 * then prototypes each candidate at the same size and prints it against
 * the bar of the same regime:
 *
 *   bar        get / exists / absent-key lookups three ways: a dependent
 *              chain (pure latency, "D"), an independent loop (what the core
 *              overlaps by itself, "I") and the server's 32-key batched
 *              pipeline (throughput, "B")
 *   hashing    per-key cost of the store's SipHash-2-4 against SipHash-1-3,
 *              AES-NI, CRC32C and multiply-mix hashes; AVX2 case folding
 *   sampling   a uniformly random key in O(1): rejection sampling over the
 *              table's slots, single and batched
 *   counters   increment in place (lookup + LOCK XADD on the value word);
 *              on one hot key also a CAS loop and per-thread cells; 1-7
 *              threads, exact totals
 *   filters    split-block Bloom (AVX2 VPMULLD/VPSLLVD/VPTEST, and scalar)
 *              and a binary fuse filter: "definitely absent"
 *   order      a sorted snapshot searched four ways: textbook binary search
 *              through ids, branchless search over inline 16-byte keys,
 *              Eytzinger layout with 4-level-ahead prefetch, and 16
 *              interleaved Eytzinger descents; successor/rank, prefix count,
 *              100-key range scans (keys; keys + values), select
 *   sketches   HyperLogLog with an incrementally kept harmonic sum (O(1) add
 *              and estimate) and an AVX2 union (VPMAXUB, plus a harmonic
 *              sum built from float exponent bits); Count-Min
 *   typos      SymSpell (edit distance 1) at 10M: a polynomial hash whose
 *              prefix/suffix form gives every deletion's hash in O(1), a
 *              bucketized table probed with AVX2 compares, probes and
 *              candidate keys prefetched as groups
 *   threads    lock-free readers on 1-7 cores; readers next to a writer
 *   writes     replace / delete / create one at a time vs prefetched
 *              batches; 1-7 concurrent writers (the writer mutex)
 *
 * Checks (a failure exits 1 before its numbers can mislead): every search
 * layout agrees with textbook binary search; prefix counts and SymSpell
 * results match brute force over all keys (Myers is checked against the
 * DP first); filters have no false negatives; sampled records are live and
 * evenly spread; HLL's kept sum equals a full recomputation (the AVX2 sum
 * agrees within 1e-6) and its estimates, of one set and of the union of two
 * disjoint sets, are within 3 sigma of the truth; Count-Min never
 * under-counts; concurrent counters and writes lose nothing.
 *
 * Usage: golden_probe [--keys N] [--cpu C] [--reps R] [--threads "1,2,4,7"]
 *                     [--bar-only]
 * Build: run.sh next to this file.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* MAP_ANONYMOUS, MADV_HUGEPAGE, affinity calls */
#endif
#include <immintrin.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "binaryfusefilter.h"   /* pinned by run.sh; -isystem: its warnings are not ours */
#include "kvasm_abi.h"

enum {
  KEYMAX = 32,           /* key buffer; the longest key is 21 bytes (+1 for a typo) */
  VAL_LEN = 100,         /* the benchmark suite's value size */
  FIELDS = 4,            /* keys per "user" */
  SLOTS = 6,             /* BK_SLOTS: record slots per bucket */
  BATCH = 32,            /* the server's GET batch */
  IBATCH = 16,           /* interleaved snapshot descents */
  MAX_REPS = 16,
  MAX_THREADS = 7,       /* CPUs 1-7: distinct P-cores on this host */
  NQ = 1 << 20,          /* lookups per timed pass */
  NW = 1 << 18,          /* writes per timed pass */
  NP = 1 << 16,          /* ordered searches per timed pass */
  NT = 1000,             /* typo queries per timed pass */
  HOT = 1000,            /* keys in the "hot" counter set */
  MAX_CAND = 256,        /* SymSpell candidates a query may collect */
  WSTRIDE = 7919,        /* prime: spreads the write-key sets over the key space */
};

static const char *const kSyl[20] = {"ka", "lo", "mi", "nu", "ra", "se", "ti", "vo", "ze", "ba",
                                     "de", "fi", "go", "hu", "ja", "ke", "li", "mo", "na", "pe"};
static const char *const kFields[FIELDS] = {"name", "email", "plan", "prefs"};
/* SipHash key of the store; every auxiliary structure reuses the store's hash. */
static const uint8_t kSeed[16] __attribute__((aligned(8))) = {9, 7, 5, 3, 1, 2, 4, 6,
                                                              8, 10, 12, 14, 16, 18, 20, 22};
/* Thread i runs on CPU g_tcpu[i]: vCPU 0 carries VM interrupt work, 8-15 are HT siblings. */
static const int g_tcpu[MAX_THREADS] = {1, 2, 3, 4, 5, 6, 7};

static uint32_t g_n;             /* keys in the store */
static KvTable *g_store;
static uint8_t *g_arena;         /* every key once, back to back */
static uint32_t *g_start;        /* key i = g_arena[g_start[i] .. g_start[i + 1]) */
static uint8_t g_sink[VAL_LEN];  /* value copies land here */
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
 * Prints a message and exits non-zero.
 * @param what  the failed condition
 * @remarks Every check guards printed numbers: a failure aborts the whole
 *          run so a wrong structure can never report a fast time.
 */
static void die(const char *what) {
  fprintf(stderr, "golden_probe: %s\n", what);
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
 * @remarks The store's table asks for huge pages the same way, so giving
 *          the probe's structures the same treatment keeps their TLB costs
 *          comparable with the bar's. The madvise is a hint: if it fails
 *          the memory still works, it only costs TLB reach.
 */
static void *big_alloc(size_t n) {
  void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("out of memory (mmap)");
  madvise(p, n, MADV_HUGEPAGE);
  return p;
}

/**
 * Releases memory from big_alloc.
 * @param p  the memory  @param n  its size in bytes
 */
static void big_free(void *p, size_t n) { munmap(p, n); }

/**
 * Pins the calling thread to one CPU.
 * @param cpu  CPU number
 */
static void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) die("pthread_setaffinity_np");
}

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

/** qsort comparator for uint64_t. */
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
 * Writes key number i: "user:<5 syllables of i/4>:<field i%4>".
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

/** Key i's bytes in the arena. */
static inline const uint8_t *akey(uint32_t i) { return g_arena + g_start[i]; }

/** Key i's length. */
static inline uint32_t alen(uint32_t i) { return g_start[i + 1] - g_start[i]; }

/**
 * Byte order of two keys (memcmp, a proper prefix first).
 * @param a  key id  @param b  key id
 * @returns <0, 0 or >0.
 */
static int key_cmp(uint32_t a, uint32_t b) {
  uint32_t la = alen(a), lb = alen(b);
  int c = memcmp(akey(a), akey(b), la < lb ? la : lb);
  return c ? c : (la > lb) - (la < lb);
}

/* The exact-get bars (ns): D = dependent chain, I = independent loop, B = batched. */
enum { BAR_NONE = -1, BAR_DEP = 0, BAR_IND = 1, BAR_BAT = 2 };
static double g_bar[3];

/**
 * Prints one result row with its ratio to the bar of the same regime.
 * @param what  operation (blank continues the previous one)
 * @param how   technique
 * @param ns    latency in ns
 * @param bar   BAR_DEP / BAR_IND / BAR_BAT, or BAR_NONE for no ratio
 * @param note  remark (may be "")
 * @remarks "GOLD" marks a ratio of at most 10: the bar this work set.
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
  printf("  %-30s %-46s %s  %s  %s\n", what, how, v, r, note);
}

/**
 * Prints this process's resident and huge-page-backed anonymous memory.
 * @remarks Tells whether the records really sat on huge pages in a run
 *          with the glibc huge-page tunables (smaps_rollup, because VmRSS
 *          is stale on this WSL2 kernel).
 */
static void print_memory(void) {
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  char line[256];
  if (!f) return;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "Rss:", 4) || !strncmp(line, "AnonHugePages:", 14)) printf("  %s", line);
  fclose(f);
}

/* ========================================================================= */
/* Query streams                                                              */
/* ========================================================================= */

static uint32_t *g_qid;               /* NQ random existing key ids */
static uint8_t (*g_qkey)[KEYMAX];     /* their keys, one after another (a request stream) */
static uint32_t *g_qlen;
static uint8_t (*g_absent)[KEYMAX];   /* NQ absent keys */
static uint32_t *g_absent_len;

/**
 * Builds the query streams: NQ random existing keys and NQ absent keys, each
 * copied into a sequential buffer.
 * @remarks A server reads keys from its request buffers, which are in cache;
 *          fetching them from the 200 MB arena instead would add a random
 *          miss to every lookup and inflate the bar. An absent key differs
 *          from a real one only in its field ("zzzz"), so it hashes and
 *          compares like one.
 */
static void make_queries(void) {
  uint64_t s = 0x243F6A8885A308D3ull;
  g_qid = xcalloc((size_t)NQ * sizeof *g_qid);
  g_qkey = xcalloc((size_t)NQ * KEYMAX);
  g_qlen = xcalloc((size_t)NQ * sizeof *g_qlen);
  g_absent = xcalloc((size_t)NQ * KEYMAX);
  g_absent_len = xcalloc((size_t)NQ * sizeof *g_absent_len);
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t id = (uint32_t)(rnd(&s) % g_n);
    g_qid[i] = id;
    g_qlen[i] = alen(id);
    memcpy(g_qkey[i], akey(id), g_qlen[i]);
    uint32_t base = (uint32_t)(rnd(&s) % g_n) & ~3u;   /* field 0 = "name": 4 bytes to replace */
    g_absent_len[i] = (uint32_t)make_key(base, g_absent[i]);
    memcpy(g_absent[i] + g_absent_len[i] - 4, "zzzz", 4);
  }
}

/* ========================================================================= */
/* The bar: exact get / exists / absent                                       */
/* ========================================================================= */

/**
 * NQ exact lookups where each key is chosen by the previous result, so
 * nothing overlaps: pure latency.
 * @param copy  1 = get (the value is copied and its last byte picks the next
 *              key), 0 = exists (the record's key length picks it)
 * @returns total ns.
 * @remarks The next key depends on data read from the record, not just on
 *          the returned pointer: otherwise branch prediction would let the
 *          next lookup start before the record's line arrives. The result
 *          picks between two far-apart positions of the stream (i or
 *          i + NQ/2), never a neighbour: a first version chose i or i + 1,
 *          which looked the same key up twice in a row a quarter of the
 *          time, from cache, and understated the latency by ~25%.
 */
static uint64_t get_dependent(int copy) {
  uint64_t t0 = now_ns(), x = 0;
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t q = (i + (uint32_t)x * (NQ / 2)) & (NQ - 1);
    uint64_t h[2];
    kv_store_hash(g_store, g_qkey[q], g_qlen[q], h);
    const KvRec *r = kv_store_get(g_store, g_qkey[q], g_qlen[q], h[0], h[1]);
    if (!r) die("dependent get missed");
    if (copy) {
      memcpy(g_sink, kv_rec_value(r), VAL_LEN);
      x = g_sink[VAL_LEN - 1] & 1;
    } else {
      x = r->key_len & 1;
    }
  }
  return now_ns() - t0;
}

/**
 * NQ exact lookups in a plain loop: independent iterations, so the core
 * overlaps what its out-of-order window allows.
 * @param copy    1 = get (copy the value), 0 = exists
 * @param absent  1 = query the absent keys (every lookup must miss)
 * @returns total ns.
 */
static uint64_t get_independent(int copy, int absent) {
  uint64_t t0 = now_ns(), found = 0;
  for (uint32_t i = 0; i < NQ; i++) {
    const uint8_t *k = absent ? g_absent[i] : g_qkey[i];
    uint32_t l = absent ? g_absent_len[i] : g_qlen[i];
    uint64_t h[2];
    kv_store_hash(g_store, k, l, h);
    const KvRec *r = kv_store_get(g_store, k, l, h[0], h[1]);
    if (r) {
      found++;
      if (copy) memcpy(g_sink, kv_rec_value(r), VAL_LEN);
    }
  }
  if (found != (absent ? 0u : (uint64_t)NQ)) die("independent lookup count");
  return now_ns() - t0;
}

/**
 * The server's batched GET pipeline over NQ keys: per batch of 32, hash all
 * and prefetch both buckets, prefetch the candidate records, then look up
 * (and copy) each.
 * @param copy    1 = get, 0 = exists
 * @param absent  1 = absent keys
 * @returns total ns.
 */
static uint64_t get_batched(int copy, int absent) {
  uint64_t t0 = now_ns(), found = 0, h[BATCH][2];
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    for (int k = 0; k < BATCH; k++) {
      const uint8_t *key = absent ? g_absent[b + (uint32_t)k] : g_qkey[b + (uint32_t)k];
      uint32_t l = absent ? g_absent_len[b + (uint32_t)k] : g_qlen[b + (uint32_t)k];
      kv_store_hash(g_store, key, l, h[k]);
      kv_store_prefetch(g_store, h[k][0], h[k][1]);
    }
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const uint8_t *key = absent ? g_absent[b + (uint32_t)k] : g_qkey[b + (uint32_t)k];
      uint32_t l = absent ? g_absent_len[b + (uint32_t)k] : g_qlen[b + (uint32_t)k];
      const KvRec *r = kv_store_get(g_store, key, l, h[k][0], h[k][1]);
      if (r) {
        found++;
        if (copy) memcpy(g_sink, kv_rec_value(r), VAL_LEN);
      }
    }
  }
  if (found != (absent ? 0u : (uint64_t)NQ)) die("batched lookup count");
  return now_ns() - t0;
}

/* ========================================================================= */
/* Hashing kernels                                                            */
/* ========================================================================= */

/** One SipHash round on four 64-bit words. */
#define SIPROUND(v0, v1, v2, v3)                                                     \
  do {                                                                               \
    v0 += v1; v1 = (v1 << 13) | (v1 >> 51); v1 ^= v0; v0 = (v0 << 32) | (v0 >> 32); \
    v2 += v3; v3 = (v3 << 16) | (v3 >> 48); v3 ^= v2;                                \
    v0 += v3; v3 = (v3 << 21) | (v3 >> 43); v3 ^= v0;                                \
    v2 += v1; v1 = (v1 << 17) | (v1 >> 47); v1 ^= v2; v2 = (v2 << 32) | (v2 >> 32); \
  } while (0)

/**
 * SipHash-c-d with a 64-bit output (the reference construction), to price
 * the round counts against the store's 128-bit SipHash-2-4.
 * @param in  bytes  @param len  length  @param k  two key words
 * @param c   compression rounds per 8-byte word  @param d  finalization rounds
 * @returns the 64-bit hash.
 */
static uint64_t siphash_cd(const uint8_t *in, size_t len, const uint64_t k[2], int c, int d) {
  uint64_t v0 = 0x736f6d6570736575ull ^ k[0], v1 = 0x646f72616e646f6dull ^ k[1];
  uint64_t v2 = 0x6c7967656e657261ull ^ k[0], v3 = 0x7465646279746573ull ^ k[1];
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t m;
    memcpy(&m, in + i, 8);
    v3 ^= m;
    for (int r = 0; r < c; r++) SIPROUND(v0, v1, v2, v3);
    v0 ^= m;
  }
  uint64_t b = (uint64_t)len << 56;
  for (size_t j = 0; i + j < len; j++) b |= (uint64_t)in[i + j] << (8 * j);
  v3 ^= b;
  for (int r = 0; r < c; r++) SIPROUND(v0, v1, v2, v3);
  v0 ^= b;
  v2 ^= 0xff;
  for (int r = 0; r < d; r++) SIPROUND(v0, v1, v2, v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

/**
 * CRC32C of a key with the SSE4.2 instruction, 8 bytes per step.
 * @param in  bytes  @param len  length
 * @returns the CRC.
 * @remarks Unkeyed and linear: attackers can collide it at will, so it is a
 *          candidate for fingerprints of trusted data only, never for
 *          bucket placement of client-chosen keys.
 */
__attribute__((target("sse4.2"))) static uint32_t crc32c_key(const uint8_t *in, size_t len) {
  uint64_t c = 0xFFFFFFFFu;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t m;
    memcpy(&m, in + i, 8);
    c = _mm_crc32_u64(c, m);
  }
  for (; i < len; i++) c = _mm_crc32_u8((uint32_t)c, in[i]);
  return (uint32_t)c ^ 0xFFFFFFFFu;
}

/**
 * A three-round AES-NI hash of a key up to 32 bytes, keyed by the seed.
 * @param in  bytes (the first min(len, 32) are used)  @param len  length
 * @returns 64 bits of the final state.
 * @remarks Fast, but without SipHash's analysis as a PRF: a candidate only
 *          where hash flooding is not a threat or its strength is argued
 *          separately.
 */
__attribute__((target("aes,sse4.1"))) static uint64_t aes_hash(const uint8_t *in, size_t len) {
  uint8_t buf[32] = {0};
  memcpy(buf, in, len < 32 ? len : 32);
  __m128i k0 = _mm_loadu_si128((const __m128i *)(const void *)kSeed);
  __m128i s = _mm_xor_si128(k0, _mm_set_epi64x(0, (long long)len));
  s = _mm_aesenc_si128(_mm_xor_si128(s, _mm_loadu_si128((const __m128i *)(const void *)buf)), k0);
  s = _mm_aesenc_si128(_mm_xor_si128(s, _mm_loadu_si128((const __m128i *)(const void *)(buf + 16))), k0);
  s = _mm_aesenc_si128(s, k0);
  return (uint64_t)_mm_extract_epi64(s, 0) ^ (uint64_t)_mm_extract_epi64(s, 1);
}

/**
 * A multiply-mix hash (wyhash-style folding of 8-byte words), keyed by the
 * seed: the cheapest candidate, again without PRF strength.
 * @param in  bytes  @param len  length (the tail is read with memcpy)
 * @returns the hash.
 */
static uint64_t mix_hash(const uint8_t *in, size_t len) {
  uint64_t k0, k1;
  memcpy(&k0, kSeed, 8);
  memcpy(&k1, kSeed + 8, 8);
  uint64_t h = k0 ^ len;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t m;
    memcpy(&m, in + i, 8);
    __uint128_t z = (__uint128_t)(m ^ k1) * (h ^ 0x9E3779B97F4A7C15ull);
    h = (uint64_t)z ^ (uint64_t)(z >> 64);
  }
  uint64_t t = 0;
  memcpy(&t, in + i, len - i);
  __uint128_t z = (__uint128_t)(t ^ k0) * (h ^ 0xD6E8FEB86659FD93ull);
  return (uint64_t)z ^ (uint64_t)(z >> 64);
}

/**
 * AVX2 ASCII lowercase of 32 bytes: bytes in 'A'..'Z' get 0x20 added.
 * @param in  source (32 readable bytes)  @param out  destination (32 bytes)
 * @remarks Signed byte compares are fine: bytes >= 0x80 compare negative and
 *          so are never in 'A'..'Z'. A case-insensitive store folds every
 *          key once on the way in, then hashes and compares as usual.
 */
__attribute__((target("avx2"))) static void fold32(const uint8_t *in, uint8_t *out) {
  __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)in);
  __m256i up = _mm256_and_si256(_mm256_cmpgt_epi8(v, _mm256_set1_epi8('A' - 1)),
                                _mm256_cmpgt_epi8(_mm256_set1_epi8('Z' + 1), v));
  _mm256_storeu_si256((__m256i *)(void *)out, _mm256_add_epi8(v, _mm256_and_si256(up, _mm256_set1_epi8(0x20))));
}

/* ========================================================================= */
/* Random sampling                                                            */
/* ========================================================================= */

/** kvasm.inc Bucket: version, 8 tags, 6 record slots (one 64-byte line). */
typedef struct {
  uint64_t ver, tags;
  const KvRec *slot[SLOTS];
} Bucket;

/** The first fields of kvasm.inc Table. */
typedef struct {
  Bucket *buckets;
  uint64_t mask;
} TableHead;

/**
 * One uniformly random key: pick a random slot of the table, retry while it
 * is empty (rejection sampling).
 * @param seed  random state
 * @returns a live record.
 * @remarks Each key occupies exactly one of the table's slots, so a uniform
 *          slot that is occupied is a uniform key. Expected tries = 1/load
 *          (1.26 at 79.5%). A slot is one aligned 8-byte load, and the
 *          record stays valid until this reader's next quiescent state
 *          (QSBR), so no lock or seqlock retry is needed. A cuckoo move in
 *          flight can make one key briefly visible in two slots or none: a
 *          negligible, transient bias.
 */
static const KvRec *sample_key(uint64_t *seed) {
  const TableHead *t = (const TableHead *)(const void *)g_store;
  for (;;) {
    uint64_t r = rnd(seed);
    const Bucket *b = &t->buckets[r & t->mask];
    const KvRec *rec = __atomic_load_n(&b->slot[((r >> 32) * SLOTS) >> 32], __ATOMIC_ACQUIRE);
    if (rec) return rec;
  }
}

/**
 * Batched sampling: 32 candidate slots per round, all buckets prefetched,
 * then every hit's record prefetched, then the keys read.
 * @param n     samples wanted (at least)
 * @param seed  random state
 * @param got   receives the number actually drawn (>= n)
 * @returns total ns.
 */
static uint64_t sample_batched(uint32_t n, uint64_t seed, uint32_t *got) {
  const TableHead *t = (const TableHead *)(const void *)g_store;
  const Bucket *b[BATCH];
  const KvRec *hit[BATCH];
  uint32_t s[BATCH], total = 0;
  uint64_t t0 = now_ns(), acc = 0;
  while (total < n) {
    for (int k = 0; k < BATCH; k++) {
      uint64_t r = rnd(&seed);
      b[k] = &t->buckets[r & t->mask];
      s[k] = (uint32_t)(((r >> 32) * SLOTS) >> 32);
      __builtin_prefetch(b[k]);
    }
    int nh = 0;
    for (int k = 0; k < BATCH; k++) {
      const KvRec *rec = __atomic_load_n(&b[k]->slot[s[k]], __ATOMIC_ACQUIRE);
      if (rec) {
        __builtin_prefetch(rec);
        hit[nh++] = rec;
      }
    }
    for (int k = 0; k < nh; k++) acc += hit[k]->data[5];   /* first byte after "user:" */
    total += (uint32_t)nh;
  }
  g_vsink += acc;
  *got = total;
  return now_ns() - t0;
}

/* ========================================================================= */
/* In-place atomic counters                                                   */
/* ========================================================================= */

/**
 * The counter word of a record: its first 8 value bytes.
 * @param r  record
 * @returns the atomic word.
 * @remarks 8-aligned by construction: malloc returns 16-aligned records, the
 *          header is 24 bytes and the key is padded to 8. Mutating a record
 *          in place breaks KvAsm's "records are immutable" rule on
 *          purpose: a counter-typed record holds an atomic cell that readers
 *          load with one aligned 8-byte access, so no reader can see a torn
 *          value. (Put/Delete of a counter key must mark the old word as
 *          retired; docs/proposals/golden-operations.md has the protocol.)
 */
static inline _Atomic uint64_t *counter_of(const KvRec *r) {
  return (_Atomic uint64_t *)(uintptr_t)kv_rec_value(r);
}

/** How an increment is applied. */
typedef enum {
  C_XADD,     /* LOCK XADD on the record's word */
  C_CAS,      /* LOCK CMPXCHG loop: the form conditional updates need */
  C_SHARD,    /* the thread's own cell, plain load + add + store: no LOCK */
} CMode;

/* Per-thread cells of one sharded counter, a cache line each (no false sharing). */
static struct {
  _Alignas(64) _Atomic uint64_t v;
} g_shard[MAX_THREADS];

/** One counter run's assignment. */
typedef struct {
  int cpu, slot, tid, hot;
  CMode mode;
  uint32_t ops, offset;
  pthread_barrier_t *start;
} CJob;

/**
 * Applies `ops` increments with the batched lookup pipeline.
 * @param j  the assignment: hot 0 = keys from the query stream (random),
 *           1 = cycling through HOT keys, 2 = one key; mode as CMode;
 *           offset = where this thread starts in the stream; slot = its
 *           QSBR reader slot; tid = its cell for C_SHARD
 * @remarks The single-writer cell needs no LOCK prefix: only its owner
 *          writes it, and readers sum all cells with plain loads.
 */
static void counter_run(const CJob *j) {
  uint64_t h[BATCH][2];
  uint32_t q[BATCH];
  for (uint32_t b = 0; b < j->ops; b += BATCH) {
    for (int k = 0; k < BATCH; k++) {
      uint32_t x = j->offset + b + (uint32_t)k;
      q[k] = j->hot == 0 ? (x & (NQ - 1)) : j->hot == 1 ? x % HOT : 0;
      kv_store_hash(g_store, g_qkey[q[k]], g_qlen[q[k]], h[k]);
      kv_store_prefetch(g_store, h[k][0], h[k][1]);
    }
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const KvRec *r = kv_store_get(g_store, g_qkey[q[k]], g_qlen[q[k]], h[k][0], h[k][1]);
      if (j->mode == C_XADD) {
        atomic_fetch_add_explicit(counter_of(r), 1, memory_order_relaxed);
      } else if (j->mode == C_CAS) {
        _Atomic uint64_t *w = counter_of(r);
        uint64_t old = atomic_load_explicit(w, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(w, &old, old + 1, memory_order_relaxed,
                                                      memory_order_relaxed)) {
        }
      } else {
        _Atomic uint64_t *c = &g_shard[j->tid].v;
        atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + 1, memory_order_relaxed);
      }
    }
    kv_qsbr_quiescent(g_store, (uint32_t)j->slot);
  }
}

/**
 * Counter thread: pin, go online as a QSBR reader, wait for the start, run,
 * go offline.
 * @param arg  CJob*
 * @returns NULL.
 */
static void *counter_thread(void *arg) {
  CJob *j = arg;
  pin(j->cpu);
  kv_qsbr_online(g_store, (uint32_t)j->slot);
  pthread_barrier_wait(j->start);
  counter_run(j);
  kv_qsbr_offline(g_store, (uint32_t)j->slot);
  return NULL;
}

/**
 * Sum of the counter words of a key set, each distinct key once.
 * @param hot  0 = every query-stream key, 1 = the HOT keys, 2 = the one key
 * @returns the sum (mod 2^64).
 * @remarks Reads through kv_store_get, as any reader would.
 */
static uint64_t counter_sum(int hot) {
  uint32_t n = hot == 0 ? NQ : hot == 1 ? HOT : 1;
  uint8_t *seen = xcalloc(g_n);
  uint64_t s = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t id = g_qid[i];
    if (seen[id]) continue;
    seen[id] = 1;
    uint64_t h[2];
    kv_store_hash(g_store, g_qkey[i], g_qlen[i], h);
    s += atomic_load(counter_of(kv_store_get(g_store, g_qkey[i], g_qlen[i], h[0], h[1])));
  }
  free(seen);
  return s;
}

/**
 * Runs T counter threads and checks that no increment was lost.
 * @param T     threads (CPUs g_tcpu[0..T))
 * @param mode  increment method
 * @param hot   key set (see counter_run)
 * @param per   increments per thread (multiple of BATCH)
 * @returns wall ns from the common start to the last thread's end.
 */
static uint64_t run_counters(int T, CMode mode, int hot, uint32_t per) {
  pthread_barrier_t start;
  pthread_t th[MAX_THREADS];
  CJob jobs[MAX_THREADS];
  pthread_barrier_init(&start, NULL, (unsigned)T + 1);
  for (int i = 0; i < T; i++) atomic_store(&g_shard[i].v, 0);
  uint64_t before = mode == C_SHARD ? 0 : counter_sum(hot);
  for (int i = 0; i < T; i++) {
    jobs[i] = (CJob){g_tcpu[i], 1 + i, i, hot, mode, per, (uint32_t)i * (NQ / MAX_THREADS), &start};
    if (pthread_create(&th[i], NULL, counter_thread, &jobs[i]) != 0) die("pthread_create");
  }
  kv_qsbr_offline(g_store, 0);   /* the main thread sleeps in join: it must not hold back reclamation */
  pthread_barrier_wait(&start);
  uint64_t t0 = now_ns();
  for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
  uint64_t wall = now_ns() - t0;
  kv_qsbr_online(g_store, 0);
  pthread_barrier_destroy(&start);
  uint64_t after = 0;
  if (mode == C_SHARD)
    for (int i = 0; i < T; i++) after += atomic_load(&g_shard[i].v);
  else
    after = counter_sum(hot);
  if (after - before != (uint64_t)per * (uint64_t)T) die("increments were lost or duplicated");
  return wall;
}

/* ========================================================================= */
/* Filters: split-block Bloom (AVX2 + scalar) and binary fuse                  */
/* ========================================================================= */

static uint32_t *g_sbbf;         /* blocks x 8 lanes of 32 bits */
static uint64_t g_sbbf_blocks;
static const uint32_t kSalt[8] = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                  0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

/**
 * Block of a key: fastrange (Lemire) of h1's top 32 bits.
 * @param h1  first hash word (its low bits pick the table bucket; the top
 *            bits used here are independent of them)
 * @returns the block index.
 */
static inline uint64_t sbbf_block(uint64_t h1) { return ((h1 >> 32) * g_sbbf_blocks) >> 32; }

/**
 * Inserts a key hash: one bit in each of the block's eight 32-bit lanes,
 * lane i's bit = (x * salt[i]) >> 27 with x = the low 32 bits of h2 (the
 * split-block Bloom filter of Apache Parquet).
 * @param h  the key's SipHash-128 (as kv_store_hash returns it)
 * @remarks O(1) and cheap enough for a create to maintain; deletes cannot
 *          clear bits, so a delete-heavy store rebuilds it periodically.
 */
static void sbbf_add(const uint64_t h[2]) {
  uint32_t *blk = g_sbbf + sbbf_block(h[0]) * 8, x = (uint32_t)h[1];
  for (int i = 0; i < 8; i++) blk[i] |= 1u << ((x * kSalt[i]) >> 27);
}

/**
 * Scalar query: are all eight bits set?
 * @param h  the key's SipHash-128
 * @returns 1 = maybe present, 0 = definitely absent (never a false negative).
 */
static int sbbf_query_scalar(const uint64_t h[2]) {
  const uint32_t *blk = g_sbbf + sbbf_block(h[0]) * 8;
  uint32_t x = (uint32_t)h[1], miss = 0;
  for (int i = 0; i < 8; i++) miss |= ~blk[i] & (1u << ((x * kSalt[i]) >> 27));
  return miss == 0;
}

/**
 * AVX2 query: the eight lane masks from one VPMULLD, VPSRLD and VPSLLVD,
 * checked against the block with one VPTEST.
 * @param h  the key's SipHash-128
 * @returns 1 = maybe present, 0 = definitely absent.
 */
__attribute__((target("avx2"))) static int sbbf_query_avx2(const uint64_t h[2]) {
  const __m256i salt = _mm256_loadu_si256((const __m256i *)(const void *)kSalt);
  __m256i m = _mm256_mullo_epi32(_mm256_set1_epi32((int)(uint32_t)h[1]), salt);
  m = _mm256_sllv_epi32(_mm256_set1_epi32(1), _mm256_srli_epi32(m, 27));
  __m256i blk = _mm256_load_si256((const __m256i *)(const void *)(g_sbbf + sbbf_block(h[0]) * 8));
  return _mm256_testc_si256(blk, m);   /* CF = 1 iff every mask bit is set in the block */
}

static binary_fuse8_t g_bff;

/* ========================================================================= */
/* Ordered snapshot                                                           */
/* ========================================================================= */

/** A key normalized for ordering: the 16 bytes after the common prefix, big-endian. */
typedef struct {
  uint64_t hi, lo;
} NKey;

/** A key being sorted: its normalized form and id. */
typedef struct {
  uint64_t hi, lo;
  uint32_t id, pad;
} SortRec;

static uint32_t g_lcp;           /* length of the prefix every key shares ("user:") */
static uint32_t *g_sorted;       /* key ids in byte order */
static NKey *g_skey;             /* normalized keys in byte order */
static const KvRec **g_srec;     /* records in key order (range scans with values) */
static NKey *g_eyt;              /* Eytzinger array: [0] sentinel, [1 .. n] keys */
static uint32_t *g_eyt_rank;     /* rank of each Eytzinger slot; [0] = n */
static uint32_t g_eyt_h;         /* levels of the Eytzinger tree */

/**
 * Normalizes a byte string: bytes [lcp, lcp + 16) as two big-endian words,
 * zero-padded, so integer comparison equals memcmp order.
 * @param k  bytes  @param len  length
 * @returns the normalized key.
 * @remarks Exact here because every key fits in lcp + 16 bytes (the build
 *          checks that no two keys normalize equal). Longer keys need a
 *          tie-break on the full key, and a probe whose first lcp bytes
 *          differ from the common prefix is answered by that comparison
 *          alone (rank 0 or n) before any search.
 */
static NKey nkey(const uint8_t *k, uint32_t len) {
  uint8_t b[16] = {0};
  uint64_t w0, w1;
  if (len > g_lcp) memcpy(b, k + g_lcp, len - g_lcp < 16 ? len - g_lcp : 16);
  memcpy(&w0, b, 8);
  memcpy(&w1, b + 8, 8);
  return (NKey){__builtin_bswap64(w0), __builtin_bswap64(w1)};
}

/** Branch-free "a < b" on normalized keys. */
static inline int nk_less(NKey a, NKey b) { return (a.hi < b.hi) | ((a.hi == b.hi) & (a.lo < b.lo)); }

/**
 * Sort order of SortRecs: normalized words, then the full keys on a tie.
 * @param x  SortRec*  @param y  SortRec*
 * @returns <0, 0 or >0.
 */
static int cmp_sortrec(const void *x, const void *y) {
  const SortRec *a = x, *b = y;
  if (a->hi != b->hi) return a->hi < b->hi ? -1 : 1;
  if (a->lo != b->lo) return a->lo < b->lo ? -1 : 1;
  return key_cmp(a->id, b->id);
}

/**
 * Textbook lower bound: binary search over sorted ids, every comparison
 * dereferencing a key in the arena (two dependent misses per level).
 * @param t  target bytes  @param tl  its length
 * @returns the rank of the first key >= target (n if none).
 */
static uint32_t plain_lower_bound(const uint8_t *t, uint32_t tl) {
  uint32_t lo = 0, hi = g_n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2, id = g_sorted[mid], l = alen(id);
    int c = memcmp(akey(id), t, l < tl ? l : tl);
    if (c < 0 || (c == 0 && l < tl)) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/**
 * Branchless lower bound over the inline normalized keys: the comparison
 * becomes a CMOV, and both possible next midpoints are prefetched.
 * @param t  normalized target
 * @returns the rank of the first key >= target (n if none).
 */
static uint32_t bl_lower_bound(NKey t) {
  const NKey *base = g_skey;
  uint32_t len = g_n;
  while (len > 1) {
    uint32_t half = len / 2;
    len -= half;
    __builtin_prefetch(&base[len / 2 - 1]);
    __builtin_prefetch(&base[half + len / 2 - 1]);
    base += nk_less(base[half - 1], t) ? half : 0;
  }
  return (uint32_t)(base - g_skey) + (uint32_t)nk_less(*base, t);
}

/**
 * In-order fill of the Eytzinger array: node k's children are 2k and 2k+1,
 * so the first levels, which every search reads, share a few cache lines.
 * @param k  node   @param i  next sorted rank to place
 * @returns the next rank after this subtree.
 */
static uint32_t eyt_fill(uint32_t k, uint32_t i) {
  if (k <= g_n) {
    i = eyt_fill(2 * k, i);
    g_eyt[k] = g_skey[i];
    g_eyt_rank[k] = i;
    i = eyt_fill(2 * k + 1, i + 1);
  }
  return i;
}

/**
 * Eytzinger lower bound (Khuong & Morin): a branch-free descent that
 * prefetches the 16 descendants four levels down (nodes 16k..16k+15 are
 * contiguous: 256 bytes = 4 lines) at every step.
 * @param t  normalized target
 * @returns the rank of the first key >= target (n if none).
 * @remarks After the loop, k's bits are the path taken (1 = right); the
 *          answer is the last node where the search went left, found by
 *          shifting off the trailing right turns and that left turn. k = 0
 *          means the search never went left: every key is < target.
 *          Prefetches past the array are harmless (they never fault).
 */
static uint32_t eyt_lower_bound(NKey t) {
  uint32_t k = 1;
  while (k <= g_n) {
    const char *p = (const char *)(g_eyt + 16 * (uint64_t)k);
    __builtin_prefetch(p);
    __builtin_prefetch(p + 64);
    __builtin_prefetch(p + 128);
    __builtin_prefetch(p + 192);
    k = 2 * k + (uint32_t)nk_less(g_eyt[k], t);
  }
  k >>= __builtin_ffs((int)~k);
  return g_eyt_rank[k];
}

/**
 * The same for IBATCH targets at once, level by level: sixteen independent
 * descents keep sixteen misses in flight (group prefetching). Each step
 * prefetches the node's grandchildren (4k..4k+3 = exactly one 64-byte line).
 * @param t    IBATCH normalized targets
 * @param out  receives IBATCH ranks
 * @remarks Every lane runs g_eyt_h steps; a lane whose node no longer
 *          exists (k > n) stays put, reading the sentinel slot 0 instead,
 *          so the loop has no data-dependent exit and compiles to CMOVs.
 */
static void eyt_lower_bound_x16(const NKey *t, uint32_t *out) {
  uint32_t k[IBATCH];
  for (int q = 0; q < IBATCH; q++) k[q] = 1;
  for (uint32_t s = 0; s < g_eyt_h; s++) {
    for (int q = 0; q < IBATCH; q++) __builtin_prefetch(g_eyt + 4 * (uint64_t)k[q]);
    for (int q = 0; q < IBATCH; q++) {
      uint32_t v = k[q], live = v <= g_n;
      uint32_t nx = 2 * v + (uint32_t)nk_less(g_eyt[live ? v : 0], t[q]);
      k[q] = live ? nx : v;
    }
  }
  for (int q = 0; q < IBATCH; q++) out[q] = g_eyt_rank[k[q] >> __builtin_ffs((int)~k[q])];
}

/* ========================================================================= */
/* SymSpell at 10M                                                            */
/* ========================================================================= */

#define M61 ((1ull << 61) - 1)
static uint64_t g_pb;                   /* secret polynomial base in [2, M61 - 3] */
static uint64_t g_pow[KEYMAX + 2];      /* g_pb^i mod M61 */

/**
 * One 64-byte SymSpell bucket: eight lanes of (fingerprint | key length)
 * and the matching keys' arena offsets.
 * @remarks A zero lane is empty. Storing the arena offset (not the key id)
 *          means a candidate costs exactly one arena access; the length
 *          rides in the lane's low 5 bits so candidates of the wrong length
 *          are dropped without touching the key.
 */
typedef struct {
  uint32_t fp[8];
  uint32_t off[8];
} SymBucket;

static SymBucket *g_symb;
static uint64_t g_symb_n;
static size_t g_symb_bytes;

/** (a * b) mod 2^61 - 1, with the Mersenne-prime fold. */
static inline uint64_t mulmod61(uint64_t a, uint64_t b) {
  __uint128_t z = (__uint128_t)a * b;
  uint64_t r = ((uint64_t)z & M61) + (uint64_t)(z >> 61);
  return r >= M61 ? r - M61 : r;
}

/** (a + b) mod 2^61 - 1 for a, b < 2^61 - 1. */
static inline uint64_t addmod61(uint64_t a, uint64_t b) {
  uint64_t r = a + b;
  return r >= M61 ? r - M61 : r;
}

/**
 * Mixes a polynomial hash and a length into 64 well-spread bits.
 * @param h  hash (< 2^61)  @param len  string length
 * @returns the mixed word: the top 32 bits pick the bucket, bits 5-31 are
 *          the fingerprint.
 */
static inline uint64_t var_mix(uint64_t h, uint32_t len) {
  uint64_t x = h ^ ((uint64_t)len << 58);
  x ^= x >> 31;
  x *= 0x7FB5D329728EA185ull;
  x ^= x >> 27;
  x *= 0x81DADEF4BC2DD44Dull;
  x ^= x >> 33;
  return x;
}

/** Bucket of a mixed variant hash (fastrange of the top 32 bits). */
static inline uint64_t sym_bucket(uint64_t x) { return ((x >> 32) * g_symb_n) >> 32; }

/** Fingerprint of a mixed variant hash, in the lane's top 27 bits (never 0). */
static inline uint32_t sym_fp(uint64_t x) {
  uint32_t f = (uint32_t)x & ~31u;
  return f ? f : 32u;
}

/**
 * Mixed hashes of a string and of each of its single-byte deletions, all in
 * O(len): with H(s) = sum (s[i]+1) B^(len-1-i) mod 2^61 - 1, prefix hashes
 * P and suffix hashes S give H(delete(s, j)) = P[j] * B^(len-1-j) + S[j+1].
 * @param s    bytes  @param len  length (1 .. KEYMAX)
 * @param out  receives the hashes: [0] = s itself, then the deletions
 * @returns how many hashes were written (a deletion equal to its left
 *          neighbour's, i.e. inside a run of one byte, is skipped).
 * @remarks Keyed by the secret base B this is a universal hash: two fixed
 *          different strings collide with probability <= len / 2^61. A
 *          collision only adds a candidate, which verification rejects.
 */
static int variant_hashes(const uint8_t *s, uint32_t len, uint64_t *out) {
  uint64_t P[KEYMAX + 1], S[KEYMAX + 1];
  P[0] = 0;
  for (uint32_t i = 0; i < len; i++) P[i + 1] = addmod61(mulmod61(P[i], g_pb), s[i] + 1u);
  S[len] = 0;
  for (uint32_t i = len; i-- > 0;) S[i] = addmod61(mulmod61(s[i] + 1u, g_pow[len - 1 - i]), S[i + 1]);
  int n = 0;
  out[n++] = var_mix(P[len], len);
  for (uint32_t j = 0; j < len; j++) {
    if (j > 0 && s[j] == s[j - 1]) continue;
    out[n++] = var_mix(addmod61(mulmod61(P[j], g_pow[len - 1 - j]), S[j + 1]), len - 1);
  }
  return n;
}

/**
 * Inserts one variant: the first lane free in its bucket or the buckets
 * after it (linear probing by whole lines).
 * @param x    mixed variant hash
 * @param off  arena offset of the key   @param len  key length (< 32)
 */
static void sym_insert(uint64_t x, uint32_t off, uint32_t len) {
  uint64_t b = sym_bucket(x);
  uint32_t lane = sym_fp(x) | len;
  for (;;) {
    SymBucket *B = &g_symb[b];
    for (int l = 0; l < 8; l++)
      if (!B->fp[l]) {
        B->fp[l] = lane;
        B->off[l] = off;
        return;
      }
    b = b + 1 == g_symb_n ? 0 : b + 1;
  }
}

/** Myers/Hyyrö bit-parallel pattern (length <= 64). */
typedef struct {
  uint64_t peq[256];
  int m;
} Myers;

/**
 * Prepares a Myers pattern.
 * @param my  pattern state  @param p  bytes  @param m  length (1 .. 64)
 */
static void myers_init(Myers *my, const uint8_t *p, int m) {
  memset(my->peq, 0, sizeof my->peq);
  for (int i = 0; i < m; i++) my->peq[p[i]] |= 1ull << i;
  my->m = m;
}

/**
 * Exact Levenshtein distance with one 64-bit column update per text byte
 * (the global form: a 1 is shifted into the horizontal delta each step).
 * @param my  prepared pattern  @param t  text  @param n  its length
 * @returns the edit distance.
 */
static int myers_dist(const Myers *my, const uint8_t *t, int n) {
  uint64_t pv = my->m == 64 ? ~0ull : (1ull << my->m) - 1, mv = 0, high = 1ull << (my->m - 1);
  int score = my->m;
  for (int j = 0; j < n; j++) {
    uint64_t eq = my->peq[t[j]], xv = eq | mv, xh = (((eq & pv) + pv) ^ pv) | eq;
    uint64_t ph = mv | ~(xh | pv), mh = pv & xh;
    if (ph & high) score++;
    else if (mh & high) score--;
    ph = (ph << 1) | 1;
    mh <<= 1;
    pv = mh | ~(xv | ph);
    mv = ph & xv;
  }
  return score;
}

/**
 * Textbook Levenshtein DP: the reference Myers is checked against.
 * @param a  bytes  @param la  length  @param b  bytes  @param lb  length
 * @returns the edit distance.
 */
static int lev_dp(const uint8_t *a, int la, const uint8_t *b, int lb) {
  int prev[KEYMAX + 1], cur[KEYMAX + 1];
  for (int j = 0; j <= lb; j++) prev[j] = j;
  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    for (int j = 1; j <= lb; j++) {
      int sub = prev[j - 1] + (a[i - 1] != b[j - 1]), del = prev[j] + 1, ins = cur[j - 1] + 1;
      cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
    }
    memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
  }
  return prev[lb];
}

/**
 * Counts the keys within edit distance 1 of q.
 * @param q  query bytes  @param m  length (1 .. KEYMAX)
 * @param grouped  1 = prefetch every variant's bucket first, then buffer the
 *                 candidates and prefetch their keys before verifying (two
 *                 overlapped rounds of misses); 0 = probe and verify in turn
 * @returns the number of distinct matching keys.
 * @remarks Each bucket is scanned with two AVX2 compares (fingerprint hits,
 *          and empty lanes); an empty lane ends the probe chain because
 *          inserts fill the first bucket with room and nothing is deleted.
 *          Candidates are de-duplicated by arena offset in a small list;
 *          more than MAX_CAND distinct candidates aborts the run rather
 *          than risk a wrong count.
 */
__attribute__((target("avx2"))) static uint32_t sym_query(const uint8_t *q, uint32_t m, int grouped) {
  uint64_t vx[KEYMAX + 1], bk[KEYMAX + 1];
  uint32_t seen[MAX_CAND], ns = 0, cand[MAX_CAND], clen[MAX_CAND], nc = 0, count = 0;
  int nv = variant_hashes(q, m, vx);
  for (int v = 0; v < nv; v++) {
    bk[v] = sym_bucket(vx[v]);
    if (grouped) __builtin_prefetch(&g_symb[bk[v]]);
  }
  Myers my;
  myers_init(&my, q, (int)m);
  const __m256i fpmask = _mm256_set1_epi32((int)~31u), zero = _mm256_setzero_si256();
  for (int v = 0; v < nv; v++) {
    const __m256i want = _mm256_set1_epi32((int)sym_fp(vx[v]));
    for (uint64_t b = bk[v];;) {
      const SymBucket *B = &g_symb[b];
      __m256i f = _mm256_load_si256((const __m256i *)(const void *)B->fp);
      uint32_t hit = (uint32_t)_mm256_movemask_ps(
          _mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_and_si256(f, fpmask), want)));
      uint32_t open = (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(f, zero)));
      for (; hit; hit &= hit - 1) {
        int lane = __builtin_ctz(hit);
        uint32_t off = B->off[lane], l = B->fp[lane] & 31u, dup = 0;
        if (l > m + 1 || l + 1 < m) continue;   /* lengths 2+ apart: distance >= 2 */
        for (uint32_t i = 0; i < ns; i++) dup |= seen[i] == off;
        if (dup) continue;
        if (ns == MAX_CAND) die("a typo query collected more candidates than MAX_CAND");
        seen[ns++] = off;
        if (grouped) {
          __builtin_prefetch(g_arena + off);
          cand[nc] = off;
          clen[nc++] = l;
        } else {
          count += myers_dist(&my, g_arena + off, (int)l) <= 1;
        }
      }
      if (open) break;
      b = b + 1 == g_symb_n ? 0 : b + 1;
    }
  }
  for (uint32_t c = 0; c < nc; c++) count += myers_dist(&my, g_arena + cand[c], (int)clen[c]) <= 1;
  return count;
}

/* ========================================================================= */
/* Sketches: HyperLogLog (O(1) add and estimate) and Count-Min               */
/* ========================================================================= */

enum { HLL_P = 14, HLL_M = 1 << HLL_P };
static uint8_t g_hll[HLL_M] __attribute__((aligned(32)));
static uint8_t g_hll2[HLL_M] __attribute__((aligned(32)));
static double g_hll_sum;         /* sum over registers of 2^-register, kept current */
static uint32_t g_hll_zeros;     /* registers still 0 (linear counting) */
static double g_pow2neg[66];

/**
 * Adds a 64-bit hash: register = top 14 bits, rank = leading zeros of the
 * other 50 + 1; the harmonic sum and zero count are updated in place, so an
 * estimate never needs a pass over the 16,384 registers.
 * @param h  the key's hash (reused from the table lookup)
 * @remarks The kept sum only ever adds differences of exact powers of two;
 *          the probe compares it with full recomputations.
 */
static inline void hll_add(uint64_t h) {
  uint32_t idx = (uint32_t)(h >> (64 - HLL_P));
  uint8_t rho = (uint8_t)(__builtin_clzll((h << HLL_P) | (1ull << (HLL_P - 1))) + 1);
  uint8_t old = g_hll[idx];
  if (rho > old) {
    g_hll_sum += g_pow2neg[rho] - g_pow2neg[old];
    g_hll_zeros -= old == 0;
    g_hll[idx] = rho;
  }
}

/**
 * The HyperLogLog estimate in O(1) from the kept sum, with the standard
 * small-range (linear counting) correction.
 * @param sum    harmonic sum  @param zeros  zero registers
 * @returns the cardinality estimate.
 */
static double hll_estimate(double sum, uint32_t zeros) {
  double m = HLL_M, e = 0.7213 / (1 + 1.079 / m) * m * m / sum;
  if (e <= 2.5 * m && zeros) e = m * log(m / zeros);
  return e;
}

/**
 * Raises registers for a hash without keeping a sum: builds a second,
 * independent sketch (the union check and benchmark need one).
 * @param reg  HLL_M registers  @param h  64-bit hash
 */
static inline void hll_raise(uint8_t *reg, uint64_t h) {
  uint32_t idx = (uint32_t)(h >> (64 - HLL_P));
  uint8_t rho = (uint8_t)(__builtin_clzll((h << HLL_P) | (1ull << (HLL_P - 1))) + 1);
  if (rho > reg[idx]) reg[idx] = rho;
}

/**
 * Adds 2^-r for 32 registers to two double accumulators and returns how
 * many of them are 0. 2^-r is built directly as a float whose exponent
 * field is 127 - r (VPMOVZXBD, VPSUBD, VPSLLD: no table, no division); the
 * 32 terms are summed in float to one vector, then widened and added.
 * @param v   32 registers
 * @param a0  accumulator for lanes 0-3   @param a1  accumulator for lanes 4-7
 * @returns the number of zero registers in v (VPCMPEQB, VPMOVMSKB, POPCNT).
 * @remarks Summing 4 terms in float first rounds at 2^-24 of the group's
 *          sum, far below HLL's own 0.8% standard error; it is what lets
 *          one double add per accumulator per 32 registers keep the 4-cycle
 *          add latency off the critical path. A first version that added
 *          every 8 registers into two accumulators was latency-bound: merge
 *          + sum took 1.53 us in a 1M-key smoke run, where copy + merge +
 *          this sum took 1.05 us.
 */
__attribute__((target("avx2,popcnt"))) static inline uint32_t hll_vec_sum(__m256i v, __m256d *a0, __m256d *a1) {
  const __m256i bias = _mm256_set1_epi32(127);
  __m128i lo = _mm256_castsi256_si128(v), hi = _mm256_extracti128_si256(v, 1);
  __m256i r[4] = {_mm256_cvtepu8_epi32(lo), _mm256_cvtepu8_epi32(_mm_srli_si128(lo, 8)), _mm256_cvtepu8_epi32(hi),
                  _mm256_cvtepu8_epi32(_mm_srli_si128(hi, 8))};
  __m256 p[4];
  for (int j = 0; j < 4; j++) p[j] = _mm256_castsi256_ps(_mm256_slli_epi32(_mm256_sub_epi32(bias, r[j]), 23));
  __m256 s = _mm256_add_ps(_mm256_add_ps(p[0], p[1]), _mm256_add_ps(p[2], p[3]));
  *a0 = _mm256_add_pd(*a0, _mm256_cvtps_pd(_mm256_castps256_ps128(s)));
  *a1 = _mm256_add_pd(*a1, _mm256_cvtps_pd(_mm256_extractf128_ps(s, 1)));
  return (uint32_t)__builtin_popcount((uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, _mm256_setzero_si256())));
}

/**
 * Harmonic sum and zero count of a register array in one AVX2 pass.
 * @param reg    registers (32-byte aligned)
 * @param m      how many (a multiple of 32; HLL_M for this probe's sketch)
 * @param zeros  receives the number of zero registers
 * @returns the harmonic sum, within ~2e-7 relative of the exact one.
 */
__attribute__((target("avx2,popcnt"))) static double hll_sum_avx2(const uint8_t *reg, int m, uint32_t *zeros) {
  __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
  uint32_t z = 0;
  for (int i = 0; i < m; i += 32) z += hll_vec_sum(_mm256_load_si256((const __m256i *)(const void *)(reg + i)), &a0, &a1);
  double l[4];
  _mm256_storeu_pd(l, _mm256_add_pd(a0, a1));
  *zeros = z;
  return (l[0] + l[1]) + (l[2] + l[3]);
}

/**
 * Union of two sketches into a third (PFMERGE dst a b) in one fused AVX2
 * pass: VPMAXUB of 32 registers, the store, and their share of the new
 * harmonic sum while they are still in a register.
 * @param dst    receives the union (32-byte aligned; may be a or b)
 * @param a      registers  @param b  registers
 * @param m      registers per sketch (a multiple of 32)
 * @param zeros  receives the union's zero-register count
 * @returns the union's harmonic sum, ready for hll_estimate.
 * @remarks Fusing replaces copy + merge + sum (three passes over 16 KB)
 *          with one, yet measured no faster (1.07 vs 1.05 us in a 1M-key
 *          smoke run): the pass is compute-bound on the sum, not on memory
 *          (by instruction mix most likely port 5: VPMOVZXBD, VCVTPS2PD;
 *          WSL2 has no PMU to confirm it). The real lever is the sketch
 *          size: cost grows with 2^p, error shrinks with 2^(-p/2).
 *          No branch depends on register values, so the time depends on m
 *          only, which lets the probe time a smaller sketch on a prefix.
 */
__attribute__((target("avx2,popcnt"))) static double hll_union_avx2(uint8_t *dst, const uint8_t *a, const uint8_t *b,
                                                                     int m, uint32_t *zeros) {
  __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
  uint32_t z = 0;
  for (int i = 0; i < m; i += 32) {
    __m256i m = _mm256_max_epu8(_mm256_load_si256((const __m256i *)(const void *)(a + i)),
                                _mm256_load_si256((const __m256i *)(const void *)(b + i)));
    _mm256_store_si256((__m256i *)(void *)(dst + i), m);
    z += hll_vec_sum(m, &a0, &a1);
  }
  double l[4];
  _mm256_storeu_pd(l, _mm256_add_pd(a0, a1));
  *zeros = z;
  return (l[0] + l[1]) + (l[2] + l[3]);
}

enum { CMS_D = 4, CMS_W = 1 << 21 };
static uint32_t *g_cms;          /* CMS_D rows of CMS_W counters (32 MB) */

/**
 * Column of row i for a key, by double hashing (Kirsch & Mitzenmacher).
 * @param h  the key's SipHash-128  @param i  row
 * @returns the column.
 */
static inline uint32_t cms_col(const uint64_t h[2], int i) {
  return (uint32_t)((h[0] + (uint64_t)i * h[1]) >> 11) & (CMS_W - 1);
}

/* ========================================================================= */
/* Concurrent readers and writers                                             */
/* ========================================================================= */

/** One reader thread's assignment. */
typedef struct {
  int cpu, slot;
  uint32_t ops, offset;
  pthread_barrier_t *start;
} RJob;

/**
 * Reader thread: batched exists over the query stream, QSBR-online and
 * quiescent after every batch (as a worker is between event batches).
 * @param arg  RJob*
 * @returns NULL.
 */
static void *reader_thread(void *arg) {
  RJob *j = arg;
  pin(j->cpu);
  kv_qsbr_online(g_store, (uint32_t)j->slot);
  pthread_barrier_wait(j->start);
  uint64_t h[BATCH][2], found = 0;
  for (uint32_t b = 0; b < j->ops; b += BATCH) {
    for (int k = 0; k < BATCH; k++) {
      uint32_t q = (j->offset + b + (uint32_t)k) & (NQ - 1);
      kv_store_hash(g_store, g_qkey[q], g_qlen[q], h[k]);
      kv_store_prefetch(g_store, h[k][0], h[k][1]);
    }
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      uint32_t q = (j->offset + b + (uint32_t)k) & (NQ - 1);
      found += kv_store_get(g_store, g_qkey[q], g_qlen[q], h[k][0], h[k][1]) != NULL;
    }
    kv_qsbr_quiescent(g_store, (uint32_t)j->slot);
  }
  if (found != j->ops) die("a concurrent reader missed a key");
  kv_qsbr_offline(g_store, (uint32_t)j->slot);
  return NULL;
}

/** One writer thread's assignment. */
typedef struct {
  int cpu;
  uint32_t max_ops, offset;
  pthread_barrier_t *start;
  uint32_t done;              /* out: puts completed */
} WJob;

static _Atomic int g_stop;       /* tells open-ended writers to finish */

/**
 * Writer thread: replace-Puts over the query stream until max_ops or g_stop
 * (the serialized writer path of ADR 0003).
 * @param arg  WJob*
 * @returns NULL.
 * @remarks Writers are not QSBR readers (they never keep a record), so
 *          they need no reader slot.
 */
static void *writer_thread(void *arg) {
  WJob *j = arg;
  pin(j->cpu);
  pthread_barrier_wait(j->start);
  uint8_t val[VAL_LEN];
  uint32_t i = 0;
  for (; i < j->max_ops && !atomic_load_explicit(&g_stop, memory_order_relaxed); i++) {
    uint32_t q = (j->offset + i) & (NQ - 1);
    uint64_t h[2];
    memset(val, (int)(i & 0xFF), VAL_LEN);
    kv_store_hash(g_store, g_qkey[q], g_qlen[q], h);
    if (kv_store_put(g_store, g_qkey[q], g_qlen[q], val, VAL_LEN, h[0], h[1]) != 0)
      die("a concurrent replace-put did not replace");
  }
  j->done = i;
  return NULL;
}

/**
 * Runs `nr` reader threads and `nw` writer threads together.
 * @param nr    readers (CPUs g_tcpu[0 .. nr))   @param per  lookups per reader
 * @param nw    writers (the CPUs after the readers)
 * @param wmax  puts per writer, or UINT32_MAX = until the readers finish
 * @param puts  receives the total puts done (may be NULL)
 * @returns wall ns from the common start to the readers' end, or to the
 *          writers' end when there are no readers.
 */
static uint64_t run_mix(int nr, uint32_t per, int nw, uint32_t wmax, uint64_t *puts) {
  pthread_barrier_t start;
  pthread_t rt[MAX_THREADS], wt[MAX_THREADS];
  RJob rj[MAX_THREADS];
  WJob wj[MAX_THREADS];
  pthread_barrier_init(&start, NULL, (unsigned)(nr + nw) + 1);
  atomic_store(&g_stop, 0);
  for (int i = 0; i < nr; i++) {
    rj[i] = (RJob){g_tcpu[i], 1 + i, per, (uint32_t)i * (NQ / MAX_THREADS), &start};
    if (pthread_create(&rt[i], NULL, reader_thread, &rj[i]) != 0) die("pthread_create");
  }
  for (int i = 0; i < nw; i++) {
    wj[i] = (WJob){g_tcpu[nr + i], wmax, (uint32_t)(nr + i) * (NQ / MAX_THREADS), &start, 0};
    if (pthread_create(&wt[i], NULL, writer_thread, &wj[i]) != 0) die("pthread_create");
  }
  kv_qsbr_offline(g_store, 0);   /* the main thread only waits: it must not hold back reclamation */
  pthread_barrier_wait(&start);
  uint64_t t0 = now_ns();
  for (int i = 0; i < nr; i++) pthread_join(rt[i], NULL);
  uint64_t wall = now_ns() - t0;
  if (wmax == UINT32_MAX) atomic_store(&g_stop, 1);   /* open-ended writers stop with the readers */
  for (int i = 0; i < nw; i++) pthread_join(wt[i], NULL);
  if (nr == 0) wall = now_ns() - t0;
  kv_qsbr_online(g_store, 0);
  pthread_barrier_destroy(&start);
  uint64_t total = 0;
  for (int i = 0; i < nw; i++) total += wj[i].done;
  if (puts) *puts = total;
  return wall;
}

/* ========================================================================= */
/* Single-thread writes                                                       */
/* ========================================================================= */

static uint8_t (*g_wkey)[KEYMAX];   /* NW distinct keys for delete/create passes */
static uint32_t *g_wlen;

/**
 * Loads key set `set` into g_wkey: NW distinct existing keys, disjoint from
 * every other set (ids (set * NW + i) * WSTRIDE mod n; WSTRIDE is prime and
 * does not divide n, which main checks).
 * @param set  set number (set * NW + NW <= n)
 */
static void load_wkeys(uint32_t set) {
  for (uint32_t i = 0; i < NW; i++)
    g_wlen[i] = (uint32_t)make_key((uint32_t)(((uint64_t)set * NW + i) * WSTRIDE % g_n), g_wkey[i]);
}

/**
 * Deletes or re-creates every key of g_wkey.
 * @param create   0 = delete (each must exist), 1 = create (each must be new)
 * @param batched  1 = per batch of 32: hash and prefetch both buckets (and,
 *                 for deletes, the records to compare), then write each
 * @returns total ns.
 * @remarks Quiescent after every batch, as a worker would be, so retired
 *          records are reclaimed as the pass goes.
 */
static uint64_t write_pass(int create, int batched) {
  uint8_t val[VAL_LEN];
  uint64_t h[BATCH][2], t0 = now_ns();
  memset(val, 0x5A, VAL_LEN);
  for (uint32_t b = 0; b < NW; b += BATCH) {
    if (batched) {
      for (int k = 0; k < BATCH; k++) {
        kv_store_hash(g_store, g_wkey[b + (uint32_t)k], g_wlen[b + (uint32_t)k], h[k]);
        kv_store_prefetch(g_store, h[k][0], h[k][1]);
      }
      if (!create)
        for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    }
    for (int k = 0; k < BATCH; k++) {
      const uint8_t *key = g_wkey[b + (uint32_t)k];
      uint32_t l = g_wlen[b + (uint32_t)k];
      if (!batched) kv_store_hash(g_store, key, l, h[k]);
      int64_t rc = create ? kv_store_put(g_store, key, l, val, VAL_LEN, h[k][0], h[k][1])
                          : kv_store_delete(g_store, key, l, h[k][0], h[k][1]);
      if (rc != 1) die(create ? "create did not create" : "delete did not delete");
    }
    kv_qsbr_quiescent(g_store, 0);
  }
  return now_ns() - t0;
}

/**
 * Replace-Puts over NW keys of the query stream.
 * @param window   which NW-key window of the stream (keeps passes apart)
 * @param batched  1 = prefetch buckets and old records per batch of 32
 * @returns total ns.
 */
static uint64_t replace_pass(uint32_t window, int batched) {
  uint8_t val[VAL_LEN];
  uint64_t h[BATCH][2], t0 = now_ns();
  memset(val, 0x3C, VAL_LEN);
  for (uint32_t b = 0; b < NW; b += BATCH) {
    uint32_t base = (window * NW + b) & (NQ - 1);
    if (batched) {
      for (int k = 0; k < BATCH; k++) {
        kv_store_hash(g_store, g_qkey[base + (uint32_t)k], g_qlen[base + (uint32_t)k], h[k]);
        kv_store_prefetch(g_store, h[k][0], h[k][1]);
      }
      for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
    }
    for (int k = 0; k < BATCH; k++) {
      const uint8_t *key = g_qkey[base + (uint32_t)k];
      uint32_t l = g_qlen[base + (uint32_t)k];
      if (!batched) kv_store_hash(g_store, key, l, h[k]);
      if (kv_store_put(g_store, key, l, val, VAL_LEN, h[k][0], h[k][1]) != 0) die("replace did not replace");
    }
    kv_qsbr_quiescent(g_store, 0);
  }
  return now_ns() - t0;
}

/* ========================================================================= */
/* Driver                                                                     */
/* ========================================================================= */

/**
 * Builds the 10M-key store and every structure, verifies each, and prints
 * every operation against the exact-lookup bars.
 * @param argc  argument count
 * @param argv  [--keys N] [--cpu C] [--reps R] [--threads "1,2,4,7"] [--bar-only]
 * @returns 0; exits 1 on a failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  int cpu = 4, reps = 5, nthr = 0, tcounts[MAX_THREADS], bar_only = 0;
  const char *tl = "1,2,4,7";
  g_n = 10000000;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--bar-only")) bar_only = 1;
    else if (i + 1 < argc && !strcmp(argv[i], "--keys")) g_n = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (i + 1 < argc && !strcmp(argv[i], "--cpu")) cpu = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--reps")) reps = atoi(argv[++i]);
    else if (i + 1 < argc && !strcmp(argv[i], "--threads")) tl = argv[++i];
    else g_n = 0;   /* unknown argument: fall into the usage message */
  }
  for (const char *p = tl; *p && nthr < MAX_THREADS;) {
    tcounts[nthr] = atoi(p);
    if (tcounts[nthr] < 1 || tcounts[nthr] > MAX_THREADS) g_n = 0;
    nthr++;
    while (*p && *p != ',') p++;
    if (*p == ',') p++;
  }
  /* 2 * reps disjoint write-key sets of NW keys must fit in the key space. */
  if (g_n < 1000000 || g_n % FIELDS || g_n % WSTRIDE == 0 || g_n > 12000000 || reps < 1 || reps > MAX_REPS ||
      nthr < 1 || (uint64_t)2 * (uint64_t)reps * NW > g_n) {
    fprintf(stderr,
            "usage: golden_probe [--keys N (multiple of 4, 1000000..12000000, >= 2 * reps * %d)] [--cpu C]\n"
            "                    [--reps 1..%d] [--threads \"a,b,...\" (each 1..%d)] [--bar-only]\n",
            NW, MAX_REPS, MAX_THREADS);
    return 2;
  }
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("sse4.2") || !__builtin_cpu_supports("aes") ||
      !__builtin_cpu_supports("popcnt"))
    die("this probe's SIMD variants need AVX2, SSE4.2, POPCNT and AES-NI");
  pin(cpu);

  /* ---- the store: n keys with the server's defaults (SSE2 tag scan, capacity = n) ---- */
  if (kv_set_scan_mode(KV_SCAN_SSE2) < 0) die("kv_set_scan_mode");
  g_store = kv_store_create(g_n, MAX_THREADS + 1, kSeed, 0);
  if (!g_store) die("kv_store_create");
  g_start = xcalloc(((size_t)g_n + 1) * sizeof *g_start);
  g_arena = xcalloc((size_t)g_n * 21 + 64);
  uint64_t t0 = now_ns();
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
      off += (uint32_t)len;
    }
    g_start[g_n] = off;
  }
  KvStats st;
  kv_store_stats(g_store, &st);
  printf("keys %u in %llu buckets (load %.1f%% of %llu slots), fill %.1f s, cpu %d, reps %d\n", g_n,
         (unsigned long long)st.nbuckets, 100.0 * g_n / (double)(st.nbuckets * SLOTS),
         (unsigned long long)(st.nbuckets * SLOTS), (double)(now_ns() - t0) / 1e9, cpu, reps);
  kv_qsbr_online(g_store, 0);
  make_queries();
  uint64_t t[MAX_REPS];
  char note[160];

  /* ================= the bar ================= */
  printf("\nTHE BAR: exact lookups at %u keys. \"x\" = ratio to the bar of the same regime\n", g_n);
  printf("(D = dependent chain, I = independent loop, B = batched pipeline); GOLD = within 10x.\n");
  for (int r = 0; r < reps; r++) t[r] = get_dependent(1);
  g_bar[BAR_DEP] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = get_independent(1, 0);
  g_bar[BAR_IND] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = get_batched(1, 0);
  g_bar[BAR_BAT] = med(t, reps, NQ);
  row("get (100-byte value copied)", "dependent chain: pure latency", g_bar[BAR_DEP], BAR_DEP, "bar D");
  row("", "independent loop", g_bar[BAR_IND], BAR_IND, "bar I");
  row("", "batched pipeline (the server's GET path)", g_bar[BAR_BAT], BAR_BAT, "bar B");
  for (int r = 0; r < reps; r++) t[r] = get_dependent(0);
  double ex_dep = med(t, reps, NQ);
  row("exists (hit)", "dependent chain", ex_dep, BAR_DEP, "");
  for (int r = 0; r < reps; r++) t[r] = get_independent(0, 0);
  row("", "independent loop", med(t, reps, NQ), BAR_IND, "");
  for (int r = 0; r < reps; r++) t[r] = get_batched(0, 0);
  row("", "batched pipeline", med(t, reps, NQ), BAR_BAT, "");
  for (int r = 0; r < reps; r++) t[r] = get_independent(0, 1);
  row("exists (absent key)", "independent loop", med(t, reps, NQ), BAR_IND, "both buckets read");
  for (int r = 0; r < reps; r++) t[r] = get_batched(0, 1);
  row("", "batched pipeline", med(t, reps, NQ), BAR_BAT, "");
  printf("  budget (10x): D %.2f us, I %.0f ns, B %.0f ns\n", g_bar[BAR_DEP] * 10 / 1e3, g_bar[BAR_IND] * 10,
         g_bar[BAR_BAT] * 10);
  printf("BARS dep=%.1f ind=%.1f bat=%.1f exists_dep=%.1f\n", g_bar[BAR_DEP], g_bar[BAR_IND], g_bar[BAR_BAT], ex_dep);
  print_memory();
  if (bar_only) return 0;

  /* ================= hashing kernels ================= */
  printf("\nHASHING: per 20-21 byte key (keys in L1; throughput of independent hashes)\n");
  {
    uint64_t acc = 0, k[2], h[2];
    memcpy(k, kSeed, 16);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) {
        kv_siphash128(g_qkey[i & 1023], g_qlen[i & 1023], k, h);
        acc += h[0];
      }
      t[r] = now_ns() - s;
    }
    row("SipHash-2-4, 128-bit", "the store's assembly (siphash.asm)", med(t, reps, NQ), BAR_BAT, "today's hash");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += siphash_cd(g_qkey[i & 1023], g_qlen[i & 1023], k, 2, 4);
      t[r] = now_ns() - s;
    }
    row("SipHash-2-4, 64-bit", "C reference", med(t, reps, NQ), BAR_BAT, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += siphash_cd(g_qkey[i & 1023], g_qlen[i & 1023], k, 1, 3);
      t[r] = now_ns() - s;
    }
    row("SipHash-1-3, 64-bit", "C (Rust's and Python's default)", med(t, reps, NQ), BAR_BAT, "keyed PRF, fewer rounds");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += aes_hash(g_qkey[i & 1023], g_qlen[i & 1023]);
      t[r] = now_ns() - s;
    }
    row("AES-NI, 3 rounds", "AESENC x3", med(t, reps, NQ), BAR_BAT, "keyed, not an analyzed PRF");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += crc32c_key(g_qkey[i & 1023], g_qlen[i & 1023]);
      t[r] = now_ns() - s;
    }
    row("CRC32C", "SSE4.2 CRC32", med(t, reps, NQ), BAR_BAT, "unkeyed: fingerprints only");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += mix_hash(g_qkey[i & 1023], g_qlen[i & 1023]);
      t[r] = now_ns() - s;
    }
    row("multiply-mix", "64x64->128 folding", med(t, reps, NQ), BAR_BAT, "keyed, not an analyzed PRF");
    uint8_t buf[64] = {0}, out[32];
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) {
        memcpy(buf, g_qkey[i & 1023], KEYMAX);
        fold32(buf, out);
        acc += out[i & 31];
      }
      t[r] = now_ns() - s;
    }
    row("ASCII case folding", "AVX2, 32 bytes per step", med(t, reps, NQ), BAR_BAT, "case-insensitive keys");
    g_vsink += acc;
  }

  /* ================= random sampling ================= */
  printf("\nSAMPLING: a uniformly random key (RANDOMKEY), O(1) expected\n");
  {
    uint64_t seed = 0xDEADBEEFCAFEull, acc = 0, fields[4] = {0};
    for (uint32_t i = 0; i < 1000000; i++) {   /* live and evenly spread */
      const KvRec *r = sample_key(&seed);
      uint64_t h[2];
      kv_store_hash(g_store, r->data, r->key_len, h);
      if (kv_store_get(g_store, r->data, r->key_len, h[0], h[1]) != r) die("a sampled record is not current");
      uint8_t last = r->data[r->key_len - 1];
      fields[last == 'e' ? 0 : last == 'l' ? 1 : last == 'n' ? 2 : 3]++;
    }
    for (int f = 0; f < 4; f++)
      if (fabs((double)fields[f] / 1e6 - 0.25) > 0.003) die("samples are not uniform over fields");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) acc += sample_key(&seed)->data[5];
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "fields %.2f/%.2f/%.2f/%.2f%% of 1M samples", fields[0] / 1e4, fields[1] / 1e4,
             fields[2] / 1e4, fields[3] / 1e4);
    row("random key", "rejection sampling over slots", med(t, reps, NQ), BAR_IND, note);
    uint32_t got = 0;
    for (int r = 0; r < reps; r++) t[r] = sample_batched(NQ, seed + (uint64_t)r, &got);
    row("", "batched: 32 slots prefetched per round", med(t, reps, got), BAR_BAT, "");
    g_vsink += acc;
  }

  /* ================= counters ================= */
  printf("\nCOUNTERS: increment in place (lookup + atomic add; no allocation), totals checked exactly\n");
  {
    uint64_t before = counter_sum(0);
    for (int r = 0; r < reps; r++) {
      CJob j = {cpu, 0, 0, 0, C_XADD, NQ, (uint32_t)r * 4096u, NULL};
      uint64_t s = now_ns();
      counter_run(&j);
      t[r] = now_ns() - s;
    }
    if (counter_sum(0) - before != (uint64_t)NQ * (uint64_t)reps) die("single-thread increments lost");
    row("increment, 1 thread", "batched lookup + LOCK XADD", med(t, reps, NQ), BAR_BAT, "random keys");
    static const struct {
      CMode mode;
      int hot;
      const char *what, *how;
    } kCases[] = {
        {C_XADD, 0, "increment, random keys", "LOCK XADD, per op per thread"},
        {C_XADD, 1, "increment, 1,000 hot keys", "LOCK XADD, per op per thread"},
        {C_XADD, 2, "increment, one key", "LOCK XADD, per op per thread"},
        {C_CAS, 2, "", "LOCK CMPXCHG loop, per op per thread"},
        {C_SHARD, 2, "", "per-thread cells (no LOCK), per op"},
    };
    int creps = reps < 3 ? reps : 3;
    uint32_t per = NQ / 4;
    for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
      for (int ti = 0; ti < nthr; ti++) {
        int T = tcounts[ti];
        for (int r = 0; r < creps; r++) t[r] = run_counters(T, kCases[c].mode, kCases[c].hot, per);
        double wall = med(t, creps, 1);
        snprintf(note, sizeof note, "%d threads: %.1f M/s in total", T, (double)per * T / (wall / 1e9) / 1e6);
        row(ti == 0 ? kCases[c].what : "", kCases[c].how, wall / per, BAR_BAT, note);
      }
    }
  }

  /* ================= filters ================= */
  printf("\nFILTERS: \"definitely absent\" before touching the table (key hashing included)\n");
  {
    g_sbbf_blocks = ((uint64_t)g_n * 11 + 255) / 256;   /* ~11 bits per key */
    g_sbbf = big_alloc(g_sbbf_blocks * 32);
    uint64_t *h1s = xcalloc((size_t)g_n * sizeof *h1s);
    uint64_t s = now_ns();
    for (uint32_t i = 0; i < g_n; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, akey(i), alen(i), h);
      sbbf_add(h);
      h1s[i] = h[0];
    }
    double sbbf_build = (double)(now_ns() - s) / g_n;
    for (uint32_t i = 0; i < g_n; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, akey(i), alen(i), h);
      if (!sbbf_query_scalar(h) || !sbbf_query_avx2(h)) die("split-block Bloom false negative");
    }
    s = now_ns();
    if (!binary_fuse8_allocate(g_n, &g_bff) || !binary_fuse8_populate(h1s, g_n, &g_bff)) die("binary fuse build");
    double bff_build = (double)(now_ns() - s) / g_n;
    for (uint32_t i = 0; i < g_n; i++)
      if (!binary_fuse8_contain(h1s[i], &g_bff)) die("binary fuse false negative");
    free(h1s);
    uint32_t fp = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      fp = 0;
      for (uint32_t i = 0; i < NQ; i++) {
        uint64_t h[2];
        kv_store_hash(g_store, g_absent[i], g_absent_len[i], h);
        fp += (uint32_t)sbbf_query_scalar(h);
      }
      t[r] = now_ns() - s2;
    }
    snprintf(note, sizeof note, "%.1f MB, %.2f%% false positives, %.0f ns/key to add",
             g_sbbf_blocks * 32 / 1048576.0, 100.0 * fp / NQ, sbbf_build);
    row("absent test", "split-block Bloom, scalar", med(t, reps, NQ), BAR_IND, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      fp = 0;
      for (uint32_t i = 0; i < NQ; i++) {
        uint64_t h[2];
        kv_store_hash(g_store, g_absent[i], g_absent_len[i], h);
        fp += (uint32_t)sbbf_query_avx2(h);
      }
      t[r] = now_ns() - s2;
    }
    row("", "split-block Bloom, AVX2 (VPTEST)", med(t, reps, NQ), BAR_IND, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns(), h[BATCH][2];
      fp = 0;
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        for (int k = 0; k < BATCH; k++) {
          kv_store_hash(g_store, g_absent[b + (uint32_t)k], g_absent_len[b + (uint32_t)k], h[k]);
          __builtin_prefetch(g_sbbf + sbbf_block(h[k][0]) * 8);
        }
        for (int k = 0; k < BATCH; k++) fp += (uint32_t)sbbf_query_avx2(h[k]);
      }
      t[r] = now_ns() - s2;
    }
    row("", "split-block Bloom, AVX2, batched", med(t, reps, NQ), BAR_BAT, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      fp = 0;
      for (uint32_t i = 0; i < NQ; i++) {
        uint64_t h[2];
        kv_store_hash(g_store, g_absent[i], g_absent_len[i], h);
        fp += binary_fuse8_contain(h[0], &g_bff);
      }
      t[r] = now_ns() - s2;
    }
    snprintf(note, sizeof note, "%.1f MB (%.2f bits/key), %.2f%% false positives, static: %.0f ns/key to build",
             binary_fuse8_size_in_bytes(&g_bff) / 1048576.0, 8.0 * binary_fuse8_size_in_bytes(&g_bff) / g_n,
             100.0 * fp / NQ, bff_build);
    row("", "binary fuse 8 (3 probes)", med(t, reps, NQ), BAR_IND, note);
    binary_fuse8_free(&g_bff);
    big_free(g_sbbf, g_sbbf_blocks * 32);
  }

  /* ================= ordered snapshot ================= */
  printf("\nORDER: a sorted snapshot (rebuilt off the hot path, published by RCU/QSBR; readers never lock)\n");
  {
    uint64_t s0 = now_ns();
    uint32_t mn = 0, mx = 0;   /* the common prefix of all keys = that of the smallest and largest */
    for (uint32_t i = 1; i < g_n; i++) {
      if (key_cmp(i, mn) < 0) mn = i;
      if (key_cmp(i, mx) > 0) mx = i;
    }
    g_lcp = 0;
    while (g_lcp < alen(mn) && g_lcp < alen(mx) && akey(mn)[g_lcp] == akey(mx)[g_lcp]) g_lcp++;
    SortRec *sr = xcalloc((size_t)g_n * sizeof *sr);
    for (uint32_t i = 0; i < g_n; i++) {
      NKey k = nkey(akey(i), alen(i));
      sr[i] = (SortRec){k.hi, k.lo, i, 0};
    }
    qsort(sr, g_n, sizeof *sr, cmp_sortrec);
    double sort_s = (double)(now_ns() - s0) / 1e9;
    g_sorted = xcalloc((size_t)g_n * sizeof *g_sorted);
    size_t skey_bytes = (size_t)g_n * sizeof *g_skey, eyt_bytes = ((size_t)g_n + 1) * sizeof *g_eyt;
    g_skey = big_alloc(skey_bytes);
    for (uint32_t i = 0; i < g_n; i++) {
      g_sorted[i] = sr[i].id;
      g_skey[i] = (NKey){sr[i].hi, sr[i].lo};
      if (i && !nk_less(g_skey[i - 1], g_skey[i])) die("two keys normalize equal: a tie-break is needed");
    }
    free(sr);
    s0 = now_ns();
    g_eyt = big_alloc(eyt_bytes);
    g_eyt_rank = xcalloc(((size_t)g_n + 1) * sizeof *g_eyt_rank);
    g_eyt[0] = (NKey){UINT64_MAX, UINT64_MAX};
    g_eyt_rank[0] = g_n;
    eyt_fill(1, 0);
    g_eyt_h = 32 - (uint32_t)__builtin_clz(g_n);
    double eyt_s = (double)(now_ns() - s0) / 1e9;
    g_srec = xcalloc((size_t)g_n * sizeof *g_srec);
    for (uint32_t i = 0; i < g_n; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, akey(g_sorted[i]), alen(g_sorted[i]), h);
      g_srec[i] = kv_store_get(g_store, akey(g_sorted[i]), alen(g_sorted[i]), h[0], h[1]);
    }
    printf("  build: sort %.2f s, Eytzinger layout %.2f s (common prefix %u bytes, %u levels); "
           "memory: inline keys %.0f MB + Eytzinger %.0f MB + ranks %.0f MB\n",
           sort_s, eyt_s, g_lcp, g_eyt_h, skey_bytes / 1048576.0, eyt_bytes / 1048576.0,
           (g_n + 1) * 4.0 / 1048576.0);
    /* Probes: random keys with their last byte bumped (a missing key's successor). */
    static uint8_t pk[NP][KEYMAX];
    static uint32_t pl[NP];
    static NKey probe[NP];
    uint64_t seed = 99, sink = 0;
    for (uint32_t i = 0; i < NP; i++) {
      uint32_t id = (uint32_t)(rnd(&seed) % g_n);
      pl[i] = alen(id);
      memcpy(pk[i], akey(id), pl[i]);
      pk[i][pl[i] - 1]++;
      probe[i] = nkey(pk[i], pl[i]);
    }
    for (uint32_t i = 0; i < 4096; i += IBATCH) {   /* every layout must agree with the textbook */
      uint32_t out[IBATCH];
      eyt_lower_bound_x16(probe + i, out);
      for (uint32_t q = 0; q < IBATCH; q++) {
        uint32_t ref = plain_lower_bound(pk[i + q], pl[i + q]);
        if (bl_lower_bound(probe[i + q]) != ref || eyt_lower_bound(probe[i + q]) != ref || out[q] != ref)
          die("a search layout disagrees with textbook binary search");
      }
    }
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NP; i++) sink += plain_lower_bound(pk[i], pl[i]);
      t[r] = now_ns() - s;
    }
    row("successor / rank", "textbook binary search (ids -> arena keys)", med(t, reps, NP), BAR_IND,
        "two dependent misses per level");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NP; i++) sink += bl_lower_bound(probe[i]);
      t[r] = now_ns() - s;
    }
    row("", "branchless (CMOV), inline keys, prefetch", med(t, reps, NP), BAR_IND, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NP; i++) sink += eyt_lower_bound(probe[i]);
      t[r] = now_ns() - s;
    }
    row("", "Eytzinger, prefetch 4 levels ahead", med(t, reps, NP), BAR_IND, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      uint32_t out[IBATCH];
      for (uint32_t i = 0; i < NP; i += IBATCH) {
        eyt_lower_bound_x16(probe + i, out);
        for (int q = 0; q < IBATCH; q++) sink += out[q];
      }
      t[r] = now_ns() - s;
    }
    row("", "Eytzinger, 16 descents interleaved", med(t, reps, NP), BAR_BAT, "");
    /* Prefix count: "user:" + the first 4 syllable letters, two bounds per prefix. */
    {
      uint8_t p[KEYMAX];
      memcpy(p, "user:kalo", 9);
      NKey l1 = nkey(p, 9);
      p[8]++;
      NKey l2 = nkey(p, 9);
      uint32_t c = eyt_lower_bound(l2) - eyt_lower_bound(l1), ref = 0;
      for (uint32_t i = 0; i < g_n; i++) ref += memcmp(akey(i), "user:kalo", 9) == 0;
      if (c != ref || ref == 0) die("prefix count disagrees with a full scan");
      snprintf(note, sizeof note, "no iteration (\"user:kalo\" = %u keys, checked)", c);
    }
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      NKey lo[IBATCH], hi[IBATCH];
      uint32_t a1[IBATCH], a2[IBATCH];
      for (uint32_t i = 0; i < NP; i += IBATCH) {
        for (int q = 0; q < IBATCH; q++) {
          uint8_t p[KEYMAX];
          memcpy(p, pk[i + (uint32_t)q], 9);
          lo[q] = nkey(p, 9);
          p[8]++;
          hi[q] = nkey(p, 9);
        }
        eyt_lower_bound_x16(lo, a1);
        eyt_lower_bound_x16(hi, a2);
        for (int q = 0; q < IBATCH; q++) sink += a2[q] - a1[q];
      }
      t[r] = now_ns() - s;
    }
    row("count by prefix / range", "2 interleaved Eytzinger descents", med(t, reps, NP), BAR_BAT, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      uint32_t out[IBATCH];
      for (uint32_t i = 0; i < 8192; i += IBATCH) {
        eyt_lower_bound_x16(probe + i, out);
        for (int q = 0; q < IBATCH; q++)
          for (uint32_t j = out[q]; j < out[q] + 100 && j < g_n; j++) sink += g_skey[j].lo;
      }
      t[r] = now_ns() - s;
    }
    row("range scan: 100 keys", "seek + 100 inline keys in order", med(t, reps, 8192), BAR_BAT,
        "per scan; keys only");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      uint32_t out[IBATCH];
      for (uint32_t i = 0; i < 8192; i += IBATCH) {
        eyt_lower_bound_x16(probe + i, out);
        for (int q = 0; q < IBATCH; q++) {
          uint32_t end = out[q] + 100 < g_n ? out[q] + 100 : g_n;
          for (uint32_t j = out[q]; j < end; j++) {
            if (j + 16 < end) {   /* software pipeline: the record 16 ahead is on its way */
              const char *p = (const char *)g_srec[j + 16];
              __builtin_prefetch(p);
              __builtin_prefetch(p + 64);
              __builtin_prefetch(p + 128);
            }
            memcpy(g_sink, kv_rec_value(g_srec[j]), VAL_LEN);
            sink += g_sink[0];
          }
        }
      }
      t[r] = now_ns() - s;
    }
    double scan_ns = med(t, reps, 8192);
    snprintf(note, sizeof note, "per scan; %.1f ns per key+value", scan_ns / 100);
    row("range scan: 100 keys + values", "seek + records, prefetched 16 ahead", scan_ns, BAR_BAT, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NP; i++) sink += g_skey[(uint32_t)(((uint64_t)i * 2654435761u) % g_n)].lo;
      t[r] = now_ns() - s;
    }
    row("select (k-th key)", "inline key array", med(t, reps, NP), BAR_IND, "O(1)");
    g_vsink += sink;
    free(g_srec);
    free(g_eyt_rank);
    big_free(g_eyt, eyt_bytes);
    big_free(g_skey, skey_bytes);
    free(g_sorted);
  }

  /* ================= sketches ================= */
  printf("\nSKETCHES: O(1) updates, O(1) answers\n");
  {
    for (int i = 0; i < 66; i++) g_pow2neg[i] = ldexp(1.0, -i);
    g_hll_sum = HLL_M;
    g_hll_zeros = HLL_M;
    uint64_t *hs = xcalloc((size_t)g_n * sizeof *hs);
    for (uint32_t i = 0; i < g_n; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, akey(i), alen(i), h);
      hs[i] = h[1];
    }
    uint64_t s = now_ns();
    for (uint32_t i = 0; i < g_n; i++) hll_add(hs[i]);
    double add_ns = (double)(now_ns() - s) / g_n;
    free(hs);
    double full = 0;
    uint32_t z = 0, zv = 0;
    for (int i = 0; i < HLL_M; i++) {
      full += g_pow2neg[g_hll[i]];
      z += g_hll[i] == 0;
    }
    double vsum = hll_sum_avx2(g_hll, HLL_M, &zv);
    if (fabs(full - g_hll_sum) > 1e-9 * full || z != g_hll_zeros) die("the kept HLL sum drifted");
    if (fabs(vsum - full) > 1e-6 * full || zv != z) die("the AVX2 HLL sum disagrees with the scalar one");
    double est = hll_estimate(g_hll_sum, g_hll_zeros), err = (est - g_n) / g_n;
    if (fabs(err) > 3 * 1.04 / sqrt(HLL_M)) die("HLL estimate outside 3 sigma");
    snprintf(note, sizeof note, "16 KB; estimate %.0f of %u (%+.2f%%); hash reused from the lookup", est, g_n,
             100 * err);
    row("distinct count: add", "HyperLogLog p=14", add_ns, BAR_BAT, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      double a = 0;
      for (int i = 0; i < 1000000; i++) a += hll_estimate(g_hll_sum + i * 1e-12, g_hll_zeros);
      t[r] = now_ns() - s2;
      g_vsink += (uint64_t)a;
    }
    row("distinct count: estimate", "kept harmonic sum", med(t, reps, 1000000), BAR_BAT, "O(1): no register pass");
    /* A second sketch over a disjoint set (the absent keys), and its exact distinct count. */
    static uint8_t other[HLL_M] __attribute__((aligned(32)));
    uint64_t *ah = xcalloc((size_t)NQ * sizeof *ah), distinct = 0;
    for (uint32_t i = 0; i < NQ; i++) {
      uint64_t h[2];
      kv_store_hash(g_store, g_absent[i], g_absent_len[i], h);
      ah[i] = h[1];
      hll_raise(other, h[1]);
    }
    qsort(ah, NQ, sizeof *ah, cmp_u64);
    for (uint32_t i = 0; i < NQ; i++) distinct += i == 0 || ah[i] != ah[i - 1];
    free(ah);
    double usum = hll_union_avx2(g_hll2, other, g_hll, HLL_M, &zv);
    double uest = hll_estimate(usum, zv), utrue = (double)g_n + (double)distinct;
    {
      double ref = 0;   /* the fused union must equal register-wise max + a scalar sum */
      uint32_t rz = 0;
      for (int i = 0; i < HLL_M; i++) {
        uint8_t m = other[i] > g_hll[i] ? other[i] : g_hll[i];
        if (g_hll2[i] != m) die("the AVX2 HLL union stored a wrong register");
        ref += g_pow2neg[m];
        rz += m == 0;
      }
      if (fabs(usum - ref) > 1e-6 * ref || rz != zv) die("the AVX2 HLL union sum disagrees with the scalar one");
    }
    if (fabs(uest - utrue) / utrue > 3 * 1.04 / sqrt(HLL_M)) die("HLL union estimate outside 3 sigma");
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      double a = 0;
      for (int i = 0; i < 10000; i++) a += hll_estimate(hll_union_avx2(g_hll2, other, g_hll, HLL_M, &zv), zv);
      t[r] = now_ns() - s2;
      g_vsink += (uint64_t)a;
    }
    snprintf(note, sizeof note, "p=14, 16 KB, 0.81%% error; estimate %.0f of %.0f (%+.2f%%)", uest, utrue,
             100 * (uest - utrue) / utrue);
    row("distinct count: union", "PFMERGE into a new sketch + estimate, AVX2", med(t, reps, 10000), BAR_BAT, note);
    for (int r = 0; r < reps; r++) {   /* the same code over 4,096 registers: a p=12 sketch's cost */
      uint64_t s2 = now_ns();
      double a = 0;
      for (int i = 0; i < 10000; i++) a += hll_estimate(hll_union_avx2(g_hll2, other, g_hll, HLL_M / 4, &zv), zv);
      t[r] = now_ns() - s2;
      g_vsink += (uint64_t)a;
    }
    row("", "the same at p=12 (timed on 4,096 registers)", med(t, reps, 10000), BAR_BAT, "4 KB, 1.63% error");
    g_cms = big_alloc((size_t)CMS_D * CMS_W * sizeof *g_cms);
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns();
      for (uint32_t i = 0; i < NQ; i++) {
        uint64_t h[2];
        kv_store_hash(g_store, g_qkey[i], g_qlen[i], h);
        for (int d = 0; d < CMS_D; d++) g_cms[(size_t)d * CMS_W + cms_col(h, d)]++;
      }
      t[r] = now_ns() - s2;
    }
    row("frequency: count a key", "Count-Min 4 x 2^21", med(t, reps, NQ), BAR_IND, "32 MB");
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns(), h[BATCH][2];
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        for (int k = 0; k < BATCH; k++) {
          kv_store_hash(g_store, g_qkey[b + (uint32_t)k], g_qlen[b + (uint32_t)k], h[k]);
          for (int d = 0; d < CMS_D; d++) __builtin_prefetch(g_cms + (size_t)d * CMS_W + cms_col(h[k], d), 1);
        }
        for (int k = 0; k < BATCH; k++)
          for (int d = 0; d < CMS_D; d++) g_cms[(size_t)d * CMS_W + cms_col(h[k], d)]++;
      }
      t[r] = now_ns() - s2;
    }
    row("", "Count-Min, batched prefetch", med(t, reps, NQ), BAR_BAT, "");
    uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s2 = now_ns(), h[BATCH][2];
      for (uint32_t b = 0; b < NQ; b += BATCH) {
        for (int k = 0; k < BATCH; k++) {
          kv_store_hash(g_store, g_qkey[b + (uint32_t)k], g_qlen[b + (uint32_t)k], h[k]);
          for (int d = 0; d < CMS_D; d++) __builtin_prefetch(g_cms + (size_t)d * CMS_W + cms_col(h[k], d));
        }
        for (int k = 0; k < BATCH; k++) {
          uint32_t m = UINT32_MAX;
          for (int d = 0; d < CMS_D; d++) {
            uint32_t c = g_cms[(size_t)d * CMS_W + cms_col(h[k], d)];
            m = c < m ? c : m;
          }
          /* Every query key was counted at least 2 * reps times above. */
          if (m < (uint32_t)(2 * reps)) die("Count-Min under-counted");
          sink += m;
        }
      }
      t[r] = now_ns() - s2;
    }
    row("frequency: estimate", "Count-Min min of 4, batched", med(t, reps, NQ), BAR_BAT,
        "never under-counts (checked)");
    g_vsink += sink;
    big_free(g_cms, (size_t)CMS_D * CMS_W * sizeof *g_cms);
  }

  /* ================= SymSpell at 10M ================= */
  printf("\nTYPOS: every key within edit distance 1 (SymSpell), at %u keys\n", g_n);
  {
    uint64_t seed = 0xC0FFEEull;
    g_pb = 2 + rnd(&seed) % (M61 - 4);
    g_pow[0] = 1;
    for (int i = 1; i < KEYMAX + 2; i++) g_pow[i] = mulmod61(g_pow[i - 1], g_pb);
    g_symb_n = ((uint64_t)g_n * 22 + 5) / 6;   /* <= 22 variants per key at <= 75% of the lanes */
    g_symb_bytes = g_symb_n * sizeof(SymBucket);
    uint64_t s = now_ns(), variants = 0;
    g_symb = big_alloc(g_symb_bytes);
    uint64_t vx[8][KEYMAX + 1];
    int nv[8];
    for (uint32_t i = 0; i < g_n; i += 8) {   /* 8 keys' variants prefetched, then inserted */
      uint32_t m = g_n - i < 8 ? g_n - i : 8;
      for (uint32_t k = 0; k < m; k++) {
        nv[k] = variant_hashes(akey(i + k), alen(i + k), vx[k]);
        for (int v = 0; v < nv[k]; v++) __builtin_prefetch(&g_symb[sym_bucket(vx[k][v])], 1);
      }
      for (uint32_t k = 0; k < m; k++) {
        for (int v = 0; v < nv[k]; v++) sym_insert(vx[k][v], g_start[i + k], alen(i + k));
        variants += (uint64_t)nv[k];
      }
    }
    double build = (double)(now_ns() - s);
    printf("  build %.1f s (%.0f ns/key), %.1f variants/key, %.2f GB = %.0f B/key at %.0f%% lane load\n", build / 1e9,
           build / g_n, (double)variants / g_n, g_symb_bytes / 1073741824.0, (double)g_symb_bytes / g_n,
           100.0 * variants / ((double)g_symb_n * 8));
    for (int i = 0; i < 20000; i++) {   /* Myers against the DP, small alphabet for many near-ties */
      uint8_t a[KEYMAX], b[KEYMAX];
      int la = 1 + (int)(rnd(&seed) % 24), lb = 1 + (int)(rnd(&seed) % 24);
      for (int j = 0; j < la; j++) a[j] = (uint8_t)('a' + rnd(&seed) % 4);
      for (int j = 0; j < lb; j++) b[j] = (uint8_t)('a' + rnd(&seed) % 4);
      Myers my;
      myers_init(&my, a, la);
      if (myers_dist(&my, b, lb) != lev_dp(a, la, b, lb)) die("Myers disagrees with the DP");
    }
    /* Typos of existing keys: a substitution, a deletion, an insertion. */
    static uint8_t tq[3][NT][KEYMAX];
    static uint32_t tqlen[3][NT];
    for (int kind = 0; kind < 3; kind++)
      for (uint32_t it = 0; it < NT; it++) {
        uint32_t id = (uint32_t)(((uint64_t)it * 2654435761u + 7u * (uint32_t)kind) % g_n), l = alen(id);
        uint8_t *q = tq[kind][it];
        if (kind == 0) {
          memcpy(q, akey(id), l);
          q[8] = 'x';
          tqlen[kind][it] = l;
        } else if (kind == 1) {
          memcpy(q, akey(id), 9);
          memcpy(q + 9, akey(id) + 10, l - 10);
          tqlen[kind][it] = l - 1;
        } else {
          memcpy(q, akey(id), 7);
          q[7] = 'q';
          memcpy(q + 8, akey(id) + 7, l - 7);
          tqlen[kind][it] = l + 1;
        }
      }
    for (int kind = 0; kind < 3; kind++)   /* exact answers against a brute-force scan of every key */
      for (uint32_t it = 0; it < 2; it++) {
        Myers my;
        myers_init(&my, tq[kind][it], (int)tqlen[kind][it]);
        uint32_t ref = 0;
        for (uint32_t i = 0; i < g_n; i++) {
          int l = (int)alen(i);
          if (l - (int)tqlen[kind][it] > 1 || (int)tqlen[kind][it] - l > 1) continue;
          ref += myers_dist(&my, akey(i), l) <= 1;
        }
        if (ref == 0 || sym_query(tq[kind][it], tqlen[kind][it], 1) != ref ||
            sym_query(tq[kind][it], tqlen[kind][it], 0) != ref)
          die("SymSpell disagrees with a brute-force scan");
      }
    static const char *const kinds[3] = {"typo: substitution", "typo: deletion", "typo: insertion"};
    for (int kind = 0; kind < 3; kind++) {
      uint64_t found = 0;
      for (int grouped = 0; grouped < 2; grouped++) {
        for (int r = 0; r < reps; r++) {
          uint64_t s2 = now_ns();
          found = 0;
          for (uint32_t it = 0; it < NT; it++) {
            uint32_t c = sym_query(tq[kind][it], tqlen[kind][it], grouped);
            if (c < 1) die("a typo did not find its own key");
            found += c;
          }
          t[r] = now_ns() - s2;
        }
        snprintf(note, sizeof note, "%.2f matches per typo", (double)found / NT);
        row(grouped ? "" : kinds[kind],
            grouped ? "SymSpell, probes + keys prefetched as groups" : "SymSpell, probes in turn",
            med(t, reps, NT), BAR_IND, grouped ? "" : note);
      }
    }
    big_free(g_symb, g_symb_bytes);
  }

  /* ================= concurrent readers ================= */
  printf("\nTHREADS: lock-free readers (QSBR; no lock, no shared write; a writer's bucket can make one wait or retry)"
         " on distinct P-cores\n");
  {
    int creps = reps < 3 ? reps : 3, tmax = 1;
    double one = 0;
    for (int ti = 0; ti < nthr; ti++) {
      int T = tcounts[ti];
      tmax = T > tmax ? T : tmax;
      for (int r = 0; r < creps; r++) t[r] = run_mix(T, NQ, 0, 0, NULL);
      double wall = med(t, creps, 1), agg = (double)NQ * T / (wall / 1e9) / 1e6;
      if (T == 1) one = agg;
      snprintf(note, sizeof note, "%d threads: %.1f M lookups/s in total (%.2fx one thread)", T, agg,
               one > 0 ? agg / one : 0.0);
      row(ti == 0 ? "exists, concurrent readers" : "", "batched pipeline, per op per thread", wall / NQ, BAR_BAT,
          note);
    }
    if (tmax >= 2) {   /* readers alone vs next to one writer, alternating (paired) */
      uint64_t ta[MAX_REPS], tb[MAX_REPS], puts = 0, allputs = 0;
      for (int r = 0; r < creps; r++) {
        ta[r] = run_mix(tmax - 1, NQ, 0, 0, NULL);
        tb[r] = run_mix(tmax - 1, NQ, 1, UINT32_MAX, &puts);
        allputs += puts;
      }
      double a = med(ta, creps, 1), b = med(tb, creps, 1);
      snprintf(note, sizeof note, "%d readers alone", tmax - 1);
      row("readers next to a writer", "batched pipeline, per op per reader", a / NQ, BAR_BAT, note);
      snprintf(note, sizeof note, "%d readers + 1 writer (%.2f M puts/s): %.2fx", tmax - 1,
               (double)allputs / creps / (b / 1e9) / 1e6, b / a);
      row("", "batched pipeline, per op per reader", b / NQ, BAR_BAT, note);
    }
  }

  /* ================= writes ================= */
  printf("\nWRITES: the serialized writer path (one futex mutex, ADR 0003)\n");
  {
    uint64_t ta[MAX_REPS], tb[MAX_REPS];
    for (int r = 0; r < reps; r++) {   /* unbatched and batched alternate: paired */
      ta[r] = replace_pass((uint32_t)(2 * r) % 4, 0);
      tb[r] = replace_pass((uint32_t)(2 * r + 1) % 4, 1);
    }
    row("put (replace)", "one at a time (today's path)", med(ta, reps, NW), BAR_IND, "");
    row("", "batched: buckets + old records prefetched", med(tb, reps, NW), BAR_BAT, "the write-batching lead");
    g_wkey = xcalloc((size_t)NW * KEYMAX);
    g_wlen = xcalloc((size_t)NW * sizeof *g_wlen);
    uint64_t da[MAX_REPS], db[MAX_REPS], ca[MAX_REPS], cb[MAX_REPS];
    for (int r = 0; r < reps; r++) {
      load_wkeys((uint32_t)(2 * r));
      da[r] = write_pass(0, 0);
      ca[r] = write_pass(1, 0);
      load_wkeys((uint32_t)(2 * r + 1));
      db[r] = write_pass(0, 1);
      cb[r] = write_pass(1, 1);
      KvStats s2;
      kv_store_stats(g_store, &s2);
      if (s2.count != g_n) die("delete + create did not restore the key count");
    }
    row("delete", "one at a time", med(da, reps, NW), BAR_IND, "");
    row("", "batched: buckets + records prefetched", med(db, reps, NW), BAR_BAT, "");
    row("put (create)", "one at a time (malloc + copy + insert)", med(ca, reps, NW), BAR_IND, "");
    row("", "batched: buckets prefetched", med(cb, reps, NW), BAR_BAT, "");
    int creps = reps < 3 ? reps : 3;
    for (int ti = 0; ti < nthr; ti++) {
      int T = tcounts[ti];
      uint64_t puts = 0;
      for (int r = 0; r < creps; r++) t[r] = run_mix(0, 0, T, NW / 2, &puts);
      double wall = med(t, creps, 1);
      snprintf(note, sizeof note, "%d writers: %.2f M puts/s in total", T, (double)puts / (wall / 1e9) / 1e6);
      row(ti == 0 ? "put (replace), concurrent" : "", "writer mutex; per put per thread", wall / (NW / 2), BAR_IND,
          note);
    }
    kv_store_reclaim(g_store);
  }
  print_memory();
  printf("\nall checks passed\n");
  return 0;
}
