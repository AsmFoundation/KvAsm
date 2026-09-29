/*
 * test_crc32c.c — verifies crc32c.asm (CRC-32C for Upload/Download).
 *
 *  1. Known answers: the standard check value (CRC-32C("123456789") =
 *     0xE3069283) and the four 32-byte vectors of RFC 3720 §B.4.
 *  2. Differential: every implementation (table/slicing-by-8, SSE4.2 when
 *     the CPU has it, both fused-copy variants, the dispatching entry points)
 *     against an independent bitwise reference, for every length 0..600 at
 *     every start alignment 0..15, plus large odd-sized buffers.
 *  3. Chaining: crc(a || b) == crc(crc(a), b) at random split points — the
 *     property the upload path relies on (it checksums chunk by chunk).
 *  4. The fused copies write exactly dst[0..len) and nothing around it.
 *  5. No overreads: buffers ending at a PROT_NONE guard page.
 *  6. Mode API: unknown modes refused, SSE4.2 refused without CPU support.
 *  7. Measurement (printed, not asserted: the host is shared and noisy):
 *     MB/s of each implementation next to glibc memcpy, in cache and in DRAM.
 *     This is the evidence behind the dispatch — see crc32c.asm.
 */
#include "kvnode_abi.h"
#include "test_common.h"

uint32_t kv_crc32c(uint32_t crc, const void *buf, size_t len);
uint32_t kv_crc32c_copy(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_sw(uint32_t crc, const void *buf, size_t len);
uint32_t kv_crc32c_hw(uint32_t crc, const void *buf, size_t len);
uint32_t kv_crc32c_copy_sw(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_hw(uint32_t crc, void *dst, const void *src, size_t len);
int kv_crc32c_hw_available(void);
int kv_crc32c_set_mode(int mode);

enum { MODE_TABLE = 0, MODE_SSE42 = 1 };

static int g_hw; /* the CPU has SSE4.2: the hw variants may be called */

/**
 * Bitwise reference CRC-32C (reflected 0x82F63B78, preset and final
 * inversion), written independently of the assembly's table method.
 * @param crc  previous result (0 for a new checksum)
 * @param p    bytes
 * @param n    byte count
 * @returns the continued CRC-32C
 */
static uint32_t ref_crc(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
  }
  return ~crc;
}

/**
 * Checks every implementation on one buffer against the reference.
 * @param crc0  value to continue from
 * @param p     input bytes
 * @param n     byte count
 * @remarks The fused copies go into a destination surrounded by canary
 *          bytes, so a store outside [dst, dst + n) is caught.
 */
static void check_all(uint32_t crc0, const uint8_t *p, size_t n) {
  static uint8_t dst_buf[2 * 65536 + 64];
  uint32_t want = ref_crc(crc0, p, n);
  CHECK_EQ_U64(kv_crc32c_sw(crc0, p, n), want);
  CHECK_EQ_U64(kv_crc32c(crc0, p, n), want);
  if (g_hw) CHECK_EQ_U64(kv_crc32c_hw(crc0, p, n), want);
  if (n > 2 * 65536) return; /* copies only up to the scratch size */
  for (int variant = 0; variant < 3; variant++) {
    if (variant == 2 && !g_hw) break;
    uint8_t *dst = dst_buf + 32;
    memset(dst_buf, 0xA5, n + 64);
    uint32_t got = variant == 0 ? kv_crc32c_copy_sw(crc0, dst, p, n)
                   : variant == 1 ? kv_crc32c_copy(crc0, dst, p, n)
                                  : kv_crc32c_copy_hw(crc0, dst, p, n);
    CHECK_EQ_U64(got, want);
    CHECK(memcmp(dst, p, n) == 0);
    for (int g = 0; g < 32; g++) {
      CHECK(dst_buf[g] == 0xA5);          /* nothing written before dst */
      CHECK(dst[n + (size_t)g] == 0xA5);  /* nor after dst + n */
    }
  }
}

/** 1. Known answers. */
static void test_vectors(void) {
  CHECK_EQ_U64(ref_crc(0, (const uint8_t *)"123456789", 9), 0xE3069283u);
  uint8_t v[32];
  memset(v, 0, 32);
  CHECK_EQ_U64(ref_crc(0, v, 32), 0x8A9136AAu); /* RFC 3720 B.4: 32 bytes of zeros */
  check_all(0, v, 32);
  memset(v, 0xFF, 32);
  CHECK_EQ_U64(ref_crc(0, v, 32), 0x62A8AB43u); /* 32 bytes of ones */
  check_all(0, v, 32);
  for (int i = 0; i < 32; i++) v[i] = (uint8_t)i;
  CHECK_EQ_U64(ref_crc(0, v, 32), 0x46DD794Eu); /* incrementing 00..1f */
  check_all(0, v, 32);
  for (int i = 0; i < 32; i++) v[i] = (uint8_t)(31 - i);
  CHECK_EQ_U64(ref_crc(0, v, 32), 0x113FDB5Cu); /* decrementing 1f..00 */
  check_all(0, v, 32);
  check_all(0, (const uint8_t *)"123456789", 9);
  check_all(0, NULL, 0);                        /* empty: CRC-32C of nothing is 0 */
  CHECK_EQ_U64(kv_crc32c(0, NULL, 0), 0);
  CHECK_EQ_U64(kv_crc32c(0x12345678u, NULL, 0), 0x12345678u); /* continuing with nothing */
}

/** 2. Differential over lengths, alignments and large buffers. */
static void test_differential(void) {
  static uint8_t buf[600 + 16];
  uint64_t s = 0xC3C3;
  for (size_t len = 0; len <= 600; len++) {
    for (size_t align = 0; align < 16; align++) {
      rng_fill(&s, buf + align, len);
      check_all((uint32_t)rng_next(&s), buf + align, len);
    }
  }
  size_t big = (3u << 20) + 13;                 /* 3 MiB + 13: not a multiple of 8 */
  uint8_t *p = malloc(big);
  CHECK(p != NULL);
  rng_fill(&s, p, big);
  check_all(0, p, big);
  check_all(0, p + 3, 131071);                  /* odd start, odd length, fits the copy scratch */
  free(p);
}

/** 3. Chaining at random split points. */
static void test_chaining(void) {
  static uint8_t buf[4096];
  uint64_t s = 0xC4A1;
  for (int i = 0; i < 20000; i++) {
    size_t n = rng_next(&s) % sizeof buf;
    rng_fill(&s, buf, n);
    size_t cut = n ? rng_next(&s) % (n + 1) : 0;
    uint32_t whole = kv_crc32c_sw(0, buf, n);
    CHECK_EQ_U64(kv_crc32c_sw(kv_crc32c_sw(0, buf, cut), buf + cut, n - cut), whole);
    if (g_hw) CHECK_EQ_U64(kv_crc32c_hw(kv_crc32c_hw(0, buf, cut), buf + cut, n - cut), whole);
    uint8_t out[4096];
    uint32_t a = kv_crc32c_copy(0, out, buf, cut);
    CHECK_EQ_U64(kv_crc32c_copy(a, out + cut, buf + cut, n - cut), whole);
    CHECK(memcmp(out, buf, n) == 0);
  }
}

/** 5. No reads past the end: every length up to a page ends at a guard page. */
static void test_guard(void) {
  long page = sysconf(_SC_PAGESIZE);
  uint64_t s = 0x6A6A;
  static uint8_t out[8192];
  for (long len = 0; len <= page && len <= 4096; len += (len < 64 ? 1 : 61)) {
    void *base;
    uint8_t *p = guard_alloc((size_t)len, &base);
    rng_fill(&s, p, (size_t)len);
    uint32_t want = ref_crc(0, p, (size_t)len);
    CHECK_EQ_U64(kv_crc32c_sw(0, p, (size_t)len), want);
    CHECK_EQ_U64(kv_crc32c_copy_sw(0, out, p, (size_t)len), want);
    if (g_hw) {
      CHECK_EQ_U64(kv_crc32c_hw(0, p, (size_t)len), want);
      CHECK_EQ_U64(kv_crc32c_copy_hw(0, out, p, (size_t)len), want);
    }
    guard_free(base);
  }
}

/** 6. Mode API. */
static void test_modes(void) {
  CHECK(kv_crc32c_set_mode(2) == -EINVAL);
  CHECK(kv_crc32c_set_mode(-1) == -EINVAL);
  if (g_hw) {
    CHECK(kv_crc32c_set_mode(MODE_SSE42) == MODE_TABLE);
    CHECK(kv_crc32c_set_mode(MODE_TABLE) == MODE_SSE42);
  } else {
    CHECK(kv_crc32c_set_mode(MODE_SSE42) == -19); /* -ENODEV, mode unchanged */
  }
  CHECK(kv_crc32c_set_mode(MODE_TABLE) == MODE_TABLE);
}

typedef uint32_t (*SumFn)(uint32_t, const void *, size_t);
typedef uint32_t (*CopyFn)(uint32_t, void *, const void *, size_t);

/**
 * Measures one function's throughput.
 * @param sum   checksum-only function, or NULL
 * @param copy  fused copy function, or NULL (exactly one of the two is set;
 *              both NULL measures glibc memcpy)
 * @param src   input buffer
 * @param dst   output buffer (copies)
 * @param n     buffer size
 * @param reps  repetitions
 * @returns MB/s (10^6 bytes per second)
 */
static double mbps(SumFn sum, CopyFn copy, const uint8_t *src, uint8_t *dst, size_t n, int reps) {
  volatile uint32_t sink = 0;
  uint64_t t0 = now_ns();
  for (int r = 0; r < reps; r++) {
    if (sum) sink ^= sum(sink, src, n);
    else if (copy) sink ^= copy(sink, dst, src, n);
    else { memcpy(dst, src, n); sink ^= dst[r % n]; }
  }
  double secs = (double)(now_ns() - t0) / 1e9;
  return (double)n * reps / secs / 1e6;
}

/** 7. Measurement, printed for the record. */
static void report_speed(void) {
  struct { const char *name; size_t n; int reps; } sizes[] = {
      {"64 KiB (L2)", 64 * 1024, 4000}, {"16 MiB (DRAM)", 16u << 20, 12}};
  for (size_t i = 0; i < 2; i++) {
    size_t n = sizes[i].n;
    uint8_t *src = malloc(n), *dst = malloc(n);
    CHECK(src && dst);
    uint64_t s = 7;
    rng_fill(&s, src, n);
    memset(dst, 0, n);
    printf("  %-14s memcpy %6.0f MB/s | table %6.0f, table+copy %6.0f", sizes[i].name,
           mbps(NULL, NULL, src, dst, n, sizes[i].reps),
           mbps(kv_crc32c_sw, NULL, src, dst, n, sizes[i].reps),
           mbps(NULL, kv_crc32c_copy_sw, src, dst, n, sizes[i].reps));
    if (g_hw)
      printf(" | sse42 %6.0f, sse42+copy %6.0f", mbps(kv_crc32c_hw, NULL, src, dst, n, sizes[i].reps),
             mbps(NULL, kv_crc32c_copy_hw, src, dst, n, sizes[i].reps));
    printf("\n");
    free(src);
    free(dst);
  }
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  g_hw = kv_crc32c_hw_available();
  printf("  cpu sse4.2 crc32: %s\n", g_hw ? "yes" : "no (hardware variants skipped)");
  test_vectors();
  test_differential();
  test_chaining();
  test_guard();
  test_modes();
  if (g_hw) { /* the dispatching entry points must also be exact in hw mode */
    CHECK(kv_crc32c_set_mode(MODE_SSE42) == MODE_TABLE);
    test_vectors();
    test_chaining();
    CHECK(kv_crc32c_set_mode(MODE_TABLE) == MODE_SSE42);
  }
  if (!getenv("KVNODE_TEST_QUICK")) report_speed();
  return test_finish("test_crc32c");
}
