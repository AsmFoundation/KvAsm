/*
 * test_snapshot.c — verifies kv_snapshot_load (snapshot.asm).
 *
 *  1. A valid snapshot (5000 records, key 4..64 bytes, value 0..300 bytes)
 *     loads completely; every value is exact; the return value is the count.
 *  2. Duplicate keys: the later record wins (same semantics as two Puts).
 *  3. Every corruption class is rejected, and the test proves WHICH check
 *     fired: the loader returns -EINVAL for all format violations, so a case
 *     could otherwise pass by tripping an earlier, unrelated check (flipping
 *     a record byte, for example, can also produce an oversized key length).
 *     Each case captures fd 2 and asserts the logged reason= text.
 *  4. A missing file returns -ENOENT; more records than the store's capacity
 *     return -ENOSPC.
 */
#include <fcntl.h>

#include "kvnode_abi.h"
#include "test_common.h"

int64_t kv_snapshot_load(KvTable *t, const char *path, const void *cfg);
extern const uint64_t kv_config_abi[];

#define SNAP_PATH "/tmp/kvnode_test_snapshot.kvs"

/* Opaque Config image; only max_key/max_value are read by the loader. Its
 * real size and field offsets come from kv_config_abi (abi.asm), so this test
 * never hard-codes the assembler's struct layout. */
static uint8_t g_cfg[256];

/**
 * Sets the loader's key/value limits inside the opaque Config image.
 * @param max_key    largest accepted key length in bytes
 * @param max_value  largest accepted value length in bytes
 */
static void set_limits(uint64_t max_key, uint64_t max_value) {
  memset(g_cfg, 0, sizeof g_cfg);
  CHECK(kv_config_abi[0] <= sizeof g_cfg);                 /* Config_size fits */
  CHECK(kv_config_abi[3] == 0xABCDEF0123456789ull);        /* table not short */
  memcpy(g_cfg + kv_config_abi[1], &max_key, 8);
  memcpy(g_cfg + kv_config_abi[2], &max_value, 8);
}

/** Growable byte buffer used to build snapshot images in memory. */
typedef struct {
  uint8_t *p;   /**< bytes (realloc-owned) */
  size_t len;   /**< bytes written */
  size_t cap;   /**< bytes allocated */
} Buf;

/**
 * Appends raw bytes, growing geometrically.
 * @param b    buffer
 * @param src  bytes to append
 * @param n    number of bytes
 */
static void put(Buf *b, const void *src, size_t n) {
  if (b->len + n > b->cap) {
    b->cap = (b->len + n) * 2;
    b->p = realloc(b->p, b->cap);
    CHECK(b->p != NULL);
  }
  memcpy(b->p + b->len, src, n);
  b->len += n;
}

/** Appends a little-endian u32 (x86-64 native order). @param b buffer @param v value */
static void put_u32(Buf *b, uint32_t v) { put(b, &v, 4); }

/** Appends a little-endian u64 (x86-64 native order). @param b buffer @param v value */
static void put_u64(Buf *b, uint64_t v) { put(b, &v, 8); }

/**
 * Appends the trailer: "KVNSEND1" + SipHash-2-4-128 (16 zero key bytes) low
 * word over every byte written so far — the same definition the C#
 * SnapshotWriter implements.
 * @param b  buffer holding header + records
 */
static void finish(Buf *b) {
  static const uint64_t zero[2] = {0, 0};
  uint64_t h[2];
  kv_siphash128(b->p, b->len, zero, h);
  put(b, "KVNSEND1", 8);
  put_u64(b, h[0]);
}

/**
 * Appends a 32-byte header with explicit fields, so cases can lie about any
 * of them.
 * @param b        buffer (normally empty)
 * @param count    declared record count
 * @param version  format version (the loader accepts only 1)
 * @param flags    flags word (the loader accepts only 0)
 */
static void header(Buf *b, uint64_t count, uint32_t version, uint32_t flags) {
  put(b, "KVNSNAP1", 8);
  put_u32(b, version);
  put_u32(b, flags);
  put_u64(b, count);
  put_u64(b, 0);
}

/**
 * Appends one record.
 * @param b     buffer
 * @param key   key bytes
 * @param klen  declared (and written) key length
 * @param val   value bytes
 * @param vlen  declared (and written) value length
 */
static void record(Buf *b, const void *key, uint32_t klen, const void *val, uint32_t vlen) {
  put_u32(b, klen);
  put_u32(b, vlen);
  put(b, key, klen);
  put(b, val, vlen);
}

/**
 * Writes an image to SNAP_PATH, replacing any previous content.
 * @param b  image
 */
static void write_file(const Buf *b) {
  FILE *f = fopen(SNAP_PATH, "wb");
  CHECK(f != NULL);
  CHECK(fwrite(b->p, 1, b->len, f) == b->len);
  CHECK(fclose(f) == 0);
}

/**
 * Loads a file into a fresh store.
 * @param path  snapshot path
 * @param cap   store capacity in keys
 * @param out   receives the table (caller destroys) or NULL to destroy here
 * @returns the loader's result (record count or negative error)
 */
static int64_t load_path(const char *path, uint64_t cap, KvTable **out) {
  uint8_t seed[16] = {1};
  KvTable *t = kv_store_create(cap, 0, seed, 0);
  CHECK(t != NULL);
  int64_t rc = kv_snapshot_load(t, path, g_cfg);
  if (out) *out = t;
  else kv_store_destroy(t);
  return rc;
}

/**
 * Loads a file expecting a failure and proves which check rejected it.
 * @param name      case label for diagnostics
 * @param path      snapshot path
 * @param cap       store capacity for the fresh table
 * @param expected  expected negative return value
 * @param reason    text that must appear in the logged rejection line
 * @remarks fd 2 is redirected to a temporary file for the duration of the
 *          load (the loader logs with a raw write(2) on fd 2, bypassing
 *          stdio) and restored before any CHECK can print.
 */
static void expect_reject_path(const char *name, const char *path, uint64_t cap,
                               int64_t expected, const char *reason) {
  fflush(stderr);
  int saved = dup(2);
  FILE *capture = tmpfile();
  CHECK(saved >= 0 && capture != NULL);
  dup2(fileno(capture), 2);
  int64_t rc = load_path(path, cap, NULL);
  dup2(saved, 2);
  close(saved);

  char log[4096] = {0};
  rewind(capture);
  size_t n = fread(log, 1, sizeof log - 1, capture);
  fclose(capture);
  int ok = rc == expected && strstr(log, reason) != NULL;
  if (!ok)
    fprintf(stderr, "  case %-16s rc=%lld (want %lld) log=%.*s\n", name, (long long)rc,
            (long long)expected, (int)n, log);
  CHECK(ok);
}

/**
 * Writes an image and runs expect_reject_path on it.
 * @param name      case label
 * @param b         image (freed here)
 * @param expected  expected negative return value
 * @param reason    expected reason text
 */
static void expect_reject(const char *name, Buf *b, int64_t expected, const char *reason) {
  write_file(b);
  expect_reject_path(name, SNAP_PATH, 100, expected, reason);
  free(b->p);
  *b = (Buf){0};
}

/** Entry point. @returns 0 when every check passed. */
int main(void) {
  set_limits(64, 300);
  uint64_t s = 0xFEED;

  /* ---- 1. valid snapshot ------------------------------------------------ */
  enum { N = 5000 };
  static uint8_t keys[N][64], vals[N][300];
  static uint32_t klen[N], vlen[N];
  Buf big = {0};
  header(&big, N, 1, 0);
  for (int i = 0; i < N; i++) {
    klen[i] = 4 + (uint32_t)(rng_next(&s) % 61);
    vlen[i] = (uint32_t)(rng_next(&s) % 301);
    memcpy(keys[i], &i, 4); /* unique 4-byte prefix: no accidental duplicates */
    rng_fill(&s, keys[i] + 4, klen[i] - 4);
    rng_fill(&s, vals[i], vlen[i]);
    record(&big, keys[i], klen[i], vals[i], vlen[i]);
  }
  finish(&big);
  write_file(&big);
  KvTable *t;
  CHECK(load_path(SNAP_PATH, N * 2, &t) == N);
  for (int i = 0; i < N; i++) {
    uint64_t h[2];
    kv_store_hash(t, keys[i], klen[i], h);
    const KvRec *r = kv_store_get(t, keys[i], klen[i], h[0], h[1]);
    CHECK(r && r->val_len == vlen[i] && memcmp(kv_rec_value(r), vals[i], vlen[i]) == 0);
  }
  KvStats st;
  kv_store_stats(t, &st);
  CHECK_EQ_U64(st.count, N);
  kv_store_destroy(t);

  /* ---- 2. duplicates: later wins ---------------------------------------- */
  Buf d = {0};
  header(&d, 2, 1, 0);
  record(&d, "dup", 3, "A", 1);
  record(&d, "dup", 3, "B", 1);
  finish(&d);
  write_file(&d);
  CHECK(load_path(SNAP_PATH, 10, &t) == 2);   /* records read, not keys */
  uint64_t h[2];
  kv_store_hash(t, "dup", 3, h);
  const KvRec *r = kv_store_get(t, "dup", 3, h[0], h[1]);
  CHECK(r && r->val_len == 1 && kv_rec_value(r)[0] == 'B');
  kv_store_stats(t, &st);
  CHECK_EQ_U64(st.count, 1);
  kv_store_destroy(t);
  free(d.p);

  /* ---- 3. corruption classes, each proven by its logged reason ---------- */
  static uint8_t filler[512];
  memset(filler, 'x', sizeof filler);
  Buf c = {0};

  put(&c, "KVNSNAPX", 8); put_u32(&c, 1); put_u32(&c, 0); put_u64(&c, 1); put_u64(&c, 0);
  record(&c, "k", 1, "v", 1); finish(&c);
  expect_reject("magic", &c, -EINVAL, "bad header magic");

  header(&c, 1, 2, 0); record(&c, "k", 1, "v", 1); finish(&c);
  expect_reject("version", &c, -EINVAL, "unsupported version or flags");

  header(&c, 1, 1, 1); record(&c, "k", 1, "v", 1); finish(&c);
  expect_reject("flags", &c, -EINVAL, "unsupported version or flags");

  header(&c, 1, 1, 0); record(&c, "k", 1, "v", 1); finish(&c);
  c.p[c.len - 16] = 'X';                      /* first trailer magic byte */
  expect_reject("trailer", &c, -EINVAL, "bad trailer magic");

  header(&c, 1, 1, 0); record(&c, "k", 1, "v", 1); finish(&c);
  c.p[c.len - 1] ^= 0x80;                     /* checksum word itself */
  expect_reject("checksum-word", &c, -EINVAL, "checksum mismatch");

  header(&c, 1, 1, 0); record(&c, "k", 1, "v", 1); finish(&c);
  c.p[41] ^= 1;                               /* the value byte: data corruption */
  expect_reject("checksum-data", &c, -EINVAL, "checksum mismatch");

  header(&c, 1, 1, 0); record(&c, "k", 1, "v", 1); finish(&c);
  c.p[33] ^= 1;                               /* key_len 1 -> 257: checksum must win */
  expect_reject("checksum-first", &c, -EINVAL, "checksum mismatch");

  header(&c, 2, 1, 0); record(&c, "k", 1, "v", 1); finish(&c);
  expect_reject("truncated-count", &c, -EINVAL, "record truncated");

  header(&c, 1, 1, 0); put_u32(&c, 1); put_u32(&c, 200); put(&c, "k", 1); put(&c, "vv", 2);
  finish(&c);                                 /* value_len points past the data */
  expect_reject("truncated-value", &c, -EINVAL, "record truncated");

  header(&c, 1, 1, 0); record(&c, "", 0, "v", 1); finish(&c);
  expect_reject("key-empty", &c, -EINVAL, "key length out of range");

  header(&c, 1, 1, 0); record(&c, filler, 65, "", 0); finish(&c);
  expect_reject("key-too-long", &c, -EINVAL, "key length out of range");

  header(&c, 1, 1, 0); record(&c, "k", 1, filler, 301); finish(&c);
  expect_reject("value-too-long", &c, -EINVAL, "value too large");

  header(&c, 1, 1, 0); record(&c, "k", 1, "v", 1); put(&c, "xx", 2); finish(&c);
  expect_reject("extra-bytes", &c, -EINVAL, "unexpected bytes after the last record");

  put(&c, "KVNSNAP1", 8);
  expect_reject("too-small", &c, -EINVAL, "file too small");

  expect_reject_path("missing", "/nonexistent/snapshot.kvs", 10, -ENOENT,
                     "cannot open or map file");

  /* ---- 4. more records than capacity ------------------------------------ */
  write_file(&big);
  expect_reject_path("capacity", SNAP_PATH, 100, -ENOSPC,
                     "store rejected record (capacity or memory limit)");

  free(big.p);
  unlink(SNAP_PATH);
  return test_finish("test_snapshot");
}
