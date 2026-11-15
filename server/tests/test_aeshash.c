/*
 * test_aeshash.c — verifies the AES key hash (aeshash.asm) and the store's
 * hash selection (store.asm kv_set_hash_mode / kv_store_hash).
 *
 * The oracle is a table-free AES-128 written here in portable C (FIPS-197,
 * straight from the specification), so the assembly is never compared with
 * itself:
 *  1. FIPS-197 Appendix C.1 — the C AES and the key schedule
 *     kv_aeshash_setup builds both reproduce the standard's vector.
 *  2. The key block: round keys equal the C key schedule; the 17 length
 *     masks equal AES_K(n + 1).
 *  3. Differential test of kv_aeshash128 against the construction written
 *     out in C (padding, length mask, CBC chain, final mask), over random
 *     seeds and every length 0..300.
 *  4. Overread proof: messages ending exactly at a PROT_NONE guard page for
 *     every length 0..96 — any read past the end would crash the test.
 *  5. Contract: out may be NULL; rax/rdx match out[]; rsi-length edge cases.
 *  6. The store under each hash mode: kv_set_hash_mode's refusals, and a
 *     table that stores and finds 20,000 keys under SipHash and under AES
 *     (two tables alive at once: each has its own key block).
 *
 * On a CPU without AES-NI the AES parts cannot run; the test then checks
 * that the store refuses the mode and reports what it skipped.
 */
#include "test_common.h"
#include "kvnode_abi.h"

/* aeshash.asm returns h1 in rax and h2 in rdx: a 128-bit integer in C. */
unsigned __int128 kv_aeshash128_r(const void *msg, size_t len, const uint8_t *block,
                                  uint64_t out[2]) __asm__("kv_aeshash128");

/* ---- AES-128 in portable C (FIPS-197), the oracle ------------------------ */

static uint8_t sbox[256];

/** Multiplies by x in GF(2^8) modulo the AES polynomial. */
static uint8_t xtime(uint8_t a) { return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0)); }

/**
 * Builds the S-box from its definition (FIPS-197 5.1.1): the multiplicative
 * inverse in GF(2^8), then the affine transformation. Computed, not typed
 * in, so a transcription error cannot hide.
 */
static void make_sbox(void) {
  uint8_t p = 1, q = 1;
  do {
    p = (uint8_t)(p ^ xtime(p));                   /* p *= 3 */
    q ^= (uint8_t)(q << 1);                        /* q /= 3 */
    q ^= (uint8_t)(q << 2);
    q ^= (uint8_t)(q << 4);
    if (q & 0x80) q ^= 0x09;
    uint8_t x = (uint8_t)(q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^
                          (uint8_t)((q << 3) | (q >> 5)) ^ (uint8_t)((q << 4) | (q >> 4)));
    sbox[p] = (uint8_t)(x ^ 0x63);
  } while (p != 1);
  sbox[0] = 0x63;
}

/**
 * AES-128 key expansion (FIPS-197 5.2).
 * @param key  the 16-byte cipher key
 * @param rk   receives the 11 round keys (176 bytes)
 */
static void c_expand(const uint8_t key[16], uint8_t rk[176]) {
  memcpy(rk, key, 16);
  uint8_t rcon = 1;
  for (int i = 16; i < 176; i += 4) {
    uint8_t t[4];
    memcpy(t, rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t x = t[0];
      t[0] = (uint8_t)(sbox[t[1]] ^ rcon);
      t[1] = sbox[t[2]];
      t[2] = sbox[t[3]];
      t[3] = sbox[x];
      rcon = xtime(rcon);
    }
    for (int j = 0; j < 4; j++) rk[i + j] = (uint8_t)(rk[i - 16 + j] ^ t[j]);
  }
}

/**
 * AES-128 encryption of one block (FIPS-197 5.1).
 * @param rk     the 11 round keys of c_expand
 * @param in     16-byte plaintext
 * @param out    receives the 16-byte ciphertext (may alias in)
 */
static void c_encrypt(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16]) {
  uint8_t s[16], t[16];
  for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
  for (int round = 1; round <= 10; round++) {
    /* SubBytes + ShiftRows: byte (row r, column c) lives at s[4c + r] and
     * moves to column c - r. */
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++) t[4 * c + r] = sbox[s[4 * ((c + r) % 4) + r]];
    if (round < 10) {
      for (int c = 0; c < 4; c++) {               /* MixColumns */
        uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3];
        s[4 * c] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        s[4 * c + 1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        s[4 * c + 2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        s[4 * c + 3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
      }
    } else {
      memcpy(s, t, 16);
    }
    for (int i = 0; i < 16; i++) s[i] ^= rk[16 * round + i];
  }
  memcpy(out, s, 16);
}

/**
 * The hash as aeshash.asm's header defines it, in C.
 * @param rk    round keys of the seed
 * @param msg   message bytes
 * @param len   message length
 * @param out   receives the 16-byte result (h1 = its first 8 bytes, little-endian)
 */
static void c_aeshash(const uint8_t rk[176], const uint8_t *msg, size_t len, uint8_t out[16]) {
  uint8_t state[16] = {0}, block[16], mask[16], ctr[16];
  size_t left = len;
  while (left > 16) {                             /* CBC chain over full blocks */
    for (int i = 0; i < 16; i++) block[i] = (uint8_t)(msg[i] ^ state[i]);
    c_encrypt(rk, block, state);
    msg += 16;
    left -= 16;
  }
  memset(block, 0, 16);                           /* the last block, zero-padded */
  memcpy(block, msg, left);
  memset(ctr, 0, 16);                             /* mask[left] = AES_K(left + 1) */
  ctr[0] = (uint8_t)(left + 1);
  c_encrypt(rk, ctr, mask);
  for (int i = 0; i < 16; i++) block[i] ^= (uint8_t)(mask[i] ^ state[i]);
  c_encrypt(rk, block, out);
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  make_sbox();
  uint64_t s = 0xA5A5F00DCAFEBABEull;

  /* 1. FIPS-197 Appendix C.1, through the C oracle (always runs). */
  static const uint8_t fkey[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
  static const uint8_t fpt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                  0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  static const uint8_t fct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                  0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
  uint8_t rk[176], ct[16];
  c_expand(fkey, rk);
  c_encrypt(rk, fpt, ct);
  CHECK(memcmp(ct, fct, 16) == 0);

  if (!kv_aes_available()) {
    /* No AES-NI: the store must refuse the mode and keep SipHash. */
    CHECK(kv_hash_best_mode() == KV_HASH_SIPHASH);
    CHECK(kv_set_hash_mode(KV_HASH_AES) < 0);
    CHECK(kv_hash_mode_get() == KV_HASH_SIPHASH);
    printf("  this CPU has no AES-NI: the AES hash itself was not exercised\n");
    return test_finish("test_aeshash");
  }

  /* 2. The key block against the C key schedule and the mask definition. */
  uint8_t *block = aligned_alloc(64, KV_AESKEY_BYTES);
  for (int rep = 0; rep < 200; rep++) {
    uint8_t seed[16];
    rng_fill(&s, seed, 16);
    if (rep == 0) memcpy(seed, fkey, 16);
    kv_aeshash_setup(seed, block);
    c_expand(seed, rk);
    CHECK(memcmp(block, rk, 176) == 0);
    for (int n = 0; n <= 16; n++) {
      uint8_t ctr[16] = {0}, m[16];
      ctr[0] = (uint8_t)(n + 1);
      c_encrypt(rk, ctr, m);
      CHECK(memcmp(block + 176 + 16 * n, m, 16) == 0);
    }
  }

  /* 3. Differential against the construction in C, every length 0..300. */
  static uint8_t buf[400];
  for (size_t len = 0; len <= 300; len++) {
    for (int rep = 0; rep < 6; rep++) {
      uint8_t seed[16], want[16];
      uint64_t out[2];
      rng_fill(&s, seed, 16);
      rng_fill(&s, buf, len);
      kv_aeshash_setup(seed, block);
      c_expand(seed, rk);
      c_aeshash(rk, buf, len, want);
      kv_aeshash128(buf, len, block, out);
      CHECK(memcmp(out, want, 16) == 0);
    }
  }

  /* 4. No overread: the message ends at a guard page. */
  for (size_t len = 0; len <= 96; len++) {
    void *base;
    uint8_t *p = guard_alloc(len, &base);
    uint8_t seed[16], want[16];
    uint64_t out[2];
    rng_fill(&s, p, len);
    rng_fill(&s, seed, 16);
    kv_aeshash_setup(seed, block);
    c_expand(seed, rk);
    c_aeshash(rk, p, len, want);
    kv_aeshash128(p, len, block, out);
    CHECK(memcmp(out, want, 16) == 0);
    guard_free(base);
  }

  /* 5. Contract: NULL out, and the register pair equals out[]. */
  {
    uint64_t out[2];
    rng_fill(&s, buf, 64);
    for (size_t len = 0; len <= 40; len += 5) {
      kv_aeshash128(buf, len, block, out);
      unsigned __int128 r = kv_aeshash128_r(buf, len, block, NULL);
      CHECK_EQ_U64((uint64_t)r, out[0]);
      CHECK_EQ_U64((uint64_t)(r >> 64), out[1]);
    }
    /* "ab" and "ab\0" differ only in length: the length mask separates them. */
    uint64_t a[2], b[2];
    kv_aeshash128("ab", 2, block, a);
    kv_aeshash128("ab", 3, block, b);            /* includes the string's NUL */
    CHECK(a[0] != b[0] || a[1] != b[1]);
  }
  free(block);

  /* 6. The store under each mode. */
  CHECK(kv_set_hash_mode(7) < 0);                /* unknown mode: refused, unchanged */
  CHECK(kv_hash_best_mode() == KV_HASH_AES);
  int before = kv_hash_mode_get();
  for (int mode = KV_HASH_SIPHASH; mode <= KV_HASH_AES; mode++) {
    CHECK(kv_set_hash_mode(mode) >= 0);
    CHECK(kv_hash_mode_get() == mode);
    uint8_t seed_a[16], seed_b[16];
    rng_fill(&s, seed_a, 16);
    rng_fill(&s, seed_b, 16);
    KvTable *ta = kv_store_create(30000, 1, seed_a, 0);
    KvTable *tb = kv_store_create(30000, 1, seed_b, 0);   /* its own key block */
    CHECK(ta != NULL && tb != NULL);
    if (!ta || !tb) break;
    kv_qsbr_online(ta, 0);
    kv_qsbr_online(tb, 0);
    enum { KEYS = 20000 };
    int placed_differently = 0;
    for (uint32_t i = 0; i < KEYS; i++) {
      char k[40];
      size_t kl = (size_t)snprintf(k, sizeof k, "aes-mode-key-%u", i * 2654435761u);
      uint64_t ha[2], hb[2];
      kv_store_hash(ta, k, kl, ha);
      kv_store_hash(tb, k, kl, hb);
      if (ha[0] != hb[0]) placed_differently++;   /* different seeds: unrelated hashes */
      CHECK(kv_store_put(ta, k, kl, &i, sizeof i, ha[0], ha[1]) == 1);
      CHECK(kv_store_put(tb, k, kl, &i, sizeof i, hb[0], hb[1]) == 1);
    }
    CHECK(placed_differently == KEYS);
    int found = 0;
    for (uint32_t i = 0; i < KEYS; i++) {
      char k[40];
      size_t kl = (size_t)snprintf(k, sizeof k, "aes-mode-key-%u", i * 2654435761u);
      uint64_t ha[2], hb[2];
      kv_store_hash(ta, k, kl, ha);
      kv_store_hash(tb, k, kl, hb);
      const KvRec *ra = kv_store_get(ta, k, kl, ha[0], ha[1]);
      const KvRec *rb = kv_store_get(tb, k, kl, hb[0], hb[1]);
      if (ra && rb && ra->val_len == sizeof i && memcmp(kv_rec_value(ra), &i, sizeof i) == 0 &&
          memcmp(kv_rec_value(rb), &i, sizeof i) == 0)
        found++;
    }
    CHECK(found == KEYS);
    kv_qsbr_offline(ta, 0);
    kv_qsbr_offline(tb, 0);
    kv_store_destroy(ta);
    kv_store_destroy(tb);
  }
  CHECK(kv_set_hash_mode(before) >= 0);          /* leave the process as it was */

  return test_finish("test_aeshash");
}
