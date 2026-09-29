/*
 * cap_probe.c — which query capabilities are O(1) or "near instant" on a
 * 100k-key collection, and what each costs? (docs/proposals/capabilities.md)
 *
 * Why it exists: the review asked what O(1) or near-instant capabilities a
 * 100k-key KvNode could offer. At that size the whole key set is ~2 MB and
 * fits in the CPU's L2/L3 cache, which changes the answer: some capabilities
 * need a dedicated structure to be O(1), others become near instant simply
 * because a brute-force pass over a compact, cache-resident key arena is
 * sub-millisecond. This probe measures both, on the server's real store, at
 * 100k keys and at 1M for scale:
 *
 *   point     exact Get through the server's batched GET pipeline (baseline)
 *   store     the scan the Export proposal would do without an index: walk
 *             the bucket array, read every record's key
 *   arena     all keys packed back to back ('\n'-separated, offsets array);
 *             what a scan-oriented copy of the key set costs to search
 *   simd      the arena searched 32 bytes at a time with AVX2 (first/last
 *             needle byte filter, Muła 2016); a SWAR/SSE2 version would be the
 *             fallback the SIMD policy requires
 *   trigram   posting lists per 3-byte substring (Russ Cox's code search):
 *             substring queries by list intersection, then verification
 *   symspell  every key and every single-byte deletion of it, hashed
 *             (Garbe's symmetric delete): typo lookups (edit distance 1) as
 *             ~20 hash probes, independent of n
 *   myers     edit distance by Myers/Hyyrö bit-parallel DP, one 64-bit word
 *             per key: brute-force typo search, any distance
 *   sorted    the keys sorted once (a snapshot): successor, rank, range and
 *             prefix COUNTS by binary search, in O(log n)
 *   bloom     a cache-line-blocked Bloom filter: "definitely absent" in one
 *             cache line (what a client could hold to skip round trips)
 *
 * Keys: "user:<name>:<field>", name = 5 syllables of the user number (base
 * 20, e.g. "kalominura"), 4 fields (name, email, plan, prefs); 20-21 bytes;
 * 100-byte values. Every query's result count is computed once by a plain,
 * obviously correct reference (per-key memmem / glob / full DP edit
 * distance) and every rival must return exactly that count, else the probe
 * exits non-zero. The Myers routine is also cross-checked against the full
 * DP on 20,000 random pairs before anything is timed.
 *
 * Usage: cap_probe --keys N (multiple of 4) [--cpu C] [--reps R]
 * Build:  run.sh next to this file (store assembled from /src/server/src).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* memmem, sched_setaffinity */
#endif
#include <immintrin.h>
#include <malloc.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kvnode_abi.h"

enum {
  KEYMAX = 32,           /* longest key: 5 + 10 + 1 + 5 = 21 bytes */
  VAL_LEN = 100,
  FIELDS = 4,
  SLOTS = 6,             /* BK_SLOTS */
  CHUNK = 8,             /* store scan: buckets gathered per prefetch step */
  FETCH_BATCH = 32,      /* point Gets per batched pipeline step */
  MAX_REPS = 16,
};

static const char *const kSyl[20] = {"ka", "lo", "mi", "nu", "ra", "se", "ti", "vo", "ze", "ba",
                                     "de", "fi", "go", "hu", "ja", "ke", "li", "mo", "na", "pe"};
static const char *const kFields[FIELDS] = {"name", "email", "plan", "prefs"};
/* SipHash key for the store and every auxiliary hash; read as two u64 words. */
static const uint8_t kSeed[16] __attribute__((aligned(8))) = {3, 1, 4, 1, 5, 9, 2, 6,
                                                              5, 3, 5, 8, 9, 7, 9, 3};

static uint32_t g_n;          /* keys */
static KvTable *g_store;

/* ------------------------------------------------------------------------- */
/* Utilities                                                                  */
/* ------------------------------------------------------------------------- */

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
 * @remarks Every check guards a printed number, so a failure aborts the run.
 */
static void die(const char *what) {
  fprintf(stderr, "cap_probe: %s\n", what);
  exit(1);
}

/**
 * malloc/calloc that cannot return NULL.
 * @param n  bytes
 * @returns zeroed memory.
 */
static void *xcalloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) die("out of memory");
  return p;
}

/**
 * Bytes currently allocated through malloc (glibc), for per-structure memory.
 * @returns mallinfo2().uordblks + hblkhd (large blocks are mmap'ed).
 */
static size_t heap_used(void) {
  struct mallinfo2 m = mallinfo2();
  return m.uordblks + m.hblkhd;
}

/**
 * The server's keyed SipHash of a byte string (low 64 bits).
 * @param p    bytes
 * @param len  length
 * @returns h1.
 */
static uint64_t hash64(const void *p, size_t len) {
  uint64_t h[2];
  kv_siphash128(p, len, (const uint64_t *)kSeed, h);
  return h[0];
}

/**
 * Writes key number i: "user:<5 syllables of i/4>:<field i%4>".
 * @param i    key index (< 4 * 20^5)
 * @param out  KEYMAX bytes
 * @returns the key length (20 or 21).
 */
static int make_key(uint32_t i, char out[KEYMAX]) {
  uint32_t u = i / FIELDS;
  char name[11];
  for (int s = 4; s >= 0; s--) {
    memcpy(name + 2 * s, kSyl[u % 20], 2);
    u /= 20;
  }
  name[10] = 0;
  int len = 5 + 10 + 1 + (int)strlen(kFields[i % FIELDS]);
  memcpy(out, "user:", 5);
  memcpy(out + 5, name, 10);
  out[15] = ':';
  memcpy(out + 16, kFields[i % FIELDS], strlen(kFields[i % FIELDS]));
  return len;
}

/**
 * Glob match ('*' any run of bytes, '?' one byte, else literal), iterative
 * with one backtrack point: linear for this probe's patterns.
 * @param p  pattern (NUL-terminated)
 * @param s  subject
 * @param n  subject length
 * @returns 1 on a whole-subject match.
 */
static int glob(const char *p, const uint8_t *s, uint32_t n) {
  const char *star = NULL;
  uint32_t i = 0, mark = 0;
  while (i < n) {
    if (*p == '*') {
      star = p++;
      mark = i;
    } else if (*p && (*p == '?' || (uint8_t)*p == s[i])) {
      p++;
      i++;
    } else if (star) {
      p = star + 1;
      i = ++mark;
    } else {
      return 0;
    }
  }
  while (*p == '*') p++;
  return *p == 0;
}

/* ------------------------------------------------------------------------- */
/* The key arena: every key back to back, '\n' after each                     */
/* ------------------------------------------------------------------------- */

static uint8_t *g_arena;      /* keys, each followed by '\n' */
static uint32_t *g_start;     /* g_start[i] = offset of key i; g_start[n] = arena length */

/** Key i's bytes in the arena. */
static inline const uint8_t *akey(uint32_t i) { return g_arena + g_start[i]; }
/** Key i's length (its '\n' excluded). */
static inline uint32_t alen(uint32_t i) { return g_start[i + 1] - g_start[i] - 1; }

/**
 * Which key contains arena offset `off` (binary search over g_start).
 * @param off  byte offset inside the arena
 * @returns the key index whose [start, next start) covers off.
 */
static uint32_t key_at(uint32_t off) {
  uint32_t lo = 0, hi = g_n;   /* invariant: g_start[lo] <= off < g_start[hi] */
  while (hi - lo > 1) {
    uint32_t mid = lo + (hi - lo) / 2;
    if (g_start[mid] <= off) lo = mid;
    else hi = mid;
  }
  return lo;
}

/* ------------------------------------------------------------------------- */
/* Edit distance: full DP (the reference) and Myers/Hyyrö bit-parallel        */
/* ------------------------------------------------------------------------- */

/**
 * Levenshtein distance by the textbook dynamic program (the reference).
 * @param a   first string   @param la  its length (<= KEYMAX)
 * @param b   second string  @param lb  its length (<= KEYMAX)
 * @returns the distance.
 */
static int lev_dp(const uint8_t *a, int la, const uint8_t *b, int lb) {
  int prev[KEYMAX + 1], cur[KEYMAX + 1];
  for (int j = 0; j <= lb; j++) prev[j] = j;
  for (int i = 1; i <= la; i++) {
    cur[0] = i;
    for (int j = 1; j <= lb; j++) {
      int sub = prev[j - 1] + (a[i - 1] != b[j - 1]);
      int del = prev[j] + 1, ins = cur[j - 1] + 1;
      cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
    }
    memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
  }
  return prev[lb];
}

/** A pattern prepared for Myers' algorithm (one 64-bit word: length <= 64). */
typedef struct {
  uint64_t peq[256];      /* peq[c]: bit i set where pattern[i] == c */
  int m;                  /* pattern length */
} Myers;

/**
 * Prepares a pattern's match masks.
 * @param my  receives the masks
 * @param p   pattern  @param m  its length (1..64)
 */
static void myers_init(Myers *my, const uint8_t *p, int m) {
  memset(my->peq, 0, sizeof my->peq);
  for (int i = 0; i < m; i++) my->peq[p[i]] |= 1ull << i;
  my->m = m;
}

/**
 * Levenshtein distance between the prepared pattern and a text, computed a
 * column at a time with 64-bit vectors (Myers 1999, in Hyyrö's global form).
 * @param my  prepared pattern
 * @param t   text  @param n  its length
 * @returns the exact distance.
 * @remarks Global (not search) form: the top row of the DP grows by 1 per
 *          text byte, so a 1 is shifted into the horizontal-positive vector
 *          each step. Bits above m hold garbage that never reaches bit m-1.
 */
static int myers_dist(const Myers *my, const uint8_t *t, int n) {
  uint64_t pv = my->m == 64 ? ~0ull : (1ull << my->m) - 1, mv = 0, high = 1ull << (my->m - 1);
  int score = my->m;
  for (int j = 0; j < n; j++) {
    uint64_t eq = my->peq[t[j]];
    uint64_t xv = eq | mv;
    uint64_t xh = (((eq & pv) + pv) ^ pv) | eq;
    uint64_t ph = mv | ~(xh | pv);
    uint64_t mh = pv & xh;
    if (ph & high) score++;
    else if (mh & high) score--;
    ph = (ph << 1) | 1;
    mh <<= 1;
    pv = mh | ~(xv | ph);
    mv = ph & xv;
  }
  return score;
}

/* ------------------------------------------------------------------------- */
/* Substring search in the arena: glibc memmem, and AVX2 first/last filter    */
/* ------------------------------------------------------------------------- */

/**
 * Counts keys containing `needle`, finding occurrences with glibc memmem.
 * @param needle  bytes (no '\n')   @param m  length (>= 1)
 * @returns matching keys (each counted once).
 */
static uint32_t arena_substr_memmem(const uint8_t *needle, size_t m) {
  uint32_t count = 0, pos = 0, end = g_start[g_n];
  for (;;) {
    const uint8_t *p = memmem(g_arena + pos, end - pos, needle, m);
    if (!p) return count;
    uint32_t k = key_at((uint32_t)(p - g_arena));
    count++;
    pos = g_start[k + 1];   /* continue after this key: count it once */
  }
}

/**
 * Counts keys containing `needle`, 32 arena bytes per step with AVX2: bytes
 * equal to the needle's first byte AND, m-1 bytes later, to its last byte
 * are candidates, confirmed by memcmp (Muła, "SIMD-friendly algorithms for
 * substring searching", 2016).
 * @param needle  bytes (no '\n')   @param m  length (>= 2)
 * @returns matching keys (each counted once).
 * @remarks The needle has no '\n', so no match can span two keys. The tail
 *          shorter than 32 + m - 1 bytes falls back to memmem.
 */
__attribute__((target("avx2"))) static uint32_t arena_substr_avx2(const uint8_t *needle, size_t m) {
  const __m256i first = _mm256_set1_epi8((char)needle[0]);
  const __m256i last = _mm256_set1_epi8((char)needle[m - 1]);
  uint32_t count = 0, end = g_start[g_n];
  size_t i = 0;
  while (i + 32 + m - 1 <= end) {
    __m256i a = _mm256_loadu_si256((const __m256i *)(g_arena + i));
    __m256i b = _mm256_loadu_si256((const __m256i *)(g_arena + i + m - 1));
    uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(_mm256_cmpeq_epi8(a, first),
                                                                   _mm256_cmpeq_epi8(b, last)));
    size_t next = i + 32;
    while (mask) {
      uint32_t bit = (uint32_t)__builtin_ctz(mask);
      mask &= mask - 1;
      if (memcmp(g_arena + i + bit + 1, needle + 1, m - 2) == 0) {
        uint32_t k = key_at((uint32_t)(i + bit));
        count++;
        next = g_start[k + 1];   /* skip the rest of this key */
        break;
      }
    }
    i = next;
  }
  if (i < end) {
    /* The tail. Every match STARTING before i was already seen: the vector
     * loop tests all start positions below i, and only jumps over keys it
     * counted. So the key holding position i is searched from i on (a match
     * there starts at >= i and has not been seen), later keys whole. */
    uint32_t k = key_at((uint32_t)i), kend = g_start[k] + alen(k);
    if (i < kend && memmem(g_arena + i, kend - i, needle, m)) count++;
    for (k++; k < g_n; k++)
      if (memmem(akey(k), alen(k), needle, m)) count++;
  }
  return count;
}

/* ------------------------------------------------------------------------- */
/* Trigram index                                                              */
/* ------------------------------------------------------------------------- */

/** One trigram's posting list (key ids, ascending by construction). */
typedef struct {
  uint32_t tri;           /* 24-bit trigram + 1 (0 = empty slot) */
  uint32_t n, cap;
  uint32_t *ids;
} Post;

static Post *g_tri;           /* open addressing, TRI_SLOTS entries */
enum { TRI_SLOTS = 1 << 16 };

/**
 * Finds (or adds) a trigram's posting list.
 * @param tri     the three bytes as a 24-bit number
 * @param create  1 to add a missing list
 * @returns the list, or NULL when missing and not created.
 * @remarks This keyspace has at most a few thousand distinct trigrams; the
 *          table is sized far above that and the probe dies if it ever fills.
 */
static Post *tri_find(uint32_t tri, int create) {
  /* Knuth multiplicative hashing: take the product's HIGH bits (the low bits
   * of tri * odd depend only on tri's low bits and would cluster). */
  uint32_t key = tri + 1, i = ((tri * 2654435761u) >> 16) & (TRI_SLOTS - 1);
  for (uint32_t probes = 0; probes < TRI_SLOTS; probes++, i = (i + 1) & (TRI_SLOTS - 1)) {
    if (g_tri[i].tri == key) return &g_tri[i];
    if (g_tri[i].tri == 0) {
      if (!create) return NULL;
      g_tri[i].tri = key;
      return &g_tri[i];
    }
  }
  die("trigram table full");
  return NULL;
}

/**
 * Indexes key i: appends i to the list of each trigram it contains (once).
 * @param i  key index (keys are added in ascending order)
 */
static void tri_add(uint32_t i) {
  const uint8_t *k = akey(i);
  uint32_t len = alen(i);
  for (uint32_t p = 0; p + 3 <= len; p++) {
    Post *l = tri_find((uint32_t)k[p] << 16 | (uint32_t)k[p + 1] << 8 | k[p + 2], 1);
    if (l->n && l->ids[l->n - 1] == i) continue;   /* the key repeats this trigram */
    if (l->n == l->cap) {
      l->cap = l->cap ? l->cap * 2 : 8;
      l->ids = realloc(l->ids, (size_t)l->cap * sizeof *l->ids);
      if (!l->ids) die("out of memory");
    }
    l->ids[l->n++] = i;
  }
}

/** qsort comparator: posting lists by length. */
static int cmp_post_len(const void *a, const void *b) {
  uint32_t x = (*(Post *const *)a)->n, y = (*(Post *const *)b)->n;
  return (x > y) - (x < y);
}

/**
 * Counts keys containing `needle` (length >= 3) with the trigram index:
 * intersect the needle's trigram lists, shortest first, then verify each
 * surviving key with memmem (a key holding every trigram may still not hold
 * the needle).
 * @param needle  bytes   @param m  length (>= 3)
 * @param cand    scratch for candidates (g_n entries)
 * @returns matching keys.
 */
static uint32_t tri_substr(const uint8_t *needle, size_t m, uint32_t *cand) {
  Post *lists[KEYMAX];
  int nl = 0;
  for (size_t p = 0; p + 3 <= m; p++) {
    Post *l = tri_find((uint32_t)needle[p] << 16 | (uint32_t)needle[p + 1] << 8 | needle[p + 2], 0);
    if (!l) return 0;                       /* a trigram no key has: no match */
    int dup = 0;
    for (int j = 0; j < nl; j++) dup |= lists[j] == l;
    if (!dup) lists[nl++] = l;
  }
  qsort(lists, (size_t)nl, sizeof *lists, cmp_post_len);
  uint32_t nc = lists[0]->n;
  memcpy(cand, lists[0]->ids, (size_t)nc * sizeof *cand);
  for (int j = 1; j < nl && nc; j++) {      /* merge-intersect, both ascending */
    const uint32_t *b = lists[j]->ids;
    uint32_t nb = lists[j]->n, x = 0, y = 0, out = 0;
    while (x < nc && y < nb) {
      if (cand[x] < b[y]) x++;
      else if (cand[x] > b[y]) y++;
      else {
        cand[out++] = cand[x];
        x++;
        y++;
      }
    }
    nc = out;
  }
  uint32_t count = 0;
  for (uint32_t c = 0; c < nc; c++)
    if (memmem(akey(cand[c]), alen(cand[c]), needle, m)) count++;
  return count;
}

/* ------------------------------------------------------------------------- */
/* SymSpell (distance 1): every key and each single-byte deletion, hashed      */
/* ------------------------------------------------------------------------- */

static uint64_t *g_sym;       /* (fingerprint << 32) | key id; 0 = empty */
static uint64_t g_sym_mask;
static uint32_t *g_seen;      /* per-key query stamp, dedupes candidates */
static uint32_t g_stamp;

/**
 * 32-bit fingerprint of a variant (never 0, so 0 marks an empty slot).
 * @param s  bytes  @param n  length
 * @returns the fingerprint.
 */
static uint32_t sym_fp(const uint8_t *s, uint32_t n) {
  uint32_t fp = (uint32_t)(hash64(s, n) >> 32);
  return fp ? fp : 1;
}

/**
 * Adds one (variant, key) pair; duplicates are allowed (a multi-map).
 * @param fp  the variant's fingerprint   @param id  the key
 */
static void sym_put(uint32_t fp, uint32_t id) {
  uint64_t i = ((uint64_t)fp * 0x9E3779B97F4A7C15ull >> 20) & g_sym_mask;
  while (g_sym[i]) i = (i + 1) & g_sym_mask;
  g_sym[i] = (uint64_t)fp << 32 | id;
}

/**
 * Indexes key i: the key itself and each distinct single-byte deletion.
 * @param i  key index
 * @returns the variants stored.
 * @remarks Deleting either of two equal adjacent bytes gives the same string,
 *          so such a deletion is stored once.
 */
static uint32_t sym_add(uint32_t i) {
  const uint8_t *k = akey(i);
  uint32_t len = alen(i), stored = 1;
  uint8_t v[KEYMAX];
  sym_put(sym_fp(k, len), i);
  for (uint32_t d = 0; d < len; d++) {
    if (d > 0 && k[d] == k[d - 1]) continue;
    memcpy(v, k, d);
    memcpy(v + d, k + d + 1, len - d - 1);
    sym_put(sym_fp(v, len - 1), i);
    stored++;
  }
  return stored;
}

/**
 * Probes one variant and verifies every new candidate (distance <= 1).
 * @param fp  the variant's fingerprint
 * @param my  the query, prepared for Myers
 * @returns matches found through this variant.
 */
static uint32_t sym_probe(uint32_t fp, const Myers *my) {
  uint32_t count = 0;
  uint64_t i = ((uint64_t)fp * 0x9E3779B97F4A7C15ull >> 20) & g_sym_mask;
  for (; g_sym[i]; i = (i + 1) & g_sym_mask) {
    if ((uint32_t)(g_sym[i] >> 32) != fp) continue;
    uint32_t id = (uint32_t)g_sym[i];
    if (g_seen[id] == g_stamp) continue;
    g_seen[id] = g_stamp;
    uint32_t len = alen(id);
    if ((int)len - my->m > 1 || my->m - (int)len > 1) continue;
    if (myers_dist(my, akey(id), (int)len) <= 1) count++;
  }
  return count;
}

/**
 * Counts keys within edit distance 1 of q: q and each of its deletions are
 * looked up; symmetric-delete guarantees every such key shares a variant.
 * @param q  query  @param m  its length
 * @returns matching keys.
 */
static uint32_t sym_query(const uint8_t *q, uint32_t m) {
  Myers my;
  uint8_t v[KEYMAX];
  myers_init(&my, q, (int)m);
  g_stamp++;
  uint32_t count = sym_probe(sym_fp(q, m), &my);
  for (uint32_t d = 0; d < m; d++) {
    if (d > 0 && q[d] == q[d - 1]) continue;
    memcpy(v, q, d);
    memcpy(v + d, q + d + 1, m - d - 1);
    count += sym_probe(sym_fp(v, m - 1), &my);
  }
  return count;
}

/* ------------------------------------------------------------------------- */
/* Sorted snapshot: successor, rank, range and prefix counts                  */
/* ------------------------------------------------------------------------- */

static uint32_t *g_sorted;    /* key ids in byte order */

/**
 * Compares two byte strings (memcmp order, shorter first on a tie).
 * @returns <0, 0 or >0.
 */
static int bytes_cmp(const uint8_t *a, uint32_t la, const uint8_t *b, uint32_t lb) {
  int c = memcmp(a, b, la < lb ? la : lb);
  return c ? c : (la > lb) - (la < lb);
}

/** qsort comparator: key ids by their key bytes. */
static int cmp_id(const void *x, const void *y) {
  uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
  return bytes_cmp(akey(a), alen(a), akey(b), alen(b));
}

/**
 * First position whose key is >= target (binary search).
 * @param t  target bytes  @param tl  its length
 * @returns 0..g_n.
 */
static uint32_t lower_bound(const uint8_t *t, uint32_t tl) {
  uint32_t lo = 0, hi = g_n;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2, id = g_sorted[mid];
    if (bytes_cmp(akey(id), alen(id), t, tl) < 0) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/**
 * How many keys start with prefix p — two binary searches, no iteration.
 * @param p  prefix (its last byte is not 0xFF)  @param pl  its length (>= 1)
 * @returns the count.
 * @remarks The keys with prefix p form one contiguous run in byte order:
 *          from lower_bound(p) up to lower_bound(p with its last byte + 1).
 */
static uint32_t prefix_count(const uint8_t *p, uint32_t pl) {
  uint8_t next[KEYMAX];
  memcpy(next, p, pl);
  next[pl - 1]++;
  return lower_bound(next, pl) - lower_bound(p, pl);
}

/* ------------------------------------------------------------------------- */
/* Blocked Bloom filter: 8 bits per key inside one 64-byte block               */
/* ------------------------------------------------------------------------- */

static uint64_t *g_bloom;     /* g_bloom_blocks x 8 words */
static uint64_t g_bloom_blocks;

/**
 * Adds a key's hash: block from h1, eight 9-bit positions from h2 (taken 7
 * bits apart, so all eight get a full 9 bits out of the 64).
 * @param h  the key's SipHash-128 (h[0] = h1, h[1] = h2)
 */
static void bloom_add(const uint64_t h[2]) {
  uint64_t *blk = g_bloom + (h[0] % g_bloom_blocks) * 8;
  for (int k = 0; k < 8; k++) {
    uint32_t bit = (uint32_t)(h[1] >> (k * 7)) & 511;
    blk[bit >> 6] |= 1ull << (bit & 63);
  }
}

/**
 * May the key be present? (No false negatives; ~1% false positives here.)
 * @param h  the key's SipHash-128
 * @returns 0 = definitely absent, 1 = possibly present.
 */
static int bloom_maybe(const uint64_t h[2]) {
  const uint64_t *blk = g_bloom + (h[0] % g_bloom_blocks) * 8;
  for (int k = 0; k < 8; k++) {
    uint32_t bit = (uint32_t)(h[1] >> (k * 7)) & 511;
    if (!(blk[bit >> 6] & (1ull << (bit & 63)))) return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* Store scan (records, no copy of the keys)                                  */
/* ------------------------------------------------------------------------- */

/** kvnode.inc Bucket and the head of Table (see export_probe.c). */
typedef struct {
  uint64_t ver, tags;
  KvRec *slot[SLOTS];
} Bucket;
typedef struct {
  Bucket *buckets;
  uint64_t mask;
} TableHead;

/** What a store scan tests each key for. */
typedef struct {
  int kind;               /* 0 substring, 1 glob */
  const uint8_t *s;       /* needle or pattern */
  size_t m;
} Pred;

/**
 * Counts records whose key satisfies the predicate, walking the bucket array
 * with the records of the next chunk prefetched (the Export scan).
 * @param pr  predicate
 * @returns matching keys.
 */
static uint32_t store_scan(const Pred *pr) {
  const TableHead *head = (const TableHead *)g_store;
  const Bucket *b = head->buckets;
  uint64_t nb = head->mask + 1;
  uint32_t count = 0;
  for (uint64_t first = 0; first < nb; first += CHUNK) {
    const KvRec *recs[CHUNK * SLOTS];
    int n = 0;
    uint64_t end = first + CHUNK < nb ? first + CHUNK : nb;
    for (uint64_t i = first; i < end; i++)
      for (int s = 0; s < SLOTS; s++)
        if (b[i].slot[s]) {
          recs[n++] = b[i].slot[s];
          __builtin_prefetch(b[i].slot[s]);
        }
    for (int k = 0; k < n; k++) {
      const KvRec *r = recs[k];
      if (pr->kind == 0 ? memmem(r->data, r->key_len, pr->s, pr->m) != NULL
                        : glob((const char *)pr->s, r->data, r->key_len))
        count++;
    }
  }
  return count;
}

/* ------------------------------------------------------------------------- */
/* Driver                                                                     */
/* ------------------------------------------------------------------------- */

/** qsort comparator for uint64_t. */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/**
 * Median of `reps` timings, each of `iters` calls, in ns per call.
 * @param t     per-rep total ns (sorted in place)
 * @param reps  repetitions   @param iters  calls per rep
 * @returns median ns per call.
 */
static double median_per(uint64_t *t, int reps, uint64_t iters) {
  qsort(t, (size_t)reps, sizeof *t, cmp_u64);
  return (double)t[reps / 2] / (double)iters;
}

/**
 * Prints one result row.
 * @param cap   capability   @param how  technique   @param ns  latency
 * @param note  result size or remark
 */
static void row(const char *cap, const char *how, double ns, const char *note) {
  if (ns < 1000) printf("  %-30s %-34s %11.1f ns   %s\n", cap, how, ns, note);
  else if (ns < 1e6) printf("  %-30s %-34s %11.2f us   %s\n", cap, how, ns / 1e3, note);
  else printf("  %-30s %-34s %11.3f ms   %s\n", cap, how, ns / 1e6, note);
}

/**
 * Builds the store and every structure, checks every query against its
 * reference, and prints latencies, build costs and memory.
 * @param argc  argument count
 * @param argv  --keys N [--cpu C] [--reps R]
 * @returns 0; exits 1 on any failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  int cpu = 4, reps = 7;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--keys")) g_n = (uint32_t)strtoul(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--cpu")) cpu = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--reps")) reps = atoi(argv[i + 1]);
  }
  if (g_n < 10000 || g_n % FIELDS || g_n > 4000000 || reps < 1 || reps > MAX_REPS) {
    fprintf(stderr, "usage: cap_probe --keys N (multiple of 4, 10000..4000000) [--cpu C] [--reps R<=%d]\n",
            MAX_REPS);
    return 2;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) die("sched_setaffinity");
  if (!__builtin_cpu_supports("avx2")) die("this probe's SIMD variant needs AVX2");

  /* Myers must agree with the full DP before any fuzzy number counts. */
  {
    uint64_t rng = 88172645463325252ull;
    for (int t = 0; t < 20000; t++) {
      uint8_t a[KEYMAX], b[KEYMAX];
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      int la = 1 + (int)(rng % 24), lb = 1 + (int)((rng >> 8) % 24);
      for (int j = 0; j < la; j++) a[j] = (uint8_t)('a' + ((rng >> (j % 50)) + (uint64_t)j * 7) % 4);
      for (int j = 0; j < lb; j++) b[j] = (uint8_t)('a' + ((rng >> ((j + 5) % 50)) + (uint64_t)j * 3) % 4);
      Myers my;
      myers_init(&my, a, la);
      if (myers_dist(&my, b, lb) != lev_dp(a, la, b, lb)) die("Myers disagrees with the DP");
    }
  }

  /* ---- the store, filled as the server would be ---- */
  if (kv_set_scan_mode(KV_SCAN_SSE2) < 0) die("kv_set_scan_mode");
  g_store = kv_store_create(g_n, 1, kSeed, 0);
  if (!g_store) die("kv_store_create");
  uint8_t val[VAL_LEN];
  char key[KEYMAX];
  for (uint32_t i = 0; i < g_n; i++) {
    uint64_t h[2];
    int len = make_key(i, key);
    for (int j = 0; j < VAL_LEN; j++) val[j] = (uint8_t)(i * 31u + (uint32_t)j);
    kv_store_hash(g_store, key, (size_t)len, h);
    if (kv_store_put(g_store, key, (size_t)len, val, VAL_LEN, h[0], h[1]) != 1) die("fill put");
  }
  kv_qsbr_online(g_store, 0);
  printf("keys %u, cpu %d, reps %d\n", g_n, cpu, reps);

  /* ---- build every structure, timed, with its memory ---- */
  printf("\nbuild (whole set; per key = the cost a create would pay to maintain it)\n");
  size_t m0 = heap_used();
  uint64_t t0 = now_ns();
  g_start = xcalloc(((size_t)g_n + 1) * sizeof *g_start);
  g_arena = xcalloc((size_t)g_n * (KEYMAX + 1));
  uint32_t off = 0;
  for (uint32_t i = 0; i < g_n; i++) {
    g_start[i] = off;
    off += (uint32_t)make_key(i, (char *)g_arena + off);
    g_arena[off++] = '\n';
  }
  g_start[g_n] = off;
  double b_arena = (double)(now_ns() - t0) / g_n;
  printf("  %-12s %9.1f ns/key %9.1f bytes/key  (%.2f MB of keys + offsets)\n", "arena", b_arena,
         (double)(off + 4.0 * g_n) / g_n, (off + 4.0 * g_n) / 1048576.0);

  size_t m1 = heap_used();
  t0 = now_ns();
  g_sorted = xcalloc((size_t)g_n * sizeof *g_sorted);
  for (uint32_t i = 0; i < g_n; i++) g_sorted[i] = i;
  qsort(g_sorted, g_n, sizeof *g_sorted, cmp_id);
  double b_sorted = (double)(now_ns() - t0) / g_n;
  printf("  %-12s %9.1f ns/key %9.1f bytes/key  (sort of the whole set: %.1f ms)\n", "sorted", b_sorted,
         (double)(heap_used() - m1) / g_n, b_sorted * g_n / 1e6);

  size_t m2 = heap_used();
  t0 = now_ns();
  g_tri = xcalloc((size_t)TRI_SLOTS * sizeof *g_tri);
  for (uint32_t i = 0; i < g_n; i++) tri_add(i);
  double b_tri = (double)(now_ns() - t0) / g_n;
  printf("  %-12s %9.1f ns/key %9.1f bytes/key\n", "trigram", b_tri, (double)(heap_used() - m2) / g_n);

  size_t m3 = heap_used();
  t0 = now_ns();
  uint64_t slots = 1;
  while (slots < (uint64_t)g_n * 22 * 2) slots <<= 1;   /* <= 22 variants per key, <= 50% load */
  g_sym = xcalloc(slots * sizeof *g_sym);
  g_sym_mask = slots - 1;
  g_seen = xcalloc((size_t)g_n * sizeof *g_seen);
  uint64_t variants = 0;
  for (uint32_t i = 0; i < g_n; i++) variants += sym_add(i);
  double b_sym = (double)(now_ns() - t0) / g_n;
  printf("  %-12s %9.1f ns/key %9.1f bytes/key  (%.1f variants per key)\n", "symspell", b_sym,
         (double)(heap_used() - m3) / g_n, (double)variants / g_n);

  size_t m4 = heap_used();
  t0 = now_ns();
  g_bloom_blocks = ((uint64_t)g_n * 10 + 511) / 512;   /* ~10 bits per key */
  g_bloom = xcalloc(g_bloom_blocks * 64);
  for (uint32_t i = 0; i < g_n; i++) {
    uint64_t h[2];
    kv_siphash128(akey(i), alen(i), (const uint64_t *)kSeed, h);
    bloom_add(h);
  }
  double b_bloom = (double)(now_ns() - t0) / g_n;
  printf("  %-12s %9.1f ns/key %9.1f bits/key\n", "bloom", b_bloom, (double)(heap_used() - m4) * 8 / g_n);
  (void)m0;

  /* ---- queries: references first ---- */
  const char *sub_sel = "lomika", *sub_wide = "kalo", *pat = "user:ka*mi*:e*";
  uint32_t ref_sel = 0, ref_wide = 0, ref_glob = 0;
  for (uint32_t i = 0; i < g_n; i++) {
    ref_sel += memmem(akey(i), alen(i), sub_sel, 6) != NULL;
    ref_wide += memmem(akey(i), alen(i), sub_wide, 4) != NULL;
    ref_glob += glob(pat, akey(i), alen(i));
  }
  /* Typo queries built from key n/3: one substitution, one deletion, one
   * insertion (distance 1), and a two-substitution one (distance 2). */
  uint32_t base = g_n / 3;
  uint8_t qs[4][KEYMAX];
  uint32_t ql[4];
  memcpy(qs[0], akey(base), alen(base)); ql[0] = alen(base); qs[0][8] = 'x';
  ql[1] = alen(base) - 1; memcpy(qs[1], akey(base), 9); memcpy(qs[1] + 9, akey(base) + 10, ql[1] - 9);
  ql[2] = alen(base) + 1; memcpy(qs[2], akey(base), 7); qs[2][7] = 'q'; memcpy(qs[2] + 8, akey(base) + 7, alen(base) - 7);
  memcpy(qs[3], akey(base), alen(base)); ql[3] = alen(base); qs[3][8] = 'x'; qs[3][11] = 'y';
  uint32_t ref_fz[4] = {0};
  for (int q = 0; q < 4; q++)
    for (uint32_t i = 0; i < g_n; i++)
      ref_fz[q] += lev_dp(qs[q], (int)ql[q], akey(i), (int)alen(i)) <= (q == 3 ? 2 : 1);
  if (!ref_fz[0] || !ref_fz[1] || !ref_fz[2] || !ref_fz[3]) die("a typo query found nothing");

  uint64_t t[MAX_REPS];
  char note[96];
  printf("\nqueries (median over %d repetitions; every result count checked against its reference)\n", reps);

  /* Exact Get: 1,024 random keys through the batched pipeline, per key. */
  {
    uint32_t ids[1024];
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 1024; i++) {
      rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
      ids[i] = (uint32_t)(rng % g_n);
    }
    for (int r = 0; r < reps; r++) {
      uint64_t h[FETCH_BATCH][2], found = 0, s = now_ns();
      for (int base_i = 0; base_i < 1024; base_i += FETCH_BATCH) {
        for (int k = 0; k < FETCH_BATCH; k++) {
          kv_store_hash(g_store, akey(ids[base_i + k]), alen(ids[base_i + k]), h[k]);
          kv_store_prefetch(g_store, h[k][0], h[k][1]);
        }
        for (int k = 0; k < FETCH_BATCH; k++) kv_store_prefetch_records(g_store, h[k][0], h[k][1]);
        for (int k = 0; k < FETCH_BATCH; k++)
          found += kv_store_get(g_store, akey(ids[base_i + k]), alen(ids[base_i + k]), h[k][0], h[k][1]) != NULL;
      }
      t[r] = now_ns() - s;
      kv_qsbr_quiescent(g_store, 0);
      if (found != 1024) die("exact get missed");
    }
    row("exact get", "cuckoo, batched pipeline", median_per(t, reps, 1024), "per key");
  }

  /* Membership: 100k absent keys ("user:<name>:zzzz"), one filter probe each. */
  {
    uint32_t fp = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      fp = 0;
      for (uint32_t i = 0; i < 100000; i++) {
        uint64_t h[2];
        char k[KEYMAX];
        int len = make_key(i * 4 % g_n, k);
        memcpy(k + len - 4, "zzzz", 4);
        kv_siphash128(k, (size_t)len, (const uint64_t *)kSeed, h);
        fp += (uint32_t)bloom_maybe(h);
      }
      t[r] = now_ns() - s;
    }
    for (uint32_t i = 0; i < g_n; i++) {       /* no false negatives, ever */
      uint64_t h[2];
      kv_siphash128(akey(i), alen(i), (const uint64_t *)kSeed, h);
      if (!bloom_maybe(h)) die("bloom false negative");
    }
    snprintf(note, sizeof note, "false positives %.2f%% (incl. hashing the key)", fp / 1000.0);
    row("definitely-absent test", "blocked bloom, 10 bits/key", median_per(t, reps, 100000), note);
  }

  /* Ordered queries on the sorted snapshot: 1,000 random probes each. */
  {
    uint8_t probe[1000][KEYMAX];
    uint32_t plen[1000];
    for (int i = 0; i < 1000; i++) {
      uint32_t id = (uint32_t)(((uint64_t)i * 2654435761u) % g_n);
      memcpy(probe[i], akey(id), alen(id));
      plen[i] = alen(id);
      probe[i][plen[i] - 1]++;                 /* a key that does not exist */
    }
    uint64_t sink = 0;
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 1000; i++) sink += lower_bound(probe[i], plen[i]);
      t[r] = now_ns() - s;
    }
    row("successor / rank", "sorted snapshot, binary search", median_per(t, reps, 1000), "any key, O(log n)");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 1000; i++) sink += prefix_count(probe[i], 9);   /* "user:kalo" style */
      t[r] = now_ns() - s;
    }
    uint32_t pc = prefix_count((const uint8_t *)"user:kalo", 9), ref_pc = 0;
    for (uint32_t i = 0; i < g_n; i++) ref_pc += alen(i) >= 9 && memcmp(akey(i), "user:kalo", 9) == 0;
    if (pc != ref_pc) die("prefix count");
    snprintf(note, sizeof note, "no iteration (\"user:kalo\" = %u keys)", pc);
    row("count by prefix", "sorted snapshot, 2 binary searches", median_per(t, reps, 1000), note);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      for (int i = 0; i < 100; i++) {
        uint32_t lo = lower_bound(probe[i], plen[i]), hi = lo + 100 < g_n ? lo + 100 : g_n;
        for (uint32_t j = lo; j < hi; j++) sink += alen(g_sorted[j]);
      }
      t[r] = now_ns() - s;
    }
    row("range scan, 100 keys", "sorted snapshot", median_per(t, reps, 100), "seek + 100 keys in order");
    if (sink == 42) printf(" ");
    /* What keeping it sorted costs a create: binary search + memmove of the tail. */
    uint32_t *copy = xcalloc(((size_t)g_n + 1000) * sizeof *copy);
    memcpy(copy, g_sorted, (size_t)g_n * sizeof *copy);
    uint64_t s = now_ns();
    for (int i = 0; i < 1000; i++) {
      uint32_t at = lower_bound(probe[i], plen[i]);   /* positions from the original order: close enough */
      memmove(copy + at + 1, copy + at, (size_t)(g_n + (uint32_t)i - at) * sizeof *copy);
      copy[at] = 0;
    }
    row("keep it sorted on create", "array insert (memmove)", (double)(now_ns() - s) / 1000, "per create: O(n)");
    free(copy);
  }

  /* Substring: store scan, arena memmem, arena AVX2, trigram index. */
  struct {
    const char *s;
    uint32_t ref;
  } subs[2] = {{sub_sel, ref_sel}, {sub_wide, ref_wide}};
  uint32_t *cand = xcalloc((size_t)g_n * sizeof *cand);
  for (int q = 0; q < 2; q++) {
    const uint8_t *n = (const uint8_t *)subs[q].s;
    size_t m = strlen(subs[q].s);
    Pred pr = {0, n, m};
    char cap[48];
    snprintf(cap, sizeof cap, "contains \"%s\" (%u keys)", subs[q].s, subs[q].ref);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      if (store_scan(&pr) != subs[q].ref) die("store scan substring");
      t[r] = now_ns() - s;
    }
    row(cap, "scan of the store's records", median_per(t, reps, 1), "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      if (arena_substr_memmem(n, m) != subs[q].ref) die("arena memmem");
      t[r] = now_ns() - s;
    }
    row("", "key arena, glibc memmem", median_per(t, reps, 1), "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      if (arena_substr_avx2(n, m) != subs[q].ref) die("arena avx2");
      t[r] = now_ns() - s;
    }
    row("", "key arena, AVX2 first/last filter", median_per(t, reps, 1), "");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      if (tri_substr(n, m, cand) != subs[q].ref) die("trigram substring");
      t[r] = now_ns() - s;
    }
    row("", "trigram index", median_per(t, reps, 1), "");
  }

  /* Wildcard (glob) over every key: store scan vs arena. */
  {
    Pred pr = {1, (const uint8_t *)pat, 0};
    char cap[48];
    snprintf(cap, sizeof cap, "glob %s (%u)", pat, ref_glob);
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns();
      if (store_scan(&pr) != ref_glob) die("store scan glob");
      t[r] = now_ns() - s;
    }
    row(cap, "scan of the store's records", median_per(t, reps, 1), "any pattern, O(n)");
    for (int r = 0; r < reps; r++) {
      uint64_t s = now_ns(), c = 0;
      for (uint32_t i = 0; i < g_n; i++) c += glob(pat, akey(i), alen(i));
      if (c != ref_glob) die("arena glob");
      t[r] = now_ns() - s;
    }
    row("", "key arena", median_per(t, reps, 1), "any pattern, O(n)");
  }

  /* Typo tolerance: Myers over the arena (any distance) vs SymSpell (distance 1). */
  {
    static const char *const kinds[4] = {"typo: substitution (d<=1)", "typo: deletion (d<=1)",
                                         "typo: insertion (d<=1)", "typo: two substitutions (d<=2)"};
    for (int q = 0; q < 4; q++) {
      int d = q == 3 ? 2 : 1;
      Myers my;
      myers_init(&my, qs[q], (int)ql[q]);
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns(), c = 0;
        for (uint32_t i = 0; i < g_n; i++) {
          int l = (int)alen(i);
          if (l - (int)ql[q] > d || (int)ql[q] - l > d) continue;   /* length filter */
          c += myers_dist(&my, akey(i), l) <= d;
        }
        if (c != ref_fz[q]) die("myers fuzzy count");
        t[r] = now_ns() - s;
      }
      snprintf(note, sizeof note, "%u matches", ref_fz[q]);
      row(kinds[q], "key arena, Myers bit-parallel", median_per(t, reps, 1), note);
      if (q == 3) continue;
      if (sym_query(qs[q], ql[q]) != ref_fz[q]) die("symspell count");   /* exact, vs the DP */
      /* Timed on 1,000 DIFFERENT typos of the same kind (a repeated query
       * would keep its ~20 table lines hot), each of which must at least
       * find the key it was made from. */
      static uint8_t tq[1000][KEYMAX];
      static uint32_t tl[1000];
      for (int it = 0; it < 1000; it++) {
        uint32_t id = (uint32_t)(((uint64_t)it * 2654435761u + 12345) % g_n), l = alen(id);
        if (q == 0) { memcpy(tq[it], akey(id), l); tq[it][8] = 'x'; tl[it] = l; }
        else if (q == 1) { memcpy(tq[it], akey(id), 9); memcpy(tq[it] + 9, akey(id) + 10, l - 10); tl[it] = l - 1; }
        else { memcpy(tq[it], akey(id), 7); tq[it][7] = 'q'; memcpy(tq[it] + 8, akey(id) + 7, l - 7); tl[it] = l + 1; }
      }
      for (int r = 0; r < reps; r++) {
        uint64_t s = now_ns();
        for (int it = 0; it < 1000; it++)
          if (sym_query(tq[it], tl[it]) < 1) die("symspell missed a typo's own key");
        t[r] = now_ns() - s;
      }
      row("", "SymSpell index (symmetric delete)", median_per(t, reps, 1000), "1,000 different typos; independent of n");
    }
  }
  return 0;
}
