/*
 * test_kvbench.c — unit tests for the parts every reported number depends on:
 *   - histogram percentiles stay within the documented 1/64 relative error
 *   - the Zipfian generator reproduces the expected popularity of rank 0
 *   - keys match redis-benchmark's 16-byte "key:%012d" format
 *   - RESP and memcached-meta parsers complete requests correctly when the
 *     response stream is split at every possible byte boundary, and flag
 *     misses, wrong values and server errors
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
 * Entry point.
 * @returns 0 when every check passed
 */
int main(void) {
  g_opt.value_size = 100;
  g_opt.depth = 16;
  g_value_template = malloc(g_opt.value_size);
  for (uint32_t i = 0; i < g_opt.value_size; i++) g_value_template[i] = (uint8_t)('a' + i % 26);
  proto_text_init();
  test_histogram();
  test_zipf();
  test_key_format();
  test_resp();
  test_memcache();
  if (g_failures) {
    fprintf(stderr, "test_kvbench: %d FAILED check(s)\n", g_failures);
    return 1;
  }
  printf("test_kvbench: all checks passed\n");
  return 0;
}
