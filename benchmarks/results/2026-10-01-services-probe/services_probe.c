/*
 * services_probe.c — more key-value services that stay within 10x of an
 * exact lookup at 10M keys, and assembly/SIMD techniques that make the
 * store itself faster. Evidence for docs/proposals/golden-services.md.
 *
 * Why it exists: after the golden catalog (golden-operations.md) and the
 * functions plan (functions-plan.md) the review asked for MORE services
 * that meet the same bar, and for the assembly and SIMD power that makes
 * them (and the store) faster. This probe measures, on the server's own
 * store code at 10M keys:
 *
 *   bar        the exact-get bars (D dependent chain, I independent loop,
 *              B the server's batched pipeline), so every ratio comes from
 *              the same run
 *   siphash4   SipHash-2-4-128 of four keys at once in AVX2 lanes (the
 *              store hashes every key; the hash is ~23% of a batched get),
 *              checked bit-exact against the store's assembly, then the
 *              batched GET pipeline with it
 *   inline     a small-value table: keys <= 16 bytes and 8-byte values live
 *              inside the 64-byte bucket, so a lookup is one cache miss,
 *              not two; against the store holding the same keys and values
 *   combining  flat combining for concurrent writers: one thread applies
 *              every pending put, prefetched as a batch, against threads
 *              that each take the store's writer mutex
 *   waitpkg    wake-up latency of a waiting thread: PAUSE spin, UMONITOR /
 *              UMWAIT (user-mode monitor-wait), futex
 *   bitmaps    BITCOUNT and AND-count of 1 MiB bitmaps (POPCNT, AVX2 nibble
 *              lookup, AVX2 Harley-Seal), SETBIT / GETBIT, BITPOS
 *   struct     field access inside a value (HGET / HINCRBY) through a
 *              16-tag SSE2 field directory, on a store of struct values
 *   sets       SISMEMBER and SINTERCARD on sorted u32 sets, scalar vs AVX2
 *   kernels    sliding-window sum, MinHash agreement, int8 dot products
 *              (scalar, AVX2 VPMADDUBSW, AVX-VNNI VPDPBUSD), fp32 FMA dot
 *   simhash    near-duplicate search over 10M 64-bit fingerprints: multi-
 *              index hashing (4 x 16-bit blocks) with an AVX2 Hamming scan
 *   geo        radius search over 10M points: PDEP Morton codes, a sorted
 *              code array, 4 interleaved range seeks, an exact distance test
 *
 * Checks: every section verifies its results against a scalar or brute-
 * force reference before its numbers print (the AVX2 hash bit-exact for
 * every query key, every inline-table answer, every increment, popcounts,
 * fields, set intersections, kernels, MIH and geo results against full
 * scans of all 10M items); a mismatch exits 1.
 *
 * Usage: services_probe [--keys N] [--cpu C] [--reps R] [--threads "1,2,4,7"]
 * Build: run.sh next to this file.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* MAP_ANONYMOUS, affinity, futex, M_PI */
#endif
#include <cpuid.h>
#include <immintrin.h>
#include <linux/futex.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <x86intrin.h>          /* __rdtsc */

#include "kvnode_abi.h"

enum {
  KEYMAX = 32,           /* key buffer; the longest key is 21 bytes */
  VAL_LEN = 100,         /* the benchmark suite's value size */
  FIELDS = 4,            /* keys per "user" */
  BATCH = 32,            /* the server's GET batch */
  MAX_REPS = 16,
  MAX_THREADS = 7,       /* CPUs 1-7: distinct P-cores on this host */
  NQ = 1 << 20,          /* lookups per timed pass */
  NW = 1 << 18,          /* writes per timed pass */
};

static const char *const kSyl[20] = {"ka", "lo", "mi", "nu", "ra", "se", "ti", "vo", "ze", "ba",
                                     "de", "fi", "go", "hu", "ja", "ke", "li", "mo", "na", "pe"};
static const char *const kFields[FIELDS] = {"name", "email", "plan", "prefs"};
/* SipHash key of every store in this probe (the AVX2 lanes must reproduce the store's hash). */
static const uint8_t kSeed[16] __attribute__((aligned(8))) = {9, 7, 5, 3, 1, 2, 4, 6,
                                                              8, 10, 12, 14, 16, 18, 20, 22};
/* Thread i runs on CPU g_tcpu[i]: vCPU 0 carries VM interrupt work, 8-15 are HT siblings. */
static const int g_tcpu[MAX_THREADS] = {1, 2, 3, 4, 5, 6, 7};

static uint32_t g_n;             /* keys in the main store */
static KvTable *g_store;         /* the main store: 20-21 byte keys, 100-byte values */
static uint8_t *g_arena;         /* every main-store key once, back to back */
static uint32_t *g_start;        /* key i = g_arena[g_start[i] .. g_start[i + 1]) */
static uint8_t g_sink[512];      /* value copies land here */
static volatile uint64_t g_vsink;   /* timed results are folded here so no loop is dead code */
static uint64_t g_k0, g_k1;      /* kSeed as SipHash key words */

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
  fprintf(stderr, "services_probe: %s\n", what);
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
 * 32-byte-aligned heap memory that cannot fail (AVX2 aligned loads).
 * @param n  bytes (rounded up to a multiple of 32, as aligned_alloc requires)
 * @returns the memory; exits on allocation failure. Release with free().
 */
static void *xalign32(size_t n) {
  void *p = aligned_alloc(32, (n + 31) & ~(size_t)31);
  if (!p) die("out of memory");
  return p;
}

/**
 * Zeroed memory for a large structure, marked for transparent huge pages.
 * @param n  bytes
 * @returns page-aligned memory; exits on failure. Release with big_free.
 * @remarks Gives the probe's structures the same TLB treatment as the
 *          store's table; the madvise is only a hint.
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
 * @remarks Exits if the kernel refuses (a CPU outside the container's set):
 *          an unpinned run would mix P-cores, E-cores and HT siblings.
 */
static void pin(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0) die("pthread_setaffinity_np");
}

/**
 * Compiler barrier: makes the compiler assume all memory may have changed.
 * @remarks Timing loops call it once per iteration. Without it GCC proves a
 *          kernel pure (same arguments, unchanged memory), computes it once
 *          and multiplies: the first smoke run reported a 1 MiB popcount at
 *          4.5 TB/s that way. Emits no instruction.
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
 * qsort comparator for uint64_t.
 * @param a, b  the two elements
 * @returns negative, zero or positive as *a is below, equal to or above *b.
 */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/**
 * qsort comparator for uint32_t.
 * @param a, b  the two elements
 * @returns negative, zero or positive as *a is below, equal to or above *b.
 */
static int cmp_u32(const void *a, const void *b) {
  uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
  return (x > y) - (x < y);
}

/**
 * qsort comparator for double (no NaNs).
 * @param a, b  the two elements
 * @returns negative, zero or positive as *a is below, equal to or above *b.
 */
static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/**
 * How often a value occurs in a sorted array.
 * @param a  sorted array
 * @param n  its size
 * @param x  the value
 * @returns the number of elements equal to x.
 * @remarks Used to compute exactly what repeated in-place increments must
 *          have produced, when a random stream hits some keys twice.
 */
static uint32_t occurrences(const uint64_t *a, uint32_t n, uint64_t x) {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {   /* first element >= x */
    uint32_t mid = lo + (hi - lo) / 2;
    if (a[mid] < x) lo = mid + 1;
    else hi = mid;
  }
  uint32_t c = 0;
  while (lo + c < n && a[lo + c] == x) c++;
  return c;
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
 * Writes small-store key number i: "c:" + 14 decimal digits (16 bytes),
 * e.g. a counter or flag keyed by a numeric id.
 * @param i    key index
 * @param out  at least 16 bytes
 */
static void make_small_key(uint32_t i, uint8_t *out) {
  out[0] = 'c';
  out[1] = ':';
  for (int d = 15; d >= 2; d--) {
    out[d] = (uint8_t)('0' + i % 10);
    i /= 10;
  }
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
  printf("  %-32s %-46s %s  %s  %s\n", what, how, v, r, note);
}

/**
 * Prints this process's resident and huge-page-backed anonymous memory.
 * @remarks smaps_rollup, because VmRSS is stale on this WSL2 kernel. Only
 *          this process's own maps are read, never another process's /proc
 *          files: the VM can hold processes stuck in D state, and reading
 *          their maps hangs.
 */
static void print_memory(void) {
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  char line[256];
  if (!f) return;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "Rss:", 4) || !strncmp(line, "AnonHugePages:", 14)) printf("  %s", line);
  fclose(f);
}

/**
 * Whether the CPU offers AVX-VNNI (CPUID.7.1:EAX[4]) and WAITPKG
 * (CPUID.7.0:ECX[5]).
 * @param vnni     receives 1 when AVX-VNNI is present, else 0
 * @param waitpkg  receives 1 when UMONITOR/UMWAIT/TPAUSE are present, else 0
 * @remarks Read from CPUID directly: the compiler's feature names for these
 *          vary across versions. CPUID only says the CPU has them; whether a
 *          hypervisor lets UMWAIT run is tested separately (umwait_works).
 */
static void cpu_extras(int *vnni, int *waitpkg) {
  unsigned a, b, c, d;
  *vnni = *waitpkg = 0;
  if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) *waitpkg = (c >> 5) & 1;
  if (__get_cpuid_count(7, 1, &a, &b, &c, &d)) *vnni = (a >> 4) & 1;
}

/* ========================================================================= */
/* Key streams                                                                */
/* ========================================================================= */

/** A stream of query keys, one after another (a request stream). */
typedef struct {
  uint8_t (*key)[KEYMAX];
  uint32_t *len;
} Stream;

static Stream g_q;               /* main-store query stream */

/**
 * NQ random keys of a key generator, copied into a sequential stream.
 * @param gen   0 = main-store keys, 1 = small-store keys
 * @param n     key-space size (ids are drawn from [0, n))
 * @param seed  random seed (non-zero)
 * @param ids   receives the NQ ids, may be NULL
 * @returns the stream (heap memory; release with stream_free).
 * @remarks A server reads keys from its request buffers, which are in
 *          cache; a random arena access per lookup would add a miss to
 *          every bar.
 */
static Stream make_stream(int gen, uint32_t n, uint64_t seed, uint32_t *ids) {
  Stream s = {xcalloc((size_t)NQ * KEYMAX), xcalloc((size_t)NQ * sizeof(uint32_t))};
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t id = (uint32_t)(rnd(&seed) % n);
    if (ids) ids[i] = id;
    if (gen == 0) {
      s.len[i] = alen(id);
      memcpy(s.key[i], akey(id), s.len[i]);
    } else {
      make_small_key(id, s.key[i]);
      s.len[i] = 16;
    }
  }
  return s;
}

/**
 * Releases a stream from make_stream.
 * @param s  the stream (its pointers are cleared)
 */
static void stream_free(Stream *s) {
  free(s->key);
  free(s->len);
  s->key = NULL;
  s->len = NULL;
}

/* ========================================================================= */
/* SipHash-2-4-128: scalar reference and four AVX2 lanes                      */
/* ========================================================================= */

/** One SipHash round on four 64-bit words (the reference definition). */
#define SIPROUND(v0, v1, v2, v3)                                                     \
  do {                                                                               \
    v0 += v1; v1 = (v1 << 13) | (v1 >> 51); v1 ^= v0; v0 = (v0 << 32) | (v0 >> 32); \
    v2 += v3; v3 = (v3 << 16) | (v3 >> 48); v3 ^= v2;                                \
    v0 += v3; v3 = (v3 << 21) | (v3 >> 43); v3 ^= v0;                                \
    v2 += v1; v1 = (v1 << 17) | (v1 >> 47); v1 ^= v2; v2 = (v2 << 32) | (v2 >> 32); \
  } while (0)

/**
 * SipHash-2-4 with a 128-bit output (the reference construction, key
 * kSeed): the oracle for the AVX2 lanes and the inline table's hash.
 * @param in   bytes
 * @param len  length
 * @param out  receives the two output words (low, high), equal to what
 *             kv_store_hash returns for a store created with kSeed
 */
static void sip128_ref(const uint8_t *in, size_t len, uint64_t out[2]) {
  uint64_t v0 = 0x736f6d6570736575ull ^ g_k0, v1 = 0x646f72616e646f6dull ^ g_k1 ^ 0xee;
  uint64_t v2 = 0x6c7967656e657261ull ^ g_k0, v3 = 0x7465646279746573ull ^ g_k1;
  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t m;
    memcpy(&m, in + i, 8);
    v3 ^= m;
    SIPROUND(v0, v1, v2, v3);
    SIPROUND(v0, v1, v2, v3);
    v0 ^= m;
  }
  uint64_t b = (uint64_t)len << 56, t = 0;
  memcpy(&t, in + i, len - i);
  b |= t;
  v3 ^= b;
  SIPROUND(v0, v1, v2, v3);
  SIPROUND(v0, v1, v2, v3);
  v0 ^= b;
  v2 ^= 0xee;
  for (int r = 0; r < 4; r++) SIPROUND(v0, v1, v2, v3);
  out[0] = v0 ^ v1 ^ v2 ^ v3;
  v1 ^= 0xdd;
  for (int r = 0; r < 4; r++) SIPROUND(v0, v1, v2, v3);
  out[1] = v0 ^ v1 ^ v2 ^ v3;
}

/** Rotate each 64-bit lane left by b (AVX2 has no 64-bit rotate: two shifts and an OR). */
#define ROTL4(x, b) _mm256_or_si256(_mm256_slli_epi64((x), (b)), _mm256_srli_epi64((x), 64 - (b)))

/** One SipHash round on four lanes; rotate-32 is a dword shuffle, rotate-16 a byte shuffle. */
#define SIPROUND4(v0, v1, v2, v3, r16)                                                     \
  do {                                                                                     \
    v0 = _mm256_add_epi64(v0, v1); v1 = ROTL4(v1, 13); v1 = _mm256_xor_si256(v1, v0);       \
    v0 = _mm256_shuffle_epi32(v0, _MM_SHUFFLE(2, 3, 0, 1));                                \
    v2 = _mm256_add_epi64(v2, v3); v3 = _mm256_shuffle_epi8(v3, r16);                      \
    v3 = _mm256_xor_si256(v3, v2);                                                         \
    v0 = _mm256_add_epi64(v0, v3); v3 = ROTL4(v3, 21); v3 = _mm256_xor_si256(v3, v0);       \
    v2 = _mm256_add_epi64(v2, v1); v1 = ROTL4(v1, 17); v1 = _mm256_xor_si256(v1, v2);       \
    v2 = _mm256_shuffle_epi32(v2, _MM_SHUFFLE(2, 3, 0, 1));                                \
  } while (0)

/**
 * A key's final SipHash block: the length in the top byte, the 0-7 tail
 * bytes below it.
 * @param k    key
 * @param len  its length
 * @returns the block.
 * @remarks For keys of 8 bytes or more the tail is one overlapping 8-byte
 *          load that ends at the key's last byte, shifted down: no
 *          variable-length memcpy (which compiles to a library call per key
 *          and cost the first smoke run ~5 ns per key). The load stays
 *          inside the key, so it is safe at the end of any buffer.
 */
static inline uint64_t sip_last_block(const uint8_t *k, uint32_t len) {
  uint64_t t = 0;
  uint32_t r = len & 7;
  if (len >= 8) {
    if (r) {
      memcpy(&t, k + len - 8, 8);
      t >>= 64 - 8 * r;
    }
  } else {
    for (uint32_t j = 0; j < len; j++) t |= (uint64_t)k[j] << (8 * j);
  }
  return ((uint64_t)len << 56) | t;
}

/**
 * One message block of four keys as a vector (lane l = key l's block i).
 * @param k  the four keys
 * @param i  block number
 * @returns the four 64-bit words.
 */
__attribute__((target("avx2"))) static inline __m256i sip_block4(const uint8_t *const k[4], uint32_t i) {
  uint64_t w0, w1, w2, w3;
  memcpy(&w0, k[0] + 8 * i, 8);
  memcpy(&w1, k[1] + 8 * i, 8);
  memcpy(&w2, k[2] + 8 * i, 8);
  memcpy(&w3, k[3] + 8 * i, 8);
  return _mm256_set_epi64x((long long)w3, (long long)w2, (long long)w1, (long long)w0);
}

/**
 * Writes four lanes' two output words.
 * @param o0   the low words
 * @param o1   the high words
 * @param out  receives out[l] = {o0[l], o1[l]}
 */
__attribute__((target("avx2"))) static inline void sip_store4(__m256i o0, __m256i o1, uint64_t out[4][2]) {
  /* Interleave so each 128-bit half holds one key's {low, high} pair. */
  __m256i lo = _mm256_unpacklo_epi64(o0, o1), hi = _mm256_unpackhi_epi64(o0, o1);
  _mm_storeu_si128((__m128i *)(void *)out[0], _mm256_castsi256_si128(lo));
  _mm_storeu_si128((__m128i *)(void *)out[1], _mm256_castsi256_si128(hi));
  _mm_storeu_si128((__m128i *)(void *)out[2], _mm256_extracti128_si256(lo, 1));
  _mm_storeu_si128((__m128i *)(void *)out[3], _mm256_extracti128_si256(hi, 1));
}

/**
 * SipHash-2-4-128 of four keys at once, one key per 64-bit AVX2 lane.
 * @param k    the four keys (only len bytes of each are read)
 * @param len  their lengths; all four must have the same number of full
 *             8-byte blocks (len / 8): callers fall back to the scalar hash
 *             otherwise
 * @param out  receives each key's two output words
 * @remarks SipHash's chain is serial inside one message, so SIMD cannot
 *          speed up ONE key; but the batched pipeline hashes 32 independent
 *          keys, so four lanes run four messages through the same rounds.
 *          AVX2 has no 64-bit rotate: 13/17/21 are shift+shift+OR, while 32
 *          is a dword shuffle and 16 a byte shuffle (one instruction each).
 *          The result is bit-exact with the scalar hash (checked for every
 *          query key), so tables built either way stay interchangeable.
 */
__attribute__((target("avx2"))) static void sip128_x4(const uint8_t *const k[4], const uint32_t len[4],
                                                     uint64_t out[4][2]) {
  const __m256i r16 = _mm256_setr_epi8(6, 7, 0, 1, 2, 3, 4, 5, 14, 15, 8, 9, 10, 11, 12, 13, 6, 7, 0, 1, 2, 3, 4,
                                       5, 14, 15, 8, 9, 10, 11, 12, 13);
  __m256i v0 = _mm256_set1_epi64x((long long)(0x736f6d6570736575ull ^ g_k0));
  __m256i v1 = _mm256_set1_epi64x((long long)(0x646f72616e646f6dull ^ g_k1 ^ 0xee));
  __m256i v2 = _mm256_set1_epi64x((long long)(0x6c7967656e657261ull ^ g_k0));
  __m256i v3 = _mm256_set1_epi64x((long long)(0x7465646279746573ull ^ g_k1));
  uint32_t nb = len[0] >> 3;
  for (uint32_t i = 0; i < nb; i++) {
    __m256i m = sip_block4(k, i);
    v3 = _mm256_xor_si256(v3, m);
    SIPROUND4(v0, v1, v2, v3, r16);
    SIPROUND4(v0, v1, v2, v3, r16);
    v0 = _mm256_xor_si256(v0, m);
  }
  __m256i m = _mm256_set_epi64x((long long)sip_last_block(k[3], len[3]), (long long)sip_last_block(k[2], len[2]),
                                (long long)sip_last_block(k[1], len[1]), (long long)sip_last_block(k[0], len[0]));
  v3 = _mm256_xor_si256(v3, m);
  SIPROUND4(v0, v1, v2, v3, r16);
  SIPROUND4(v0, v1, v2, v3, r16);
  v0 = _mm256_xor_si256(v0, m);
  v2 = _mm256_xor_si256(v2, _mm256_set1_epi64x(0xee));
  for (int r = 0; r < 4; r++) SIPROUND4(v0, v1, v2, v3, r16);
  __m256i o0 = _mm256_xor_si256(_mm256_xor_si256(v0, v1), _mm256_xor_si256(v2, v3));
  v1 = _mm256_xor_si256(v1, _mm256_set1_epi64x(0xdd));
  for (int r = 0; r < 4; r++) SIPROUND4(v0, v1, v2, v3, r16);
  __m256i o1 = _mm256_xor_si256(_mm256_xor_si256(v0, v1), _mm256_xor_si256(v2, v3));
  sip_store4(o0, o1, out);
}

/**
 * SipHash-2-4-128 of eight keys: two four-lane groups with their rounds
 * interleaved instruction by instruction.
 * @param k    the eight keys (only len bytes of each are read)
 * @param len  their lengths; all eight must have the same number of full
 *             8-byte blocks
 * @param out  receives each key's two output words
 * @remarks One four-lane group is latency-bound: each round is a chain of
 *          dependent adds, rotates and XORs, and the core's vector ports
 *          idle while it waits. A second independent group fills those
 *          ports (two chains in flight), the same trick the scalar code
 *          could only get from the out-of-order window.
 */
__attribute__((target("avx2"))) static void sip128_x8(const uint8_t *const k[8], const uint32_t len[8],
                                                     uint64_t out[8][2]) {
  const __m256i r16 = _mm256_setr_epi8(6, 7, 0, 1, 2, 3, 4, 5, 14, 15, 8, 9, 10, 11, 12, 13, 6, 7, 0, 1, 2, 3, 4,
                                       5, 14, 15, 8, 9, 10, 11, 12, 13);
  __m256i a0 = _mm256_set1_epi64x((long long)(0x736f6d6570736575ull ^ g_k0));
  __m256i a1 = _mm256_set1_epi64x((long long)(0x646f72616e646f6dull ^ g_k1 ^ 0xee));
  __m256i a2 = _mm256_set1_epi64x((long long)(0x6c7967656e657261ull ^ g_k0));
  __m256i a3 = _mm256_set1_epi64x((long long)(0x7465646279746573ull ^ g_k1));
  __m256i b0 = a0, b1 = a1, b2 = a2, b3 = a3;
  uint32_t nb = len[0] >> 3;
  for (uint32_t i = 0; i < nb; i++) {
    __m256i ma = sip_block4(k, i), mb = sip_block4(k + 4, i);
    a3 = _mm256_xor_si256(a3, ma);
    b3 = _mm256_xor_si256(b3, mb);
    SIPROUND4(a0, a1, a2, a3, r16);
    SIPROUND4(b0, b1, b2, b3, r16);
    SIPROUND4(a0, a1, a2, a3, r16);
    SIPROUND4(b0, b1, b2, b3, r16);
    a0 = _mm256_xor_si256(a0, ma);
    b0 = _mm256_xor_si256(b0, mb);
  }
  __m256i ma = _mm256_set_epi64x((long long)sip_last_block(k[3], len[3]), (long long)sip_last_block(k[2], len[2]),
                                 (long long)sip_last_block(k[1], len[1]), (long long)sip_last_block(k[0], len[0]));
  __m256i mb = _mm256_set_epi64x((long long)sip_last_block(k[7], len[7]), (long long)sip_last_block(k[6], len[6]),
                                 (long long)sip_last_block(k[5], len[5]), (long long)sip_last_block(k[4], len[4]));
  a3 = _mm256_xor_si256(a3, ma);
  b3 = _mm256_xor_si256(b3, mb);
  SIPROUND4(a0, a1, a2, a3, r16);
  SIPROUND4(b0, b1, b2, b3, r16);
  SIPROUND4(a0, a1, a2, a3, r16);
  SIPROUND4(b0, b1, b2, b3, r16);
  a0 = _mm256_xor_si256(a0, ma);
  b0 = _mm256_xor_si256(b0, mb);
  a2 = _mm256_xor_si256(a2, _mm256_set1_epi64x(0xee));
  b2 = _mm256_xor_si256(b2, _mm256_set1_epi64x(0xee));
  for (int r = 0; r < 4; r++) {
    SIPROUND4(a0, a1, a2, a3, r16);
    SIPROUND4(b0, b1, b2, b3, r16);
  }
  __m256i oa0 = _mm256_xor_si256(_mm256_xor_si256(a0, a1), _mm256_xor_si256(a2, a3));
  __m256i ob0 = _mm256_xor_si256(_mm256_xor_si256(b0, b1), _mm256_xor_si256(b2, b3));
  a1 = _mm256_xor_si256(a1, _mm256_set1_epi64x(0xdd));
  b1 = _mm256_xor_si256(b1, _mm256_set1_epi64x(0xdd));
  for (int r = 0; r < 4; r++) {
    SIPROUND4(a0, a1, a2, a3, r16);
    SIPROUND4(b0, b1, b2, b3, r16);
  }
  __m256i oa1 = _mm256_xor_si256(_mm256_xor_si256(a0, a1), _mm256_xor_si256(a2, a3));
  __m256i ob1 = _mm256_xor_si256(_mm256_xor_si256(b0, b1), _mm256_xor_si256(b2, b3));
  sip_store4(oa0, oa1, out);
  sip_store4(ob0, ob1, out + 4);
}

/* Hash methods of a group of keys. */
enum { HASH_ASM = 0, HASH_X4 = 1, HASH_X8 = 2 };

/**
 * Hashes eight consecutive stream keys with the chosen method; AVX2
 * methods fall back group by group when the keys' block counts differ.
 * @param t     store (its hash key must be kSeed: the lanes use g_k0/g_k1)
 * @param s     stream
 * @param i     index of the first of the eight keys
 * @param h     receives the eight hash pairs
 * @param mode  HASH_ASM (the store's assembly, key by key), HASH_X4 (two
 *              four-lane calls) or HASH_X8 (one interleaved eight-lane call)
 */
static inline void hash8(KvTable *t, const Stream *s, uint32_t i, uint64_t h[8][2], int mode) {
  const uint8_t *kp[8];
  uint32_t kl[8], same = 1;
  for (int l = 0; l < 8; l++) {
    kp[l] = s->key[i + (uint32_t)l];
    kl[l] = s->len[i + (uint32_t)l];
    same &= (kl[l] >> 3) == (kl[0] >> 3);
  }
  if (mode == HASH_X8 && same) {
    sip128_x8(kp, kl, h);
  } else if (mode != HASH_ASM && same) {
    sip128_x4(kp, kl, h);
    sip128_x4(kp + 4, kl + 4, h + 4);
  } else {
    for (int l = 0; l < 8; l++) kv_store_hash(t, kp[l], kl[l], h[l]);
  }
}

/* ========================================================================= */
/* The bars                                                                   */
/* ========================================================================= */

/**
 * Copies a value to the sink with a compile-time size.
 * @param v     the value
 * @param vlen  VAL_LEN or 8 (the only value sizes in this probe)
 * @remarks Each branch is a constant-size memcpy, which GCC inlines as a few
 *          vector moves (as the golden probe's copy, and the server's). With
 *          a variable size GCC emitted `rep movsq`, whose startup cost added
 *          ~25 ns to every batched get: the first certified run's bar B
 *          read 83 ns while the golden probe, run as a control the same
 *          hour, read 56-64 ns.
 */
static inline void copy_value(const uint8_t *v, uint32_t vlen) {
  if (vlen == VAL_LEN) memcpy(g_sink, v, VAL_LEN);
  else memcpy(g_sink, v, 8);
}

/**
 * NQ exact gets where each key is chosen by the previous value (pure
 * latency).
 * @param t     store
 * @param s     stream (every key present)
 * @param vlen  value bytes to copy (the store's values must be this long)
 * @returns total ns; exits if a key is missing.
 * @remarks The next key is i or i + NQ/2, picked by the value's last byte:
 *          never a neighbour, so no lookup is answered from cache (the
 *          golden probe's lesson).
 */
static uint64_t bar_dependent(KvTable *t, const Stream *s, uint32_t vlen) {
  uint64_t t0 = now_ns(), x = 0;
  for (uint32_t i = 0; i < NQ; i++) {
    uint32_t q = (i + (uint32_t)x * (NQ / 2)) & (NQ - 1);
    uint64_t h[2];
    kv_store_hash(t, s->key[q], s->len[q], h);
    const KvRec *r = kv_store_get(t, s->key[q], s->len[q], h[0], h[1]);
    if (!r) die("dependent get missed");
    copy_value(kv_rec_value(r), vlen);
    x = g_sink[vlen - 1] & 1;
  }
  return now_ns() - t0;
}

/**
 * NQ exact gets in a plain loop (independent iterations: the core overlaps
 * them as far as its out-of-order window allows).
 * @param t     store
 * @param s     stream (every key present)
 * @param vlen  value bytes to copy
 * @returns total ns; exits if a key is missing.
 */
static uint64_t bar_independent(KvTable *t, const Stream *s, uint32_t vlen) {
  uint64_t t0 = now_ns();
  for (uint32_t i = 0; i < NQ; i++) {
    uint64_t h[2];
    kv_store_hash(t, s->key[i], s->len[i], h);
    const KvRec *r = kv_store_get(t, s->key[i], s->len[i], h[0], h[1]);
    if (!r) die("independent get missed");
    copy_value(kv_rec_value(r), vlen);
  }
  return now_ns() - t0;
}

/**
 * Hashes a batch of 32 stream keys and prefetches both buckets of each.
 * @param t     store
 * @param s     stream
 * @param b     index of the batch's first key
 * @param h     receives the 32 hash pairs
 * @param mode  HASH_ASM, HASH_X4 or HASH_X8
 * @remarks HASH_ASM interleaves one hash and one prefetch per key, exactly
 *          as the server's GET loop does; the AVX2 modes prefetch each
 *          group of eight right after hashing it, so in every mode the
 *          first misses overlap the remaining hashing.
 */
static inline void hash_prefetch_batch(KvTable *t, const Stream *s, uint32_t b, uint64_t h[BATCH][2], int mode) {
  if (mode == HASH_ASM) {
    for (int k = 0; k < BATCH; k++) {
      kv_store_hash(t, s->key[b + (uint32_t)k], s->len[b + (uint32_t)k], h[k]);
      kv_store_prefetch(t, h[k][0], h[k][1]);
    }
    return;
  }
  for (int k = 0; k < BATCH; k += 8) {
    hash8(t, s, b + (uint32_t)k, &h[k], mode);
    for (int l = k; l < k + 8; l++) kv_store_prefetch(t, h[l][0], h[l][1]);
  }
}

/**
 * The server's batched GET pipeline over NQ keys: per batch of 32, hash
 * each key and prefetch its two buckets, prefetch the candidate records,
 * then look up and copy each value.
 * @param t     store
 * @param s     stream (every key present)
 * @param vlen  value bytes to copy
 * @param mode  hash method: HASH_ASM (the server today), HASH_X4, HASH_X8
 * @returns total ns; exits if a key is missing.
 */
static uint64_t bar_batched(KvTable *t, const Stream *s, uint32_t vlen, int mode) {
  uint64_t t0 = now_ns(), h[BATCH][2];
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    hash_prefetch_batch(t, s, b, h, mode);
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(t, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const KvRec *r = kv_store_get(t, s->key[b + (uint32_t)k], s->len[b + (uint32_t)k], h[k][0], h[k][1]);
      if (!r) die("batched get missed");
      copy_value(kv_rec_value(r), vlen);
    }
  }
  return now_ns() - t0;
}

/* ========================================================================= */
/* The inline small-value table                                               */
/* ========================================================================= */

/**
 * One 64-byte bucket of the inline table: a seqlock version, two 1-byte
 * tags and lengths, and two entries of (16-byte key, 8-byte value).
 * @remarks Key and value live in the bucket's own cache line, so a lookup
 *          is one line per candidate bucket (both prefetched together) and
 *          no record dereference: the store's second dependent miss is gone.
 *          Keys are still compared exactly (ADR 0004): all 16 bytes and the
 *          length. A tag of 0 marks an empty entry.
 */
typedef struct {
  _Atomic uint64_t ver;
  uint8_t tag[2], len[2];
  uint32_t pad;
  struct {
    uint8_t key[16];
    _Atomic uint64_t val;
  } e[2];
} __attribute__((aligned(64))) IBucket;
_Static_assert(sizeof(IBucket) == 64, "an inline bucket is one cache line");

static IBucket *g_ib;
static uint64_t g_ib_mask, g_ib_buckets;

/**
 * A key's 8-bit tag.
 * @param h2  the high word of the key's SipHash-128
 * @returns its top byte, or 1 when that is 0 (0 marks an empty entry).
 */
static inline uint8_t ib_tag(uint64_t h2) {
  uint8_t t = (uint8_t)(h2 >> 56);
  return t ? t : 1;
}

/**
 * The other candidate bucket of an entry (partial-key cuckoo: computed
 * from a bucket and the tag alone, as the store's LOCATE does).
 * @param b    one candidate bucket
 * @param tag  the entry's tag
 * @returns the other candidate; ib_alt(ib_alt(b, tag), tag) == b.
 * @remarks The odd XOR offset can never be 0, so the two candidates always
 *          differ.
 */
static inline uint64_t ib_alt(uint64_t b, uint8_t tag) {
  return (b ^ ((((uint64_t)tag * 0x9E3779B97F4A7C15ull) >> 32) | 1)) & g_ib_mask;
}

/**
 * Inserts a key at build time (single writer, no concurrent readers): a
 * free entry in either bucket, else a random walk of displacements.
 * @param key  16 bytes (zero-padded)
 * @param len  key length (<= 16)
 * @param val  value
 * @param h    the key's SipHash-128
 * @remarks Exits if 2,000 displacements find no room (the load is too high
 *          for two-way buckets of two). Displacing an entry needs only its
 *          bucket and tag, never its key's hash. A live table would bump
 *          the bucket version around each change (the seqlock readers check).
 */
static void ib_insert(const uint8_t key[16], uint8_t len, uint64_t val, const uint64_t h[2]) {
  uint8_t tag = ib_tag(h[1]), klen = len, kbuf[16];
  memcpy(kbuf, key, 16);
  uint64_t b = h[0] & g_ib_mask, seed = h[0] | 1, v = val;
  for (int kick = 0; kick < 2000; kick++) {
    uint64_t c[2] = {b, ib_alt(b, tag)};
    for (int i = 0; i < 2; i++)
      for (int s = 0; s < 2; s++)
        if (!g_ib[c[i]].tag[s]) {
          IBucket *B = &g_ib[c[i]];
          B->tag[s] = tag;
          B->len[s] = klen;
          memcpy(B->e[s].key, kbuf, 16);
          atomic_store_explicit(&B->e[s].val, v, memory_order_relaxed);
          return;
        }
    /* Both full: evict a random entry of a random candidate; it moves on to its other bucket. */
    uint64_t r = rnd(&seed);
    uint64_t from = c[r & 1];
    IBucket *B = &g_ib[from];
    int s = (int)((r >> 1) & 1);
    uint8_t ot = B->tag[s], ol = B->len[s], ok[16];
    uint64_t ov = atomic_load_explicit(&B->e[s].val, memory_order_relaxed);
    memcpy(ok, B->e[s].key, 16);
    B->tag[s] = tag;
    B->len[s] = klen;
    memcpy(B->e[s].key, kbuf, 16);
    atomic_store_explicit(&B->e[s].val, v, memory_order_relaxed);
    tag = ot;
    klen = ol;
    v = ov;
    memcpy(kbuf, ok, 16);
    b = ib_alt(from, tag);
  }
  die("inline table insertion failed (load too high)");
}

/**
 * Finds a key's entry in one bucket under the bucket's seqlock.
 * @param B    bucket
 * @param q    the key as 16 zero-padded bytes
 * @param tag  its tag
 * @param len  its length
 * @returns the entry's value cell, or NULL.
 * @remarks Readers never write: they read the version, the entries, and the
 *          version again (after an acquire fence, so the entry reads cannot
 *          sink below it), retrying while a writer holds it odd: the store's
 *          protocol, in one line.
 */
static inline _Atomic uint64_t *ib_find(IBucket *B, __m128i q, uint8_t tag, uint8_t len) {
  for (;;) {
    uint64_t v = atomic_load_explicit(&B->ver, memory_order_acquire);
    if (v & 1) {
      _mm_pause();
      continue;
    }
    _Atomic uint64_t *hit = NULL;
    for (int s = 0; s < 2; s++)
      if (B->tag[s] == tag && B->len[s] == len &&
          _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(const void *)B->e[s].key), q)) ==
              0xFFFF)
        hit = &B->e[s].val;
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&B->ver, memory_order_relaxed) == v) return hit;
  }
}

/**
 * Looks a key up in both candidate buckets.
 * @param key  16 zero-padded bytes
 * @param len  key length
 * @param h    the key's SipHash-128
 * @returns the value cell, or NULL when absent.
 */
static inline _Atomic uint64_t *ib_get(const uint8_t key[16], uint8_t len, const uint64_t h[2]) {
  uint8_t tag = ib_tag(h[1]);
  uint64_t b1 = h[0] & g_ib_mask;
  __m128i q = _mm_loadu_si128((const __m128i *)(const void *)key);
  _Atomic uint64_t *c = ib_find(&g_ib[b1], q, tag, len);
  return c ? c : ib_find(&g_ib[ib_alt(b1, tag)], q, tag, len);
}

static KvTable *g_sstore;        /* the store holding the same small keys and values */
static Stream g_sq;              /* small-key query stream */
static uint32_t *g_sqid;         /* its ids */

/**
 * Inline-table operations over the small-key stream.
 * @param mode  0 = dependent gets, 1 = independent gets, 2 = batched gets
 *              (both buckets prefetched per batch of 32), 3 = batched
 *              increments (LOCK XADD on the value in the bucket)
 * @param hm    batched modes: HASH_ASM, or HASH_X8 for the AVX2 lanes
 * @returns total ns; exits if a key is missing.
 * @remarks Hashes with the store's assembly (or the bit-exact lanes) so the
 *          comparison with the store isolates the table layout.
 */
static uint64_t ib_pass(int mode, int hm) {
  uint64_t t0 = now_ns(), x = 0, h[BATCH][2];
  if (mode <= 1) {
    for (uint32_t i = 0; i < NQ; i++) {
      uint32_t q = mode == 0 ? (i + (uint32_t)x * (NQ / 2)) & (NQ - 1) : i;
      uint64_t hh[2];
      kv_store_hash(g_sstore, g_sq.key[q], 16, hh);
      _Atomic uint64_t *c = ib_get(g_sq.key[q], 16, hh);
      if (!c) die("inline get missed");
      uint64_t v = atomic_load_explicit(c, memory_order_relaxed);
      memcpy(g_sink, &v, 8);
      x = g_sink[7] & 1;
    }
    return now_ns() - t0;
  }
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    for (int k = 0; k < BATCH; k += 8) {
      hash8(g_sstore, &g_sq, b + (uint32_t)k, &h[k], hm);
      for (int l = k; l < k + 8; l++) {
        uint64_t b1 = h[l][0] & g_ib_mask;
        if (mode == 3) {   /* increments: prefetch for writing (the line arrives exclusive) */
          __builtin_prefetch(&g_ib[b1], 1);
          __builtin_prefetch(&g_ib[ib_alt(b1, ib_tag(h[l][1]))], 1);
        } else {
          __builtin_prefetch(&g_ib[b1]);
          __builtin_prefetch(&g_ib[ib_alt(b1, ib_tag(h[l][1]))]);
        }
      }
    }
    for (int k = 0; k < BATCH; k++) {
      _Atomic uint64_t *c = ib_get(g_sq.key[b + (uint32_t)k], 16, h[k]);
      if (!c) die("inline batched get missed");
      if (mode == 3) {
        atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
      } else {
        uint64_t v = atomic_load_explicit(c, memory_order_relaxed);
        memcpy(g_sink, &v, 8);
      }
    }
  }
  return now_ns() - t0;
}

/**
 * The same batched increments on the store holding the same keys: the
 * server's GET pipeline, then LOCK XADD on the record's 8-byte value.
 * @returns total ns; exits if a key is missing.
 * @remarks Values are 8-aligned (record header 24 bytes, key padded to 8),
 *          so the locked add never splits a cache line.
 */
static uint64_t sstore_incr_pass(void) {
  uint64_t t0 = now_ns(), h[BATCH][2];
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    hash_prefetch_batch(g_sstore, &g_sq, b, h, HASH_ASM);
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(g_sstore, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const KvRec *r = kv_store_get(g_sstore, g_sq.key[b + (uint32_t)k], 16, h[k][0], h[k][1]);
      if (!r) die("store increment missed");
      atomic_fetch_add_explicit((_Atomic uint64_t *)(uintptr_t)kv_rec_value(r), 1, memory_order_relaxed);
    }
  }
  return now_ns() - t0;
}

/* ========================================================================= */
/* Flat combining for concurrent writers                                      */
/* ========================================================================= */

enum { FC_IDLE = 0, FC_PENDING = 1, FC_DONE = 2 };

/** One thread's request slot, a cache line of its own (no false sharing between publishers). */
typedef struct {
  _Alignas(64) _Atomic uint32_t state;
  uint32_t len;
  const uint8_t *key;
  const uint8_t *val;
  uint64_t h1, h2;
  int64_t rc;
} FcSlot;

static FcSlot g_fc[MAX_THREADS];
static _Alignas(64) _Atomic int g_fc_lock;   /* held by the current combiner */
static int g_fc_threads;

/**
 * A put through flat combining: publish the request, then either become
 * the combiner and apply every pending request (prefetched as one batch),
 * or wait until a combiner applied ours.
 * @param tid  this thread's slot (< g_fc_threads)
 * @param key  key bytes
 * @param len  key length
 * @param val  VAL_LEN value bytes (must stay valid until this returns)
 * @param h1   the key's hash, low word
 * @param h2   the key's hash, high word
 * @returns kv_store_put's result for this request (1 created, 0 replaced,
 *          negative errno).
 * @remarks Only the combiner touches the store, so the store's writer mutex
 *          is never contended, and the combiner prefetches every pending
 *          request's buckets and records before applying them (the
 *          measured write-batching gain). Hendler, Incze, Shavit, Tzafrir,
 *          SPAA 2010. Release/acquire on `state` hands the request fields to
 *          the combiner and the result back.
 */
static int64_t fc_put(int tid, const uint8_t *key, uint32_t len, const uint8_t *val, uint64_t h1, uint64_t h2) {
  FcSlot *me = &g_fc[tid];
  me->key = key;
  me->len = len;
  me->val = val;
  me->h1 = h1;
  me->h2 = h2;
  atomic_store_explicit(&me->state, FC_PENDING, memory_order_release);
  for (;;) {
    if (atomic_load_explicit(&me->state, memory_order_acquire) == FC_DONE) break;
    /* Test before test-and-set: waiting threads only read the lock line. */
    if (atomic_load_explicit(&g_fc_lock, memory_order_relaxed) == 0 &&
        atomic_exchange_explicit(&g_fc_lock, 1, memory_order_acquire) == 0) {
      int idx[MAX_THREADS], n = 0;
      for (int i = 0; i < g_fc_threads; i++)
        if (atomic_load_explicit(&g_fc[i].state, memory_order_acquire) == FC_PENDING) idx[n++] = i;
      for (int k = 0; k < n; k++) kv_store_prefetch(g_store, g_fc[idx[k]].h1, g_fc[idx[k]].h2);
      for (int k = 0; k < n; k++) kv_store_prefetch_records(g_store, g_fc[idx[k]].h1, g_fc[idx[k]].h2);
      for (int k = 0; k < n; k++) {
        FcSlot *s = &g_fc[idx[k]];
        s->rc = kv_store_put(g_store, s->key, s->len, s->val, VAL_LEN, s->h1, s->h2);
        atomic_store_explicit(&s->state, FC_DONE, memory_order_release);
      }
      atomic_store_explicit(&g_fc_lock, 0, memory_order_release);
    } else {
      _mm_pause();
    }
  }
  atomic_store_explicit(&me->state, FC_IDLE, memory_order_relaxed);
  return me->rc;
}

/** One writer thread's assignment. */
typedef struct {
  int cpu, tid, combining;
  uint32_t ops, offset;
  pthread_barrier_t *start;
} WJob;

/**
 * Writer thread: replace-puts over the query stream, either taking the
 * store's writer mutex itself (kv_store_put) or through flat combining.
 * @param arg  WJob*
 * @returns NULL; exits if a put does not replace (every key exists).
 */
static void *writer_thread(void *arg) {
  WJob *j = arg;
  pin(j->cpu);
  uint8_t val[VAL_LEN];
  memset(val, 0x6B, VAL_LEN);
  pthread_barrier_wait(j->start);
  for (uint32_t i = 0; i < j->ops; i++) {
    uint32_t q = (j->offset + i) & (NQ - 1);
    uint64_t h[2];
    kv_store_hash(g_store, g_q.key[q], g_q.len[q], h);
    int64_t rc = j->combining ? fc_put(j->tid, g_q.key[q], g_q.len[q], val, h[0], h[1])
                              : kv_store_put(g_store, g_q.key[q], g_q.len[q], val, VAL_LEN, h[0], h[1]);
    if (rc != 0) die("a replace-put did not replace");
  }
  return NULL;
}

/**
 * Runs T writer threads and returns the wall time.
 * @param T          threads (1..MAX_THREADS)
 * @param per        puts per thread
 * @param combining  1 = flat combining, 0 = each thread takes the mutex
 * @returns ns from the common start to the last join.
 * @remarks The main thread goes QSBR-offline while it waits, so the
 *          writers' incremental reclamation is never held back by it.
 */
static uint64_t run_writers(int T, uint32_t per, int combining) {
  pthread_barrier_t start;
  pthread_t th[MAX_THREADS];
  WJob jobs[MAX_THREADS];
  pthread_barrier_init(&start, NULL, (unsigned)T + 1);
  g_fc_threads = T;
  for (int i = 0; i < T; i++) {
    jobs[i] = (WJob){g_tcpu[i], i, combining, per, (uint32_t)i * (NQ / MAX_THREADS), &start};
    if (pthread_create(&th[i], NULL, writer_thread, &jobs[i]) != 0) die("pthread_create");
  }
  kv_qsbr_offline(g_store, 0);
  pthread_barrier_wait(&start);
  uint64_t t0 = now_ns();
  for (int i = 0; i < T; i++) pthread_join(th[i], NULL);
  uint64_t wall = now_ns() - t0;
  kv_qsbr_online(g_store, 0);
  pthread_barrier_destroy(&start);
  return wall;
}

/* ========================================================================= */
/* WAITPKG: how fast a waiting thread wakes                                   */
/* ========================================================================= */

static _Alignas(64) _Atomic uint32_t g_ping;
static _Alignas(64) _Atomic uint32_t g_pong;
static int g_wait_mode;          /* 0 PAUSE spin, 1 UMWAIT, 2 futex */
static sigjmp_buf g_ill_jmp;
static uint32_t g_pp_rounds;

/**
 * SIGILL handler for the one UMWAIT trial: jumps back into umwait_works.
 * @param sig  the signal number (unused)
 */
static void on_sigill(int sig) {
  (void)sig;
  siglongjmp(g_ill_jmp, 1);
}

/**
 * Waits on a cache line with UMONITOR/UMWAIT: the core sleeps in C0.1
 * until the monitored line is written (or the TSC deadline passes).
 * @param w     word to watch
 * @param want  value to wait for
 * @remarks No system call and no spinning: the wake-up is a cache-coherence
 *          event. The value is re-checked between arming the monitor and
 *          sleeping, so a store in that gap is never missed. The OS caps
 *          each sleep (IA32_UMWAIT_CONTROL); the loop simply re-arms.
 */
__attribute__((target("waitpkg"))) static void wait_umwait(_Atomic uint32_t *w, uint32_t want) {
  while (atomic_load_explicit(w, memory_order_acquire) != want) {
    _umonitor((void *)(uintptr_t)w);
    if (atomic_load_explicit(w, memory_order_acquire) == want) break;
    _umwait(1, __rdtsc() + 200000);
  }
}

/**
 * Waits for a value with the selected method (g_wait_mode).
 * @param w     word
 * @param want  value
 */
static void wait_for(_Atomic uint32_t *w, uint32_t want) {
  if (g_wait_mode == 1) {
    wait_umwait(w, want);
  } else if (g_wait_mode == 2) {
    uint32_t v;
    while ((v = atomic_load_explicit(w, memory_order_acquire)) != want)
      syscall(SYS_futex, (uint32_t *)(uintptr_t)w, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
  } else {
    while (atomic_load_explicit(w, memory_order_acquire) != want) _mm_pause();
  }
}

/**
 * Publishes a value, and wakes a futex waiter in futex mode.
 * @param w  word
 * @param v  value
 * @remarks FUTEX_WAIT re-checks the word in the kernel, so a wake issued
 *          before the waiter sleeps is never lost.
 */
static void post(_Atomic uint32_t *w, uint32_t v) {
  atomic_store_explicit(w, v, memory_order_release);
  if (g_wait_mode == 2) syscall(SYS_futex, (uint32_t *)(uintptr_t)w, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

/**
 * The echoing thread of the ping-pong: waits for each ping, answers it.
 * @param arg  unused
 * @returns NULL.
 */
static void *ponger(void *arg) {
  (void)arg;
  pin(2);
  for (uint32_t i = 1; i <= g_pp_rounds; i++) {
    wait_for(&g_ping, i);
    post(&g_pong, i);
  }
  return NULL;
}

/**
 * Round-trip time between two threads on two P-cores (CPUs 1 and 2).
 * @param mode    wait method (0 PAUSE spin, 1 UMWAIT, 2 futex)
 * @param rounds  round trips
 * @returns ns per round trip.
 * @remarks Moves the calling thread to CPU 1; the caller re-pins it.
 */
static double ping_pong(int mode, uint32_t rounds) {
  pthread_t th;
  g_wait_mode = mode;
  g_pp_rounds = rounds;
  atomic_store(&g_ping, 0);
  atomic_store(&g_pong, 0);
  pin(1);
  if (pthread_create(&th, NULL, ponger, NULL) != 0) die("pthread_create");
  uint64_t t0 = now_ns();
  for (uint32_t i = 1; i <= rounds; i++) {
    post(&g_ping, i);
    wait_for(&g_pong, i);
  }
  uint64_t t = now_ns() - t0;
  pthread_join(th, NULL);
  return (double)t / rounds;
}

/**
 * Whether UMONITOR/UMWAIT execute here: a hypervisor may refuse them
 * (#UD, SIGILL) even when CPUID lists WAITPKG.
 * @returns 1 when one UMWAIT ran and returned, else 0.
 */
__attribute__((target("waitpkg"))) static int umwait_works(void) {
  struct sigaction sa, old;
  static _Atomic uint32_t probe;
  volatile int ok = 0;   /* volatile: survives the siglongjmp */
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_sigill;
  sigaction(SIGILL, &sa, &old);
  if (!sigsetjmp(g_ill_jmp, 1)) {
    _umonitor((void *)(uintptr_t)&probe);
    _umwait(1, __rdtsc() + 1000);
    ok = 1;
  }
  sigaction(SIGILL, &old, NULL);
  return ok;
}

/*
 * Timed kernels are never inlined, so a scalar row and its SIMD row pay the
 * same call (a SIMD function cannot inline into the baseline-compiled
 * driver, a scalar one could). Scalar references are also never auto-
 * vectorized: at -O2 GCC turns the "scalar" loops into SSE2, which the
 * first smoke run showed beating the AVX2 rows for that reason alone.
 */
#define KERNEL __attribute__((noinline))
#define SCALAR __attribute__((noinline, optimize("no-tree-vectorize")))

/* ========================================================================= */
/* Bitmaps                                                                    */
/* ========================================================================= */

enum { BM_BYTES = 1 << 20 };     /* one bitmap: 8,388,608 bits (e.g. "active today" per user id) */

/**
 * Popcount with the POPCNT instruction, four independent accumulators.
 * @param w  words
 * @param n  word count (multiple of 4)
 * @returns set bits.
 */
__attribute__((target("popcnt"))) SCALAR static uint64_t pop_scalar(const uint64_t *w, size_t n) {
  uint64_t a = 0, b = 0, c = 0, d = 0;
  for (size_t i = 0; i < n; i += 4) {
    a += (uint64_t)__builtin_popcountll(w[i]);
    b += (uint64_t)__builtin_popcountll(w[i + 1]);
    c += (uint64_t)__builtin_popcountll(w[i + 2]);
    d += (uint64_t)__builtin_popcountll(w[i + 3]);
  }
  return a + b + c + d;
}

/**
 * Per-byte popcounts of 32 bytes: two VPSHUFB lookups of a 16-entry nibble
 * table (Muła's method).
 * @param v  32 bytes
 * @returns 32 byte counts (0..8 each).
 */
__attribute__((target("avx2"))) static inline __m256i pop_bytes(__m256i v) {
  const __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 0, 1, 1, 2, 1, 2, 2, 3, 1,
                                       2, 2, 3, 2, 3, 3, 4);
  const __m256i low = _mm256_set1_epi8(0x0f);
  __m256i lo = _mm256_and_si256(v, low), hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), low);
  return _mm256_add_epi8(_mm256_shuffle_epi8(lut, lo), _mm256_shuffle_epi8(lut, hi));
}

/**
 * Horizontal sum of four 64-bit lanes, in registers.
 * @param v  four u64
 * @returns their sum.
 * @remarks Extract-and-add instead of a store and four loads: a 64-bit load
 *          from the upper half of a just-stored 256-bit vector can miss
 *          store-to-load forwarding and stall for ~15 cycles.
 */
__attribute__((target("avx2"))) static inline uint64_t hsum64(__m256i v) {
  __m128i x = _mm_add_epi64(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
  x = _mm_add_epi64(x, _mm_unpackhi_epi64(x, x));
  return (uint64_t)_mm_cvtsi128_si64(x);
}

/**
 * Popcount with the AVX2 nibble lookup: byte counts accumulate for up to 31
 * vectors, then VPSADBW folds them into 64-bit lanes.
 * @param p      bytes (32-byte aligned)
 * @param bytes  length (multiple of 32)
 * @returns set bits.
 * @remarks 31 vectors at most 8 per byte = 248: the byte counters cannot
 *          overflow before the fold.
 */
__attribute__((target("avx2"))) KERNEL static uint64_t pop_avx2_lookup(const uint8_t *p, size_t bytes) {
  __m256i acc = _mm256_setzero_si256(), local = _mm256_setzero_si256();
  for (size_t i = 0, k = 0; i < bytes; i += 32) {
    local = _mm256_add_epi8(local, pop_bytes(_mm256_load_si256((const __m256i *)(const void *)(p + i))));
    if (++k == 31) {
      acc = _mm256_add_epi64(acc, _mm256_sad_epu8(local, _mm256_setzero_si256()));
      local = _mm256_setzero_si256();
      k = 0;
    }
  }
  acc = _mm256_add_epi64(acc, _mm256_sad_epu8(local, _mm256_setzero_si256()));
  return hsum64(acc);
}

/** Carry-save adder over 256 bits: h:l = a + b + c per bit position (each argument evaluated once). */
#define CSA(h, l, a, b, c)                                                                   \
  do {                                                                                       \
    __m256i a_ = (a), b_ = (b), c_ = (c), u_ = _mm256_xor_si256(a_, b_);                       \
    (h) = _mm256_or_si256(_mm256_and_si256(a_, b_), _mm256_and_si256(u_, c_));                \
    (l) = _mm256_xor_si256(u_, c_);                                                          \
  } while (0)

/**
 * Popcount of 32 bytes as four 64-bit lane sums.
 * @param v  32 bytes
 * @returns per-lane counts.
 */
__attribute__((target("avx2"))) static inline __m256i pop256(__m256i v) {
  return _mm256_sad_epu8(pop_bytes(v), _mm256_setzero_si256());
}

/**
 * Popcount with AVX2 Harley-Seal (Muła, Kurz, Lemire 2018): a tree of
 * carry-save adders turns 16 vectors into one "sixteens" vector, so only
 * one in 16 vectors pays the lookup popcount.
 * @param d     vectors (32-byte aligned)
 * @param b     optional second operand: when not NULL, counts d AND b
 *              (the fused BITOP AND + BITCOUNT of a retention query)
 * @param nvec  vector count (multiple of 16)
 * @returns set bits (of d, or of d AND b).
 */
__attribute__((target("avx2"))) KERNEL static uint64_t pop_avx2_hs(const __m256i *d, const __m256i *b, size_t nvec) {
  __m256i total = _mm256_setzero_si256(), ones = total, twos = total, fours = total, eights = total;
  __m256i sixteens, twosA, twosB, foursA, foursB, eightsA, eightsB;
#define LD(i) (b ? _mm256_and_si256(_mm256_load_si256(d + (i)), _mm256_load_si256(b + (i))) \
                 : _mm256_load_si256(d + (i)))
  for (size_t i = 0; i < nvec; i += 16) {
    CSA(twosA, ones, ones, LD(i), LD(i + 1));
    CSA(twosB, ones, ones, LD(i + 2), LD(i + 3));
    CSA(foursA, twos, twos, twosA, twosB);
    CSA(twosA, ones, ones, LD(i + 4), LD(i + 5));
    CSA(twosB, ones, ones, LD(i + 6), LD(i + 7));
    CSA(foursB, twos, twos, twosA, twosB);
    CSA(eightsA, fours, fours, foursA, foursB);
    CSA(twosA, ones, ones, LD(i + 8), LD(i + 9));
    CSA(twosB, ones, ones, LD(i + 10), LD(i + 11));
    CSA(foursA, twos, twos, twosA, twosB);
    CSA(twosA, ones, ones, LD(i + 12), LD(i + 13));
    CSA(twosB, ones, ones, LD(i + 14), LD(i + 15));
    CSA(foursB, twos, twos, twosA, twosB);
    CSA(eightsB, fours, fours, foursA, foursB);
    CSA(sixteens, eights, eights, eightsA, eightsB);
    total = _mm256_add_epi64(total, pop256(sixteens));
  }
#undef LD
  total = _mm256_slli_epi64(total, 4);
  total = _mm256_add_epi64(total, _mm256_slli_epi64(pop256(eights), 3));
  total = _mm256_add_epi64(total, _mm256_slli_epi64(pop256(fours), 2));
  total = _mm256_add_epi64(total, _mm256_slli_epi64(pop256(twos), 1));
  total = _mm256_add_epi64(total, pop256(ones));
  return hsum64(total);
}

/**
 * Scalar AND-popcount (the BITOP AND + BITCOUNT reference).
 * @param a, b  words
 * @param n     word count (multiple of 4)
 * @returns set bits of a AND b.
 */
__attribute__((target("popcnt"))) SCALAR static uint64_t and_pop_scalar(const uint64_t *a, const uint64_t *b, size_t n) {
  uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (size_t i = 0; i < n; i += 4) {
    s0 += (uint64_t)__builtin_popcountll(a[i] & b[i]);
    s1 += (uint64_t)__builtin_popcountll(a[i + 1] & b[i + 1]);
    s2 += (uint64_t)__builtin_popcountll(a[i + 2] & b[i + 2]);
    s3 += (uint64_t)__builtin_popcountll(a[i + 3] & b[i + 3]);
  }
  return s0 + s1 + s2 + s3;
}

/**
 * First set bit at or after a position (BITPOS), scalar: word by word.
 * @param w      words
 * @param nbits  bitmap size (multiple of 256)
 * @param from   start bit (< nbits)
 * @returns the bit index, or nbits when none.
 */
SCALAR static uint64_t bitpos_scalar(const uint64_t *w, uint64_t nbits, uint64_t from) {
  uint64_t i = from >> 6, x = w[i] & (~0ull << (from & 63)), nw = nbits >> 6;
  while (!x) {
    if (++i == nw) return nbits;
    x = w[i];
  }
  return (i << 6) + (uint64_t)__builtin_ctzll(x);
}

/**
 * The same with AVX2: whole 32-byte blocks of zeros are skipped with one
 * VPTEST each, then TZCNT finds the bit.
 * @param w      words (32-byte aligned)
 * @param nbits  bitmap size (multiple of 256)
 * @param from   start bit (< nbits)
 * @returns the bit index, or nbits when none.
 */
__attribute__((target("avx2,bmi"))) KERNEL static uint64_t bitpos_avx2(const uint64_t *w, uint64_t nbits, uint64_t from) {
  uint64_t i = from >> 6, nw = nbits >> 6;
  uint64_t x = w[i] & (~0ull << (from & 63));
  while (!x && (++i & 3)) x = w[i];   /* finish the current 4-word block word by word */
  if (!x) {
    for (; i < nw; i += 4)
      if (!_mm256_testz_si256(_mm256_load_si256((const __m256i *)(const void *)(w + i)),
                              _mm256_set1_epi8(-1)))
        break;
    if (i >= nw) return nbits;
    while (!(x = w[i])) i++;
  }
  return (i << 6) + _tzcnt_u64(x);
}

enum { BM_BLOCK_WORDS = 512 };   /* 4 KiB per counted block: 256 u32 counts per MiB (0.1% overhead) */

/**
 * SETBIT that keeps the bitmap's block counts and total exact.
 * @param w      bitmap words
 * @param cnt    per-block set-bit counts
 * @param total  the bitmap's set-bit count
 * @param bit    bit index
 * @returns 1 when the bit was newly set, 0 when it was already set.
 * @remarks LOCK BTS (GCC emits it for a fetch-or tested on the same bit)
 *          reports whether this call flipped the bit, so only a flip pays
 *          the two LOCK XADDs, and concurrent SETBITs of the same bit count
 *          it once. A reader racing an in-flight SETBIT can see the bit
 *          before its count (BITCOUNT off by the in-flight updates, never
 *          more): the same answer an unordered read of a moving value gives.
 */
static inline int setbit_counted(uint64_t *w, uint32_t *cnt, uint64_t *total, uint32_t bit) {
  uint64_t m = 1ull << (bit & 63);
  if (__atomic_fetch_or(&w[bit >> 6], m, __ATOMIC_RELAXED) & m) return 0;
  __atomic_fetch_add(&cnt[bit / (BM_BLOCK_WORDS * 64)], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(total, 1, __ATOMIC_RELAXED);
  return 1;
}

/**
 * Set bits in words [a, b) of a bitmap (AVX2 nibble lookup).
 * @param w  bitmap words (32-byte aligned)
 * @param a  first word (multiple of 4)
 * @param b  end word (multiple of 4, >= a)
 * @returns the count (0 for an empty range).
 */
static inline uint64_t pop_words(const uint64_t *w, size_t a, size_t b) {
  return b > a ? pop_avx2_lookup((const uint8_t *)(w + a), (b - a) * 8) : 0;
}

/**
 * Sum of n u32 counts with AVX2 (eight per add), scalar tail.
 * @param c  counts
 * @param n  how many
 * @returns their sum.
 */
__attribute__((target("avx2"))) KERNEL static uint64_t sum_u32(const uint32_t *c, size_t n) {
  __m256i acc = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(c + i));
    acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(v)));
    acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_extracti128_si256(v, 1)));
  }
  uint64_t s = hsum64(acc);
  for (; i < n; i++) s += c[i];
  return s;
}

/**
 * Set bits of words [ws, we) inside one counted block: the range itself or
 * the block's count minus its complement, whichever touches fewer words.
 * @param w    bitmap words
 * @param cnt  block counts
 * @param b    the block
 * @param ws   first word (multiple of 4, inside block b)
 * @param we   end word (multiple of 4, <= the block's end)
 * @returns the count.
 */
static inline uint64_t pop_in_block(const uint64_t *w, const uint32_t *cnt, size_t b, size_t ws, size_t we) {
  size_t b0 = b * BM_BLOCK_WORDS, b1 = b0 + BM_BLOCK_WORDS;
  if (we - ws <= BM_BLOCK_WORDS / 2) return pop_words(w, ws, we);
  return cnt[b] - pop_words(w, b0, ws) - pop_words(w, we, b1);
}

/**
 * BITCOUNT of words [ws, we) through the block-count directory: full
 * blocks come from their counts, each edge block from pop_in_block (at
 * most half a block, 2 KiB, scanned per edge).
 * @param w    bitmap words (32-byte aligned)
 * @param cnt  block counts (kept by setbit_counted)
 * @param ws   first word (multiple of 4)
 * @param we   end word (multiple of 4, ws <= we <= the bitmap's words)
 * @returns the count of set bits in the range.
 * @remarks Turns an O(range) scan (1 MiB = 14 us at 74 GB/s) into O(1)
 *          memory: <= 4 KiB of words plus <= 256 counts. A block-aligned
 *          end never touches the count past the last block.
 */
static uint64_t bitcount_dir(const uint64_t *w, const uint32_t *cnt, size_t ws, size_t we) {
  size_t bs = ws / BM_BLOCK_WORDS, be = we / BM_BLOCK_WORDS;
  if (bs == be || (be == bs + 1 && we % BM_BLOCK_WORDS == 0))
    return pop_in_block(w, cnt, bs, ws, we);
  uint64_t c = pop_in_block(w, cnt, bs, ws, (bs + 1) * BM_BLOCK_WORDS);
  if (we % BM_BLOCK_WORDS) c += pop_in_block(w, cnt, be, be * BM_BLOCK_WORDS, we);
  return c + sum_u32(cnt + bs + 1, be - bs - 1);
}

/* ========================================================================= */
/* Struct values: fields inside a value                                       */
/* ========================================================================= */

/*
 * Struct value layout (offsets from the value's first byte, which the store
 * keeps 8-aligned):
 *   [0..4)    magic "KVST"     [4] field count (<= 16)
 *   [8..24)   16 field tags (CRC32C of the name, top byte; 0 = unused lane)
 *   [24..56)  16 u16 offsets of the field records
 *   [56..440) 16 field records of 24 bytes: [u8 name_len][name <= 15 bytes]
 *             [8-byte value at +16]
 * A lookup touches the directory (bytes 0..56) and one field record, which
 * holds the name and the value together; values are 8-aligned, so a numeric
 * field takes LOCK XADD in place and never splits a cache line.
 */
enum { ST_FIELDS = 16, ST_TAGS = 8, ST_OFFS = 24, ST_RECS = 56, ST_REC = 24, ST_LEN = ST_RECS + ST_FIELDS * ST_REC };
static const char *const kStFields[ST_FIELDS] = {"name",  "email",  "plan",    "prefs",    "country", "lang",
                                                 "tz",    "created", "lastSeen", "logins", "credits", "tier",
                                                 "flags", "ref",    "score",   "verified"};
static uint8_t g_st_tag[ST_FIELDS];
static uint8_t g_st_len[ST_FIELDS];

/**
 * CRC32C of a short string (SSE4.2 CRC32 instruction): the field-name hash.
 * @param s  bytes
 * @param n  length
 * @returns the CRC.
 */
__attribute__((target("sse4.2"))) static uint32_t crc32c_str(const uint8_t *s, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = _mm_crc32_u8(c, s[i]);
  return ~c;
}

/**
 * A field name's 1-byte tag.
 * @param name  NUL-terminated field name
 * @returns the CRC's top byte, or 1 when that is 0 (0 marks an unused lane).
 */
static uint8_t st_tag_of(const char *name) {
  uint8_t t = (uint8_t)(crc32c_str((const uint8_t *)name, strlen(name)) >> 24);
  return t ? t : 1;
}

/**
 * Builds a struct value with all 16 fields.
 * @param out  ST_LEN bytes
 * @param id   record id (field f's value is id * 1000003 + f)
 */
static void st_build(uint8_t *out, uint32_t id) {
  memset(out, 0, ST_LEN);
  memcpy(out, "KVST", 4);
  out[4] = ST_FIELDS;
  for (int f = 0; f < ST_FIELDS; f++) {
    uint16_t off = (uint16_t)(ST_RECS + ST_REC * f);
    out[ST_TAGS + f] = g_st_tag[f];
    memcpy(out + ST_OFFS + 2 * f, &off, 2);
    out[off] = g_st_len[f];
    memcpy(out + off + 1, kStFields[f], g_st_len[f]);
    uint64_t v = (uint64_t)id * 1000003u + (uint64_t)f;
    memcpy(out + off + 16, &v, 8);
  }
}

/**
 * The tag-match lanes of a field: one SSE2 compare of the tag against all
 * 16 tag lanes (PCMPEQB, PMOVMSKB), limited to the value's field count.
 * @param v    the struct value
 * @param tag  the field name's tag
 * @returns a bit mask of candidate lanes (usually one bit).
 */
static inline uint32_t st_lanes(const uint8_t *v, uint8_t tag) {
  __m128i tags = _mm_loadu_si128((const __m128i *)(const void *)(v + ST_TAGS));
  uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(tags, _mm_set1_epi8((char)tag)));
  return m & (uint32_t)((1u << v[4]) - 1);
}

/**
 * A candidate lane's field record.
 * @param v     the struct value
 * @param lane  lane index (< 16)
 * @returns the 24-byte field record.
 */
static inline const uint8_t *st_rec(const uint8_t *v, uint32_t lane) {
  uint16_t off;
  memcpy(&off, v + ST_OFFS + 2 * lane, 2);
  return v + off;
}

/**
 * Finds a field inside a struct value: tag lanes first, then an exact name
 * compare of each candidate.
 * @param v     the struct value
 * @param name  field name
 * @param nlen  its length
 * @param tag   its tag
 * @returns the field's 8-byte value, or NULL when absent.
 */
static inline const uint8_t *st_field(const uint8_t *v, const char *name, size_t nlen, uint8_t tag) {
  for (uint32_t m = st_lanes(v, tag); m; m &= m - 1) {
    const uint8_t *f = st_rec(v, (uint32_t)__builtin_ctz(m));
    if (f[0] == nlen && !memcmp(f + 1, name, nlen)) return f + 16;
  }
  return NULL;
}

/*
 * glibc's memcpy, called through a pointer the compiler cannot see through:
 * the server copies values with `call memcpy wrt ..plt` (grpc.asm), and a
 * constant 440-byte memcpy would otherwise be inlined as `rep movsq`, whose
 * startup cost would handicap the whole-value get against HGET.
 */
static void *(*volatile g_memcpy)(void *, const void *, size_t) = memcpy;

/**
 * Whole-value gets, HGET or HINCRBY over a struct store, one pass.
 * @param t     the struct store
 * @param s     its query stream
 * @param fld   NQ field numbers (< ST_FIELDS)
 * @param mode  0 = get the whole value (the server's pipeline + copy);
 *              1 = HGET one field; 2 = HINCRBY one field
 * @param sink  accumulates the fields read (mode 1)
 * @returns total ns; exits if a key or field is missing.
 * @remarks Modes 1-2 add two stages to the server's pipeline: once the
 *          record is found, prefetch its directory; once the directory is
 *          read, prefetch the field record; then compare the name and read
 *          (or LOCK XADD) the value. Each stage issues 32 independent
 *          misses, so the two extra dependent misses per key overlap.
 */
static uint64_t st_pass(KvTable *t, const Stream *s, const uint8_t *fld, int mode, uint64_t *sink) {
  uint64_t t0 = now_ns(), h[BATCH][2], acc = 0;
  const uint8_t *val[BATCH], *fr[BATCH];
  for (uint32_t b = 0; b < NQ; b += BATCH) {
    hash_prefetch_batch(t, s, b, h, HASH_ASM);
    for (int k = 0; k < BATCH; k++) kv_store_prefetch_records(t, h[k][0], h[k][1]);
    for (int k = 0; k < BATCH; k++) {
      const KvRec *r = kv_store_get(t, s->key[b + (uint32_t)k], s->len[b + (uint32_t)k], h[k][0], h[k][1]);
      if (!r) die("struct get missed");
      val[k] = kv_rec_value(r);
      if (mode == 0) {
        g_memcpy(g_sink, val[k], ST_LEN);
      } else {
        __builtin_prefetch(val[k]);
        __builtin_prefetch(val[k] + ST_RECS - 1);   /* the directory may straddle two lines */
      }
    }
    if (mode == 0) continue;
    for (int k = 0; k < BATCH; k++) {
      uint32_t m = st_lanes(val[k], g_st_tag[fld[b + (uint32_t)k]]);
      fr[k] = m ? st_rec(val[k], (uint32_t)__builtin_ctz(m)) : NULL;
      if (fr[k] && mode == 2) {   /* HINCRBY: fetch the field's line exclusive, ready for the locked add */
        __builtin_prefetch(fr[k], 1);
        __builtin_prefetch(fr[k] + ST_REC - 1, 1);
      } else if (fr[k]) {
        __builtin_prefetch(fr[k]);
        __builtin_prefetch(fr[k] + ST_REC - 1);
      }
    }
    for (int k = 0; k < BATCH; k++) {
      uint32_t f = fld[b + (uint32_t)k];
      const uint8_t *p = fr[k] && fr[k][0] == g_st_len[f] && !memcmp(fr[k] + 1, kStFields[f], g_st_len[f])
                             ? fr[k] + 16
                             : st_field(val[k], kStFields[f], g_st_len[f], g_st_tag[f]);   /* tag collision */
      if (!p) die("struct field missing");
      if (mode == 2) {
        atomic_fetch_add_explicit((_Atomic uint64_t *)(uintptr_t)p, 1, memory_order_relaxed);
      } else {
        uint64_t x;
        memcpy(&x, p, 8);
        acc += x;
      }
    }
  }
  *sink += acc;
  return now_ns() - t0;
}

/* ========================================================================= */
/* Sets: sorted u32 arrays                                                    */
/* ========================================================================= */

/**
 * Membership by branchless binary search (CMOV), then one compare.
 * @param a  sorted set
 * @param n  size (>= 1)
 * @param x  element
 * @returns 1 when present, else 0.
 */
SCALAR static int set_has_bl(const uint32_t *a, uint32_t n, uint32_t x) {
  const uint32_t *base = a;
  uint32_t len = n;
  while (len > 1) {
    uint32_t half = len / 2;
    base += base[half - 1] < x ? half : 0;
    len -= half;
  }
  return *base == x;
}

/**
 * Membership by AVX2 linear scan: eight elements per VPCMPEQD, OR-reduced.
 * @param a  set (32-byte aligned)
 * @param n  size (multiple of 8)
 * @param x  element
 * @returns 1 when present, else 0.
 */
__attribute__((target("avx2"))) KERNEL static int set_has_avx2(const uint32_t *a, uint32_t n, uint32_t x) {
  __m256i q = _mm256_set1_epi32((int)x), acc = _mm256_setzero_si256();
  for (uint32_t i = 0; i < n; i += 8)
    acc = _mm256_or_si256(acc, _mm256_cmpeq_epi32(_mm256_load_si256((const __m256i *)(const void *)(a + i)), q));
  return !_mm256_testz_si256(acc, acc);
}

/**
 * Intersection size by a branchy merge (the textbook).
 * @param a   first set
 * @param na  its size
 * @param b   second set
 * @param nb  its size
 * @returns |a ∩ b|.
 */
SCALAR static uint32_t inter_scalar(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
  uint32_t i = 0, j = 0, c = 0;
  while (i < na && j < nb) {
    if (a[i] < b[j]) i++;
    else if (a[i] > b[j]) j++;
    else {
      c++;
      i++;
      j++;
    }
  }
  return c;
}

/**
 * Intersection size by a branchless merge: both cursors advance by
 * comparison results, so no branch depends on the data.
 * @param a   first set
 * @param na  its size
 * @param b   second set
 * @param nb  its size
 * @returns |a ∩ b|.
 */
SCALAR static uint32_t inter_branchless(const uint32_t *a, uint32_t na, const uint32_t *b, uint32_t nb) {
  uint32_t i = 0, j = 0, c = 0;
  while (i < na && j < nb) {
    uint32_t x = a[i], y = b[j];
    c += x == y;
    i += x <= y;
    j += y <= x;
  }
  return c;
}

/**
 * Intersection size with AVX2 all-pairs blocks (Schlegel et al.; Lemire,
 * Boytsov, Kurz): eight elements of a against eight of b with eight
 * VPCMPEQD over the rotations of b's block (VPERMD), then the block with
 * the smaller maximum advances; the tails finish with the scalar merge.
 * @param a   first set (32-byte aligned)
 * @param na  its size
 * @param b   second set (32-byte aligned)
 * @param nb  its size
 * @returns |a ∩ b|.
 * @remarks Sets have distinct elements, so each equal pair is counted
 *          exactly once: when its two blocks are current together. Tail
 *          elements were never in a compared block, so the scalar merge
 *          cannot count a pair twice.
 */
__attribute__((target("avx2,popcnt"))) KERNEL static uint32_t inter_avx2(const uint32_t *a, uint32_t na, const uint32_t *b,
                                                                   uint32_t nb) {
  uint32_t i = 0, j = 0, c = 0;
  const __m256i rot = _mm256_setr_epi32(1, 2, 3, 4, 5, 6, 7, 0);
  while (i + 8 <= na && j + 8 <= nb) {
    __m256i va = _mm256_load_si256((const __m256i *)(const void *)(a + i));
    __m256i vb = _mm256_load_si256((const __m256i *)(const void *)(b + j));
    __m256i m = _mm256_cmpeq_epi32(va, vb);
    for (int r = 1; r < 8; r++) {
      vb = _mm256_permutevar8x32_epi32(vb, rot);
      m = _mm256_or_si256(m, _mm256_cmpeq_epi32(va, vb));
    }
    c += (uint32_t)__builtin_popcount((unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(m)));
    uint32_t amax = a[i + 7], bmax = b[j + 7];
    i += amax <= bmax ? 8 : 0;
    j += bmax <= amax ? 8 : 0;
  }
  return c + inter_scalar(a + i, na - i, b + j, nb - j);
}

/* ========================================================================= */
/* Value kernels: window sums, MinHash, vector dot products                   */
/* ========================================================================= */

/**
 * Sum of 64 u32 window buckets (a sliding-window counter's total), scalar.
 * @param w  64 buckets
 * @returns their sum.
 */
SCALAR static uint64_t win_sum_scalar(const uint32_t *w) {
  uint64_t s = 0;
  for (int i = 0; i < 64; i++) s += w[i];
  return s;
}

/**
 * The same with AVX2: widen to 64-bit lanes (VPMOVZXDQ) and add.
 * @param w  64 buckets (32-byte aligned)
 * @returns their sum.
 */
__attribute__((target("avx2"))) KERNEL static uint64_t win_sum_avx2(const uint32_t *w) {
  __m256i acc = _mm256_setzero_si256();
  for (int i = 0; i < 64; i += 8) {
    __m256i v = _mm256_load_si256((const __m256i *)(const void *)(w + i));
    acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_castsi256_si128(v)));
    acc = _mm256_add_epi64(acc, _mm256_cvtepu32_epi64(_mm256_extracti128_si256(v, 1)));
  }
  return hsum64(acc);
}

/**
 * MinHash agreement of two 128-entry signatures (Jaccard estimate =
 * agreement / 128), scalar.
 * @param a  first signature
 * @param b  second signature
 * @returns entries that agree.
 */
SCALAR static uint32_t minhash_scalar(const uint32_t *a, const uint32_t *b) {
  uint32_t c = 0;
  for (int i = 0; i < 128; i++) c += a[i] == b[i];
  return c;
}

/**
 * The same with AVX2: VPCMPEQD + VMOVMSKPS + POPCNT per eight entries.
 * @param a  first signature (32-byte aligned)
 * @param b  second signature (32-byte aligned)
 * @returns entries that agree.
 */
__attribute__((target("avx2,popcnt"))) KERNEL static uint32_t minhash_avx2(const uint32_t *a, const uint32_t *b) {
  uint32_t c = 0;
  for (int i = 0; i < 128; i += 8) {
    __m256i e = _mm256_cmpeq_epi32(_mm256_load_si256((const __m256i *)(const void *)(a + i)),
                                   _mm256_load_si256((const __m256i *)(const void *)(b + i)));
    c += (uint32_t)__builtin_popcount((unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(e)));
  }
  return c;
}

enum { DIM = 256 };

/**
 * int8 dot product (u8 x s8 -> s32), scalar reference.
 * @param u  DIM unsigned bytes
 * @param s  DIM signed bytes
 * @returns the exact dot product.
 */
SCALAR static int32_t dot_i8_scalar(const uint8_t *u, const int8_t *s) {
  int32_t acc = 0;
  for (int i = 0; i < DIM; i++) acc += (int32_t)u[i] * (int32_t)s[i];
  return acc;
}

/**
 * Horizontal sum of eight s32 lanes.
 * @param v  eight s32
 * @returns their sum.
 */
__attribute__((target("avx2"))) static inline int32_t hsum32(__m256i v) {
  __m128i x = _mm_add_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
  x = _mm_add_epi32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(1, 0, 3, 2)));
  x = _mm_add_epi32(x, _mm_shuffle_epi32(x, _MM_SHUFFLE(2, 3, 0, 1)));
  return _mm_cvtsi128_si32(x);
}

/**
 * int8 dot product with AVX2: VPMADDUBSW (u8 x s8 pairs to s16) then
 * VPMADDWD by ones (s16 pairs to s32).
 * @param u  DIM unsigned bytes (32-byte aligned)
 * @param s  DIM signed bytes (32-byte aligned)
 * @returns the dot product; exact only while a pair sum fits s16
 *          (VPMADDUBSW saturates): here u <= 127 and |s| <= 127.
 */
__attribute__((target("avx2"))) KERNEL static int32_t dot_i8_avx2(const uint8_t *u, const int8_t *s) {
  __m256i acc = _mm256_setzero_si256();
  const __m256i ones = _mm256_set1_epi16(1);
  for (int i = 0; i < DIM; i += 32) {
    __m256i p = _mm256_maddubs_epi16(_mm256_load_si256((const __m256i *)(const void *)(u + i)),
                                     _mm256_load_si256((const __m256i *)(const void *)(s + i)));
    acc = _mm256_add_epi32(acc, _mm256_madd_epi16(p, ones));
  }
  return hsum32(acc);
}

/**
 * int8 dot product with AVX-VNNI: VPDPBUSD multiplies four u8 x s8 pairs
 * per dword lane and accumulates into s32, in one instruction.
 * @param u  DIM unsigned bytes (32-byte aligned)
 * @param s  DIM signed bytes (32-byte aligned)
 * @returns the exact dot product (s32 accumulation, no s16 saturation).
 * @remarks Four accumulators: VPDPBUSD reads its accumulator, so one
 *          accumulator chains every instruction behind the previous one's
 *          5-cycle latency (the first smoke run measured exactly that, and
 *          the AVX2 pair, whose chain is a 1-cycle add, won).
 */
__attribute__((target("avxvnni,avx2"))) KERNEL static int32_t dot_i8_vnni(const uint8_t *u, const int8_t *s) {
  __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
  for (int i = 0; i < DIM; i += 128) {
    a0 = _mm256_dpbusd_avx_epi32(a0, _mm256_load_si256((const __m256i *)(const void *)(u + i)),
                                 _mm256_load_si256((const __m256i *)(const void *)(s + i)));
    a1 = _mm256_dpbusd_avx_epi32(a1, _mm256_load_si256((const __m256i *)(const void *)(u + i + 32)),
                                 _mm256_load_si256((const __m256i *)(const void *)(s + i + 32)));
    a2 = _mm256_dpbusd_avx_epi32(a2, _mm256_load_si256((const __m256i *)(const void *)(u + i + 64)),
                                 _mm256_load_si256((const __m256i *)(const void *)(s + i + 64)));
    a3 = _mm256_dpbusd_avx_epi32(a3, _mm256_load_si256((const __m256i *)(const void *)(u + i + 96)),
                                 _mm256_load_si256((const __m256i *)(const void *)(s + i + 96)));
  }
  return hsum32(_mm256_add_epi32(_mm256_add_epi32(a0, a1), _mm256_add_epi32(a2, a3)));
}

/**
 * fp32 dot product, scalar.
 * @param a  DIM floats
 * @param b  DIM floats
 * @returns the dot product.
 */
SCALAR static float dot_f32_scalar(const float *a, const float *b) {
  float acc = 0;
  for (int i = 0; i < DIM; i++) acc += a[i] * b[i];
  return acc;
}

/**
 * fp32 dot product with AVX2 FMA, four accumulators (hides FMA latency).
 * @param a  DIM floats (32-byte aligned)
 * @param b  DIM floats (32-byte aligned)
 * @returns the dot product (summation order differs from the scalar one).
 */
__attribute__((target("avx2,fma"))) KERNEL static float dot_f32_fma(const float *a, const float *b) {
  __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
  for (int i = 0; i < DIM; i += 32) {
    s0 = _mm256_fmadd_ps(_mm256_load_ps(a + i), _mm256_load_ps(b + i), s0);
    s1 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 8), _mm256_load_ps(b + i + 8), s1);
    s2 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 16), _mm256_load_ps(b + i + 16), s2);
    s3 = _mm256_fmadd_ps(_mm256_load_ps(a + i + 24), _mm256_load_ps(b + i + 24), s3);
  }
  __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
  __m128 x = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
  x = _mm_add_ps(x, _mm_movehl_ps(x, x));
  x = _mm_add_ss(x, _mm_movehdup_ps(x));
  return _mm_cvtss_f32(x);
}

/* ========================================================================= */
/* TopK: heavy hitters with HeavyKeeper                                       */
/* ========================================================================= */

enum { HK_ROWS = 2, HK_W = 4096, TOPK = 100, TOPK_PAD = 104 };

/** One HeavyKeeper bucket: a key fingerprint and its (decayed) count. */
typedef struct {
  uint32_t fp, cnt;
} HkCell;

static HkCell g_hk[HK_ROWS][HK_W];               /* 64 KiB: stays in L2 */
static uint32_t g_hk_decay[256];                 /* P(decay) = 1.08^-count, as a 32-bit threshold */
static uint64_t g_hk_rng;
static _Alignas(32) uint32_t g_top_id[TOPK_PAD]; /* min-heap by count; unused lanes hold UINT32_MAX */
static uint32_t g_top_cnt[TOPK];
static int g_top_n;

/**
 * splitmix64 finalizer: a fast, well-mixed hash of an id.
 * @param x  input
 * @returns 64 mixed bits.
 * @remarks Stands in for the key's SipHash, which a server already has
 *          from the lookup that precedes a sketch update.
 */
static inline uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ull;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

/**
 * Empties the sketch and the heap, and fills the decay table.
 * @remarks HeavyKeeper's exponential decay (b = 1.08): a bucket held by
 *          another key loses one count with probability b^-count, so a
 *          heavy key is almost never evicted and a light one is quickly.
 */
static void hk_init(void) {
  memset(g_hk, 0, sizeof g_hk);
  for (int c = 0; c < 256; c++) g_hk_decay[c] = (uint32_t)(4294967295.0 * pow(1.08, -c));
  for (int i = 0; i < TOPK_PAD; i++) g_top_id[i] = UINT32_MAX;
  memset(g_top_cnt, 0, sizeof g_top_cnt);
  g_top_n = 0;
  g_hk_rng = 0x9E3779B97F4A7C15ull;
}

/**
 * Whether an id is in the top-K heap: 13 AVX2 compares of eight ids.
 * @param id  the id
 * @returns its heap index, or -1.
 * @remarks K = 100 ids fit in 13 vectors, so a linear SIMD scan replaces
 *          the hash map a heap of keys usually needs.
 */
__attribute__((target("avx2"))) static int top_find(uint32_t id) {
  __m256i q = _mm256_set1_epi32((int)id);
  for (int i = 0; i < TOPK_PAD; i += 8) {
    __m256i e = _mm256_cmpeq_epi32(_mm256_load_si256((const __m256i *)(const void *)(g_top_id + i)), q);
    uint32_t m = (uint32_t)_mm256_movemask_ps(_mm256_castsi256_ps(e));
    if (m) return i + __builtin_ctz(m);
  }
  return -1;
}

/**
 * Swaps two heap entries.
 * @param a, b  heap indexes
 */
static inline void top_swap(int a, int b) {
  uint32_t i = g_top_id[a], c = g_top_cnt[a];
  g_top_id[a] = g_top_id[b];
  g_top_cnt[a] = g_top_cnt[b];
  g_top_id[b] = i;
  g_top_cnt[b] = c;
}

/**
 * Restores the min-heap below an entry whose count grew.
 * @param i  heap index
 */
static void top_down(int i) {
  for (;;) {
    int l = 2 * i + 1, r = l + 1, m = i;
    if (l < g_top_n && g_top_cnt[l] < g_top_cnt[m]) m = l;
    if (r < g_top_n && g_top_cnt[r] < g_top_cnt[m]) m = r;
    if (m == i) return;
    top_swap(i, m);
    i = m;
  }
}

/**
 * Restores the min-heap above a newly appended entry.
 * @param i  heap index
 */
static void top_up(int i) {
  while (i > 0) {
    int p = (i - 1) / 2;
    if (g_top_cnt[p] <= g_top_cnt[i]) return;
    top_swap(i, p);
    i = p;
  }
}

/**
 * Counts one occurrence of an id: HeavyKeeper buckets in both rows, then
 * the top-K heap.
 * @param id  the id (< UINT32_MAX)
 * @remarks Single writer (a server would keep one per worker and merge on
 *          read, as with counter cells). Cost: two L2-resident buckets, one
 *          SIMD heap scan, a sift of at most log2(100) = 7 levels.
 */
static void hk_update(uint32_t id) {
  uint64_t h = mix64(id);
  uint32_t fp = (uint32_t)(h >> 32) | 1, est = 0;
  for (int r = 0; r < HK_ROWS; r++) {
    HkCell *c = &g_hk[r][(h >> (12 * r)) & (HK_W - 1)];
    if (c->fp == fp) {
      c->cnt++;
    } else if (c->cnt == 0) {
      c->fp = fp;
      c->cnt = 1;
    } else if ((uint32_t)rnd(&g_hk_rng) < g_hk_decay[c->cnt < 255 ? c->cnt : 255] && --c->cnt == 0) {
      c->fp = fp;   /* decayed to zero: the newcomer takes the bucket */
      c->cnt = 1;
    }
    if (c->fp == fp && c->cnt > est) est = c->cnt;
  }
  int pos = top_find(id);
  if (pos >= 0) {
    if (est > g_top_cnt[pos]) {
      g_top_cnt[pos] = est;
      top_down(pos);
    }
  } else if (g_top_n < TOPK) {
    g_top_id[g_top_n] = id;
    g_top_cnt[g_top_n] = est;
    top_up(g_top_n++);
  } else if (est > g_top_cnt[0]) {
    g_top_id[0] = id;
    g_top_cnt[0] = est;
    top_down(0);
  }
}

/* ========================================================================= */
/* Quantiles: DDSketch                                                        */
/* ========================================================================= */

enum { DD_BINS = 2048, DD_GROUP = 64 };
#define DD_ALPHA 0.01
static uint64_t g_dd[DD_BINS], g_dd_coarse[DD_BINS / DD_GROUP], g_dd_n;
static double g_dd_gamma, g_dd_inv_ln_gamma;

/**
 * Empties the sketch and sets its accuracy.
 * @remarks gamma = (1 + a) / (1 - a) makes every bucket [gamma^(i-1),
 *          gamma^i] narrow enough that its representative value is within
 *          a relative a of anything in it (Masson, Rim, Lee, VLDB 2019).
 */
static void dd_init(void) {
  memset(g_dd, 0, sizeof g_dd);
  memset(g_dd_coarse, 0, sizeof g_dd_coarse);
  g_dd_n = 0;
  g_dd_gamma = (1 + DD_ALPHA) / (1 - DD_ALPHA);
  g_dd_inv_ln_gamma = 1.0 / log(g_dd_gamma);
}

/**
 * Adds a value (>= 1): one log, two increments.
 * @param x  the value (e.g. a latency in microseconds)
 * @remarks The coarse counts (one per 64 buckets) cost one more increment
 *          and let a quantile query skip 64 buckets per step.
 */
static inline void dd_add(double x) {
  int i = (int)ceil(log(x) * g_dd_inv_ln_gamma);
  i = i < 0 ? 0 : i >= DD_BINS ? DD_BINS - 1 : i;
  g_dd[i]++;
  g_dd_coarse[i / DD_GROUP]++;
  g_dd_n++;
}

/**
 * A bucket's representative value.
 * @param i  bucket
 * @returns 2 gamma^i / (gamma + 1), within a relative alpha of the bucket.
 */
static double dd_value(int i) { return 2.0 * pow(g_dd_gamma, i) / (g_dd_gamma + 1.0); }

/**
 * The q-quantile by walking every bucket.
 * @param q  quantile in [0, 1]
 * @returns the estimate of the value of rank floor(q (n - 1)).
 */
KERNEL static double dd_quantile_walk(double q) {
  uint64_t rank = (uint64_t)(q * (double)(g_dd_n - 1)), c = 0;
  for (int i = 0; i < DD_BINS; i++) {
    c += g_dd[i];
    if (c > rank) return dd_value(i);
  }
  return dd_value(DD_BINS - 1);
}

/**
 * The q-quantile through the coarse counts: skip whole groups of 64
 * buckets, then walk inside one group.
 * @param q  quantile in [0, 1]
 * @returns the same estimate as dd_quantile_walk.
 */
KERNEL static double dd_quantile_2lvl(double q) {
  uint64_t rank = (uint64_t)(q * (double)(g_dd_n - 1)), c = 0;
  int g = 0;
  while (g < DD_BINS / DD_GROUP - 1 && c + g_dd_coarse[g] <= rank) c += g_dd_coarse[g++];
  for (int i = g * DD_GROUP; i < DD_BINS; i++) {
    c += g_dd[i];
    if (c > rank) return dd_value(i);
  }
  return dd_value(DD_BINS - 1);
}

/* ========================================================================= */
/* SimHash: near-duplicate search with multi-index hashing                    */
/* ========================================================================= */

enum { MIH_T = 4, MIH_B = 1 << 16, MIH_MAXD = 3, MIH_MAXOUT = 64 };
static uint64_t *g_fp;                    /* n fingerprints, by id */
static uint32_t *g_mih_off[MIH_T];        /* MIH_B + 1 offsets per table (CSR) */
static uint64_t *g_mih_code[MIH_T];       /* codes grouped by block value */
static uint32_t *g_mih_id[MIH_T];         /* their ids */

/**
 * Block t (16 bits) of a 64-bit code.
 * @param x  code
 * @param t  block number (0..3)
 * @returns bits [16t, 16t + 16).
 */
static inline uint32_t mih_blk(uint64_t x, int t) { return (uint32_t)(x >> (16 * t)) & 0xFFFF; }

/**
 * Whether an earlier table already reported a code: it did when the code's
 * block there equals the query's (both tables test the same distance).
 * @param code  candidate
 * @param q     query
 * @param t     current table
 * @returns 1 when a table before t holds it under the query's block.
 * @remarks Reports each match exactly once without a de-duplication set.
 */
static inline int mih_seen(uint64_t code, uint64_t q, int t) {
  int dup = 0;
  for (int u = 0; u < t; u++) dup |= mih_blk(code, u) == mih_blk(q, u);
  return dup;
}

/**
 * Near-duplicate query, scalar scan. By the pigeonhole principle a code
 * within Hamming distance 3 of q agrees with q exactly on at least one of
 * its four 16-bit blocks, so the four posting lists of q's blocks hold
 * every answer (Norouzi, Punjani, Fleet, CVPR 2012).
 * @param q        64-bit query fingerprint
 * @param out      receives matching ids (at most MIH_MAXOUT)
 * @param scanned  incremented by the number of candidates examined
 * @returns matches written to out.
 */
__attribute__((target("popcnt"))) static uint32_t mih_query_scalar(uint64_t q, uint32_t *out, uint64_t *scanned) {
  uint32_t n = 0;
  for (int t = 0; t < MIH_T; t++) {
    uint32_t blk = mih_blk(q, t), lo = g_mih_off[t][blk], hi = g_mih_off[t][blk + 1];
    *scanned += hi - lo;
    for (uint32_t i = lo; i < hi; i++) {
      uint64_t c = g_mih_code[t][i];
      if (__builtin_popcountll(c ^ q) <= MIH_MAXD && !mih_seen(c, q, t) && n < MIH_MAXOUT)
        out[n++] = g_mih_id[t][i];
    }
  }
  return n;
}

/**
 * The same with an AVX2 Hamming scan: XOR four codes with the query,
 * popcount each 64-bit lane (nibble VPSHUFB + VPSADBW), compare with the
 * distance bound (VPCMPGTQ), and visit only the lanes that pass.
 * @param q        query
 * @param out      receives matching ids (at most MIH_MAXOUT)
 * @param scanned  incremented by the number of candidates examined
 * @returns matches written to out.
 * @remarks The heads of the four posting lists are prefetched before the
 *          first scan, so their four misses overlap.
 */
__attribute__((target("avx2,popcnt"))) static uint32_t mih_query_avx2(uint64_t q, uint32_t *out, uint64_t *scanned) {
  uint32_t n = 0;
  const __m256i vq = _mm256_set1_epi64x((long long)q), lim = _mm256_set1_epi64x(MIH_MAXD + 1);
  for (int t = 0; t < MIH_T; t++) {
    uint32_t lo = g_mih_off[t][mih_blk(q, t)];
    __builtin_prefetch(&g_mih_code[t][lo]);
    __builtin_prefetch(&g_mih_code[t][lo] + 8);
  }
  for (int t = 0; t < MIH_T; t++) {
    uint32_t blk = mih_blk(q, t), lo = g_mih_off[t][blk], hi = g_mih_off[t][blk + 1], i = lo;
    const uint64_t *codes = g_mih_code[t];
    *scanned += hi - lo;
    for (; i + 4 <= hi; i += 4) {
      __m256i x = _mm256_xor_si256(_mm256_loadu_si256((const __m256i *)(const void *)(codes + i)), vq);
      __m256i cnt = _mm256_sad_epu8(pop_bytes(x), _mm256_setzero_si256());
      uint32_t m = (uint32_t)_mm256_movemask_pd(_mm256_castsi256_pd(_mm256_cmpgt_epi64(lim, cnt)));
      for (; m; m &= m - 1) {
        uint32_t k = i + (uint32_t)__builtin_ctz(m);
        if (!mih_seen(codes[k], q, t) && n < MIH_MAXOUT) out[n++] = g_mih_id[t][k];
      }
    }
    for (; i < hi; i++)
      if (__builtin_popcountll(codes[i] ^ q) <= MIH_MAXD && !mih_seen(codes[i], q, t) && n < MIH_MAXOUT)
        out[n++] = g_mih_id[t][i];
  }
  return n;
}

/* ========================================================================= */
/* Geo: radius search with Morton codes                                       */
/* ========================================================================= */

#define LAT0 29.5
#define LAT1 33.5
#define LON0 34.0
#define LON1 36.0
#define M_PER_DEG 111320.0
static double g_qlon_span;       /* longitude degrees quantized to 2^32: square cells in meters */
static uint64_t *g_geo_code;     /* sorted Morton codes */
static double *g_geo_lat, *g_geo_lon;   /* coordinates in code order */
static uint32_t *g_geo_id;              /* ids in code order */
static double *g_pt_lat, *g_pt_lon;     /* coordinates in id order (the brute-force reference) */

/**
 * Quantizes a longitude to 32 bits over g_qlon_span degrees from LON0.
 * @param lon  degrees
 * @returns the quantum, clamped to [0, 2^32 - 1].
 * @remarks g_qlon_span = latitude span / cos(mid latitude), so one quantum
 *          is the same distance in both axes and Morton cells are squares.
 */
static inline uint32_t qlon(double lon) {
  double x = (lon - LON0) / g_qlon_span * 4294967295.0;
  return x <= 0 ? 0 : x >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)x;
}

/**
 * Quantizes a latitude to 32 bits over [LAT0, LAT1].
 * @param lat  degrees
 * @returns the quantum, clamped to [0, 2^32 - 1].
 */
static inline uint32_t qlat(double lat) {
  double y = (lat - LAT0) / (LAT1 - LAT0) * 4294967295.0;
  return y <= 0 ? 0 : y >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)y;
}

/**
 * Morton (Z-order) code of a point with BMI2: PDEP deposits the x bits into
 * the even positions and the y bits into the odd ones, one instruction each.
 * @param x  quantized longitude
 * @param y  quantized latitude
 * @returns the 64-bit interleaved code.
 * @remarks PDEP is 3 cycles on Intel since Haswell but microcoded (slow)
 *          on AMD before Zen 3: the shift-and-mask version is the fallback.
 */
__attribute__((target("bmi2"))) static inline uint64_t morton_pdep(uint32_t x, uint32_t y) {
  return _pdep_u64(x, 0x5555555555555555ull) | _pdep_u64(y, 0xAAAAAAAAAAAAAAAAull);
}

/**
 * Spreads 32 bits to the even bit positions of a 64-bit word ("magic bits").
 * @param v  bits
 * @returns v's bit i at position 2i.
 */
static inline uint64_t spread32(uint32_t v) {
  uint64_t x = v;
  x = (x | (x << 16)) & 0x0000FFFF0000FFFFull;
  x = (x | (x << 8)) & 0x00FF00FF00FF00FFull;
  x = (x | (x << 4)) & 0x0F0F0F0F0F0F0F0Full;
  x = (x | (x << 2)) & 0x3333333333333333ull;
  x = (x | (x << 1)) & 0x5555555555555555ull;
  return x;
}

/**
 * Morton code without BMI2.
 * @param x  quantized longitude
 * @param y  quantized latitude
 * @returns the same code as morton_pdep.
 */
static inline uint64_t morton_magic(uint32_t x, uint32_t y) { return spread32(x) | (spread32(y) << 1); }

/**
 * Morton codes of n points with PDEP, summed (a timing loop).
 * @param n  points
 * @returns the sum of the codes.
 */
__attribute__((target("bmi2"), noinline)) static uint64_t morton_bench_pdep(uint32_t n) {
  uint64_t s = 0;
  for (uint32_t i = 0; i < n; i++) s += morton_pdep(i, ~i);
  return s;
}

/**
 * Morton codes of n points with shifts and masks, summed (a timing loop).
 * @param n  points
 * @returns the sum of the codes.
 */
__attribute__((noinline)) static uint64_t morton_bench_magic(uint32_t n) {
  uint64_t s = 0;
  for (uint32_t i = 0; i < n; i++) s += morton_magic(i, ~i);
  return s;
}

/**
 * Equirectangular distance test, the same formula for the index and the
 * brute-force reference (so both agree to the bit).
 * @param lat      the point's latitude
 * @param lon      the point's longitude
 * @param lat0     the center's latitude
 * @param lon0     the center's longitude
 * @param coslat0  cos(lat0)
 * @param r        radius in meters
 * @returns 1 when the point is within r meters of the center.
 */
static inline int geo_within(double lat, double lon, double lat0, double lon0, double coslat0, double r) {
  double dx = (lon - lon0) * coslat0 * M_PER_DEG, dy = (lat - lat0) * M_PER_DEG;
  return dx * dx + dy * dy <= r * r;
}

/**
 * Four branchless lower bounds over the sorted code array in lockstep,
 * each step prefetching both possible next midpoints of every search.
 * @param n    array size (>= 1)
 * @param t    four targets
 * @param pos  receives their ranks (first index with code >= target)
 * @remarks Four searches give the core four independent miss chains; the
 *          prefetches start the next level's miss before the compare that
 *          picks it resolves.
 */
static void lb4(uint32_t n, const uint64_t t[4], uint32_t pos[4]) {
  const uint64_t *base[4] = {g_geo_code, g_geo_code, g_geo_code, g_geo_code};
  uint32_t len = n;
  while (len > 1) {
    uint32_t half = len / 2;
    len -= half;
    if (len > 1)
      for (int q = 0; q < 4; q++) {
        __builtin_prefetch(&base[q][len / 2 - 1]);
        __builtin_prefetch(&base[q][half + len / 2 - 1]);
      }
    for (int q = 0; q < 4; q++) base[q] += base[q][half - 1] < t[q] ? half : 0;
  }
  for (int q = 0; q < 4; q++) pos[q] = (uint32_t)(base[q] - g_geo_code) + (base[q][0] < t[q]);
}

enum { GEO_PFX_BITS = 20 };
static uint32_t *g_geo_pfx;      /* (1 << GEO_PFX_BITS) + 1 entries: first index of each code prefix */

/**
 * Builds the prefix table over the sorted codes: g_geo_pfx[p] = the first
 * index whose top GEO_PFX_BITS bits are >= p (a radix table, the simplest
 * learned index: it maps a key straight to a small slice of the array).
 * @param n  codes
 * @remarks 4 MiB for 2^20 prefixes, 0.4 B per point at 10M; one counting
 *          pass and one prefix sum.
 */
static void geo_build_pfx(uint32_t n) {
  size_t np = (size_t)1 << GEO_PFX_BITS;
  g_geo_pfx = big_alloc((np + 1) * 4);
  for (uint32_t i = 0; i < n; i++) g_geo_pfx[(g_geo_code[i] >> (64 - GEO_PFX_BITS)) + 1]++;
  for (size_t p = 0; p < np; p++) g_geo_pfx[p + 1] += g_geo_pfx[p];
}

/**
 * Four lower bounds through the prefix table: the table gives each target
 * its slice (codes sharing its top bits, ~23 at 10M points), a branchless
 * search finishes inside the slice. Stages run across all four targets so
 * their misses overlap.
 * @param t    four targets
 * @param pos  receives their ranks (first index with code >= target)
 * @remarks Two dependent misses per seek (table entry, slice) instead of
 *          a binary search's ~10 deep levels. Every code in a slice shares
 *          the target's prefix, so a target above the whole slice ranks at
 *          the slice's end, which is the next prefix's first code: exact.
 */
static void lb4_pfx(const uint64_t t[4], uint32_t pos[4]) {
  uint32_t p[4], lo[4], len[4];
  for (int q = 0; q < 4; q++) {
    p[q] = (uint32_t)(t[q] >> (64 - GEO_PFX_BITS));
    __builtin_prefetch(&g_geo_pfx[p[q]]);
  }
  for (int q = 0; q < 4; q++) {
    lo[q] = g_geo_pfx[p[q]];
    len[q] = g_geo_pfx[p[q] + 1] - lo[q];
    __builtin_prefetch(&g_geo_code[lo[q]]);
    __builtin_prefetch(&g_geo_code[lo[q] + len[q] / 2]);
  }
  for (int q = 0; q < 4; q++) {
    if (!len[q]) {
      pos[q] = lo[q];
      continue;
    }
    const uint64_t *base = g_geo_code + lo[q];
    uint32_t L = len[q];
    while (L > 1) {
      uint32_t half = L / 2;
      base += base[half - 1] < t[q] ? half : 0;
      L -= half;
    }
    pos[q] = (uint32_t)(base - g_geo_code) + (base[0] < t[q]);
  }
}

/**
 * Points within r meters. The query's bounding box, quantized, fits in at
 * most 2 x 2 Morton cells of side 2^k; each cell is one contiguous code
 * range (four interleaved seeks), scanned with a PEXT box test and the
 * exact distance test.
 * @param n        points
 * @param lat0     center latitude
 * @param lon0     center longitude
 * @param r        radius in meters
 * @param out      receives ids (at most max)
 * @param max      capacity of out
 * @param scanned  incremented by the number of codes examined
 * @param pfx      1 = seek through the prefix table, 0 = four interleaved
 *                 binary searches over the whole array
 * @returns matches written to out; exits if the radius needs cells wider
 *          than this probe's scheme.
 * @remarks The box is widened by one quantum each side, so rounding in the
 *          quantization can never exclude a point the distance test keeps.
 */
__attribute__((target("bmi2"))) static uint32_t geo_query(uint32_t n, double lat0, double lon0, double r,
                                                           uint32_t *out, uint32_t max, uint64_t *scanned, int pfx) {
  double coslat0 = cos(lat0 * M_PI / 180.0), dlat = r / M_PER_DEG, dlon = r / (M_PER_DEG * coslat0);
  uint32_t x0 = qlon(lon0 - dlon), x1 = qlon(lon0 + dlon), y0 = qlat(lat0 - dlat), y1 = qlat(lat0 + dlat);
  x0 -= x0 > 0;
  y0 -= y0 > 0;
  x1 += x1 < UINT32_MAX;
  y1 += y1 < UINT32_MAX;
  uint32_t span = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 1;
  uint32_t k = span <= 1 ? 0 : 32 - (uint32_t)__builtin_clz(span - 1);
  if (k >= 31) die("geo radius too large for this probe's cell scheme");
  uint32_t cx[2] = {x0 >> k, x1 >> k}, cy[2] = {y0 >> k, y1 >> k};
  uint64_t lo[4], hi[4];
  int nc = 0;
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < 2; j++) {
      if ((i && cx[1] == cx[0]) || (j && cy[1] == cy[0])) {
        lo[nc] = hi[nc] = 0;   /* a duplicate cell: an empty range keeps the lockstep at four */
      } else {
        lo[nc] = morton_pdep(cx[i] << k, cy[j] << k);
        hi[nc] = lo[nc] + (1ull << (2 * k));
      }
      nc++;
    }
  uint32_t pos[4], m = 0;
  if (pfx) lb4_pfx(lo, pos);
  else lb4(n, lo, pos);
  for (int c = 0; c < 4; c++) {
    for (uint32_t i = pos[c]; i < n && g_geo_code[i] < hi[c]; i++) {
      uint64_t code = g_geo_code[i];
      uint32_t x = (uint32_t)_pext_u64(code, 0x5555555555555555ull);
      uint32_t y = (uint32_t)_pext_u64(code, 0xAAAAAAAAAAAAAAAAull);
      (*scanned)++;
      if (x >= x0 && x <= x1 && y >= y0 && y <= y1 &&
          geo_within(g_geo_lat[i], g_geo_lon[i], lat0, lon0, coslat0, r) && m < max)
        out[m++] = g_geo_id[i];
    }
  }
  return m;
}

/* ========================================================================= */
/* Driver                                                                     */
/* ========================================================================= */

/**
 * Builds every structure, verifies each against its reference, and prints
 * every measurement against the main store's exact-get bars.
 * @param argc  argument count
 * @param argv  [--keys N] [--cpu C] [--reps R] [--threads "1,2,4,7"]
 * @returns 0; exits 1 on a failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  int cpu = 4, reps = 5, nthr = 0, tcounts[MAX_THREADS], vnni = 0, waitpkg = 0;
  const char *tl = "1,2,4,7";
  g_n = 10000000;
  for (int i = 1; i < argc; i++) {
    if (i + 1 < argc && !strcmp(argv[i], "--keys")) g_n = (uint32_t)strtoul(argv[++i], NULL, 10);
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
  if (g_n < 1000000 || g_n % FIELDS || g_n > 12000000 || reps < 1 || reps > MAX_REPS || nthr < 1) {
    fprintf(stderr, "usage: services_probe [--keys N (multiple of 4, 1000000..12000000)] [--cpu C]\n"
                    "                      [--reps 1..%d] [--threads \"a,b,...\" (each 1..%d)]\n",
            MAX_REPS, MAX_THREADS);
    return 2;
  }
  if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("popcnt") || !__builtin_cpu_supports("bmi2") ||
      !__builtin_cpu_supports("sse4.2") || !__builtin_cpu_supports("fma"))
    die("this probe needs AVX2, FMA, POPCNT, BMI2 and SSE4.2");
  cpu_extras(&vnni, &waitpkg);
  memcpy(&g_k0, kSeed, 8);
  memcpy(&g_k1, kSeed + 8, 8);
  g_qlon_span = (LAT1 - LAT0) / cos((LAT0 + LAT1) / 2 * M_PI / 180.0);
  pin(cpu);

  /* ---- the main store: n keys, the server's defaults ---- */
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
  printf("main store: %u keys of 20-21 bytes, 100-byte values; fill %.1f s; cpu %d, reps %d; avx-vnni %s, "
         "waitpkg %s\n",
         g_n, (double)(now_ns() - t0) / 1e9, cpu, reps, vnni ? "yes" : "no", waitpkg ? "yes" : "no");
  kv_qsbr_online(g_store, 0);
  g_q = make_stream(0, g_n, 0x243F6A8885A308D3ull, NULL);
  uint64_t t[MAX_REPS], ta[MAX_REPS], tb[MAX_REPS];
  char note[192];

  /* ================= the bar ================= */
  printf("\nTHE BAR: exact gets on the main store (x = ratio to the bar of the same regime; GOLD = within 10x)\n");
  for (int r = 0; r < reps; r++) t[r] = bar_dependent(g_store, &g_q, VAL_LEN);
  g_bar[BAR_DEP] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = bar_independent(g_store, &g_q, VAL_LEN);
  g_bar[BAR_IND] = med(t, reps, NQ);
  for (int r = 0; r < reps; r++) t[r] = bar_batched(g_store, &g_q, VAL_LEN, HASH_ASM);
  g_bar[BAR_BAT] = med(t, reps, NQ);
  row("get (100-byte value)", "dependent chain", g_bar[BAR_DEP], BAR_DEP, "bar D");
  row("", "independent loop", g_bar[BAR_IND], BAR_IND, "bar I");
  row("", "batched pipeline (the server's GET path)", g_bar[BAR_BAT], BAR_BAT, "bar B");
  printf("BARS dep=%.1f ind=%.1f bat=%.1f\n", g_bar[BAR_DEP], g_bar[BAR_IND], g_bar[BAR_BAT]);

  /* ================= SipHash in four AVX2 lanes ================= */
  printf("\nSIPHASH x4: SipHash-2-4-128 of four keys per AVX2 instruction stream\n");
  {
    for (uint32_t i = 0; i < NQ; i++) {   /* the C reference == the store's assembly, for every query key */
      uint64_t a[2], b[2];
      sip128_ref(g_q.key[i], g_q.len[i], a);
      kv_store_hash(g_store, g_q.key[i], g_q.len[i], b);
      if (a[0] != b[0] || a[1] != b[1]) die("reference SipHash differs from the store's");
    }
    for (uint32_t i = 0; i < NQ; i += 8) {   /* both AVX2 widths == the store's assembly, for every query key */
      uint64_t h4[8][2], h8[8][2], hs[8][2];
      hash8(g_store, &g_q, i, h4, HASH_X4);
      hash8(g_store, &g_q, i, h8, HASH_X8);
      hash8(g_store, &g_q, i, hs, HASH_ASM);
      if (memcmp(h4, hs, sizeof hs) || memcmp(h8, hs, sizeof hs)) die("AVX2 SipHash lanes differ from the store's");
    }
    uint64_t acc = 0;
    static const char *const hnames[3] = {"the store's assembly (SipHash-2-4-128)", "four AVX2 lanes per call",
                                          "eight AVX2 lanes, two groups interleaved"};
    double hns[3];
    for (int m = 0; m < 3; m++) {
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns(), h[8][2];
        for (uint32_t i = 0; i < NQ; i += 8) {
          hash8(g_store, &g_q, i & 1023, h, m);
          acc += h[7][0];
        }
        t[r] = now_ns() - s;
      }
      hns[m] = med(t, reps, NQ);
      if (m) snprintf(note, sizeof note, "%.2fx the assembly; bit-exact (every query key)", hns[m] / hns[0]);
      row(m ? "" : "hash one 20-21 byte key", hnames[m], hns[m], BAR_BAT, m ? note : "keys in L1, throughput");
    }
    for (int r = 0; r < reps; r++) {   /* the batched GET with each hash method, rotating: paired */
      t[r] = bar_batched(g_store, &g_q, VAL_LEN, HASH_ASM);
      ta[r] = bar_batched(g_store, &g_q, VAL_LEN, HASH_X4);
      tb[r] = bar_batched(g_store, &g_q, VAL_LEN, HASH_X8);
    }
    double a = med(t, reps, NQ), b4 = med(ta, reps, NQ), b8 = med(tb, reps, NQ);
    row("batched get (the bar B)", "hash: the store's assembly", a, BAR_BAT, "paired with the next rows");
    snprintf(note, sizeof note, "%.3fx the assembly", b4 / a);
    row("", "hash: four AVX2 lanes", b4, BAR_BAT, note);
    snprintf(note, sizeof note, "%.3fx the assembly", b8 / a);
    row("", "hash: eight AVX2 lanes", b8, BAR_BAT, note);
    g_vsink += acc;
  }

  /* ================= the inline small-value table ================= */
  printf("\nINLINE TABLE: keys <= 16 bytes and 8-byte values inside the 64-byte bucket (one miss per lookup)\n");
  {
    g_sstore = kv_store_create(g_n, MAX_THREADS + 1, kSeed, 0);
    if (!g_sstore) die("kv_store_create (small)");
    g_ib_buckets = 1;
    while (g_ib_buckets * 2 * 6 / 10 < g_n) g_ib_buckets <<= 1;   /* entry load <= 60% */
    g_ib_mask = g_ib_buckets - 1;
    g_ib = big_alloc(g_ib_buckets * sizeof(IBucket));
    uint64_t s0 = now_ns();
    for (uint32_t i = 0; i < g_n; i++) {
      uint8_t k[16];
      make_small_key(i, k);
      uint64_t v = (uint64_t)i * 2654435761u, h[2];
      kv_store_hash(g_sstore, k, 16, h);
      if (kv_store_put(g_sstore, k, 16, &v, 8, h[0], h[1]) != 1) die("small store fill");
    }
    double fill_store = (double)(now_ns() - s0) / 1e9;
    s0 = now_ns();
    for (uint32_t i = 0; i < g_n; i++) {
      uint8_t k[16];
      make_small_key(i, k);
      uint64_t h[2];
      sip128_ref(k, 16, h);
      ib_insert(k, 16, (uint64_t)i * 2654435761u, h);
    }
    double fill_ib = (double)(now_ns() - s0) / 1e9;
    KvStats st;
    kv_store_stats(g_sstore, &st);
    g_sqid = xcalloc((size_t)NQ * sizeof *g_sqid);
    g_sq = make_stream(1, g_n, 0x13198A2E03707344ull, g_sqid);
    for (uint32_t i = 0; i < NQ; i++) {   /* every answer exact */
      uint64_t h[2];
      sip128_ref(g_sq.key[i], 16, h);
      _Atomic uint64_t *c = ib_get(g_sq.key[i], 16, h);
      if (!c || atomic_load(c) != (uint64_t)g_sqid[i] * 2654435761u) die("inline table answer wrong");
    }
    for (uint32_t i = 0; i < 100000; i++) {   /* absent keys stay absent */
      uint8_t k[16];
      make_small_key(g_n + i, k);
      uint64_t h[2];
      sip128_ref(k, 16, h);
      if (ib_get(k, 16, h)) die("inline table found an absent key");
    }
    double mem_store = (double)st.mem_used + (double)st.nbuckets * 64, mem_ib = (double)g_ib_buckets * 64;
    printf("  fill: store %.1f s, inline %.1f s; memory: store %.0f B/key (records + buckets), inline %.0f B/key "
           "(%.0f%% entry load)\n",
           fill_store, fill_ib, mem_store / g_n, mem_ib / g_n, 100.0 * g_n / (double)(g_ib_buckets * 2));
    const char *names[3] = {"dependent chain", "independent loop", "batched"};
    for (int mode = 0; mode < 3; mode++) {
      for (int r = 0; r < reps; r++) {   /* store and inline table alternate: paired */
        ta[r] = mode == 0 ? bar_dependent(g_sstore, &g_sq, 8)
                : mode == 1 ? bar_independent(g_sstore, &g_sq, 8)
                            : bar_batched(g_sstore, &g_sq, 8, HASH_ASM);
        tb[r] = ib_pass(mode, HASH_ASM);
      }
      double a = med(ta, reps, NQ), b = med(tb, reps, NQ);
      char how[64];
      snprintf(how, sizeof how, "store, same keys: %s", names[mode]);
      row(mode == 0 ? "get (16-byte key, 8-byte value)" : "", how, a, mode, "");
      snprintf(how, sizeof how, "inline table: %s", names[mode]);
      snprintf(note, sizeof note, "%.2fx the store", b / a);
      row("", how, b, mode, note);
    }
    for (int r = 0; r < reps; r++) t[r] = ib_pass(2, HASH_X8);
    row("", "inline table: batched + 8-lane AVX2 hashing", med(t, reps, NQ), BAR_BAT, "");
    for (int r = 0; r < reps; r++) {   /* increments, paired */
      ta[r] = sstore_incr_pass();
      tb[r] = ib_pass(3, HASH_ASM);
    }
    {   /* exact: each sampled key gained reps x (its occurrences in the stream), in both tables */
      uint64_t *sorted = xcalloc((size_t)NQ * 8);
      for (uint32_t i = 0; i < NQ; i++) sorted[i] = g_sqid[i];
      qsort(sorted, NQ, 8, cmp_u64);
      for (uint32_t i = 0; i < 2000; i++) {
        uint64_t want = (uint64_t)g_sqid[i] * 2654435761u + (uint64_t)reps * occurrences(sorted, NQ, g_sqid[i]);
        uint64_t h[2];
        sip128_ref(g_sq.key[i], 16, h);
        const KvRec *rec = kv_store_get(g_sstore, g_sq.key[i], 16, h[0], h[1]);
        uint64_t sv;
        memcpy(&sv, kv_rec_value(rec), 8);
        if (atomic_load(ib_get(g_sq.key[i], 16, h)) != want || sv != want) die("increments are not exact");
      }
      free(sorted);
    }
    double a = med(ta, reps, NQ), b = med(tb, reps, NQ);
    row("increment (LOCK XADD in place)", "store, same keys: batched", a, BAR_BAT, "exact (checked)");
    snprintf(note, sizeof note, "%.2fx the store", b / a);
    row("", "inline table: batched", b, BAR_BAT, note);
    big_free(g_ib, g_ib_buckets * sizeof(IBucket));
    kv_store_destroy(g_sstore);
    stream_free(&g_sq);
    free(g_sqid);
  }

  /* ================= flat combining ================= */
  printf("\nCOMBINING: concurrent replace-puts (100-byte values), each thread taking the writer mutex vs flat "
         "combining\n");
  {
    uint32_t per = NW / 2;
    int creps = reps < 3 ? reps : 3;
    for (int ti = 0; ti < nthr; ti++) {
      int T = tcounts[ti];
      for (int r = 0; r < creps; r++) {   /* alternating: paired */
        ta[r] = run_writers(T, per, 0);
        tb[r] = run_writers(T, per, 1);
      }
      double a = med(ta, creps, (double)per * T), b = med(tb, creps, (double)per * T);
      snprintf(note, sizeof note, "%d thread%s: %.2f M puts/s", T, T > 1 ? "s" : "", 1e3 / a);
      row(ti == 0 ? "put (replace), all threads" : "", "each thread takes the writer mutex", a, BAR_NONE, note);
      snprintf(note, sizeof note, "%d thread%s: %.2f M puts/s (%.2fx)", T, T > 1 ? "s" : "", 1e3 / b, a / b);
      row("", "flat combining (prefetched batches)", b, BAR_NONE, note);
    }
    kv_store_reclaim(g_store);
  }

  /* ================= WAITPKG ================= */
  printf("\nWAITPKG: one-way wake-up between two P-cores (half a round trip)\n");
  {
    uint32_t rounds = 200000;
    double spin = ping_pong(0, rounds);
    row("wake a waiting thread", "PAUSE spin (burns its core)", spin / 2, BAR_NONE, "");
    if (waitpkg && umwait_works()) {
      double um = ping_pong(1, rounds);
      snprintf(note, sizeof note, "%.2fx the spin; the waiter's core idles in C0.1", um / spin);
      row("", "UMONITOR + UMWAIT", um / 2, BAR_NONE, note);
    } else {
      row("", "UMONITOR + UMWAIT", 0, BAR_NONE, "not available here");
    }
    double fx = ping_pong(2, rounds / 10);
    snprintf(note, sizeof note, "%.1fx the spin", fx / spin);
    row("", "futex wait / wake (system calls)", fx / 2, BAR_NONE, note);
    pin(cpu);
  }

  /* ================= bitmaps ================= */
  printf("\nBITMAPS: 1 MiB bitmaps (8,388,608 bits, e.g. one bit per user id per day)\n");
  {
    uint64_t *A = big_alloc(BM_BYTES), *B = big_alloc(BM_BYTES), *S = big_alloc(BM_BYTES), seed = 77;
    size_t nw = BM_BYTES / 8;
    for (size_t i = 0; i < nw; i++) {
      A[i] = rnd(&seed) & rnd(&seed);              /* ~25% density */
      B[i] = rnd(&seed) | rnd(&seed);              /* ~75% density */
      S[i] = (rnd(&seed) % 1000) == 0 ? (1ull << (rnd(&seed) & 63)) : 0;   /* sparse: one word in 1,000 */
    }
    uint64_t ref = pop_scalar(A, nw);
    if (pop_avx2_lookup((const uint8_t *)A, BM_BYTES) != ref ||
        pop_avx2_hs((const __m256i *)(const void *)A, NULL, BM_BYTES / 32) != ref)
      die("AVX2 popcount disagrees with POPCNT");
    if (pop_avx2_hs((const __m256i *)(const void *)A, (const __m256i *)(const void *)B, BM_BYTES / 32) !=
        and_pop_scalar(A, B, nw))
      die("AVX2 AND-popcount disagrees");
    uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++, clobber()) sink += pop_scalar(A, nw);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%.1f GB/s", BM_BYTES / med(t, reps, 100));
    row("BITCOUNT, 1 MiB", "POPCNT, 4 accumulators", med(t, reps, 100), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++, clobber()) sink += pop_avx2_lookup((const uint8_t *)A, BM_BYTES);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%.1f GB/s", BM_BYTES / med(t, reps, 100));
    row("", "AVX2 nibble lookup (VPSHUFB, VPSADBW)", med(t, reps, 100), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++, clobber()) sink += pop_avx2_hs((const __m256i *)(const void *)A, NULL, BM_BYTES / 32);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%.1f GB/s", BM_BYTES / med(t, reps, 100));
    row("", "AVX2 Harley-Seal (carry-save adders)", med(t, reps, 100), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++, clobber()) sink += and_pop_scalar(A, B, nw);
      t[r] = now_ns() - s;
    }
    row("AND-count of two 1 MiB bitmaps", "POPCNT (retention: active both days)", med(t, reps, 100), BAR_DEP, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++, clobber())
        sink += pop_avx2_hs((const __m256i *)(const void *)A, (const __m256i *)(const void *)B, BM_BYTES / 32);
      t[r] = now_ns() - s;
    }
    row("", "AVX2 Harley-Seal, AND fused", med(t, reps, 100), BAR_DEP, "");
    uint32_t *bits = xcalloc(1000000 * sizeof *bits);
    for (int i = 0; i < 1000000; i++) bits[i] = (uint32_t)(rnd(&seed) % ((uint64_t)BM_BYTES * 8));
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 1000000; i++)
        __atomic_fetch_or(&A[bits[i] >> 6], 1ull << (bits[i] & 63), __ATOMIC_RELAXED);
      t[r] = now_ns() - s;
    }
    row("SETBIT (in place)", "LOCK OR on the word", med(t, reps, 1000000), BAR_BAT, "plus the key's lookup");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 1000000; i++) sink += (A[bits[i] >> 6] >> (bits[i] & 63)) & 1;
      t[r] = now_ns() - s;
    }
    row("GETBIT", "load + shift", med(t, reps, 1000000), BAR_BAT, "plus the key's lookup");
    for (int i = 0; i < 2000; i++)   /* BITPOS: AVX2 == scalar */
      if (bitpos_avx2(S, (uint64_t)BM_BYTES * 8, bits[i]) != bitpos_scalar(S, (uint64_t)BM_BYTES * 8, bits[i]))
        die("AVX2 BITPOS disagrees");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 20000; i++) sink += bitpos_scalar(S, (uint64_t)BM_BYTES * 8, bits[i]);
      t[r] = now_ns() - s;
    }
    row("BITPOS (next set bit, sparse)", "word by word", med(t, reps, 20000), BAR_DEP, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 20000; i++) sink += bitpos_avx2(S, (uint64_t)BM_BYTES * 8, bits[i]);
      t[r] = now_ns() - s;
    }
    row("", "AVX2: VPTEST skips 32-byte zero blocks", med(t, reps, 20000), BAR_DEP, "");
    /* ---- the block-count directory: SETBIT keeps per-4 KiB counts, range BITCOUNT reads them ---- */
    size_t nblk = nw / BM_BLOCK_WORDS;
    uint32_t *cnt = xcalloc(nblk * sizeof *cnt);
    uint64_t total = 0, flips = 0;
    for (size_t b = 0; b < nblk; b++) {
      cnt[b] = (uint32_t)pop_scalar(A + b * BM_BLOCK_WORDS, BM_BLOCK_WORDS);
      total += cnt[b];
    }
    uint32_t *fresh = xcalloc(1000000 * sizeof *fresh);   /* new positions: most bits flip */
    for (int r = 0; r < reps; r++) {
      for (int i = 0; i < 1000000; i++) fresh[i] = (uint32_t)(rnd(&seed) % ((uint64_t)BM_BYTES * 8));
      uint64_t s = now_ns();
      for (int i = 0; i < 1000000; i++) flips += (uint64_t)setbit_counted(A, cnt, &total, fresh[i]);
      t[r] = now_ns() - s;
    }
    {   /* the counts are exact after every SETBIT */
      uint64_t all = 0;
      for (size_t b = 0; b < nblk; b++) {
        uint64_t c = pop_scalar(A + b * BM_BLOCK_WORDS, BM_BLOCK_WORDS);
        if (c != cnt[b]) die("block count drifted");
        all += c;
      }
      if (all != total) die("bitmap total drifted");
    }
    snprintf(note, sizeof note, "%.0f%% of the bits flipped; plus the key's lookup",
             100.0 * (double)flips / (1000000.0 * reps));
    row("SETBIT, counts maintained", "LOCK BTS; on a flip, 2 x LOCK XADD", med(t, reps, 1000000), BAR_BAT, note);
    enum { NRANGE = 4096 };
    size_t *rs = xcalloc(NRANGE * sizeof *rs), *re = xcalloc(NRANGE * sizeof *re);
    double avg = 0;
    for (int i = 0; i < NRANGE; i++) {   /* random word-aligned (32-byte) ranges, empty ones included */
      size_t a = (size_t)(rnd(&seed) % (nw / 4 + 1)) * 4, b = (size_t)(rnd(&seed) % (nw / 4 + 1)) * 4;
      rs[i] = a < b ? a : b;
      re[i] = a < b ? b : a;
      avg += (double)(re[i] - rs[i]) * 8;
      if (bitcount_dir(A, cnt, rs[i], re[i]) != pop_scalar(A + rs[i], re[i] - rs[i]))
        die("directory BITCOUNT disagrees with a scan");
    }
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < NRANGE; i++) sink += pop_words(A, rs[i], re[i]);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "ranges average %.0f KiB", avg / NRANGE / 1024);
    row("BITCOUNT of a byte range", "AVX2 scan of the range", med(t, reps, NRANGE), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < NRANGE; i++) sink += bitcount_dir(A, cnt, rs[i], re[i]);
      t[r] = now_ns() - s;
    }
    row("", "block counts + <= 2 x 2 KiB scanned", med(t, reps, NRANGE), BAR_DEP, "exact (checked)");
    free(fresh);
    free(rs);
    free(re);
    free(cnt);
    g_vsink += sink;
    free(bits);
    big_free(A, BM_BYTES);
    big_free(B, BM_BYTES);
    big_free(S, BM_BYTES);
  }

  /* ================= struct values ================= */
  printf("\nSTRUCT VALUES: 1M keys with 16-field values (%d bytes); fields found by one SSE2 tag compare\n", ST_LEN);
  {
    for (int f = 0; f < ST_FIELDS; f++) {
      g_st_tag[f] = st_tag_of(kStFields[f]);
      g_st_len[f] = (uint8_t)strlen(kStFields[f]);
    }
    uint32_t sn = 1000000;
    KvTable *ts = kv_store_create(sn, MAX_THREADS + 1, kSeed, 0);
    if (!ts) die("kv_store_create (struct)");
    uint8_t v[ST_LEN];
    for (uint32_t i = 0; i < sn; i++) {
      uint64_t h[2];
      st_build(v, i);
      kv_store_hash(ts, akey(i), alen(i), h);
      if (kv_store_put(ts, akey(i), alen(i), v, ST_LEN, h[0], h[1]) != 1) die("struct store fill");
    }
    uint32_t *sid = xcalloc((size_t)NQ * sizeof *sid);
    Stream sq = make_stream(0, sn, 31337, sid);
    uint8_t *fld = xcalloc(NQ);
    uint64_t seed = 4711, sink = 0;
    for (uint32_t i = 0; i < NQ; i++) fld[i] = (uint8_t)(rnd(&seed) % ST_FIELDS);
    for (uint32_t i = 0; i < 100000; i++) {   /* every field read back exactly */
      uint64_t h[2], x;
      kv_store_hash(ts, sq.key[i], sq.len[i], h);
      const KvRec *r = kv_store_get(ts, sq.key[i], sq.len[i], h[0], h[1]);
      const uint8_t *f = st_field(kv_rec_value(r), kStFields[fld[i]], g_st_len[fld[i]], g_st_tag[fld[i]]);
      if (!f) die("struct field not found");
      memcpy(&x, f, 8);
      if (x != (uint64_t)sid[i] * 1000003u + fld[i]) die("struct field value wrong");
    }
    for (int r = 0; r < reps; r++) {   /* the three variants alternate: paired */
      ta[r] = st_pass(ts, &sq, fld, 0, &sink);
      tb[r] = st_pass(ts, &sq, fld, 1, &sink);
      t[r] = st_pass(ts, &sq, fld, 2, &sink);
    }
    {   /* HINCRBY exact: each sampled (key, field) gained reps x its occurrences */
      uint64_t *pairs = xcalloc((size_t)NQ * 8);
      for (uint32_t i = 0; i < NQ; i++) pairs[i] = (uint64_t)sid[i] << 8 | fld[i];
      qsort(pairs, NQ, 8, cmp_u64);
      for (uint32_t i = 0; i < 2000; i++) {
        uint64_t h[2], x, want = (uint64_t)sid[i] * 1000003u + fld[i] +
                                 (uint64_t)reps * occurrences(pairs, NQ, (uint64_t)sid[i] << 8 | fld[i]);
        kv_store_hash(ts, sq.key[i], sq.len[i], h);
        const KvRec *r = kv_store_get(ts, sq.key[i], sq.len[i], h[0], h[1]);
        memcpy(&x, st_field(kv_rec_value(r), kStFields[fld[i]], g_st_len[fld[i]], g_st_tag[fld[i]]), 8);
        if (x != want) die("HINCRBY is not exact");
      }
      free(pairs);
    }
    double whole = med(ta, reps, NQ), one = med(tb, reps, NQ), inc = med(t, reps, NQ);
    snprintf(note, sizeof note, "%d bytes copied", ST_LEN);
    row("get the whole value", "batched pipeline", whole, BAR_BAT, note);
    snprintf(note, sizeof note, "%.2fx the whole get; 8 bytes on the wire", one / whole);
    row("HGET one field", "batched + directory + field stages", one, BAR_BAT, note);
    snprintf(note, sizeof note, "%.2fx the whole get; exact (checked)", inc / whole);
    row("HINCRBY one field", "batched + LOCK XADD on the field", inc, BAR_BAT, note);
    st_build(v, 7);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 1000000; i++) {
        int f = i & 15;
        sink += (uintptr_t)st_field(v, kStFields[f], g_st_len[f], g_st_tag[f]);
      }
      t[r] = now_ns() - s;
    }
    row("", "the field lookup alone (value in L1)", med(t, reps, 1000000), BAR_BAT, "PCMPEQB over 16 tags");
    g_vsink += sink;
    free(sid);
    free(fld);
    stream_free(&sq);
    kv_store_destroy(ts);
  }

  /* ================= sets ================= */
  printf("\nSETS: sorted u32 arrays in a value (tags, small member lists)\n");
  {
    enum { SN = 1024, SMALL = 64 };
    uint32_t *A = xalign32(SN * 4), *B = xalign32(SN * 4), *Sm = xalign32(SMALL * 4);
    uint32_t *probe = xcalloc(1000000 * 4);
    uint64_t seed = 4242;
    for (int pass = 0; pass < 3; pass++) {   /* distinct sorted elements from [0, 20480) */
      uint32_t *X = pass == 0 ? A : pass == 1 ? B : Sm, n = pass == 2 ? SMALL : SN;
      uint8_t *used = xcalloc(20480);
      for (uint32_t i = 0; i < n;) {
        uint32_t x = (uint32_t)(rnd(&seed) % 20480);
        if (!used[x]) {
          used[x] = 1;
          X[i++] = x;
        }
      }
      qsort(X, n, 4, cmp_u32);
      free(used);
    }
    for (int i = 0; i < 1000000; i++) probe[i] = (uint32_t)(rnd(&seed) % 20480);
    for (int i = 0; i < 100000; i++)
      if (set_has_bl(A, SN, probe[i]) != set_has_avx2(A, SN, probe[i]) ||
          set_has_bl(Sm, SMALL, probe[i]) != set_has_avx2(Sm, SMALL, probe[i]))
        die("set membership disagrees");
    uint32_t ic = inter_scalar(A, SN, B, SN);
    if (inter_branchless(A, SN, B, SN) != ic || inter_avx2(A, SN, B, SN) != ic) die("set intersection disagrees");
    uint64_t sink = 0;
    for (int sz = 0; sz < 2; sz++) {
      const uint32_t *X = sz ? A : Sm;
      uint32_t n = sz ? SN : SMALL;
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns();
        for (int i = 0; i < 1000000; i++) sink += (uint64_t)set_has_bl(X, n, probe[i]);
        t[r] = now_ns() - s;
      }
      snprintf(note, sizeof note, "%u elements", n);
      row(sz ? "" : "SISMEMBER (value in cache)", "branchless binary search", med(t, reps, 1000000), BAR_BAT, note);
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns();
        for (int i = 0; i < 1000000; i++) sink += (uint64_t)set_has_avx2(X, n, probe[i]);
        t[r] = now_ns() - s;
      }
      row("", "AVX2 scan (VPCMPEQD, 8 per step)", med(t, reps, 1000000), BAR_BAT, note);
    }
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 10000; i++, clobber()) sink += inter_scalar(A, SN, B, SN);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "1,024 x 1,024 elements, %u in common", ic);
    row("SINTERCARD", "branchy merge", med(t, reps, 10000), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 10000; i++, clobber()) sink += inter_branchless(A, SN, B, SN);
      t[r] = now_ns() - s;
    }
    row("", "branchless merge", med(t, reps, 10000), BAR_DEP, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 10000; i++, clobber()) sink += inter_avx2(A, SN, B, SN);
      t[r] = now_ns() - s;
    }
    row("", "AVX2 all-pairs blocks (8 x 8, VPERMD)", med(t, reps, 10000), BAR_DEP, "");
    g_vsink += sink;
    free(A);
    free(B);
    free(Sm);
    free(probe);
  }

  /* ================= value kernels ================= */
  printf("\nKERNELS: per-value computations once the record is in cache\n");
  {
    uint32_t *w = xalign32(64 * 4), *ma = xalign32(128 * 4), *mb = xalign32(128 * 4);
    uint8_t *u = xalign32(DIM);
    int8_t *sv = xalign32(DIM);
    float *fa = xalign32(DIM * 4), *fb = xalign32(DIM * 4);
    uint64_t seed = 99, sink = 0;
    for (int i = 0; i < 64; i++) w[i] = (uint32_t)(rnd(&seed) % 100000);
    for (int i = 0; i < 128; i++) {
      ma[i] = (uint32_t)rnd(&seed);
      mb[i] = (i % 3) ? ma[i] : (uint32_t)rnd(&seed);
    }
    for (int i = 0; i < DIM; i++) {
      u[i] = (uint8_t)(rnd(&seed) % 128);
      sv[i] = (int8_t)((int)(rnd(&seed) % 255) - 127);
      fa[i] = (float)((int)(rnd(&seed) % 2001) - 1000) / 1000.0f;
      fb[i] = (float)((int)(rnd(&seed) % 2001) - 1000) / 1000.0f;
    }
    if (win_sum_scalar(w) != win_sum_avx2(w)) die("window sum disagrees");
    if (minhash_scalar(ma, mb) != minhash_avx2(ma, mb)) die("MinHash agreement disagrees");
    if (dot_i8_scalar(u, sv) != dot_i8_avx2(u, sv)) die("AVX2 int8 dot disagrees");
    if (vnni && dot_i8_scalar(u, sv) != dot_i8_vnni(u, sv)) die("AVX-VNNI int8 dot disagrees");
    if (fabsf(dot_f32_scalar(fa, fb) - dot_f32_fma(fa, fb)) > 1e-3f) die("FMA dot disagrees");
    static const struct {
      const char *what, *how;
      int kind;
    } ks[] = {
        {"sliding-window sum (64 buckets)", "scalar", 0},
        {"", "AVX2 (VPMOVZXDQ + VPADDQ)", 1},
        {"MinHash agreement (128 x u32)", "scalar", 2},
        {"", "AVX2 (VPCMPEQD + VMOVMSKPS + POPCNT)", 3},
        {"int8 dot product (256 dims)", "scalar", 4},
        {"", "AVX2 (VPMADDUBSW + VPMADDWD)", 5},
        {"", "AVX-VNNI (VPDPBUSD)", 6},
        {"fp32 dot product (256 dims)", "scalar", 7},
        {"", "AVX2 FMA, 4 accumulators", 8},
    };
    for (size_t c = 0; c < sizeof ks / sizeof ks[0]; c++) {
      if (ks[c].kind == 6 && !vnni) {
        row(ks[c].what, ks[c].how, 0, BAR_NONE, "AVX-VNNI not available");
        continue;
      }
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns();
        for (int i = 0; i < 1000000; i++) {
          /* The barrier makes every call re-read its operands, so none can be hoisted or folded
             across iterations. (Changing an operand byte instead made the next 256-bit load miss
             store-to-load forwarding, a stall that hit only the SIMD rows.) */
          clobber();
          switch (ks[c].kind) {
            case 0: sink += win_sum_scalar(w); break;
            case 1: sink += win_sum_avx2(w); break;
            case 2: sink += minhash_scalar(ma, mb); break;
            case 3: sink += minhash_avx2(ma, mb); break;
            case 4: sink += (uint64_t)(int64_t)dot_i8_scalar(u, sv); break;
            case 5: sink += (uint64_t)(int64_t)dot_i8_avx2(u, sv); break;
            case 6: sink += (uint64_t)(int64_t)dot_i8_vnni(u, sv); break;
            case 7: sink += (uint64_t)(int64_t)dot_f32_scalar(fa, fb); break;
            default: sink += (uint64_t)(int64_t)dot_f32_fma(fa, fb); break;
          }
        }
        t[r] = now_ns() - s;
      }
      row(ks[c].what, ks[c].how, med(t, reps, 1000000), BAR_BAT, "");
    }
    g_vsink += sink;
    free(w);
    free(ma);
    free(mb);
    free(u);
    free(sv);
    free(fa);
    free(fb);
  }

  /* ================= TopK ================= */
  printf("\nTOPK: the 100 heaviest of %u keys from a Zipf(1) stream of %u updates (HeavyKeeper 2 x 4096 = 64 KiB "
         "+ heap)\n",
         g_n, NQ);
  {
    uint32_t *zs = xcalloc((size_t)NQ * 4);
    uint64_t seed = 0x70C4ull;
    double lnN = log((double)g_n);
    for (uint32_t i = 0; i < NQ; i++) {   /* rank = floor(e^U) - 1, U uniform in [0, ln n): P(rank) ~ 1/rank */
      double u = (double)(rnd(&seed) >> 11) / 9007199254740992.0;
      uint64_t rank = (uint64_t)exp(u * lnN) - 1;
      if (rank >= g_n) rank = g_n - 1;
      zs[i] = (uint32_t)((rank * 2654435761ull) % g_n);   /* a bijection: heavy keys are spread over the ids */
    }
    uint64_t *sorted = xcalloc((size_t)NQ * 8), *pairs = xcalloc((size_t)NQ * 8);
    for (uint32_t i = 0; i < NQ; i++) sorted[i] = zs[i];
    qsort(sorted, NQ, 8, cmp_u64);
    uint32_t np = 0;
    for (uint32_t i = 0; i < NQ;) {   /* exact counts: runs of the sorted stream */
      uint32_t j = i;
      while (j < NQ && sorted[j] == sorted[i]) j++;
      pairs[np++] = (uint64_t)(j - i) << 32 | sorted[i];
      i = j;
    }
    qsort(pairs, np, 8, cmp_u64);
    uint32_t c100 = (uint32_t)(pairs[np - TOPK] >> 32);   /* the 100th largest true count */
    hk_init();
    for (uint32_t i = 0; i < NQ; i++) hk_update(zs[i]);
    uint32_t good = 0;
    double err = 0;
    for (int k = 0; k < g_top_n; k++) {   /* a reported key is right when its true count reaches the 100th's */
      uint32_t truec = occurrences(sorted, NQ, g_top_id[k]);
      good += truec >= c100;
      err += fabs((double)g_top_cnt[k] - truec) / truec;
    }
    if (g_top_n != TOPK || good < 90) die("TopK missed too many heavy hitters");
    for (int r = 0; r < reps; r++) {
      hk_init();
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) hk_update(zs[i]);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%u/100 truly top-100 (#100 seen %u times); count error %.1f%%", good, c100,
             100.0 * err / g_top_n);
    row("TOPK update (heavy hitters)", "HeavyKeeper + AVX2 heap membership", med(t, reps, NQ), BAR_BAT, note);
    free(zs);
    free(sorted);
    free(pairs);
  }

  /* ================= quantiles ================= */
  printf("\nQUANTILES: DDSketch (1%% relative accuracy, 2,048 buckets = 16 KiB) over %u lognormal latencies\n", NQ);
  {
    double *xs = xcalloc((size_t)NQ * 8), *ys = xcalloc((size_t)NQ * 8), sum = 0;
    uint64_t seed = 0xDD5Cull;
    for (uint32_t i = 0; i < NQ; i++) {   /* Box-Muller: median 1,000 us, sigma 1 (in log space) */
      double u1 = ((double)(rnd(&seed) >> 11) + 1.0) / 9007199254740992.0;   /* (0, 1]: log(u1) is finite */
      double u2 = (double)(rnd(&seed) >> 11) / 9007199254740992.0;
      double z = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
      xs[i] = exp(log(1000.0) + z);
      if (xs[i] < 1.0) xs[i] = 1.0;
    }
    dd_init();
    for (uint32_t i = 0; i < NQ; i++) dd_add(xs[i]);
    memcpy(ys, xs, (size_t)NQ * 8);
    qsort(ys, NQ, 8, cmp_double);
    static const double qv[4] = {0.5, 0.9, 0.99, 0.999};
    double worst = 0;
    for (int k = 0; k < 4; k++) {   /* the guarantee: within 1% of the value of the same rank */
      double exact = ys[(size_t)(qv[k] * (double)(NQ - 1))], e1 = dd_quantile_walk(qv[k]),
             e2 = dd_quantile_2lvl(qv[k]), rel = fabs(e1 - exact) / exact;
      if (e1 != e2 || rel > DD_ALPHA * (1 + 1e-9)) die("DDSketch quantile outside its guarantee");
      if (rel > worst) worst = rel;
    }
    for (int r = 0; r < reps; r++) {
      dd_init();
      uint64_t s = now_ns();
      for (uint32_t i = 0; i < NQ; i++) dd_add(xs[i]);
      t[r] = now_ns() - s;
    }
    row("record a latency", "DDSketch: log, two increments", med(t, reps, NQ), BAR_BAT, "16 KiB, in cache");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100000; i++, clobber()) sum += dd_quantile_walk(qv[i & 3]);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "p50/p90/p99/p99.9 worst error %.2f%%", 100 * worst);
    row("quantile (p50..p99.9)", "walk the buckets", med(t, reps, 100000), BAR_DEP, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100000; i++, clobber()) sum += dd_quantile_2lvl(qv[i & 3]);
      t[r] = now_ns() - s;
    }
    row("", "64-bucket group counts, then one group", med(t, reps, 100000), BAR_DEP, "same answers (checked)");
    g_vsink += (uint64_t)sum;
    free(xs);
    free(ys);
  }

  /* ================= SimHash near-duplicate search ================= */
  printf("\nSIMHASH: near-duplicates (Hamming distance <= 3) among %u 64-bit fingerprints\n", g_n);
  {
    uint64_t seed = 0x5EED5EED5EEDull, s0 = now_ns();
    g_fp = big_alloc((size_t)g_n * 8);
    for (uint32_t i = 0; i < g_n; i++) g_fp[i] = rnd(&seed);
    for (int tb2 = 0; tb2 < MIH_T; tb2++) {   /* CSR posting lists per 16-bit block value */
      g_mih_off[tb2] = xcalloc(((size_t)MIH_B + 1) * 4);
      g_mih_code[tb2] = big_alloc((size_t)g_n * 8);
      g_mih_id[tb2] = big_alloc((size_t)g_n * 4);
      uint32_t *cnt = xcalloc((size_t)MIH_B * 4);
      for (uint32_t i = 0; i < g_n; i++) cnt[mih_blk(g_fp[i], tb2)]++;
      for (uint32_t b = 0, acc = 0; b < MIH_B; b++) {
        g_mih_off[tb2][b] = acc;
        acc += cnt[b];
        cnt[b] = g_mih_off[tb2][b];
      }
      g_mih_off[tb2][MIH_B] = g_n;
      for (uint32_t i = 0; i < g_n; i++) {
        uint32_t pos = cnt[mih_blk(g_fp[i], tb2)]++;
        g_mih_code[tb2][pos] = g_fp[i];
        g_mih_id[tb2][pos] = i;
      }
      free(cnt);
    }
    double build = (double)(now_ns() - s0);
    /* Queries: even ones are a stored fingerprint with 0-3 bits flipped, odd ones random. */
    enum { NQM = 4096 };
    uint64_t *qs = xcalloc(NQM * 8);
    uint32_t *src = xcalloc(NQM * 4);
    for (int i = 0; i < NQM; i++) {
      if (i & 1) {
        qs[i] = rnd(&seed);
        src[i] = UINT32_MAX;
      } else {
        uint32_t id = (uint32_t)(rnd(&seed) % g_n);
        uint64_t q = g_fp[id];
        int flips = (int)(rnd(&seed) % 4);
        for (int f = 0; f < flips; f++) q ^= 1ull << (rnd(&seed) & 63);
        qs[i] = q;
        src[i] = id;
      }
    }
    uint32_t out[MIH_MAXOUT], out2[MIH_MAXOUT], refids[MIH_MAXOUT];
    for (int i = 0; i < 24; i++) {   /* exact answers against a scan of all n fingerprints */
      uint64_t sc = 0;
      uint32_t m = mih_query_avx2(qs[i], out, &sc), m2 = mih_query_scalar(qs[i], out2, &sc), ref = 0;
      for (uint32_t k = 0; k < g_n; k++)
        if (__builtin_popcountll(g_fp[k] ^ qs[i]) <= MIH_MAXD && ref < MIH_MAXOUT) refids[ref++] = k;
      qsort(out, m, 4, cmp_u32);
      qsort(out2, m2, 4, cmp_u32);
      if (m != ref || m2 != ref || memcmp(out, refids, (size_t)m * 4) || memcmp(out2, refids, (size_t)m * 4))
        die("MIH disagrees with brute force");
    }
    for (int i = 0; i < NQM; i += 2) {   /* every planted near-duplicate is found */
      uint64_t sc = 0;
      uint32_t m = mih_query_avx2(qs[i], out, &sc), found = 0;
      for (uint32_t k = 0; k < m; k++) found |= out[k] == src[i];
      if (!found) die("MIH missed a planted near-duplicate");
    }
    printf("  build %.2f s (%.0f ns per fingerprint); %.0f MB (48 B per fingerprint: 4 tables of code + id)\n",
           build / 1e9, build / g_n, (double)g_n * 48 / 1048576.0);
    uint64_t scanned = 0, matches = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      scanned = matches = 0;
      for (int i = 0; i < NQM; i++) matches += mih_query_scalar(qs[i], out, &scanned);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%.0f candidates per query", (double)scanned / NQM);
    row("near-duplicate search", "MIH, scalar POPCNT scan", med(t, reps, NQM), BAR_IND, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      scanned = matches = 0;
      for (int i = 0; i < NQM; i++) matches += mih_query_avx2(qs[i], out, &scanned);
      t[r] = now_ns() - s;
    }
    snprintf(note, sizeof note, "%.2f matches per query", (double)matches / NQM);
    row("", "MIH, AVX2 Hamming scan", med(t, reps, NQM), BAR_IND, note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns(), sc = 0;
      for (uint32_t k = 0; k < g_n; k++) sc += __builtin_popcountll(g_fp[k] ^ qs[r]) <= MIH_MAXD;
      t[r] = now_ns() - s;
      g_vsink += sc;
    }
    row("", "brute force: XOR + POPCNT over all", med(t, reps, 1), BAR_IND, "the alternative");
    for (int tb2 = 0; tb2 < MIH_T; tb2++) {
      free(g_mih_off[tb2]);
      big_free(g_mih_code[tb2], (size_t)g_n * 8);
      big_free(g_mih_id[tb2], (size_t)g_n * 4);
    }
    big_free(g_fp, (size_t)g_n * 8);
    free(qs);
    free(src);
  }

  /* ================= geo radius search ================= */
  printf("\nGEO: points within a radius, among %u uniform points (a 445 x 190 km region)\n", g_n);
  {
    uint64_t seed = 0x6E0ull, s0;
    g_pt_lat = big_alloc((size_t)g_n * 8);
    g_pt_lon = big_alloc((size_t)g_n * 8);
    for (uint32_t i = 0; i < g_n; i++) {
      g_pt_lat[i] = LAT0 + (LAT1 - LAT0) * ((double)(rnd(&seed) >> 11) / 9007199254740992.0);
      g_pt_lon[i] = LON0 + (LON1 - LON0) * ((double)(rnd(&seed) >> 11) / 9007199254740992.0);
    }
    uint64_t *code = big_alloc((size_t)g_n * 8), *tmpc = big_alloc((size_t)g_n * 8);
    uint32_t *idv = big_alloc((size_t)g_n * 4), *tmpi = big_alloc((size_t)g_n * 4);
    for (uint32_t i = 0; i < 1000000; i++) {   /* PDEP == magic bits */
      uint32_t x = (uint32_t)rnd(&seed), y = (uint32_t)rnd(&seed);
      if (morton_pdep(x, y) != morton_magic(x, y)) die("PDEP Morton code disagrees");
    }
    uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      sink += morton_bench_pdep(1000000);
      t[r] = now_ns() - s;
    }
    row("Morton code of a point", "BMI2 PDEP (2 instructions)", med(t, reps, 1000000), BAR_BAT, "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      sink += morton_bench_magic(1000000);
      t[r] = now_ns() - s;
    }
    row("", "shifts and masks (magic bits)", med(t, reps, 1000000), BAR_BAT, "");
    s0 = now_ns();
    for (uint32_t i = 0; i < g_n; i++) {
      code[i] = morton_pdep(qlon(g_pt_lon[i]), qlat(g_pt_lat[i]));
      idv[i] = i;
    }
    for (int pass = 0; pass < 4; pass++) {   /* LSD radix sort, 16 bits per pass (stable) */
      uint32_t *cnt = xcalloc(65536 * 4);
      int sh = 16 * pass;
      for (uint32_t i = 0; i < g_n; i++) cnt[(code[i] >> sh) & 0xFFFF]++;
      for (uint32_t b = 0, acc = 0; b < 65536; b++) {
        uint32_t c = cnt[b];
        cnt[b] = acc;
        acc += c;
      }
      for (uint32_t i = 0; i < g_n; i++) {
        uint32_t p = cnt[(code[i] >> sh) & 0xFFFF]++;
        tmpc[p] = code[i];
        tmpi[p] = idv[i];
      }
      uint64_t *xc = code;
      code = tmpc;
      tmpc = xc;
      uint32_t *xi = idv;
      idv = tmpi;
      tmpi = xi;
      free(cnt);
    }
    for (uint32_t i = 1; i < g_n; i++)
      if (code[i - 1] > code[i]) die("radix sort failed");
    g_geo_code = code;
    g_geo_id = idv;
    g_geo_lat = big_alloc((size_t)g_n * 8);
    g_geo_lon = big_alloc((size_t)g_n * 8);
    for (uint32_t i = 0; i < g_n; i++) {
      g_geo_lat[i] = g_pt_lat[idv[i]];
      g_geo_lon[i] = g_pt_lon[idv[i]];
    }
    double build_s = (double)(now_ns() - s0) / 1e9;
    s0 = now_ns();
    geo_build_pfx(g_n);
    printf("  index build %.2f s (Morton codes + 4-pass radix sort) + %.3f s (prefix table); %.0f MB (28 B per "
           "point: code, id, lat, lon) + 4 MiB prefix table\n",
           build_s, (double)(now_ns() - s0) / 1e9, (double)g_n * 28 / 1048576.0);
    enum { NG = 4096, GMAX = 8192 };
    double *qla = xcalloc(NG * 8), *qlo = xcalloc(NG * 8);
    uint32_t *res = xcalloc(GMAX * 4), *ref = xcalloc(GMAX * 4);
    for (int i = 0; i < NG; i++) {   /* centers at least 0.1 degree inside the region */
      qla[i] = LAT0 + 0.1 + (LAT1 - LAT0 - 0.2) * ((double)(rnd(&seed) >> 11) / 9007199254740992.0);
      qlo[i] = LON0 + 0.1 + (LON1 - LON0 - 0.2) * ((double)(rnd(&seed) >> 11) / 9007199254740992.0);
    }
    const double radii[2] = {200.0, 1000.0};
    for (int ri = 0; ri < 2; ri++) {
      double rad = radii[ri];
      for (int i = 0; i < 12; i++) {   /* exact answers, both seek methods, against a scan of all points */
        double c0 = cos(qla[i] * M_PI / 180.0);
        uint32_t nref = 0;
        for (uint32_t k = 0; k < g_n; k++)
          if (geo_within(g_pt_lat[k], g_pt_lon[k], qla[i], qlo[i], c0, rad) && nref < GMAX) ref[nref++] = k;
        for (int pfx = 0; pfx < 2; pfx++) {
          uint64_t sc = 0;
          uint32_t m = geo_query(g_n, qla[i], qlo[i], rad, res, GMAX, &sc, pfx);
          qsort(res, m, 4, cmp_u32);
          if (m != nref || memcmp(res, ref, (size_t)m * 4)) die("geo radius search disagrees with brute force");
        }
      }
      char what[48];
      snprintf(what, sizeof what, "points within %.0f m", rad);
      for (int pfx = 0; pfx < 2; pfx++) {
        uint64_t scanned = 0, found = 0;
        for (int r = 0; r < reps; r++) {
          uint64_t s = now_ns();
          scanned = found = 0;
          for (int i = 0; i < NG; i++) found += geo_query(g_n, qla[i], qlo[i], rad, res, GMAX, &scanned, pfx);
          t[r] = now_ns() - s;
        }
        snprintf(note, sizeof note, "%.1f results, %.0f codes scanned per query", (double)found / NG,
                 (double)scanned / NG);
        row(pfx ? "" : what,
            pfx ? "same, seeks through the 2^20 prefix table" : "4 Morton ranges (binary-search seeks), PEXT box",
            med(t, reps, NG), BAR_IND, note);
      }
    }
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns(), c = 0;
      double c0 = cos(qla[r] * M_PI / 180.0);
      for (uint32_t k = 0; k < g_n; k++) c += geo_within(g_pt_lat[k], g_pt_lon[k], qla[r], qlo[r], c0, 200.0);
      t[r] = now_ns() - s;
      g_vsink += c;
    }
    row("", "brute force: distance to every point", med(t, reps, 1), BAR_IND, "the alternative");
    g_vsink += sink;
    free(qla);
    free(qlo);
    free(res);
    free(ref);
  }
  printf("\n");
  print_memory();
  printf("all checks passed\n");
  return 0;
}
