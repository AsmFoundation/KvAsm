/*
 * export_probe.c — design-stage measurements behind
 * docs/proposals/export-watch.md (Export / Watch with key patterns).
 *
 * Why it exists: the proposal has to choose how an Export gets a consistent,
 * point-in-time view of a store that never stops taking writes, and what a
 * pattern-filtered full scan costs. Those choices hinge on four numbers that
 * estimates cannot settle: how long a writer would wait if the snapshot were
 * taken under the writer lock, what a copy-on-write page costs the writer
 * that triggers it, how fast a full scan can read and match keys, and how
 * fast an exporter can produce output. This probe measures them on the
 * server's real store code (store.asm, cuckoo.asm, siphash.asm, util.asm,
 * wlock.asm), single-threaded and pinned, with the benchmark suite's record
 * shape: 16-byte keys ("k:%014llu") and 100-byte values.
 *
 * Measurements (each repeated --reps times; the median is reported):
 *   collect-pf   copy every live record pointer out of the bucket array into
 *                a prefaulted array: the writer-lock hold of the naive
 *                consistent snapshot, best case
 *   collect-new  the same into a freshly malloc'd array (page faults
 *                included), as a real server would do per export
 *   tablecopy    memcpy of the whole bucket array into prefaulted memory:
 *                the most copy-on-write can ever copy
 *   page-pf      one cold 4 KiB page of buckets (random page order, caches
 *                thrashed first) into a hot destination: COW's copy alone
 *   page-fault   the same into a fresh MADV_NOHUGEPAGE mapping, so each copy
 *                also takes the first-touch fault of its destination page:
 *                what a writer pays, all in, the first time it modifies a
 *                page during an export
 *   occupancy    bucket scan that reads tags only: the floor of any scan
 *   prefix       scan + read every key + compare a 10-byte prefix ("k:00000001":
 *                indexes 1,000,000-1,999,999, so no key of a 1M store and 1M
 *                keys of the 4M and 16M stores; every key is still read and
 *                compared), records prefetched one chunk ahead
 *   dfa          the same with a byte-class DFA for the glob "k:*7?" (tens
 *                digit 7, exactly 10% of the keys), run over the whole key
 *   emit         match everything and copy each key and value, with
 *                protobuf-shaped field headers, into 64 KiB output batches:
 *                the export production rate before the network
 * Every count is checked (records seen, prefix and DFA matches, bytes
 * emitted); a mismatch exits non-zero, so a number is never reported for a
 * scan that silently skipped records.
 *
 * Usage:   export_probe --keys N [--cpu C] [--reps R]
 * Build:   see run.sh next to this file (assembles the store modules from
 *          /src/server/src into a scratch directory; no repo build output is
 *          touched).
 * Caveats: WSL2 vCPUs on a shared host; absolute numbers are indicative.
 *          The decisions drawn from them rest on orders of magnitude
 *          (microseconds vs milliseconds), not on small differences.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* sched_setaffinity; run.sh also passes -D_GNU_SOURCE */
#endif
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include "kvnode_abi.h"

enum {
  KEY_LEN = 16,           /* "k:" + 14 decimal digits */
  VAL_LEN = 100,          /* the benchmark suite's value size */
  SLOTS = 6,              /* BK_SLOTS in kvnode.inc */
  PAGE = 4096,            /* the proposed copy-on-write granule: 64 buckets */
  CHUNK = 8,              /* buckets gathered (and their records prefetched) per step */
  OUT_BATCH = 64 * 1024,  /* export message budget */
  MAX_REPS = 64,
};

/** One bucket exactly as kvnode.inc's `Bucket` lays it out: one 64-byte line. */
typedef struct {
  uint64_t ver;           /* seqlock version (odd while a writer modifies it) */
  uint64_t tags;          /* six 1-byte tags, 0 = empty slot */
  KvRec *slot[SLOTS];     /* record pointers, NULL = empty */
} Bucket;

/**
 * The first two fields of kvnode.inc's `Table` (its read-mostly line 0).
 * KvTable is opaque to C; this probe needs only the bucket array and its mask,
 * whose offsets have not changed since 0.1.0 (test_abi pins the layout).
 */
typedef struct {
  Bucket *buckets;        /* Bucket[mask + 1], 64-byte aligned */
  uint64_t mask;          /* nbuckets - 1 */
} TableHead;

/** Scan modes of scan(): what is done with each occupied slot. */
typedef enum { SCAN_PREFIX, SCAN_DFA, SCAN_EMIT } ScanMode;

/** Result of one scan() pass, checked against the expected counts. */
typedef struct {
  uint64_t seen;          /* occupied slots visited */
  uint64_t matched;       /* keys the matcher accepted */
  uint64_t bytes;         /* output bytes produced (SCAN_EMIT only) */
} ScanResult;

/* ---- byte-class DFA for the glob "k:*7?" -----------------------------------
 * Classes: 0 = any other byte, 1 = 'k', 2 = ':', 3 = '7'.
 * States: 0 start, 1 after "k", 2..5 inside the tail with code = state - 2 =
 * (last byte was '7') << 1 | (the byte before it was '7'), 6 dead.
 * Accepting: the byte before the last is '7' (code & 1), i.e. states 3 and 5.
 * This is the shape the proposed engine executes: one class-map load and one
 * transition load per key byte, tables resident in L1. */
enum { DFA_CLASSES = 4, DFA_STATES = 7, DFA_DEAD = 6 };
static uint8_t g_class[256];
static uint8_t g_next[DFA_STATES * DFA_CLASSES];
static uint8_t g_accept[DFA_STATES];

/**
 * Builds the "k:*7?" tables above.
 * @remarks Hand-built rather than compiled from the glob: the probe measures
 *          the executor's cost per byte, which does not depend on how the
 *          table was produced.
 */
static void dfa_build(void) {
  memset(g_class, 0, sizeof g_class);
  g_class['k'] = 1;
  g_class[':'] = 2;
  g_class['7'] = 3;
  for (int s = 0; s < DFA_STATES; s++)
    for (int c = 0; c < DFA_CLASSES; c++) g_next[s * DFA_CLASSES + c] = DFA_DEAD;
  g_next[0 * DFA_CLASSES + 1] = 1;              /* "k" */
  g_next[1 * DFA_CLASSES + 2] = 2;              /* "k:" -> tail, code 0 */
  for (int code = 0; code < 4; code++) {        /* tail: shift the '7' flags */
    for (int c = 0; c < DFA_CLASSES; c++) {
      int last7 = (c == 3);
      g_next[(2 + code) * DFA_CLASSES + c] = (uint8_t)(2 + ((last7 << 1) | (code >> 1)));
    }
  }
  memset(g_accept, 0, sizeof g_accept);
  g_accept[3] = g_accept[5] = 1;                /* code & 1: byte before last was '7' */
}

/**
 * Runs the DFA over a whole key.
 * @param key  key bytes
 * @param len  key length
 * @returns 1 if the key matches "k:*7?", else 0.
 * @remarks Stops at the dead state, as the real executor will: a key that
 *          cannot match costs only the bytes up to the first mismatch.
 */
static int dfa_match(const uint8_t *key, uint32_t len) {
  unsigned s = 0;
  for (uint32_t i = 0; i < len; i++) {
    s = g_next[s * DFA_CLASSES + g_class[key[i]]];
    if (s == DFA_DEAD) return 0;
  }
  return g_accept[s];
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

/**
 * Prints a message and exits non-zero.
 * @param what  the failed condition, for the log
 * @remarks Every check in this probe guards a reported number, so any
 *          failure aborts the whole run rather than printing partial data.
 */
static void die(const char *what) {
  fprintf(stderr, "export_probe: %s\n", what);
  exit(1);
}

/**
 * Formats key i as "k:" + 14 zero-padded digits (exactly KEY_LEN bytes).
 * @param i    key index (< 10^14; main caps --keys far below that)
 * @param out  KEY_LEN + 1 bytes (a NUL is written after the key)
 * @remarks The modulo is a no-op for every index main produces; it gives the
 *          compiler the 14-digit bound that proves the key never truncates.
 */
static void make_key(uint64_t i, char out[KEY_LEN + 1]) {
  snprintf(out, KEY_LEN + 1, "k:%014llu", (unsigned long long)(i % 100000000000000ull));
}

/**
 * Copies every live record pointer out of the bucket array — the whole
 * critical section of the naive consistent snapshot.
 * @param b    bucket array
 * @param nb   number of buckets
 * @param out  receives the pointers (room for every live record)
 * @returns the number of pointers written.
 */
static uint64_t collect(const Bucket *b, uint64_t nb, KvRec **out) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < nb; i++) {
    for (int s = 0; s < SLOTS; s++) {
      KvRec *r = b[i].slot[s];
      if (r) out[n++] = r;
    }
  }
  return n;
}

/**
 * Counts occupied slots from the tag words alone (no record is touched).
 * @param b   bucket array
 * @param nb  number of buckets
 * @returns occupied slots.
 */
static uint64_t occupancy(const Bucket *b, uint64_t nb) {
  uint64_t n = 0;
  for (uint64_t i = 0; i < nb; i++) {
    uint64_t t = b[i].tags;
    for (int s = 0; s < SLOTS; s++) n += ((t >> (8 * s)) & 0xFF) != 0;
  }
  return n;
}

/**
 * Gathers the record pointers of up to CHUNK buckets and prefetches each
 * record's header and key (and, when emitting, its value lines).
 * @param b      bucket array
 * @param nb     number of buckets
 * @param first  first bucket of the chunk
 * @param out    receives up to CHUNK * SLOTS pointers
 * @param lines  record lines to prefetch per record (2 = key, 3 = whole
 *               140-byte record at any 16-byte alignment)
 * @returns pointers gathered.
 */
static int gather(const Bucket *b, uint64_t nb, uint64_t first, KvRec **out, int lines) {
  int n = 0;
  uint64_t end = first + CHUNK < nb ? first + CHUNK : nb;
  for (uint64_t i = first; i < end; i++) {
    for (int s = 0; s < SLOTS; s++) {
      KvRec *r = b[i].slot[s];
      if (!r) continue;
      out[n++] = r;
      /* Header + key end at byte 40, which crosses a line boundary whenever
       * the record starts past offset 24 of its line; a third line covers a
       * 140-byte record at any alignment (the GET path's rule, cuckoo.asm). */
      __builtin_prefetch((const char *)r);
      __builtin_prefetch((const char *)r + 39);
      if (lines > 2) {
        __builtin_prefetch((const char *)r + 64);
        __builtin_prefetch((const char *)r + 128);
      }
    }
  }
  return n;
}

/**
 * Full scan in the exporter's intended pipeline: while chunk c is matched,
 * chunk c + 1's records are already on their way (software prefetch).
 * @param b     bucket array
 * @param nb    number of buckets
 * @param mode  what to do with each record (see ScanMode)
 * @param out   OUT_BATCH-byte output buffer (SCAN_EMIT)
 * @returns the counts, checked by the caller.
 */
static ScanResult scan(const Bucket *b, uint64_t nb, ScanMode mode, uint8_t *out) {
  static const uint8_t prefix[10] = {'k', ':', '0', '0', '0', '0', '0', '0', '0', '1'};
  KvRec *cur[CHUNK * SLOTS], *next[CHUNK * SLOTS];
  ScanResult res = {0, 0, 0};
  size_t pos = 0;
  int lines = mode == SCAN_EMIT ? 3 : 2;
  int n = gather(b, nb, 0, cur, lines);
  for (uint64_t first = 0; first < nb; first += CHUNK) {
    int m = first + CHUNK < nb ? gather(b, nb, first + CHUNK, next, lines) : 0;
    for (int k = 0; k < n; k++) {
      const KvRec *r = cur[k];
      int hit;
      res.seen++;
      switch (mode) {
        case SCAN_PREFIX:
          hit = r->key_len >= sizeof prefix && memcmp(r->data, prefix, sizeof prefix) == 0;
          break;
        case SCAN_DFA:
          hit = dfa_match(r->data, r->key_len);
          break;
        default:
          hit = 1;
          break;
      }
      if (!hit) continue;
      res.matched++;
      if (mode == SCAN_EMIT) {
        /* Entry{key = 1, value = 2} inside the batch's repeated field 2:
         * 12 <len> 0A <klen> key 12 <vlen> value (both lengths < 128 here). */
        size_t need = 6 + r->key_len + r->val_len;
        if (pos + need > OUT_BATCH) {
          res.bytes += pos;   /* the batch would be framed and sent here */
          pos = 0;
        }
        out[pos++] = 0x12;
        out[pos++] = (uint8_t)(4 + r->key_len + r->val_len);
        out[pos++] = 0x0A;
        out[pos++] = (uint8_t)r->key_len;
        memcpy(out + pos, r->data, r->key_len);
        pos += r->key_len;
        out[pos++] = 0x12;
        out[pos++] = (uint8_t)r->val_len;
        memcpy(out + pos, kv_rec_value(r), r->val_len);
        pos += r->val_len;
      }
    }
    memcpy(cur, next, (size_t)m * sizeof *cur);
    n = m;
  }
  res.bytes += pos;
  return res;
}

/**
 * Evicts the probe's data from the CPU caches by streaming through a buffer
 * larger than the last-level cache (36 MiB on the i9-13900K).
 * @param buf   scratch buffer
 * @param size  its size in bytes
 * @returns a value derived from the reads, so they cannot be optimized away.
 */
static uint64_t thrash(uint8_t *buf, size_t size) {
  uint64_t x = 0;
  for (size_t i = 0; i < size; i += 64) {
    buf[i]++;
    x += buf[i];
  }
  return x;
}

/** qsort comparator for uint64_t. */
static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/**
 * Prints one measurement: median, min and max of the repetitions, plus a
 * per-unit figure from the median.
 * @param name   measurement name
 * @param t      per-repetition times in ns (sorted in place)
 * @param reps   number of repetitions
 * @param units  units of work per repetition (keys or pages)
 * @param unit   unit name for the per-unit column
 */
static void report(const char *name, uint64_t *t, int reps, uint64_t units, const char *unit) {
  qsort(t, (size_t)reps, sizeof *t, cmp_u64);
  uint64_t med = t[reps / 2];
  printf("%-12s median %10.3f ms  (min %10.3f, max %10.3f)  %8.2f ns/%s\n", name, med / 1e6,
         t[0] / 1e6, t[reps - 1] / 1e6, (double)med / (double)units, unit);
}

/**
 * Parses the command line, fills a store, runs every measurement.
 * @param argc  argument count
 * @param argv  --keys N [--cpu C] [--reps R]
 * @returns 0 on success; exits 1 on any failed check, 2 on bad usage.
 */
int main(int argc, char **argv) {
  uint64_t nkeys = 0;
  int cpu = 4, reps = 5;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--keys")) nkeys = strtoull(argv[i + 1], NULL, 10);
    else if (!strcmp(argv[i], "--cpu")) cpu = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--reps")) reps = atoi(argv[i + 1]);
  }
  /* 10^9 keys would already need ~160 GB of records; the cap also keeps every
   * index inside make_key's 14 digits. */
  if (nkeys < 1000 || nkeys > 1000000000ull || reps < 1 || reps > MAX_REPS) {
    fprintf(stderr, "usage: export_probe --keys N(1000..1e9) [--cpu C] [--reps R(1..%d)]\n", MAX_REPS);
    return 2;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) die("sched_setaffinity");

  /* ---- fill the store exactly as the server would (hash, then put) ---- */
  static const uint8_t seed[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  KvTable *t = kv_store_create(nkeys, 1, seed, 0);
  if (!t) die("kv_store_create");
  uint8_t val[VAL_LEN];
  char key[KEY_LEN + 1];
  uint64_t expect_prefix = 0, expect_dfa = 0;
  for (uint64_t i = 0; i < nkeys; i++) {
    uint64_t h[2];
    make_key(i, key);
    for (int j = 0; j < VAL_LEN; j++) val[j] = (uint8_t)(i * 31 + (uint64_t)j);
    kv_store_hash(t, key, KEY_LEN, h);
    if (kv_store_put(t, key, KEY_LEN, val, VAL_LEN, h[0], h[1]) != 1) die("kv_store_put");
    expect_prefix += memcmp(key, "k:00000001", 10) == 0;
    expect_dfa += key[KEY_LEN - 2] == '7';
  }
  KvStats st;
  kv_store_stats(t, &st);
  if (st.count != nkeys) die("store count after fill");
  const TableHead *head = (const TableHead *)t;
  const Bucket *b = head->buckets;
  uint64_t nb = head->mask + 1;
  uint64_t table_bytes = nb * sizeof(Bucket);
  uint64_t pages = table_bytes / PAGE;
  printf("keys %llu  buckets %llu  table %.1f MiB  slot load %.1f%%  pages %llu  cpu %d  reps %d\n",
         (unsigned long long)nkeys, (unsigned long long)nb, table_bytes / 1048576.0,
         100.0 * (double)nkeys / (double)(nb * SLOTS), (unsigned long long)pages, cpu, reps);

  size_t thrash_size = 256u << 20;
  uint8_t *junk = malloc(thrash_size);
  KvRec **ptrs = malloc(nkeys * sizeof *ptrs);
  uint8_t *tcopy = mmap(NULL, table_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  uint8_t *out = malloc(OUT_BATCH);
  uint64_t *perm = malloc(pages * sizeof *perm);
  if (!junk || !ptrs || tcopy == MAP_FAILED || !out || !perm) die("allocation");
  memset(junk, 1, thrash_size);
  memset(ptrs, 0, nkeys * sizeof *ptrs);     /* prefault: collect-pf measures the scan alone */
  memset(tcopy, 0, table_bytes);
  uint64_t sink = 0;

  /* Random page order (xorshift-driven Fisher-Yates): consecutive copies hit
   * unrelated pages, as writers touching random keys would. */
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  for (uint64_t i = 0; i < pages; i++) perm[i] = i;
  for (uint64_t i = pages - 1; i > 0; i--) {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    uint64_t j = rng % (i + 1), tmp = perm[i];
    perm[i] = perm[j];
    perm[j] = tmp;
  }
  dfa_build();

  uint64_t tv[9][MAX_REPS];
  for (int r = 0; r < reps; r++) {
    uint64_t t0, n;

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    n = collect(b, nb, ptrs);
    tv[0][r] = now_ns() - t0;
    if (n != nkeys) die("collect-pf count");

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    KvRec **fresh = malloc(nkeys * sizeof *fresh);
    if (!fresh) die("collect-new allocation");
    n = collect(b, nb, fresh);
    tv[1][r] = now_ns() - t0;
    if (n != nkeys) die("collect-new count");
    sink += (uint64_t)(uintptr_t)fresh[n / 2];
    free(fresh);

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    memcpy(tcopy, b, table_bytes);
    tv[2][r] = now_ns() - t0;

    sink += thrash(junk, thrash_size);
    uint8_t page_buf[PAGE] __attribute__((aligned(64)));
    t0 = now_ns();
    for (uint64_t i = 0; i < pages; i++) {
      memcpy(page_buf, (const uint8_t *)b + perm[i] * PAGE, PAGE);
      sink += page_buf[(i * 64) & (PAGE - 1)];
    }
    tv[3][r] = now_ns() - t0;

    /* A snapshot copy area as the proposal would map it: NORESERVE, 4 KiB
     * pages (MADV_NOHUGEPAGE, so a first touch zeroes 4 KiB, not 2 MiB). */
    uint8_t *snap = mmap(NULL, table_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (snap == MAP_FAILED) die("snapshot mapping");
    madvise(snap, table_bytes, MADV_NOHUGEPAGE);
    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    for (uint64_t i = 0; i < pages; i++)
      memcpy(snap + perm[i] * PAGE, (const uint8_t *)b + perm[i] * PAGE, PAGE);
    tv[4][r] = now_ns() - t0;
    sink += snap[(pages / 2) * PAGE];
    munmap(snap, table_bytes);

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    n = occupancy(b, nb);
    tv[5][r] = now_ns() - t0;
    if (n != nkeys) die("occupancy count");

    ScanResult s;
    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    s = scan(b, nb, SCAN_PREFIX, out);
    tv[6][r] = now_ns() - t0;
    if (s.seen != nkeys || s.matched != expect_prefix) die("prefix counts");

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    s = scan(b, nb, SCAN_DFA, out);
    tv[7][r] = now_ns() - t0;
    if (s.seen != nkeys || s.matched != expect_dfa) die("dfa counts");

    sink += thrash(junk, thrash_size);
    t0 = now_ns();
    s = scan(b, nb, SCAN_EMIT, out);
    tv[8][r] = now_ns() - t0;
    if (s.seen != nkeys || s.matched != nkeys ||
        s.bytes != nkeys * (6 + KEY_LEN + VAL_LEN))
      die("emit counts");
    sink += out[s.bytes % OUT_BATCH];
  }

  report("collect-pf", tv[0], reps, nkeys, "key");
  report("collect-new", tv[1], reps, nkeys, "key");
  report("tablecopy", tv[2], reps, pages, "page");
  report("page-pf", tv[3], reps, pages, "page");
  report("page-fault", tv[4], reps, pages, "page");
  report("occupancy", tv[5], reps, nkeys, "key");
  report("prefix", tv[6], reps, nkeys, "key");
  report("dfa", tv[7], reps, nkeys, "key");
  report("emit", tv[8], reps, nkeys, "key");
  uint64_t emit_med = tv[8][reps / 2];
  printf("emit rate    %.0f MB/s of output, %.2f M keys/s; prefix matches %llu, dfa matches %llu\n",
         (double)(nkeys * (6 + KEY_LEN + VAL_LEN)) / ((double)emit_med / 1e9) / 1e6,
         (double)nkeys / ((double)emit_med / 1e9) / 1e6, (unsigned long long)expect_prefix,
         (unsigned long long)expect_dfa);
  fprintf(stderr, "(sink %llu)\n", (unsigned long long)sink);
  kv_store_destroy(t);
  return 0;
}
