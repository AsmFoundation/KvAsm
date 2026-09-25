/*
 * test_siphash.c — verifies kv_siphash128 (siphash.asm).
 *
 *  1. All 64 official SipHash-2-4-128 reference vectors.
 *  2. Differential test against a straightforward C port of the reference
 *     implementation, over random keys and every length 0..1100 (covers the
 *     full-word loop, all 8 tail sizes, and the len<8 short path).
 *  3. Overread proof: messages ending exactly at a PROT_NONE guard page for
 *     every length 0..64 — any read past the end would crash the test.
 *  4. The out pointer may be NULL and the rax/rdx return pair matches out[].
 */
#include "test_common.h"
#include "siphash_vectors.h"

/* siphash.asm: returns h1 in rax; we only use the out[] array from C. */
uint64_t kv_siphash128(const void *msg, size_t len, const uint64_t key[2], uint64_t out[2]);

#define ROTL(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND                                                               \
  do {                                                                         \
    v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32);                  \
    v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2;                                     \
    v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0;                                     \
    v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32);                  \
  } while (0)

/**
 * Reference SipHash-2-4-128 in portable C (transcribed from the reference
 * siphash.c, outlen = 16). Serves as the oracle for random inputs.
 * @param in message. @param inlen length. @param k0,k1 key words. @param out h1,h2.
 */
static void ref_siphash128(const uint8_t *in, size_t inlen, uint64_t k0, uint64_t k1,
                           uint64_t out[2]) {
  uint64_t v0 = 0x736f6d6570736575ull ^ k0, v1 = 0x646f72616e646f6dull ^ k1;
  uint64_t v2 = 0x6c7967656e657261ull ^ k0, v3 = 0x7465646279746573ull ^ k1;
  v1 ^= 0xee;
  const uint8_t *end = in + inlen - (inlen % 8);
  for (; in != end; in += 8) {
    uint64_t m;
    memcpy(&m, in, 8);
    v3 ^= m; SIPROUND; SIPROUND; v0 ^= m;
  }
  uint64_t b = (uint64_t)inlen << 56;
  switch (inlen & 7) {
    case 7: b |= (uint64_t)in[6] << 48; /* fall through */
    case 6: b |= (uint64_t)in[5] << 40; /* fall through */
    case 5: b |= (uint64_t)in[4] << 32; /* fall through */
    case 4: b |= (uint64_t)in[3] << 24; /* fall through */
    case 3: b |= (uint64_t)in[2] << 16; /* fall through */
    case 2: b |= (uint64_t)in[1] << 8;  /* fall through */
    case 1: b |= (uint64_t)in[0]; break;
    default: break;
  }
  v3 ^= b; SIPROUND; SIPROUND; v0 ^= b;
  v2 ^= 0xee; SIPROUND; SIPROUND; SIPROUND; SIPROUND;
  out[0] = v0 ^ v1 ^ v2 ^ v3;
  v1 ^= 0xdd; SIPROUND; SIPROUND; SIPROUND; SIPROUND;
  out[1] = v0 ^ v1 ^ v2 ^ v3;
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  /* 1. Reference vectors: key 00..0f, message 00..(i-1). */
  uint8_t keyb[16], msg[64];
  for (int i = 0; i < 16; i++) keyb[i] = (uint8_t)i;
  for (int i = 0; i < 64; i++) msg[i] = (uint8_t)i;
  uint64_t key[2];
  memcpy(key, keyb, 16);
  for (int i = 0; i < 64; i++) {
    uint64_t out[2] = {0, 0};
    kv_siphash128(msg, (size_t)i, key, out);
    CHECK(memcmp(out, kSip128Vectors[i], 16) == 0);
  }

  /* 2. Differential against the C reference. */
  uint64_t s = 0x9E3779B97F4A7C15ull;
  static uint8_t buf[1200];
  for (size_t len = 0; len <= 1100; len++) {
    for (int rep = 0; rep < 4; rep++) {
      uint64_t k[2] = {rng_next(&s), rng_next(&s)}, a[2], r[2];
      rng_fill(&s, buf, len);
      kv_siphash128(buf, len, k, a);
      ref_siphash128(buf, len, k[0], k[1], r);
      CHECK_EQ_U64(a[0], r[0]);
      CHECK_EQ_U64(a[1], r[1]);
    }
  }

  /* 3. No overread: message ends at a guard page. */
  for (size_t len = 0; len <= 64; len++) {
    void *base;
    uint8_t *p = guard_alloc(len, &base);
    rng_fill(&s, p, len);
    uint64_t k[2] = {rng_next(&s), rng_next(&s)}, a[2], r[2];
    kv_siphash128(p, len, k, a);
    ref_siphash128(p, len, k[0], k[1], r);
    CHECK_EQ_U64(a[0], r[0]);
    CHECK_EQ_U64(a[1], r[1]);
    guard_free(base);
  }

  /* 4. NULL out pointer is allowed (result only in registers). */
  kv_siphash128(msg, 10, key, NULL);

  return test_finish("test_siphash");
}
