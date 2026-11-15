/* =============================================================================
 * hashbench.c — the store's key hash against its alternatives (hashes.asm).
 *
 * For each hash and key length it prints two costs:
 *   latency     ns per hash when every hash's input depends on the previous
 *               hash's output — the cost when the hash is the critical path;
 *   throughput  ns per hash over independent keys — the cost when the CPU can
 *               overlap consecutive hashes (what a batch of requests allows).
 * A hash called once per request, between ~1,400 other instructions, costs
 * between the two. Every figure is the minimum of 15 trials on a pinned vCPU;
 * calib_add turns the times into core cycles.
 *
 * Correctness first (the program exits non-zero on any mismatch):
 *   - sip_2_4_128 must equal the server's kv_siphash128 on every length 0..96;
 *   - every SipHash variant must equal the reference C implementation below
 *     (the SipHash authors' algorithm, parameterized by rounds and width);
 *   - kv_aeshash128 with a zero length mask must reproduce the FIPS-197
 *     Appendix C.1 AES-128 vector, and its chained path must equal a C
 *     CBC-MAC built from the same single-block primitive for lengths 17..96;
 *   - no hash may read past its input (inputs end at a PROT_NONE page).
 *
 * Usage: hashbench   (run pinned: taskset -c N ./hashbench)
 * ============================================================================= */
#define _GNU_SOURCE
#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

typedef unsigned __int128 u128;

/* hashes.asm and the server's siphash.asm; 128-bit results come back in rax:rdx. */
u128 kv_siphash128(const void *msg, size_t len, const uint64_t key[2], uint64_t *out);
u128 sip_2_4_128(const void *msg, size_t len, const uint64_t key[2]);
uint64_t sip_2_4_64(const void *msg, size_t len, const uint64_t key[2]);
u128 sip_1_3_128(const void *msg, size_t len, const uint64_t key[2]);
uint64_t sip_1_3_64(const void *msg, size_t len, const uint64_t key[2]);
u128 kv_aeshash128(const void *msg, size_t len, const void *aeskey);
uint64_t calib_add(uint64_t n);

/* ---- reference SipHash-c-d (64- or 128-bit), after Aumasson & Bernstein ---- */
#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND do { \
    v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); \
    v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; \
    v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)

static void ref_sip(const uint8_t *in, size_t len, const uint64_t k[2], int c, int d, int bits,
                    uint64_t out[2]) {
  uint64_t v0 = 0x736f6d6570736575ull ^ k[0], v1 = 0x646f72616e646f6dull ^ k[1];
  uint64_t v2 = 0x6c7967656e657261ull ^ k[0], v3 = 0x7465646279746573ull ^ k[1];
  if (bits == 128) v1 ^= 0xee;
  size_t full = len / 8;
  for (size_t i = 0; i < full; i++) {
    uint64_t m;
    memcpy(&m, in + i * 8, 8);
    v3 ^= m;
    for (int r = 0; r < c; r++) SIPROUND;
    v0 ^= m;
  }
  uint64_t b = (uint64_t)len << 56;
  for (size_t i = 0; i < len % 8; i++) b |= (uint64_t)in[full * 8 + i] << (8 * i);
  v3 ^= b;
  for (int r = 0; r < c; r++) SIPROUND;
  v0 ^= b;
  v2 ^= bits == 128 ? 0xee : 0xff;
  for (int r = 0; r < d; r++) SIPROUND;
  out[0] = v0 ^ v1 ^ v2 ^ v3;
  out[1] = 0;
  if (bits == 128) {
    v1 ^= 0xdd;
    for (int r = 0; r < d; r++) SIPROUND;
    out[1] = v0 ^ v1 ^ v2 ^ v3;
  }
}

/* ---- AES-128 key schedule in C (FIPS-197 5.2): only to build round keys ---- */
static uint8_t sbox[256];
static void make_sbox(void) {
  /* multiplicative inverse in GF(2^8) followed by the affine map */
  uint8_t p = 1, q = 1;
  do {
    p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
    q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
    if (q & 0x80) q ^= 0x09;
    uint8_t x = (uint8_t)(q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                          (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4)));
    sbox[p] = (uint8_t)(x ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
}
static void expand_key(const uint8_t key[16], uint8_t rk[176]) {
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4];
    memcpy(t, rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t x = t[0];
      t[0] = (uint8_t)(sbox[t[1]] ^ rcon); t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
      rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
    }
    for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
  }
}

/* The key block kv_aeshash128 takes: 11 round keys, then 17 length masks. */
typedef struct { uint8_t rk[176]; uint8_t mask[17][16]; } __attribute__((aligned(16))) AesKey;

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng(void) {            /* xorshift64*: test data only */
  rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
  return rng_state * 0x2545F4914F6CDD1Dull;
}

enum { H_SERVER, H_SIP24_128, H_SIP24_64, H_SIP13_128, H_SIP13_64, H_AES, H_COUNT };
static const char *names[H_COUNT] = {"SipHash-2-4-128 (server today)", "SipHash-2-4-128 (macro copy)",
                                     "SipHash-2-4-64", "SipHash-1-3-128", "SipHash-1-3-64",
                                     "AES-128 PRF (AES-NI)"};
static uint64_t sipkey[2];
static AesKey aeskey;

/* One call of hash `h`; returns (h1, h2) with h2 = 0 for the 64-bit variants. */
static inline u128 call(int h, const void *m, size_t len) {
  switch (h) {
    case H_SERVER: return kv_siphash128(m, len, sipkey, NULL);
    case H_SIP24_128: return sip_2_4_128(m, len, sipkey);
    case H_SIP24_64: return sip_2_4_64(m, len, sipkey);
    case H_SIP13_128: return sip_1_3_128(m, len, sipkey);
    case H_SIP13_64: return sip_1_3_64(m, len, sipkey);
    default: return kv_aeshash128(m, len, &aeskey);
  }
}

int main(void) {
  make_sbox();
  sipkey[0] = rng(); sipkey[1] = rng();
  uint8_t k16[16];
  for (int i = 0; i < 16; i++) k16[i] = (uint8_t)rng();
  expand_key(k16, aeskey.rk);
  for (int n = 0; n < 17; n++) for (int i = 0; i < 16; i++) aeskey.mask[n][i] = (uint8_t)rng();

  /* Inputs end exactly at an inaccessible page: any overread faults. */
  size_t pg = 4096;
  uint8_t *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED || mprotect(map + pg, pg, PROT_NONE) != 0) { perror("mmap"); return 1; }
  uint8_t *end = map + pg;

  /* ---- correctness ---- */
  int checks = 0;
  for (size_t len = 0; len <= 96; len++)
    for (int rep = 0; rep < 50; rep++) {
      uint8_t *m = end - len;
      for (size_t i = 0; i < len; i++) m[i] = (uint8_t)rng();
      uint64_t ref[2];
      u128 r = kv_siphash128(m, len, sipkey, NULL);
      ref_sip(m, len, sipkey, 2, 4, 128, ref);
      if ((uint64_t)r != ref[0] || (uint64_t)(r >> 64) != ref[1]) { printf("server siphash != reference at len %zu\n", len); return 1; }
      r = sip_2_4_128(m, len, sipkey);
      if ((uint64_t)r != ref[0] || (uint64_t)(r >> 64) != ref[1]) { printf("sip_2_4_128 mismatch at len %zu\n", len); return 1; }
      ref_sip(m, len, sipkey, 2, 4, 64, ref);
      if (sip_2_4_64(m, len, sipkey) != ref[0]) { printf("sip_2_4_64 mismatch at len %zu\n", len); return 1; }
      ref_sip(m, len, sipkey, 1, 3, 128, ref);
      r = sip_1_3_128(m, len, sipkey);
      if ((uint64_t)r != ref[0] || (uint64_t)(r >> 64) != ref[1]) { printf("sip_1_3_128 mismatch at len %zu\n", len); return 1; }
      ref_sip(m, len, sipkey, 1, 3, 64, ref);
      if (sip_1_3_64(m, len, sipkey) != ref[0]) { printf("sip_1_3_64 mismatch at len %zu\n", len); return 1; }
      /* AES: rebuild the hash in C from the single-block primitive itself
       * (16-byte inputs with a zero mask are plain AES encryptions). */
      AesKey plain = aeskey;
      memset(plain.mask, 0, sizeof plain.mask);
      uint8_t st[16] = {0}, blk[16];
      size_t off = 0, left = len;
      while (left > 16) {
        for (int i = 0; i < 16; i++) blk[i] = (uint8_t)(m[off + i] ^ st[i]);
        u128 e = kv_aeshash128(blk, 16, &plain);
        memcpy(st, &e, 16);
        off += 16; left -= 16;
      }
      memset(blk, 0, 16);
      memcpy(blk, m + off, left);
      for (int i = 0; i < 16; i++) blk[i] ^= (uint8_t)(aeskey.mask[left][i] ^ st[i]);
      u128 want = kv_aeshash128(blk, 16, &plain);
      u128 got = kv_aeshash128(m, len, &aeskey);
      if (want != got) { printf("kv_aeshash128 chain mismatch at len %zu\n", len); return 1; }
      checks += 6;
    }
  {
    /* FIPS-197 Appendix C.1 */
    static const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    AesKey fk;
    memset(&fk, 0, sizeof fk);
    expand_key(key, fk.rk);
    u128 e = kv_aeshash128(pt, 16, &fk);
    if (memcmp(&e, ct, 16) != 0) { printf("AES-128 FIPS-197 vector mismatch\n"); return 1; }
    checks++;
  }
  printf("correctness: %d checks passed (reference SipHash, FIPS-197 AES-128, no overreads)\n", checks);

  /* ---- speed ---- */
  double cyc = 1e30;
  for (int t = 0; t < 9; t++) {
    double t0 = now_ns();
    calib_add(200000000ull);
    double d = (now_ns() - t0) / 200e6;
    if (d < cyc) cyc = d;
  }
  printf("core cycle: %.4f ns (%.2f GHz)\n", cyc, 1.0 / cyc);

  static const size_t lens[] = {8, 16, 24, 32, 64};
  enum { N = 1 << 12, ROUNDS = 256 };            /* 4,096 keys x 256 rounds per trial */
  uint8_t *keys = aligned_alloc(64, (size_t)N * 64);
  for (size_t i = 0; i < (size_t)N * 64; i++) keys[i] = (uint8_t)rng();
  printf("%-32s %5s  %18s  %18s\n", "hash", "bytes", "latency ns (cycles)", "throughput ns (cycles)");
  for (int h = 0; h < H_COUNT; h++)
    for (size_t li = 0; li < sizeof lens / sizeof lens[0]; li++) {
      size_t len = lens[li];
      double lat = 1e30, thr = 1e30;
      uint64_t sink = 0;
      for (int t = 0; t < 15; t++) {
        /* latency: the next input is the previous output (one 16-byte store,
         * so both 8- and 16-byte loads of it forward from the store buffer) */
        uint8_t buf[64] __attribute__((aligned(16)));
        memcpy(buf, keys, 64);
        double t0 = now_ns();
        for (int i = 0; i < N * 64; i++) {
          u128 r = call(h, buf, len);
          _mm_store_si128((__m128i *)buf, _mm_set_epi64x((long long)(uint64_t)(r >> 64) ^ 0x55, (long long)(uint64_t)r));
        }
        double d = (now_ns() - t0) / (N * 64.0);
        if (d < lat) lat = d;
        sink += buf[0];
        t0 = now_ns();
        uint64_t acc = 0;
        for (int r = 0; r < ROUNDS; r++)
          for (int i = 0; i < N; i++) acc += (uint64_t)call(h, keys + (size_t)i * 64, len);
        d = (now_ns() - t0) / ((double)N * ROUNDS);
        if (d < thr) thr = d;
        sink += acc;
      }
      printf("%-32s %5zu  %8.2f (%6.1f)   %8.2f (%6.1f)%s\n", names[h], len, lat, lat / cyc, thr, thr / cyc,
             sink == 42 ? "!" : "");
    }
  return 0;
}
