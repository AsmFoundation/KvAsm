/*
 * crc32c.c — CRC-32C (Castagnoli) for the streaming workloads.
 *
 * Why kvbench needs it, and why it stays off the hot path: an Upload's
 * response and a Download's last message carry the server's CRC-32C of the
 * value, and kvbench must check it against the value it wrote. Every value is
 * "key id (8 bytes) || the shared template", so the CRC of any key's value is
 * the combination of the CRC of 8 bytes with the (precomputed) CRC of the
 * template's tail — O(1) per request, with no pass over megabytes of data.
 * Download bytes are compared with the expected value in place instead
 * (cheaper than a CRC and exact), so a client thread spends its time on the
 * transport, not on checksums, and the server stays the bottleneck.
 *
 * The combination is zlib's crc32_combine (x^(8n) mod P in GF(2), then one
 * carry-less multiply) for the Castagnoli polynomial.
 */
#include "kvbench.h"

/** Reflected Castagnoli polynomial (the one SSE4.2's crc32 instruction uses). */
#define CRC32C_POLY 0x82F63B78u

/** Byte-at-a-time table (the fallback path). */
static uint32_t g_crc_table[256];
/** g_x2n_table[k] = x^(2^k) mod P, for crc32c_shift_op. */
static uint32_t g_x2n_table[32];
/** Precomputed by value_crc_init: CRC of template[8..value_size) and its shift. */
static uint32_t g_tail_crc, g_tail_op;
/** 1 when the CPU has SSE4.2 (decided once in crc32c_init). */
static int g_have_sse42;
/** Makes the one-time table setup safe to trigger from any thread. */
static pthread_once_t g_crc_once = PTHREAD_ONCE_INIT;
/**
 * Test hook: 1 forces the table path even on an SSE4.2 CPU, so the unit
 * tests cover both implementations on any machine. Never set it while
 * worker threads run (a plain int, read without synchronization).
 */
int crc32c_force_table;

/**
 * Multiplies two polynomials modulo P (reflected bit order, zlib's
 * multmodp).
 * @param a  first factor
 * @param b  second factor
 * @returns a * b mod P
 * @remarks Up to 32 iterations: about 100 cycles, paid once per request.
 */
static uint32_t multmodp(uint32_t a, uint32_t b) {
  uint32_t m = 1u << 31, p = 0;
  for (;;) {
    if (a & m) {
      p ^= b;
      if ((a & (m - 1)) == 0) break;
    }
    m >>= 1;
    b = b & 1 ? (b >> 1) ^ CRC32C_POLY : b >> 1;
  }
  return p;
}

/**
 * Builds the byte table and the x^(2^k) table and probes the CPU. Runs once
 * per process through pthread_once (crc32c_ready), so no caller can see a
 * half-built table.
 */
static void crc32c_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ CRC32C_POLY : c >> 1;
    g_crc_table[i] = c;
  }
  uint32_t p = 1u << 30; /* x^1 */
  g_x2n_table[0] = p;
  for (int n = 1; n < 32; n++) g_x2n_table[n] = p = multmodp(p, p);
  __builtin_cpu_init();
  g_have_sse42 = __builtin_cpu_supports("sse4.2");
}

/** Ensures crc32c_init ran (a single acquire load once it has). */
static void crc32c_ready(void) { pthread_once(&g_crc_once, crc32c_init); }

/**
 * Table path: one byte per step (~1 GB/s). Only used where the CPU lacks
 * SSE4.2 or a test forces it.
 * @param crc  inverted running state
 * @param p    bytes
 * @param n    count
 * @returns the updated inverted state
 */
static uint32_t crc_table_run(uint32_t crc, const uint8_t *p, size_t n) {
  while (n--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return crc;
}

/**
 * SSE4.2 path: 8 bytes per crc32 instruction (one dependency chain, ~8 GB/s
 * — plenty for the once-per-run template CRC and the 8-byte key ids).
 * @param crc  inverted running state
 * @param p    bytes
 * @param n    count
 * @returns the updated inverted state
 */
__attribute__((target("sse4.2"))) static uint32_t crc_sse42_run(uint32_t crc, const uint8_t *p,
                                                                size_t n) {
  uint64_t c = crc;
  for (; n >= 8; p += 8, n -= 8) {
    uint64_t v;
    memcpy(&v, p, 8);
    c = __builtin_ia32_crc32di(c, v);
  }
  uint32_t c32 = (uint32_t)c;
  for (; n; p++, n--) c32 = __builtin_ia32_crc32qi(c32, *p);
  return c32;
}

/**
 * CRC-32C continued from crc; see kvbench.h. Thread-safe.
 * @param crc  previous result, 0 to start
 * @param p    bytes
 * @param n    count
 * @returns the continued checksum
 */
uint32_t crc32c_update(uint32_t crc, const uint8_t *p, size_t n) {
  crc32c_ready();
  crc = ~crc;
  crc = g_have_sse42 && !crc32c_force_table ? crc_sse42_run(crc, p, n) : crc_table_run(crc, p, n);
  return ~crc;
}

/**
 * x^(8*len) mod P; see kvbench.h. Thread-safe.
 * @param len  byte count
 * @returns the shift operator
 */
uint32_t crc32c_shift_op(uint64_t len) {
  crc32c_ready();
  /* x^(8*len) = x^(len * 2^3): walk len's bits with the x^(2^k) table from
   * k = 3, as zlib's x2nmodp does. */
  uint32_t p = 1u << 31; /* x^0 */
  unsigned k = 3;
  while (len) {
    if (len & 1) p = multmodp(g_x2n_table[k & 31], p);
    len >>= 1;
    k++;
  }
  return p;
}

/**
 * crc(A || B); see kvbench.h. Pure arithmetic, thread-safe.
 * @param crc_a  crc(A)
 * @param crc_b  crc(B)
 * @param op     crc32c_shift_op(len(B))
 * @returns crc(A || B)
 */
uint32_t crc32c_combine_op(uint32_t crc_a, uint32_t crc_b, uint32_t op) {
  return multmodp(op, crc_a) ^ crc_b;
}

/**
 * Precomputes value_crc's two parts from g_value_template; see kvbench.h.
 * @remarks Main thread only, after the template is filled and before any
 *          worker starts: the parts are plain globals the workers then read.
 */
void value_crc_init(void) {
  crc32c_ready();
  uint32_t tail = g_opt.value_size > 8 ? g_opt.value_size - 8 : 0;
  g_tail_crc = crc32c_update(0, g_value_template + 8, tail);
  g_tail_op = crc32c_shift_op(tail);
}

/**
 * CRC-32C of value_fill(key)'s bytes in O(1); see kvbench.h.
 * @param key  key id
 * @returns the value's checksum
 * @remarks Valid only after value_crc_init.
 */
uint32_t value_crc(uint64_t key) {
  uint8_t id[8];
  memcpy(id, &key, 8);
  return crc32c_combine_op(crc32c_update(0, id, 8), g_tail_crc, g_tail_op);
}
