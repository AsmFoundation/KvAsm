/*
 * test_util.c — verifies kv_keyeq and kv_tag_match (util.asm).
 *
 * kv_keyeq: every length 0..200, equal buffers compare equal, a single
 * flipped bit at every position compares unequal, and buffers ending at a
 * guard page never fault (no overread of either operand).
 * kv_tag_match: 2M random tag words x edge-case bytes against a byte loop —
 * the SWAR result must be exact, never a false positive or negative.
 */
#include "test_common.h"

int kv_keyeq(const void *a, const void *b, size_t len);
uint64_t kv_tag_match(uint64_t tags, uint64_t tag);

/** Byte-loop oracle for kv_tag_match. */
static uint64_t ref_tag_match(uint64_t tags, uint8_t tag) {
  uint64_t r = 0;
  for (int i = 0; i < 8; i++)
    if ((uint8_t)(tags >> (8 * i)) == tag) r |= 0x80ull << (8 * i);
  return r;
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  uint64_t s = 0x243F6A8885A308D3ull;
  static uint8_t a[256], b[256];

  for (size_t len = 0; len <= 200; len++) {
    rng_fill(&s, a, len);
    memcpy(b, a, len);
    CHECK(kv_keyeq(a, b, len) == 1);
    for (size_t pos = 0; pos < len; pos++) {
      b[pos] ^= (uint8_t)(1u << (pos % 8));
      CHECK(kv_keyeq(a, b, len) == 0);
      b[pos] = a[pos];
    }
  }

  /* Both operands end at guard pages: any overread faults. */
  for (size_t len = 0; len <= 64; len++) {
    void *ba, *bb;
    uint8_t *pa = guard_alloc(len, &ba), *pb = guard_alloc(len, &bb);
    rng_fill(&s, pa, len);
    memcpy(pb, pa, len);
    CHECK(kv_keyeq(pa, pb, len) == 1);
    if (len) {
      pb[len - 1] ^= 0x80;
      CHECK(kv_keyeq(pa, pb, len) == 0);
    }
    guard_free(ba);
    guard_free(bb);
  }

  static const uint8_t edge[] = {0x00, 0x01, 0x7f, 0x80, 0x81, 0xfe, 0xff};
  for (int iter = 0; iter < 2000000; iter++) {
    uint8_t tag = (uint8_t)rng_next(&s);
    uint64_t tags = 0, r = rng_next(&s);
    for (int i = 0; i < 8; i++) {
      uint8_t pick = (uint8_t)(r >> (8 * i));
      uint8_t byte = (pick & 3) == 0 ? tag
                   : (pick & 3) == 1 ? edge[pick % sizeof edge]
                   : (pick & 3) == 2 ? (uint8_t)(tag ^ (1u << (pick % 8)))
                                     : (uint8_t)rng_next(&s);
      tags |= (uint64_t)byte << (8 * i);
    }
    CHECK_EQ_U64(kv_tag_match(tags, tag), ref_tag_match(tags, tag));
  }

  return test_finish("test_util");
}
