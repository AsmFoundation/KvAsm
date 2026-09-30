/*
 * test_kvbench.c — unit tests for the parts every reported number depends on:
 *   - histogram percentiles stay within the documented 1/64 relative error
 *   - the Zipfian generator reproduces the expected popularity of rank 0
 *   - keys match redis-benchmark's 16-byte "key:%012d" format
 *   - RESP and memcached-meta parsers complete requests correctly when the
 *     response stream is split at every possible byte boundary, and flag
 *     misses, wrong values and server errors
 *   - CRC-32C matches the standard check values on both paths (SSE4.2 and
 *     table), and its O(1) combination (value_crc) equals a full pass
 *   - the Upload stream generator emits exactly the documented messages
 *     (checked by an independent decoder) whatever the piece sizes asked
 *   - the Download verifier accepts a correct body split at every byte and
 *     classifies each broken rule of the DownloadResponse contract
 * Links the real engine/protocol code; main.c is replaced by the globals here.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "engine.h"

Options g_opt;
_Atomic int g_phase = PHASE_WARMUP;
uint8_t *g_value_template;
Zipf g_zipf;

static int g_failures;

/** Records a failure without aborting, so one run reports every broken case. */
#define CHECK(expr)                                                                  \
  do {                                                                               \
    if (!(expr)) {                                                                   \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                \
      g_failures++;                                                                  \
    }                                                                                \
  } while (0)

/** Percentiles of 1..100000 ns are within 1/64 of the exact nearest-rank value. */
static void test_histogram(void) {
  static Hist h;
  for (uint64_t v = 1; v <= 100000; v++) hist_record(&h, v);
  const double pcts[] = {50, 90, 99, 99.9};
  for (size_t i = 0; i < sizeof pcts / sizeof pcts[0]; i++) {
    double exact = pcts[i] / 100.0 * 100000.0;
    double got = (double)hist_percentile(&h, pcts[i]);
    CHECK(fabs(got - exact) / exact <= 1.0 / 64.0);
  }
  CHECK(hist_percentile(&h, 100) == 100000);
  CHECK(h.total == 100000);
  /* every value maps into its own bucket's range */
  for (uint64_t v = 1; v < (1ull << 40); v = v * 3 + 1) {
    uint64_t mid = hist_bucket_value(hist_bucket(v));
    CHECK(fabs((double)mid - (double)v) / (double)v <= 1.0 / 64.0);
  }
  static Hist a, b;
  hist_record(&a, 10);
  hist_record(&b, 1000000);
  hist_merge(&a, &b);
  CHECK(a.total == 2 && a.max == 1000000);
}

/** Rank 0 of Zipf(n, theta) has probability 1 / zeta(n, theta). */
static void test_zipf(void) {
  Zipf z;
  zipf_init(&z, 1000, 0.99);
  uint64_t rng = 12345, zero = 0, n = 2000000;
  for (uint64_t i = 0; i < n; i++) {
    uint64_t r = zipf_next(&z, &rng);
    CHECK(r < 1000);
    zero += r == 0;
  }
  double expected = 1.0 / z.zetan, got = (double)zero / (double)n;
  CHECK(fabs(got - expected) / expected < 0.03);
  for (uint64_t r = 0; r < 1000; r++) CHECK(scramble(r, 1000) < 1000);
}

/** Keys are byte-identical to redis-benchmark's __rand_int__ keys. */
static void test_key_format(void) {
  char k[KEY_BYTES + 1] = {0};
  key_format(k, 42);
  CHECK(strcmp(k, "key:000000000042") == 0);
  key_format(k, 999999999999ull);
  CHECK(strcmp(k, "key:999999999999") == 0);
}

/** A fake connection with enough state for the text parsers. */
typedef struct {
  Worker w;
  Conn c;
} Fake;

/**
 * Prepares a fake connection with n pending requests.
 * @param f    fake connection
 * @param ops  operations in send order
 * @param keys key ids in send order
 * @param n    count
 */
static void fake_init(Fake *f, const int *ops, const uint64_t *keys, int n) {
  memset(f, 0, sizeof *f);
  f->c.w = &f->w;
  f->c.fifo = calloc(16, sizeof(Pending));
  f->c.fifo_mask = 15;
  f->c.in_cap = 1 << 20;
  f->c.in = malloc(f->c.in_cap);
  for (int i = 0; i < n; i++) {
    fifo_push(&f->c, ops[i], keys[i], 0);
    f->c.inflight++;
  }
}

/**
 * Feeds a response stream in chunks of `step` bytes (the last may be short).
 * @param p      protocol
 * @param f      fake connection
 * @param data   full response stream
 * @param len    stream length
 * @param step   chunk size
 */
static void feed(const Proto *p, Fake *f, const uint8_t *data, size_t len, size_t step) {
  for (size_t off = 0; off < len; off += step) {
    size_t n = len - off < step ? len - off : step;
    memcpy(f->c.in + f->c.in_len, data + off, n);
    p->input(&f->c, NULL, n, 1);
  }
}

/**
 * Builds the value bytes the server would return for a key.
 * @param dst  value_size bytes
 * @param key  key id
 * @param bad  1 to corrupt the last byte
 */
static void value_for(uint8_t *dst, uint64_t key, int bad) {
  value_fill(dst, key);
  if (bad) dst[g_opt.value_size - 1] ^= 1;
}

/**
 * Runs one parser case at every chunk size from 1 byte to the whole stream.
 * @param p          protocol
 * @param stream     response bytes
 * @param len        length
 * @param ops        pending operations
 * @param keys       pending keys
 * @param n          pending count
 * @param misses     expected misses
 * @param bad        expected wrong values
 * @param errors     expected errors
 */
static void parse_case(const Proto *p, const uint8_t *stream, size_t len, const int *ops,
                       const uint64_t *keys, int n, uint64_t misses, uint64_t bad,
                       uint64_t errors) {
  for (size_t step = 1; step <= len; step++) {
    Fake f;
    fake_init(&f, ops, keys, n);
    feed(p, &f, stream, len, step);
    CHECK(f.w.done_all == (uint64_t)n);
    CHECK(f.w.misses == misses);
    CHECK(f.w.bad_values == bad);
    CHECK(f.w.errors == errors);
    CHECK(f.c.inflight == 0 && f.c.in_len == 0);
    free(f.c.fifo);
    free(f.c.in);
  }
}

/** RESP: hit, miss, OK, PONG, error, and a value belonging to another key. */
static void test_resp(void) {
  uint8_t v7[100], v9bad[100];
  value_for(v7, 7, 0);
  value_for(v9bad, 9, 1);
  uint8_t s[512];
  size_t n = 0;
  n += (size_t)sprintf((char *)s + n, "$100\r\n");
  memcpy(s + n, v7, 100), n += 100;
  n += (size_t)sprintf((char *)s + n, "\r\n$-1\r\n+OK\r\n+PONG\r\n$100\r\n");
  memcpy(s + n, v9bad, 100), n += 100;
  n += (size_t)sprintf((char *)s + n, "\r\n-ERR out of memory\r\n");
  const int ops[] = {OP_GET, OP_GET, OP_SET, OP_PING, OP_GET, OP_SET};
  const uint64_t keys[] = {7, 8, 9, 0, 9, 10};
  parse_case(&proto_resp, s, n, ops, keys, 6, 1, 1, 1);
}

/** memcached meta: VA hit, EN miss, HD stored, MN no-op, wrong value, error. */
static void test_memcache(void) {
  uint8_t v3[100], v4bad[100];
  value_for(v3, 3, 0);
  value_for(v4bad, 4, 1);
  uint8_t s[512];
  size_t n = 0;
  n += (size_t)sprintf((char *)s + n, "VA 100\r\n");
  memcpy(s + n, v3, 100), n += 100;
  n += (size_t)sprintf((char *)s + n, "\r\nEN\r\nHD\r\nMN\r\nVA 100\r\n");
  memcpy(s + n, v4bad, 100), n += 100;
  n += (size_t)sprintf((char *)s + n, "\r\nSERVER_ERROR out of memory\r\n");
  const int ops[] = {OP_GET, OP_GET, OP_SET, OP_PING, OP_GET, OP_SET};
  const uint64_t keys[] = {3, 5, 6, 0, 4, 11};
  parse_case(&proto_memcache, s, n, ops, keys, 6, 1, 1, 1);
}

/**
 * Switches the run's value size: a fresh template (the same pattern as
 * main.c) and value_crc's precomputation.
 * @param size  value bytes (>= 8)
 */
static void set_value_size(uint32_t size) {
  free(g_value_template);
  g_opt.value_size = size;
  g_value_template = malloc(size);
  for (uint32_t i = 0; i < size; i++) g_value_template[i] = (uint8_t)('a' + i % 26);
  value_crc_init();
}

/**
 * CRC-32C: check values on both paths, continuation, combination at every
 * split of a buffer, and value_crc against a full pass for several sizes.
 */
static void test_crc32c(void) {
  for (int table = 0; table <= 1; table++) {
    crc32c_force_table = table;
    CHECK(crc32c_update(0, (const uint8_t *)"123456789", 9) == 0xE3069283u);
    CHECK(crc32c_update(0, (const uint8_t *)"abc", 3) == 0x364B3FB7u);
    CHECK(crc32c_update(0, NULL, 0) == 0);
  }
  crc32c_force_table = 0;
  uint8_t buf[300];
  uint64_t rng = 99;
  for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)rng_next(&rng);
  uint32_t whole = crc32c_update(0, buf, sizeof buf);
  crc32c_force_table = 1;
  CHECK(crc32c_update(0, buf, sizeof buf) == whole); /* paths agree on odd tails too */
  crc32c_force_table = 0;
  for (size_t k = 0; k <= sizeof buf; k++) {
    uint32_t a = crc32c_update(0, buf, k), b = crc32c_update(0, buf + k, sizeof buf - k);
    CHECK(crc32c_update(a, buf + k, sizeof buf - k) == whole);
    CHECK(crc32c_combine_op(a, b, crc32c_shift_op(sizeof buf - k)) == whole);
  }
  const uint32_t sizes[] = {8, 9, 100, 4096, 70001};
  for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
    set_value_size(sizes[i]);
    uint8_t *v = malloc(sizes[i]);
    const uint64_t keys[] = {0, 1, 42, 0x0123456789ABCDEFull, 999999999999ull};
    for (size_t j = 0; j < sizeof keys / sizeof keys[0]; j++) {
      value_fill(v, keys[j]);
      CHECK(value_crc(keys[j]) == crc32c_update(0, v, sizes[i]));
    }
    free(v);
  }
  set_value_size(100);
}

/**
 * Reads a protobuf varint for the reference decoder.
 * @param p    cursor, advanced
 * @param end  one past the last byte
 * @param out  receives the value
 * @returns 1, or 0 when truncated
 */
static int ref_varint(const uint8_t **p, const uint8_t *end, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64 && *p < end; shift += 7) {
    uint8_t b = *(*p)++;
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return 1;
    }
  }
  return 0;
}

/**
 * Decodes an Upload request stream independently of grpc.c and checks every
 * rule of UploadRequest plus the generator's own promises (chunk sizes, the
 * crc32c in the last message).
 * @param s      stream bytes
 * @param n      length
 * @param key    key id it was generated for
 * @param chunk  data bytes per message
 */
static void check_upload_stream(const uint8_t *s, size_t n, uint64_t key, uint32_t chunk) {
  const uint8_t *p = s, *end = s + n;
  uint8_t *data = malloc(g_opt.value_size + 1);
  size_t got = 0;
  uint64_t total = UINT64_MAX, crc = UINT64_MAX;
  int msgs = 0, key_ok = 0;
  char want_key[KEY_BYTES];
  key_format(want_key, key);
  while (p < end) {
    CHECK(end - p >= 5 && p[0] == 0);
    if (end - p < 5) break;
    uint32_t mlen = (uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4];
    p += 5;
    CHECK((size_t)(end - p) >= mlen);
    const uint8_t *mend = p + mlen;
    CHECK(crc == UINT64_MAX); /* nothing follows the message carrying crc32c */
    size_t msg_data = 0;
    while (p < mend) {
      uint64_t tag = 0, v = 0; /* a failed read leaves them 0: "unexpected field" */
      CHECK(ref_varint(&p, mend, &tag));
      if (tag == 0x0A || tag == 0x1A) {
        CHECK(ref_varint(&p, mend, &v) && v <= (uint64_t)(mend - p));
        if (tag == 0x0A) {
          CHECK(msgs == 0 && v == KEY_BYTES && memcmp(p, want_key, KEY_BYTES) == 0);
          key_ok = msgs == 0;
        } else {
          CHECK(got + v <= g_opt.value_size);
          if (got + v <= g_opt.value_size) memcpy(data + got, p, (size_t)v);
          got += (size_t)v;
          msg_data += (size_t)v;
        }
        p += v;
      } else if (tag == 0x10) {
        CHECK(msgs == 0 && ref_varint(&p, mend, &total));
      } else if (tag == 0x25) {
        CHECK(mend - p >= 4);
        uint32_t c;
        memcpy(&c, p, 4);
        crc = c;
        p += 4;
      } else {
        CHECK(!"unexpected field in UploadRequest");
        p = mend;
      }
    }
    /* Every message but the last carries exactly one chunk. */
    if (crc == UINT64_MAX) CHECK(msg_data == chunk);
    msgs++;
  }
  CHECK(key_ok && total == g_opt.value_size && got == g_opt.value_size);
  uint8_t *want = malloc(g_opt.value_size);
  value_fill(want, key);
  CHECK(got == g_opt.value_size && memcmp(data, want, g_opt.value_size) == 0);
  CHECK(crc == crc32c_update(0, want, g_opt.value_size));
  CHECK(msgs == (int)((g_opt.value_size + chunk - 1) / chunk));
  free(want);
  free(data);
}

/**
 * Upload generator: one-shot and random-piece generation give the same
 * bytes, the length is upload_stream_len(), and the stream decodes to
 * exactly the value, for chunk sizes around the value size.
 */
static void test_upload_stream(void) {
  const struct {
    uint32_t size, chunk;
  } cases[] = {{8, 1},   {8, 65536}, {100, 7},     {100, 99},     {100, 100},
               {100, 101}, {127, 128}, {70000, 65536}, {131072, 65536}, {300, 16}};
  uint64_t rng = 7;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    set_value_size(cases[i].size);
    g_opt.chunk = cases[i].chunk;
    uint64_t len = upload_stream_len();
    uint8_t *a = malloc(len + 64), *b = malloc(len + 64);
    const uint64_t key = 1000 + i;
    UpGen u;
    up_start(&u, key);
    CHECK(up_emit(&u, a, len + 64) == len); /* exactly len, then the end */
    CHECK(up_emit(&u, a, 1) == 0);
    up_start(&u, key);
    size_t off = 0;
    while (off < len) {
      size_t want = 1 + rng_next(&rng) % 37;
      size_t n = up_emit(&u, b + off, want);
      CHECK(n == want || off + n == len);
      if (n == 0) break;
      off += n;
    }
    CHECK(off == len && memcmp(a, b, len) == 0);
    check_upload_stream(a, len, key, cases[i].chunk);
    free(a);
    free(b);
  }
  g_opt.chunk = 0;
  set_value_size(100);
}

/** Growable byte buffer for building Download bodies in the tests. */
typedef struct {
  uint8_t b[8192];
  size_t n, msg; /**< length; start of the open message */
} Enc;

/** Opens a gRPC message (prefix filled by enc_end). */
static void enc_begin(Enc *e) {
  e->msg = e->n;
  e->n += 5;
}

/**
 * Closes the open message.
 * @param e     buffer
 * @param flag  the prefix's compressed flag (0 is correct)
 */
static void enc_end(Enc *e, uint8_t flag) {
  uint32_t mlen = (uint32_t)(e->n - e->msg - 5);
  e->b[e->msg] = flag;
  e->b[e->msg + 1] = (uint8_t)(mlen >> 24);
  e->b[e->msg + 2] = (uint8_t)(mlen >> 16);
  e->b[e->msg + 3] = (uint8_t)(mlen >> 8);
  e->b[e->msg + 4] = (uint8_t)mlen;
}

/** Appends a varint. */
static void enc_varint(Enc *e, uint64_t v) {
  while (v >= 0x80) {
    e->b[e->n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  e->b[e->n++] = (uint8_t)v;
}

/** Appends raw bytes. */
static void enc_raw(Enc *e, const void *p, size_t n) {
  memcpy(e->b + e->n, p, n);
  e->n += n;
}

/** Appends a fixed32 field (tag, then 4 bytes little-endian). */
static void enc_fixed32(Enc *e, uint8_t tag, uint32_t v) {
  e->b[e->n++] = tag;
  enc_raw(e, &v, 4);
}

/* Ways to break a Download body, one per verifier rule. */
enum {
  BRK_NONE, BRK_DATA_BYTE, BRK_CRC, BRK_TOTAL, BRK_TRUNCATE, BRK_NO_CRC, BRK_AFTER_CRC,
  BRK_COMPRESSED, BRK_TOTAL_LATE, BRK_FIELD_OVERRUN, BRK_UNKNOWN_FIELD, BRK_CRC_NOT_LAST,
  BRK_SHORT, BRK_NO_TOTAL, BRK_EMPTY_MESSAGE
};

/**
 * Builds the Download body a server sends for the whole value of key in
 * chunks of `chunk` bytes, broken in one way.
 * @param e      receives the body
 * @param key    key id
 * @param chunk  data bytes per message
 * @param brk    BRK_*
 */
static void build_download(Enc *e, uint64_t key, uint32_t chunk, int brk) {
  uint8_t v[256];
  uint32_t n = g_opt.value_size;
  value_fill(v, key);
  if (brk == BRK_DATA_BYTE) v[n / 2] ^= 0x20;
  uint32_t crc = crc32c_update(0, v, n);
  if (brk == BRK_CRC) crc ^= 1;
  uint32_t sent = brk == BRK_SHORT ? n - 1 : n; /* SHORT: one byte missing, crc of what was sent */
  if (brk == BRK_SHORT) crc = crc32c_update(0, v, sent);
  memset(e, 0, sizeof *e);
  uint32_t msgs = (sent + chunk - 1) / chunk;
  for (uint32_t m = 0; m < msgs; m++) {
    enc_begin(e);
    if (m == 0 && brk != BRK_TOTAL_LATE && brk != BRK_NO_TOTAL) {
      e->b[e->n++] = 0x08;
      enc_varint(e, brk == BRK_TOTAL ? n + 1 : n);
    }
    if (m == 1 && brk == BRK_TOTAL_LATE) {
      e->b[e->n++] = 0x08;
      enc_varint(e, n);
    }
    if (m == 0 && brk == BRK_UNKNOWN_FIELD) {
      e->b[e->n++] = 0x20;
      enc_varint(e, 1);
    }
    uint32_t off = m * chunk, len = sent - off < chunk ? sent - off : chunk;
    int last = m == msgs - 1;
    if (last && brk == BRK_CRC_NOT_LAST) {
      /* crc32c before the last data field of the stream */
      enc_fixed32(e, 0x1D, crc);
      e->b[e->n++] = 0x12;
      enc_varint(e, len);
      enc_raw(e, v + off, len);
      enc_end(e, 0);
      continue;
    }
    e->b[e->n++] = 0x12;
    if (m == 0 && brk == BRK_FIELD_OVERRUN) {
      enc_varint(e, len + 40); /* claims more than the message holds */
      enc_raw(e, v + off, len);
    } else {
      enc_varint(e, len);
      enc_raw(e, v + off, len);
    }
    if (last && brk != BRK_NO_CRC) enc_fixed32(e, 0x1D, crc);
    enc_end(e, m == 0 && brk == BRK_COMPRESSED ? 1 : 0);
  }
  if (brk == BRK_AFTER_CRC) {
    enc_begin(e);
    e->b[e->n++] = 0x12;
    enc_varint(e, 0);
    enc_end(e, 0);
  }
  if (brk == BRK_EMPTY_MESSAGE) {
    enc_begin(e);
    enc_end(e, 0);
  }
  if (brk == BRK_TRUNCATE) e->n--;
}

/**
 * Feeds a body to a fresh parser in pieces of `step` bytes and judges it.
 * @param e     body
 * @param key   key id the parser expects
 * @param step  piece size
 * @returns dl_judge's verdict
 */
static int run_download(const Enc *e, uint64_t key, size_t step) {
  DlParse d;
  dl_start(&d, key);
  for (size_t off = 0; off < e->n; off += step) dl_feed(&d, e->b + off, e->n - off < step ? e->n - off : step);
  return dl_judge(&d);
}

/**
 * Download verifier: a correct body at every split, then each broken rule
 * at several splits, with the verdict the run's validity depends on.
 */
static void test_download_parser(void) {
  static Enc e;
  const uint32_t chunks[] = {7, 50, 99, 100, 1000};
  for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
    build_download(&e, 7, chunks[i], BRK_NONE);
    for (size_t step = 1; step <= e.n; step++) CHECK(run_download(&e, 7, step) == ST_OK);
  }
  const struct {
    int brk, want;
  } cases[] = {{BRK_DATA_BYTE, ST_BAD_VALUE},   {BRK_CRC, ST_BAD_VALUE},
               {BRK_TOTAL, ST_BAD_VALUE},       {BRK_SHORT, ST_BAD_VALUE},
               {BRK_TRUNCATE, ST_ERROR},        {BRK_NO_CRC, ST_ERROR},
               {BRK_AFTER_CRC, ST_ERROR},       {BRK_COMPRESSED, ST_ERROR},
               {BRK_TOTAL_LATE, ST_ERROR},      {BRK_FIELD_OVERRUN, ST_ERROR},
               {BRK_UNKNOWN_FIELD, ST_ERROR},   {BRK_CRC_NOT_LAST, ST_ERROR},
               {BRK_NO_TOTAL, ST_ERROR},        {BRK_EMPTY_MESSAGE, ST_ERROR}};
  const size_t steps[] = {1, 2, 3, 5, 64, 4096};
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    build_download(&e, 7, 30, cases[i].brk);
    for (size_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
      int got = run_download(&e, 7, steps[s]);
      if (got != cases[i].want)
        fprintf(stderr, "download case %d step %zu: got %d, want %d\n", cases[i].brk, steps[s], got,
                cases[i].want);
      CHECK(got == cases[i].want);
    }
  }
  /* Another key's (correct) value: its id bytes and checksum differ. */
  build_download(&e, 8, 30, BRK_NONE);
  CHECK(run_download(&e, 7, 13) == ST_BAD_VALUE);
}

/**
 * Entry point.
 * @returns 0 when every check passed
 */
int main(void) {
  g_opt.value_size = 100;
  g_opt.depth = 16;
  g_value_template = malloc(g_opt.value_size);
  for (uint32_t i = 0; i < g_opt.value_size; i++) g_value_template[i] = (uint8_t)('a' + i % 26);
  proto_text_init();
  value_crc_init();
  test_histogram();
  test_zipf();
  test_key_format();
  test_resp();
  test_memcache();
  test_crc32c();
  test_upload_stream();
  test_download_parser();
  if (g_failures) {
    fprintf(stderr, "test_kvbench: %d FAILED check(s)\n", g_failures);
    return 1;
  }
  printf("test_kvbench: all checks passed\n");
  return 0;
}
