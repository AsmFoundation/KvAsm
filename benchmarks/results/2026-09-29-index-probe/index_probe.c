/*
 * index_probe.c — can prefix, suffix and pattern queries avoid the full scan,
 * and what does an index cost the write path? (docs/proposals/key-indexes.md)
 *
 * Why it exists: without an index, every filtered Export scans the whole
 * table (export_probe: 11-41 ns per key, whatever the result size). The
 * review asked how prefix, suffix and even regex queries could be O(1). The
 * lower bound for a query that returns k keys is O(k), so the goal is
 * "O(1) to locate + O(k) to emit", independent of the store size n. That
 * needs an index, and every index adds work to the writes that create and
 * delete keys (replacing a value leaves the key set, and so any key index,
 * unchanged). This probe measures both sides, on the server's real store,
 * for the two rival index shapes and the scan they would replace:
 *
 *   SCAN  no index: walk the bucket array, read every key, match, emit
 *         (the proposal's Export path; records prefetched a chunk ahead)
 *   DIR   segment directories: every key joins the hash-addressed directory
 *         of each of its ':'-terminated prefixes (depth 1 and 2 here) and of
 *         its last segment (a suffix directory). O(1) hash work per key per
 *         directory, but only delimiter-aligned prefixes and suffixes are
 *         indexed; anything else filters the nearest directory
 *   ART   adaptive radix tries (libart, Leis et al.'s ART in C, pinned): one
 *         over the keys (any prefix), one over the reversed keys (any
 *         suffix). O(key length) per key, independent of n
 *
 * Keys are hierarchical, as most real keyspaces are: "t%03u:u%07u:<field>",
 * 1,000 tenants x U users x 4 fields (name, email, plan, prefs); 18-19 bytes;
 * 100-byte values. U = 250 (1M keys) or 4,000 (16M keys).
 *
 * Measured, per rival:
 *   build    ns per key to index the whole store (random key order)
 *   memory   bytes per key the index allocates (glibc mallinfo2 delta)
 *   churn    at full size, 10% of the keys (random order): ns per index
 *            delete and per index insert; the store's own kv_store_delete +
 *            kv_store_put for the same keys is the base write cost
 *   queries  the median time to produce every matching key AND value into
 *            64 KiB output batches (what an Export produces before the
 *            network). Index rivals fetch values through the store with the
 *            server's batched GET pipeline (hash, prefetch buckets, prefetch
 *            records, look up). SCAN reads records directly.
 *     Q1 prefix  "t007:"           one tenant           k = 4U
 *     Q2 prefix  "t007:u0000123:"  one user             k = 4
 *     Q3 prefix  "t007:u00001"     users 100-199        k = 400  (not delimiter-aligned)
 *     Q4 suffix  ":email"          every email          k = n/4
 *     Q5 glob    "t007:u00001*:e*" literal prefix + wildcards  k = 100
 *     Q6 glob    "*u000012*"       no literal prefix or suffix: no index helps  k = 40,000
 * Every query's result count is checked against the generator's arithmetic,
 * and every fetched value is checked against its key; a mismatch exits
 * non-zero, so no number is printed for a query that returned wrong data.
 *
 * Usage: index_probe --keys N (a multiple of 4,000; 1000000 or 16000000)
 *        [--cpu C] [--reps R]
 * Build:  run.sh next to this file (fetches libart at a pinned commit and
 *         verifies its SHA-256, assembles the store from /src/server/src).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* sched_setaffinity */
#endif
#include <malloc.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "art.h"
#include "kvasm_abi.h"

enum {
  VAL_LEN = 100,
  FIELDS = 4,
  TENANTS = 1000,
  KEYMAX = 32,           /* longest generated key is 19 bytes */
  DIRMAX = 24,           /* longest directory name: "t999:u9999999:" = 14 */
  SLOTS = 6,             /* BK_SLOTS */
  CHUNK = 8,             /* scan: buckets gathered per prefetch step */
  FETCH_BATCH = 32,      /* index rivals: keys per batched value fetch */
  OUT_BATCH = 64 * 1024, /* export message budget */
  MAX_REPS = 16,
  NQ = 6,                /* queries */
};

static const char *const kFields[FIELDS] = {"name", "email", "plan", "prefs"};
/* SipHash key for the store and the directory table; read as two u64 words. */
static const uint8_t kSeed[16] __attribute__((aligned(8))) = {7, 1, 4, 2, 8, 5, 7, 1,
                                                              4, 2, 8, 5, 7, 1, 4, 2};

static uint32_t g_users;      /* users per tenant */
static uint64_t g_nkeys;
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
  fprintf(stderr, "index_probe: %s\n", what);
  exit(1);
}

/**
 * malloc that cannot return NULL (the probe cannot continue without memory).
 * @param n  bytes
 * @returns the block.
 */
static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p) die("out of memory");
  return p;
}

/**
 * Bytes currently allocated through malloc (glibc), for per-index memory.
 * @returns mallinfo2().uordblks.
 */
static size_t heap_used(void) { return mallinfo2().uordblks; }

/**
 * Writes key number i: "t%03u:u%07u:<field>".
 * @param i    key index: i = (tenant * users + user) * 4 + field
 * @param out  KEYMAX bytes
 * @returns the key length (18 or 19).
 * @remarks The modulos are no-ops for every index the probe generates; they
 *          give the compiler the digit bounds that prove no truncation.
 */
static int make_key(uint64_t i, char out[KEYMAX]) {
  uint32_t f = (uint32_t)(i % FIELDS);
  uint64_t tu = i / FIELDS;
  uint32_t u = (uint32_t)(tu % g_users), t = (uint32_t)(tu / g_users);
  return snprintf(out, KEYMAX, "t%03u:u%07u:%s", t % 1000u, u % 10000000u, kFields[f]);
}

/**
 * Recovers the key index from a generated key (the inverse of make_key).
 * @param k    key bytes
 * @param len  key length
 * @returns the index, or UINT64_MAX if the key is not a generated one.
 */
static uint64_t key_index(const uint8_t *k, uint32_t len) {
  if (len < 18 || k[0] != 't' || k[4] != ':' || k[5] != 'u' || k[13] != ':') return UINT64_MAX;
  uint64_t t = 0, u = 0;
  for (int i = 1; i < 4; i++) t = t * 10 + (uint64_t)(k[i] - '0');
  for (int i = 6; i < 13; i++) u = u * 10 + (uint64_t)(k[i] - '0');
  for (uint64_t f = 0; f < FIELDS; f++) {
    size_t fl = strlen(kFields[f]);
    if (len == 14 + fl && memcmp(k + 14, kFields[f], fl) == 0) return (t * g_users + u) * FIELDS + f;
  }
  return UINT64_MAX;
}

/**
 * Fills a value exactly as the store was filled, for key index i.
 * @param i    key index
 * @param val  VAL_LEN bytes
 */
static void make_value(uint64_t i, uint8_t val[VAL_LEN]) {
  for (int j = 0; j < VAL_LEN; j++) val[j] = (uint8_t)(i * 31 + (uint64_t)j);
}

/**
 * Glob match ('*' any run of bytes, '?' one byte, anything else literal),
 * iterative with one backtrack point: linear for the probe's patterns.
 * @param p   pattern (NUL-terminated)
 * @param s   subject bytes
 * @param n   subject length
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
/* Output: what an Export would frame (Entry{key, value} in 64 KiB batches)   */
/* ------------------------------------------------------------------------- */

/** Output sink shared by every rival, so every query does the same copying. */
typedef struct {
  uint8_t buf[OUT_BATCH];
  size_t pos;
  uint64_t bytes;         /* total bytes produced */
  uint64_t count;         /* entries produced */
} Out;

/**
 * Appends Entry{key, value} with protobuf-shaped headers, "sending" (resetting)
 * the batch when it is full, and checks the value belongs to the key.
 * @param o     sink
 * @param key   key bytes
 * @param klen  key length
 * @param val   value bytes
 * @param vlen  value length
 * @remarks Exits if the value is not the one generated for this key: a rival
 *          that fetched the wrong record would otherwise go unnoticed.
 */
static void emit(Out *o, const uint8_t *key, uint32_t klen, const uint8_t *val, uint32_t vlen) {
  uint64_t idx = key_index(key, klen);
  if (idx == UINT64_MAX || vlen != VAL_LEN || val[0] != (uint8_t)(idx * 31) ||
      val[VAL_LEN - 1] != (uint8_t)(idx * 31 + VAL_LEN - 1))
    die("emitted value does not belong to its key");
  size_t need = 6 + klen + vlen;
  if (o->pos + need > OUT_BATCH) {
    o->bytes += o->pos;
    o->pos = 0;
  }
  o->buf[o->pos++] = 0x12;
  o->buf[o->pos++] = (uint8_t)(4 + klen + vlen);
  o->buf[o->pos++] = 0x0A;
  o->buf[o->pos++] = (uint8_t)klen;
  memcpy(o->buf + o->pos, key, klen);
  o->pos += klen;
  o->buf[o->pos++] = 0x12;
  o->buf[o->pos++] = (uint8_t)vlen;
  memcpy(o->buf + o->pos, val, vlen);
  o->pos += vlen;
  o->count++;
}

/* ------------------------------------------------------------------------- */
/* Batched value fetch for index rivals (the server's GET pipeline)           */
/* ------------------------------------------------------------------------- */

/** Keys waiting for their values; flushed FETCH_BATCH at a time. */
typedef struct {
  uint8_t key[FETCH_BATCH][KEYMAX];
  uint32_t len[FETCH_BATCH];
  uint64_t h[FETCH_BATCH][2];
  int n;
  Out *out;
} Fetch;

/**
 * Looks up every queued key the way the server answers a batch of GETs: hash
 * and prefetch both buckets for all, then prefetch candidate records for all,
 * then look each up and emit it.
 * @param f  queue (emptied)
 * @remarks The calling thread is a QSBR reader kept online by the caller;
 *          every returned record is used before the next quiescent state.
 */
static void fetch_flush(Fetch *f) {
  for (int k = 0; k < f->n; k++) {
    kv_store_hash(g_store, f->key[k], f->len[k], f->h[k]);
    kv_store_prefetch(g_store, f->h[k][0], f->h[k][1]);
  }
  for (int k = 0; k < f->n; k++) kv_store_prefetch_records(g_store, f->h[k][0], f->h[k][1]);
  for (int k = 0; k < f->n; k++) {
    const KvRec *r = kv_store_get(g_store, f->key[k], f->len[k], f->h[k][0], f->h[k][1]);
    if (!r) die("an indexed key is missing from the store");
    emit(f->out, r->data, r->key_len, kv_rec_value(r), r->val_len);
  }
  f->n = 0;
}

/**
 * Queues one key (copied: index iterators may reuse their buffers).
 * @param f    queue
 * @param key  key bytes
 * @param len  key length (<= KEYMAX)
 */
static void fetch_add(Fetch *f, const uint8_t *key, uint32_t len) {
  memcpy(f->key[f->n], key, len);
  f->len[f->n] = len;
  if (++f->n == FETCH_BATCH) fetch_flush(f);
}

/* ------------------------------------------------------------------------- */
/* The queries                                                                */
/* ------------------------------------------------------------------------- */

typedef enum { QK_PREFIX, QK_SUFFIX, QK_GLOB } QueryKind;

/** One query and what the generator says it must return. */
typedef struct {
  const char *name;
  QueryKind kind;
  const char *text;       /* prefix, suffix or glob */
  const char *lit;        /* for globs: the literal prefix an index can use ("" = none) */
  uint64_t expect;        /* result count, filled in main */
} Query;

static Query g_q[NQ] = {
    {"Q1 prefix t007:", QK_PREFIX, "t007:", "", 0},
    {"Q2 prefix t007:u0000123:", QK_PREFIX, "t007:u0000123:", "", 0},
    {"Q3 prefix t007:u00001", QK_PREFIX, "t007:u00001", "", 0},
    {"Q4 suffix :email", QK_SUFFIX, ":email", "", 0},
    {"Q5 glob t007:u00001*:e*", QK_GLOB, "t007:u00001*:e*", "t007:u00001", 0},
    {"Q6 glob *u000012*", QK_GLOB, "*u000012*", "", 0},
};

/**
 * Does a key satisfy a query? (The exact semantics every rival must match.)
 * @param q    query
 * @param k    key
 * @param len  key length
 * @returns 1 if it matches.
 */
static int query_match(const Query *q, const uint8_t *k, uint32_t len) {
  size_t tl = strlen(q->text);
  switch (q->kind) {
    case QK_PREFIX: return len >= tl && memcmp(k, q->text, tl) == 0;
    case QK_SUFFIX: return len >= tl && memcmp(k + len - tl, q->text, tl) == 0;
    default: return glob(q->text, k, len);
  }
}

/* ------------------------------------------------------------------------- */
/* SCAN: the proposal's Export path without an index                          */
/* ------------------------------------------------------------------------- */

/** kvasm.inc Bucket (one cache line) and the head of Table (see export_probe.c). */
typedef struct {
  uint64_t ver, tags;
  KvRec *slot[SLOTS];
} Bucket;
typedef struct {
  Bucket *buckets;
  uint64_t mask;
} TableHead;

/**
 * Gathers the record pointers of up to CHUNK buckets and prefetches each
 * record's first three lines (header, key, value at any 16-byte alignment).
 * @param b      bucket array
 * @param nb     bucket count
 * @param first  first bucket
 * @param out    receives up to CHUNK * SLOTS pointers
 * @returns pointers gathered.
 */
static int gather(const Bucket *b, uint64_t nb, uint64_t first, KvRec **out) {
  int n = 0;
  uint64_t end = first + CHUNK < nb ? first + CHUNK : nb;
  for (uint64_t i = first; i < end; i++) {
    for (int s = 0; s < SLOTS; s++) {
      KvRec *r = b[i].slot[s];
      if (!r) continue;
      out[n++] = r;
      __builtin_prefetch((const char *)r);
      __builtin_prefetch((const char *)r + 64);
      __builtin_prefetch((const char *)r + 128);
    }
  }
  return n;
}

/**
 * Full-scan query: every record is read, matched, and emitted if it matches.
 * @param q  query
 * @param o  sink
 */
static void scan_query(const Query *q, Out *o) {
  const TableHead *head = (const TableHead *)g_store;
  const Bucket *b = head->buckets;
  uint64_t nb = head->mask + 1;
  KvRec *cur[CHUNK * SLOTS], *next[CHUNK * SLOTS];
  int n = gather(b, nb, 0, cur);
  for (uint64_t first = 0; first < nb; first += CHUNK) {
    int m = first + CHUNK < nb ? gather(b, nb, first + CHUNK, next) : 0;
    for (int k = 0; k < n; k++) {
      const KvRec *r = cur[k];
      if (query_match(q, r->data, r->key_len)) emit(o, r->data, r->key_len, kv_rec_value(r), r->val_len);
    }
    memcpy(cur, next, (size_t)m * sizeof *cur);
    n = m;
  }
}

/* ------------------------------------------------------------------------- */
/* DIR: segment directories                                                   */
/* ------------------------------------------------------------------------- */

typedef struct Dir Dir;

/**
 * One indexed key. In a server the record header would point here (and a
 * replace would carry the pointer over), so a delete needs no extra lookup;
 * the probe models that with a direct array indexed by key number.
 */
typedef struct {
  Dir *dir[3];            /* 0: depth-1 prefix, 1: depth-2 prefix, 2: suffix */
  uint32_t pos[3];        /* position in each directory's member array */
  uint8_t len;
  uint8_t key[KEYMAX];
} Member;

/** A directory: a name and an unordered member array (swap-remove). */
struct Dir {
  uint32_t count, cap;
  Member **m;
  uint8_t len;
  uint8_t name[DIRMAX];
};

/** Directory lookup table: open addressing, linear probing, never shrinks. */
typedef struct {
  Dir **slot;
  uint64_t mask;
} DirMap;

static DirMap g_dirs;
static Member **g_members;   /* key index -> Member (the record's back-pointer) */

/**
 * Finds a directory by name, creating it if asked.
 * @param name    directory name bytes
 * @param len     name length (<= DIRMAX)
 * @param create  1 to create a missing directory
 * @returns the directory, or NULL when missing and not created.
 * @remarks Hashes with the server's SipHash (a real index must resist
 *          flooding, as the table does, ADR 0004). The table is sized in
 *          main for every directory the generator can produce, at <= 50%
 *          load, so probing always terminates.
 */
static Dir *dir_find(const uint8_t *name, uint32_t len, int create) {
  uint64_t h[2];
  kv_siphash128(name, len, (const uint64_t *)kSeed, h);
  for (uint64_t i = h[0] & g_dirs.mask;; i = (i + 1) & g_dirs.mask) {
    Dir *d = g_dirs.slot[i];
    if (!d) {
      if (!create) return NULL;
      d = xmalloc(sizeof *d);
      d->count = 0;
      d->cap = 0;
      d->m = NULL;
      d->len = (uint8_t)len;
      memcpy(d->name, name, len);
      g_dirs.slot[i] = d;
      return d;
    }
    if (d->len == len && memcmp(d->name, name, len) == 0) return d;
  }
}

/**
 * The three directory names of a key: depth-1 prefix ("t007:"), depth-2
 * prefix ("t007:u0000123:"), and suffix ("\x01:email"; the 0x01 marker keeps
 * suffix names apart from prefix names in the one table).
 * @param k     key
 * @param len   key length
 * @param name  receives three names
 * @param nl    receives their lengths
 */
static void dir_names(const uint8_t *k, uint32_t len, uint8_t name[3][DIRMAX], uint32_t nl[3]) {
  memcpy(name[0], k, 5);
  nl[0] = 5;
  memcpy(name[1], k, 14);
  nl[1] = 14;
  name[2][0] = 0x01;
  memcpy(name[2] + 1, k + 13, len - 13);
  nl[2] = 1 + len - 13;
}

/**
 * Indexes key i: joins its three directories.
 * @param i  key index
 */
static void dir_insert(uint64_t i) {
  char key[KEYMAX];
  uint8_t name[3][DIRMAX];
  uint32_t nl[3];
  int len = make_key(i, key);
  Member *m = xmalloc(sizeof *m);
  m->len = (uint8_t)len;
  memcpy(m->key, key, (size_t)len);
  dir_names(m->key, (uint32_t)len, name, nl);
  for (int s = 0; s < 3; s++) {
    Dir *d = dir_find(name[s], nl[s], 1);
    if (d->count == d->cap) {
      d->cap = d->cap ? d->cap * 2 : 4;
      d->m = realloc(d->m, (size_t)d->cap * sizeof *d->m);
      if (!d->m) die("out of memory");
    }
    d->m[d->count] = m;
    m->dir[s] = d;
    m->pos[s] = d->count++;
  }
  g_members[i] = m;
}

/**
 * Removes key i from its three directories (swap-remove: O(1) per directory).
 * @param i  key index (must be indexed)
 */
static void dir_delete(uint64_t i) {
  Member *m = g_members[i];
  for (int s = 0; s < 3; s++) {
    Dir *d = m->dir[s];
    uint32_t p = m->pos[s];
    Member *last = d->m[--d->count];
    d->m[p] = last;
    last->pos[s] = p;   /* a directory holds members in one role only */
  }
  free(m);
  g_members[i] = NULL;
}

/**
 * Answers a query from the directories: the directory the query names
 * exactly when there is one, else the nearest enclosing depth-1 directory
 * filtered by the query; a query no directory can narrow returns 0.
 * @param q  query
 * @param o  sink
 * @returns 1 if answered, 0 if DIR cannot help (the caller would scan).
 */
static int dir_query(const Query *q, Out *o) {
  Fetch f = {.n = 0, .out = o};
  const char *t = q->kind == QK_GLOB ? q->lit : q->text;
  size_t tl = strlen(t);
  Dir *d = NULL;
  int exact = 0;
  if (q->kind == QK_SUFFIX) {
    uint8_t name[DIRMAX];
    name[0] = 0x01;
    memcpy(name + 1, t, tl);
    d = dir_find(name, (uint32_t)tl + 1, 0);
    exact = 1;
  } else if (tl >= 5) {
    /* A name ending at a delimiter at depth 1 or 2 is itself a directory. */
    if (q->kind == QK_PREFIX && (tl == 5 || tl == 14) && t[tl - 1] == ':') {
      d = dir_find((const uint8_t *)t, (uint32_t)tl, 0);
      exact = 1;
    } else {
      d = dir_find((const uint8_t *)t, 5, 0);   /* the tenant directory */
    }
  } else {
    return 0;
  }
  if (d) {
    for (uint32_t j = 0; j < d->count; j++) {
      const Member *m = d->m[j];
      if (exact || query_match(q, m->key, m->len)) fetch_add(&f, m->key, m->len);
    }
    fetch_flush(&f);
  }
  return 1;
}

/**
 * Frees every directory and member.
 */
static void dir_free(void) {
  for (uint64_t i = 0; i < g_nkeys; i++) free(g_members[i]);
  for (uint64_t i = 0; i <= g_dirs.mask; i++) {
    if (g_dirs.slot[i]) {
      free(g_dirs.slot[i]->m);
      free(g_dirs.slot[i]);
    }
  }
  free(g_dirs.slot);
  g_dirs.slot = NULL;
}

/* ------------------------------------------------------------------------- */
/* ART: forward trie (prefixes) and reversed trie (suffixes)                  */
/* ------------------------------------------------------------------------- */

static art_tree g_fwd, g_rev;

/**
 * Writes key i terminated by a NUL (no indexed key is then a prefix of
 * another, which libart, like most ARTs, requires), optionally reversed.
 * @param i        key index
 * @param reverse  1 for the suffix trie
 * @param out      KEYMAX + 1 bytes
 * @returns the indexed length (key length + 1).
 */
static int art_key(uint64_t i, int reverse, uint8_t out[KEYMAX + 1]) {
  char key[KEYMAX];
  int len = make_key(i, key);
  for (int j = 0; j < len; j++) out[j] = (uint8_t)key[reverse ? len - 1 - j : j];
  out[len] = 0;
  return len + 1;
}

/** Context for the trie iteration callback. */
typedef struct {
  const Query *q;
  Fetch *f;
  int reversed;           /* keys come from the suffix trie */
} ArtCtx;

/**
 * libart callback: un-reverse if needed, filter by the query, queue the key.
 * @param data   ArtCtx*
 * @param key    indexed key (NUL-terminated form)
 * @param klen   its length, NUL included
 * @param value  unused
 * @returns 0 (continue).
 */
static int art_visit(void *data, const unsigned char *key, uint32_t klen, void *value) {
  (void)value;
  ArtCtx *c = data;
  uint8_t k[KEYMAX];
  uint32_t len = klen - 1;
  for (uint32_t j = 0; j < len; j++) k[j] = key[c->reversed ? len - 1 - j : j];
  if (c->q->kind != QK_GLOB || query_match(c->q, k, len)) fetch_add(c->f, k, len);
  return 0;
}

/**
 * Answers a query from the tries: prefixes and glob literal prefixes from the
 * forward trie, suffixes from the reversed one.
 * @param q  query
 * @param o  sink
 * @returns 1 if answered, 0 if no trie can narrow it (the caller would scan).
 */
static int art_query(const Query *q, Out *o) {
  Fetch f = {.n = 0, .out = o};
  ArtCtx c = {q, &f, 0};
  const char *t = q->kind == QK_GLOB ? q->lit : q->text;
  size_t tl = strlen(t);
  if (tl == 0) return 0;
  if (q->kind == QK_SUFFIX) {
    uint8_t r[KEYMAX];
    for (size_t j = 0; j < tl; j++) r[j] = (uint8_t)t[tl - 1 - j];
    c.reversed = 1;
    art_iter_prefix(&g_rev, r, (int)tl, art_visit, &c);
  } else {
    art_iter_prefix(&g_fwd, (const unsigned char *)t, (int)tl, art_visit, &c);
  }
  fetch_flush(&f);
  return 1;
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
 * Times a query `reps` times with one rival and checks every result count.
 * @param q      query
 * @param rival  0 SCAN, 1 DIR, 2 ART
 * @param reps   repetitions
 * @returns median ns, or UINT64_MAX when the rival cannot narrow the query.
 * @remarks The thread is QSBR-online while it reads records and announces a
 *          quiescent state after each query (no record pointer is kept).
 */
static uint64_t time_query(const Query *q, int rival, int reps) {
  static Out o;
  uint64_t t[MAX_REPS];
  for (int r = 0; r < reps; r++) {
    o.pos = 0;
    o.bytes = 0;
    o.count = 0;
    uint64_t t0 = now_ns();
    int ok = rival == 0 ? (scan_query(q, &o), 1) : rival == 1 ? dir_query(q, &o) : art_query(q, &o);
    t[r] = now_ns() - t0;
    kv_qsbr_quiescent(g_store, 0);
    if (!ok) return UINT64_MAX;
    if (o.count != q->expect) {
      fprintf(stderr, "index_probe: %s rival %d returned %llu, expected %llu\n", q->name, rival,
              (unsigned long long)o.count, (unsigned long long)q->expect);
      exit(1);
    }
  }
  qsort(t, (size_t)reps, sizeof *t, cmp_u64);
  return t[reps / 2];
}

/**
 * Parses options, builds the store and each index, and prints the results.
 * @param argc  argument count
 * @param argv  --keys N [--cpu C] [--reps R]
 * @returns 0; exits 1 on any failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  int cpu = 4, reps = 5;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--keys")) g_nkeys = strtoull(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--cpu")) cpu = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--reps")) reps = atoi(argv[i + 1]);
  }
  /* 200 users per tenant at least, so Q3/Q5's user range 100-199 exists and
   * Q6's 120-129 too; 10^7 users keeps make_key's 7 digits exact. */
  if (g_nkeys % (TENANTS * FIELDS) != 0 || g_nkeys / (TENANTS * FIELDS) < 200 ||
      g_nkeys / (TENANTS * FIELDS) > 10000000 || reps < 1 || reps > MAX_REPS) {
    fprintf(stderr, "usage: index_probe --keys N (multiple of 4000, 800000..4e10) [--cpu C] [--reps R<=%d]\n",
            MAX_REPS);
    return 2;
  }
  g_users = (uint32_t)(g_nkeys / (TENANTS * FIELDS));
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) die("sched_setaffinity");

  /* Expected result counts, from the generator's arithmetic. */
  g_q[0].expect = (uint64_t)g_users * FIELDS;
  g_q[1].expect = FIELDS;
  g_q[2].expect = 100 * FIELDS;
  g_q[3].expect = g_nkeys / FIELDS;
  g_q[4].expect = 100;
  g_q[5].expect = (uint64_t)TENANTS * 10 * FIELDS;

  /* ---- the store, filled as the server would be ---- */
  /* The assembled default is SWAR; the server selects SSE2 at startup
   * (KVASM_SCAN default, ADR 0006), so the probe's lookups do too. */
  if (kv_set_scan_mode(KV_SCAN_SSE2) < 0) die("kv_set_scan_mode");
  g_store = kv_store_create(g_nkeys, 1, kSeed, 0);
  if (!g_store) die("kv_store_create");
  uint8_t val[VAL_LEN];
  char key[KEYMAX];
  for (uint64_t i = 0; i < g_nkeys; i++) {
    uint64_t h[2];
    int len = make_key(i, key);
    make_value(i, val);
    kv_store_hash(g_store, key, (size_t)len, h);
    if (kv_store_put(g_store, key, (size_t)len, val, VAL_LEN, h[0], h[1]) != 1) die("fill put");
  }
  kv_qsbr_online(g_store, 0);

  /* Random key order for index builds and churn (xorshift Fisher-Yates). */
  uint64_t *perm = xmalloc(g_nkeys * sizeof *perm);
  uint64_t rng = 0x2545F4914F6CDD1Dull;
  for (uint64_t i = 0; i < g_nkeys; i++) perm[i] = i;
  for (uint64_t i = g_nkeys - 1; i > 0; i--) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    uint64_t j = rng % (i + 1), tmp = perm[i];
    perm[i] = perm[j];
    perm[j] = tmp;
  }
  uint64_t churn = g_nkeys / 10;
  printf("keys %llu (1,000 tenants x %u users x 4 fields), cpu %d, reps %d, churn %llu keys\n",
         (unsigned long long)g_nkeys, g_users, cpu, reps, (unsigned long long)churn);

  uint64_t tq[3][NQ];
  double build_ns[3] = {0}, mem_b[3] = {0}, del_ns[3] = {0}, ins_ns[3] = {0};
  double art_fwd_build = 0, art_rev_build = 0, art_fwd_mem = 0, art_rev_mem = 0;
  double art_fwd_del = 0, art_fwd_ins = 0, art_rev_del = 0, art_rev_ins = 0;

  /* ---- DIR ---- */
  {
    uint64_t dirs = (uint64_t)TENANTS * g_users + TENANTS + FIELDS, slots = 1;
    while (slots < dirs * 2) slots <<= 1;
    g_dirs.slot = calloc(slots, sizeof *g_dirs.slot);
    g_dirs.mask = slots - 1;
    g_members = calloc(g_nkeys, sizeof *g_members);
    if (!g_dirs.slot || !g_members) die("out of memory");
    size_t m0 = heap_used();
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < g_nkeys; i++) dir_insert(perm[i]);
    build_ns[1] = (double)(now_ns() - t0) / (double)g_nkeys;
    mem_b[1] = (double)(heap_used() - m0) / (double)g_nkeys;
    for (int q = 0; q < NQ; q++) tq[1][q] = time_query(&g_q[q], 1, reps);
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) dir_delete(perm[i]);
    del_ns[1] = (double)(now_ns() - t0) / (double)churn;
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) dir_insert(perm[i]);
    ins_ns[1] = (double)(now_ns() - t0) / (double)churn;
    for (int q = 0; q < NQ; q++)   /* the index still answers correctly after churn */
      if (g_q[q].kind != QK_GLOB || g_q[q].lit[0]) (void)time_query(&g_q[q], 1, 1);
    dir_free();
    free(g_members);
  }

  /* ---- ART ---- */
  {
    uint8_t k[KEYMAX + 1];
    art_tree_init(&g_fwd);
    art_tree_init(&g_rev);
    size_t m0 = heap_used();
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < g_nkeys; i++) {
      int len = art_key(perm[i], 0, k);
      art_insert(&g_fwd, k, len, (void *)(uintptr_t)1);
    }
    art_fwd_build = (double)(now_ns() - t0) / (double)g_nkeys;
    size_t m1 = heap_used();
    art_fwd_mem = (double)(m1 - m0) / (double)g_nkeys;
    t0 = now_ns();
    for (uint64_t i = 0; i < g_nkeys; i++) {
      int len = art_key(perm[i], 1, k);
      art_insert(&g_rev, k, len, (void *)(uintptr_t)1);
    }
    art_rev_build = (double)(now_ns() - t0) / (double)g_nkeys;
    art_rev_mem = (double)(heap_used() - m1) / (double)g_nkeys;
    if (art_size(&g_fwd) != g_nkeys || art_size(&g_rev) != g_nkeys) die("art size after build");
    for (int q = 0; q < NQ; q++) tq[2][q] = time_query(&g_q[q], 2, reps);
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      int len = art_key(perm[i], 0, k);
      if (!art_delete(&g_fwd, k, len)) die("art fwd delete");
    }
    art_fwd_del = (double)(now_ns() - t0) / (double)churn;
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      int len = art_key(perm[i], 0, k);
      art_insert(&g_fwd, k, len, (void *)(uintptr_t)1);
    }
    art_fwd_ins = (double)(now_ns() - t0) / (double)churn;
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      int len = art_key(perm[i], 1, k);
      if (!art_delete(&g_rev, k, len)) die("art rev delete");
    }
    art_rev_del = (double)(now_ns() - t0) / (double)churn;
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      int len = art_key(perm[i], 1, k);
      art_insert(&g_rev, k, len, (void *)(uintptr_t)1);
    }
    art_rev_ins = (double)(now_ns() - t0) / (double)churn;
    if (art_size(&g_fwd) != g_nkeys || art_size(&g_rev) != g_nkeys) die("art size after churn");
    for (int q = 0; q < NQ; q++)
      if (g_q[q].kind != QK_GLOB || g_q[q].lit[0]) (void)time_query(&g_q[q], 2, 1);
    art_tree_destroy(&g_fwd);
    art_tree_destroy(&g_rev);
    build_ns[2] = art_fwd_build + art_rev_build;
    mem_b[2] = art_fwd_mem + art_rev_mem;
    del_ns[2] = art_fwd_del + art_rev_del;
    ins_ns[2] = art_fwd_ins + art_rev_ins;
  }

  /* ---- SCAN (no index) ---- */
  for (int q = 0; q < NQ; q++) tq[0][q] = time_query(&g_q[q], 0, reps);

  /* ---- the store's own write cost for the same churn (the base) ---- */
  kv_qsbr_offline(g_store, 0);
  {
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      uint64_t h[2];
      int len = make_key(perm[i], key);
      kv_store_hash(g_store, key, (size_t)len, h);
      if (kv_store_delete(g_store, key, (size_t)len, h[0], h[1]) != 1) die("store delete");
    }
    del_ns[0] = (double)(now_ns() - t0) / (double)churn;
    t0 = now_ns();
    for (uint64_t i = 0; i < churn; i++) {
      uint64_t h[2];
      int len = make_key(perm[i], key);
      make_value(perm[i], val);
      kv_store_hash(g_store, key, (size_t)len, h);
      if (kv_store_put(g_store, key, (size_t)len, val, VAL_LEN, h[0], h[1]) != 1) die("store put");
    }
    ins_ns[0] = (double)(now_ns() - t0) / (double)churn;
  }

  /* ---- report ---- */
  printf("\nwrite path (ns per key; index work only, the store's own cost on the first line)\n");
  printf("%-26s %10s %10s %12s %12s\n", "", "build", "bytes/key", "delete", "insert");
  printf("%-26s %10s %10s %12.1f %12.1f\n", "store (base: delete, put)", "-", "-", del_ns[0], ins_ns[0]);
  printf("%-26s %10.1f %10.1f %12.1f %12.1f\n", "DIR (3 directories)", build_ns[1], mem_b[1], del_ns[1], ins_ns[1]);
  printf("%-26s %10.1f %10.1f %12.1f %12.1f\n", "ART forward (prefixes)", art_fwd_build, art_fwd_mem, art_fwd_del, art_fwd_ins);
  printf("%-26s %10.1f %10.1f %12.1f %12.1f\n", "ART reversed (suffixes)", art_rev_build, art_rev_mem, art_rev_del, art_rev_ins);
  printf("%-26s %10.1f %10.1f %12.1f %12.1f\n", "ART both", build_ns[2], mem_b[2], del_ns[2], ins_ns[2]);
  printf("\nqueries (median ms to produce every matching key+value; speedup vs SCAN)\n");
  printf("%-28s %9s %11s %20s %20s\n", "", "k", "SCAN", "DIR", "ART");
  for (int q = 0; q < NQ; q++) {
    char d[40], a[40];
    if (tq[1][q] == UINT64_MAX) snprintf(d, sizeof d, "(scan)");
    else snprintf(d, sizeof d, "%.4f (%.0fx)", tq[1][q] / 1e6, (double)tq[0][q] / (double)tq[1][q]);
    if (tq[2][q] == UINT64_MAX) snprintf(a, sizeof a, "(scan)");
    else snprintf(a, sizeof a, "%.4f (%.0fx)", tq[2][q] / 1e6, (double)tq[0][q] / (double)tq[2][q]);
    printf("%-28s %9llu %11.3f %20s %20s\n", g_q[q].name, (unsigned long long)g_q[q].expect,
           tq[0][q] / 1e6, d, a);
  }
  kv_store_destroy(g_store);
  free(perm);
  return 0;
}
