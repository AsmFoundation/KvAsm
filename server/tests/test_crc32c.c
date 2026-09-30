/*
 * test_crc32c.c — verifies crc32c.asm (CRC-32C for Upload/Download).
 *
 *  1. Known answers: the standard check value (CRC-32C("123456789") =
 *     0xE3069283) and the four 32-byte vectors of RFC 3720 §B.4.
 *  2. Differential: every implementation (table/slicing-by-8, SSE4.2, the
 *     128-bit PCLMULQDQ and 256-bit VPCLMULQDQ folding paths when the CPU
 *     has them, each in both shapes — checksum only and fused copy — plus
 *     the dispatching entry points and the non-temporal copies an upload's
 *     record uses, at every destination alignment) against an independent bitwise
 *     reference, for every length 0..600 at every start alignment 0..15
 *     (which crosses the folding paths' 256-byte hand-over, one and two
 *     128-byte blocks, their 16-byte loop and every tail length), lengths
 *     around every multiple of 128 up to 4 KiB, and large odd-sized buffers.
 *  3. Chaining: crc(a || b) == crc(crc(a), b) at random split points — the
 *     property the upload path relies on (it checksums chunk by chunk).
 *  4. The fused copies write exactly dst[0..len) and nothing around it.
 *  5. No overreads: buffers ending at a PROT_NONE guard page.
 *  6. Mode API: unknown modes refused, each hardware mode refused without
 *     CPU support, kv_crc32c_best_mode = the fastest available.
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
uint32_t kv_crc32c_pclmul(uint32_t crc, const void *buf, size_t len);
uint32_t kv_crc32c_vpclmul(uint32_t crc, const void *buf, size_t len);
uint32_t kv_crc32c_copy_sw(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_hw(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_pclmul(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_vpclmul(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_vpclmul_nt(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_pclmul_nt(uint32_t crc, void *dst, const void *src, size_t len);
uint32_t kv_crc32c_copy_nt(uint32_t crc, void *dst, const void *src, size_t len);
int kv_crc32c_hw_available(void);
int kv_crc32c_pclmul_available(void);
int kv_crc32c_vpclmul_available(void);
int kv_crc32c_best_mode(void);
int kv_crc32c_set_mode(int mode);

enum { MODE_TABLE = 0, MODE_SSE42 = 1, MODE_PCLMUL = 2, MODE_VPCLMUL = 3 };

typedef uint32_t (*SumFn)(uint32_t, const void *, size_t);
typedef uint32_t (*CopyFn)(uint32_t, void *, const void *, size_t);

/** One implementation in both shapes, and whether this CPU can run it. */
typedef struct {
  const char *name;
  SumFn sum;
  CopyFn copy;
  int available;
} Impl;

/* Filled by main: table first (always available), then the hardware paths. */
static Impl g_impls[] = {
    {"table", kv_crc32c_sw, kv_crc32c_copy_sw, 1},
    {"sse42", kv_crc32c_hw, kv_crc32c_copy_hw, 0},
    {"pclmul", kv_crc32c_pclmul, kv_crc32c_copy_pclmul, 0},
    {"vpclmul", kv_crc32c_vpclmul, kv_crc32c_copy_vpclmul, 0},
};
#define N_IMPLS (sizeof g_impls / sizeof g_impls[0])

/**
 * The fused copies beyond each implementation's own: the dispatchers and
 * the non-temporal variants (an upload's record copy). `needs` is the
 * g_impls index whose availability the entry requires (0 = runs anywhere).
 */
static const struct {
  const char *name;
  CopyFn copy;
  int needs;
} g_copies[] = {
    {"kv_crc32c_copy", kv_crc32c_copy, 0},
    {"kv_crc32c_copy_nt", kv_crc32c_copy_nt, 0},
    {"pclmul NT", kv_crc32c_copy_pclmul_nt, 2},
    {"vpclmul NT", kv_crc32c_copy_vpclmul_nt, 3},
};
#define N_COPIES (sizeof g_copies / sizeof g_copies[0])

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
 * Checks every available implementation (both shapes) and the dispatching
 * entry points on one buffer against the reference.
 * @param crc0  value to continue from
 * @param p     input bytes
 * @param n     byte count
 * @remarks The fused copies go into a destination surrounded by canary
 *          bytes, so a store outside [dst, dst + n) is caught.
 */
static void check_all(uint32_t crc0, const uint8_t *p, size_t n) {
  static uint8_t dst_buf[2 * 65536 + 96];
  uint32_t want = ref_crc(crc0, p, n);
  CHECK_EQ_U64(kv_crc32c(crc0, p, n), want);
  for (size_t i = 0; i < N_IMPLS; i++)
    if (g_impls[i].available) CHECK_EQ_U64(g_impls[i].sum(crc0, p, n), want);
  if (n > 2 * 65536) return; /* copies only up to the scratch size */
  /* i < N_IMPLS: each implementation's copy; then g_copies (dispatchers and
   * NT variants) at a destination offset that varies with n, so the NT
   * alignment heads take every length 0..31. */
  for (size_t i = 0; i < N_IMPLS + N_COPIES; i++) {
    if (i < N_IMPLS && !g_impls[i].available) continue;
    if (i >= N_IMPLS && !g_impls[g_copies[i - N_IMPLS].needs].available) continue;
    uint8_t *dst = dst_buf + 32 + (i >= N_IMPLS ? n % 32 : 0);
    memset(dst_buf, 0xA5, n + 96);
    uint32_t got = i < N_IMPLS ? g_impls[i].copy(crc0, dst, p, n)
                               : g_copies[i - N_IMPLS].copy(crc0, dst, p, n);
    CHECK_EQ_U64(got, want);
    CHECK(memcmp(dst, p, n) == 0);
    for (uint8_t *g = dst_buf; g < dst; g++) CHECK(*g == 0xA5); /* nothing written before dst */
    for (int g = 0; g < 32; g++) CHECK(dst[n + (size_t)g] == 0xA5); /* nor after dst + n */
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
  /* All-zero and all-one buffers long enough for the folding paths: a
   * wrong fold constant cannot hide behind random data. */
  static uint8_t z[4096];
  memset(z, 0, sizeof z);
  check_all(0, z, sizeof z);
  check_all(0xFFFFFFFFu, z, sizeof z);
  memset(z, 0xFF, sizeof z);
  check_all(0, z, sizeof z);
}

/** 2. Differential over lengths, alignments and large buffers. */
static void test_differential(void) {
  static uint8_t buf[4200 + 16];
  uint64_t s = 0xC3C3;
  for (size_t len = 0; len <= 600; len++) {
    for (size_t align = 0; align < 16; align++) {
      rng_fill(&s, buf + align, len);
      check_all((uint32_t)rng_next(&s), buf + align, len);
    }
  }
  /* Around every multiple of 128 (the main loops' block) up to 4 KiB, at
   * two alignments: the last full block, the 16-byte loop's 0..7 steps and
   * every tail length all get exercised at every block count. */
  for (size_t base = 640; base <= 4096; base += 128) {
    for (size_t d = 0; d < 32; d++) {
      size_t len = base - 16 + d;
      for (size_t align = 0; align < 16; align += 7) {
        rng_fill(&s, buf + align, len);
        check_all((uint32_t)rng_next(&s), buf + align, len);
      }
    }
  }
  size_t big = (3u << 20) + 13;                 /* 3 MiB + 13: not a multiple of 8 */
  uint8_t *p = malloc(big);
  CHECK(p != NULL);
  rng_fill(&s, p, big);
  check_all(0, p, big);
  check_all(0, p + 3, 131071);                  /* odd start, odd length, fits the copy scratch */
  check_all(0x9E3779B9u, p + 1, 65536 + 16384 + 5);
  free(p);
}

/** 3. Chaining at random split points, for every implementation. */
static void test_chaining(void) {
  static uint8_t buf[4096];
  uint64_t s = 0xC4A1;
  for (int i = 0; i < 20000; i++) {
    size_t n = rng_next(&s) % sizeof buf;
    rng_fill(&s, buf, n);
    size_t cut = n ? rng_next(&s) % (n + 1) : 0;
    uint32_t whole = kv_crc32c_sw(0, buf, n);
    for (size_t k = 0; k < N_IMPLS; k++) {
      if (!g_impls[k].available) continue;
      CHECK_EQ_U64(g_impls[k].sum(g_impls[k].sum(0, buf, cut), buf + cut, n - cut), whole);
    }
    uint8_t out[4096];
    uint32_t a = kv_crc32c_copy(0, out, buf, cut);
    CHECK_EQ_U64(kv_crc32c_copy(a, out + cut, buf + cut, n - cut), whole);
    CHECK(memcmp(out, buf, n) == 0);
    /* the NT entry chains the same way (the upload path mixes both across
     * a value's chunks only by size, but each must continue any CRC) */
    memset(out, 0, n);
    a = kv_crc32c_copy_nt(0, out, buf, cut);
    CHECK_EQ_U64(kv_crc32c_copy_nt(a, out + cut, buf + cut, n - cut), whole);
    CHECK(memcmp(out, buf, n) == 0);
  }
}

/** 5. No reads past the end: every length up to a page ends at a guard page. */
static void test_guard(void) {
  long page = sysconf(_SC_PAGESIZE);
  uint64_t s = 0x6A6A;
  static uint8_t out[8192];
  for (long len = 0; len <= page && len <= 4096; len += (len < 64 ? 1 : len < 600 ? 7 : 61)) {
    void *base;
    uint8_t *p = guard_alloc((size_t)len, &base);
    rng_fill(&s, p, (size_t)len);
    uint32_t want = ref_crc(0, p, (size_t)len);
    for (size_t k = 0; k < N_IMPLS; k++) {
      if (!g_impls[k].available) continue;
      CHECK_EQ_U64(g_impls[k].sum(0, p, (size_t)len), want);
      CHECK_EQ_U64(g_impls[k].copy(0, out, p, (size_t)len), want);
    }
    for (size_t k = 0; k < N_COPIES; k++)
      if (g_impls[g_copies[k].needs].available)
        CHECK_EQ_U64(g_copies[k].copy(0, out + 5, p, (size_t)len), want);
    guard_free(base);
  }
}

/** 6. Mode API. */
static void test_modes(void) {
  CHECK(kv_crc32c_set_mode(4) == -EINVAL);
  CHECK(kv_crc32c_set_mode(-1) == -EINVAL);
  int prev = MODE_TABLE;
  for (int m = MODE_SSE42; m <= MODE_VPCLMUL; m++) {
    if (g_impls[m].available) {
      CHECK(kv_crc32c_set_mode(m) == prev);
      prev = m;
    } else {
      CHECK(kv_crc32c_set_mode(m) == -19); /* -ENODEV, mode unchanged */
    }
  }
  CHECK(kv_crc32c_set_mode(MODE_TABLE) == prev);
  CHECK(kv_crc32c_set_mode(MODE_TABLE) == MODE_TABLE);
  int best = MODE_TABLE;
  for (int m = MODE_SSE42; m <= MODE_VPCLMUL; m++)
    if (g_impls[m].available) best = m;
  CHECK_EQ_U64(kv_crc32c_best_mode(), best);
}

/**
 * Measures one function's throughput.
 * @param sum       checksum-only function, or NULL
 * @param copy      fused copy function, or NULL (both NULL: glibc memcpy)
 * @param src       input buffer
 * @param dst       output buffer (copies)
 * @param n         bytes per call
 * @param src_span  bytes the source walks through (n = the same hot piece
 *                  every call; larger = each call reads the next n bytes,
 *                  streaming through DRAM)
 * @param dst_span  the same for the destination
 * @param reps      calls
 * @returns MB/s (10^6 bytes per second)
 * @remarks Separate spans model the transfer paths: an upload copies a hot
 *          read buffer into cold records, a download cold records into a
 *          hot output buffer.
 */
static double mbps(SumFn sum, CopyFn copy, const uint8_t *src, uint8_t *dst, size_t n,
                   size_t src_span, size_t dst_span, int reps) {
  volatile uint32_t sink = 0;
  size_t so = 0, d_o = 0;
  uint64_t t0 = now_ns();
  for (int r = 0; r < reps; r++) {
    if (sum) sink ^= sum(sink, src + so, n);
    else if (copy) sink ^= copy(sink, dst + d_o, src + so, n);
    else { memcpy(dst + d_o, src + so, n); sink ^= dst[d_o + (size_t)r % n]; }
    so = so + 2 * n > src_span ? 0 : so + n;
    d_o = d_o + 2 * n > dst_span ? 0 : d_o + n;
  }
  double secs = (double)(now_ns() - t0) / 1e9;
  return (double)n * reps / secs / 1e6;
}

/** 7. Measurement, printed for the record. */
static void report_speed(void) {
  /* 16 KiB calls are what the transfer paths make (one HTTP/2 frame's
   * payload); 64 KiB hot and 64 MiB spans bracket cache and DRAM. */
  struct { const char *name; size_t n, src_span, dst_span; int reps; } cases[] = {
      {"16 KiB hot", 16 * 1024, 16 * 1024, 16 * 1024, 40000},
      {"16K hot->64M (up)", 16 * 1024, 16 * 1024, 64u << 20, 16000},
      {"64M->16K hot (dn)", 16 * 1024, 64u << 20, 16 * 1024, 16000},
      {"16 KiB 64M->64M", 16 * 1024, 64u << 20, 64u << 20, 16000},
      {"64 KiB hot", 64 * 1024, 64 * 1024, 64 * 1024, 8000},
      {"16 MiB (DRAM)", 16u << 20, 64u << 20, 64u << 20, 16}};
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    size_t ss = cases[i].src_span, ds = cases[i].dst_span, n = cases[i].n;
    int reps = cases[i].reps;
    uint8_t *src = malloc(ss), *dst = malloc(ds);
    CHECK(src && dst);
    uint64_t s = 7;
    rng_fill(&s, src, ss);
    memset(dst, 0, ds);
    printf("  %-18s memcpy %6.0f MB/s", cases[i].name, mbps(NULL, NULL, src, dst, n, ss, ds, reps));
    for (size_t k = 0; k < N_IMPLS; k++) {
      if (!g_impls[k].available) continue;
      printf(" | %s %6.0f, +copy %6.0f", g_impls[k].name,
             mbps(g_impls[k].sum, NULL, src, dst, n, ss, ds, reps),
             mbps(NULL, g_impls[k].copy, src, dst, n, ss, ds, reps));
    }
    for (size_t k = 2; k < N_COPIES; k++)
      if (g_impls[g_copies[k].needs].available)
        printf(" | %s %6.0f", g_copies[k].name, mbps(NULL, g_copies[k].copy, src, dst, n, ss, ds, reps));
    printf("\n");
    free(src);
    free(dst);
  }
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  g_impls[MODE_SSE42].available = kv_crc32c_hw_available();
  g_impls[MODE_PCLMUL].available = kv_crc32c_pclmul_available();
  g_impls[MODE_VPCLMUL].available = kv_crc32c_vpclmul_available();
  printf("  cpu: sse4.2 crc32 %s, pclmulqdq %s, avx2+vpclmulqdq %s\n",
         g_impls[MODE_SSE42].available ? "yes" : "no", g_impls[MODE_PCLMUL].available ? "yes" : "no",
         g_impls[MODE_VPCLMUL].available ? "yes" : "no (256-bit path skipped)");
  /* A CPU with a faster path has the slower ones (the folding paths finish
   * with crc32): the test would otherwise skip checks silently. */
  CHECK(!g_impls[MODE_PCLMUL].available || g_impls[MODE_SSE42].available);
  CHECK(!g_impls[MODE_VPCLMUL].available || g_impls[MODE_PCLMUL].available);
  test_vectors();
  test_differential();
  test_chaining();
  test_guard();
  test_modes();
  /* The dispatching entry points must be exact in every mode the CPU has. */
  for (int m = MODE_SSE42; m <= MODE_VPCLMUL; m++) {
    if (!g_impls[m].available) continue;
    CHECK(kv_crc32c_set_mode(m) == MODE_TABLE);
    test_vectors();
    test_chaining();
    CHECK(kv_crc32c_set_mode(MODE_TABLE) == m);
  }
  if (!getenv("KVNODE_TEST_QUICK")) report_speed();
  return test_finish("test_crc32c");
}
