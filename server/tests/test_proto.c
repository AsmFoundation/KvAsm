/*
 * test_proto.c — verifies the protobuf codec (proto.asm).
 *
 *  1. Well-formed messages: key only, key+value, reversed field order,
 *     unknown fields of every legal wire type interleaved, last-wins for
 *     repeated singular fields, empty message, zero-length key.
 *  2. Malformed messages: truncated/overlong/overflowing varints, lengths
 *     beyond the end, group and undefined wire types, field number 0, tags
 *     >= 2^32 — all must return -EINVAL.
 *  3. Differential fuzzing: 2M random and mutated messages decoded by the
 *     assembly and by an independent C reference decoder must agree exactly
 *     (status, pointers, lengths).
 *  4. Overread proof: messages ending at a guard page.
 *  5. Varint encoder: round-trips and boundary lengths.
 */
#include "kvnode_abi.h"
#include "test_common.h"

typedef struct {
  const uint8_t *key;
  uint64_t key_len;
  const uint8_t *value;
  uint64_t value_len;
  uint32_t has_key, has_value;
} PbKv;

int64_t kv_pb_decode_kv(const void *buf, size_t len, PbKv *out);
uint64_t kv_pb_varint_len(uint64_t v);
uint64_t kv_pb_write_varint(uint8_t *dst, uint64_t v);

/** Reference varint reader: 0 on success. */
static int ref_varint(const uint8_t **p, const uint8_t *end, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift <= 63; shift += 7) {
    if (*p >= end) return -1;
    uint8_t b = *(*p)++;
    if (shift == 63 && (b & 0x7F) > 1) return -1;
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) { *out = v; return 0; }
  }
  return -1;
}

/** Independent C decoder with the same contract as kv_pb_decode_kv. */
static int ref_decode(const uint8_t *buf, size_t len, PbKv *o) {
  memset(o, 0, sizeof *o);
  const uint8_t *p = buf, *end = buf + len;
  while (p < end) {
    uint64_t tag, v;
    if (ref_varint(&p, end, &tag) || (tag >> 3) == 0 || tag > 0xFFFFFFFFull) return -EINVAL;
    switch (tag & 7) {
      case 0: if (ref_varint(&p, end, &v)) return -EINVAL; break;
      case 1: if ((size_t)(end - p) < 8) return -EINVAL; p += 8; break;
      case 5: if ((size_t)(end - p) < 4) return -EINVAL; p += 4; break;
      case 2:
        if (ref_varint(&p, end, &v) || v > (uint64_t)(end - p)) return -EINVAL;
        if ((tag >> 3) == 1) { o->key = p; o->key_len = v; o->has_key = 1; }
        if ((tag >> 3) == 2) { o->value = p; o->value_len = v; o->has_value = 1; }
        p += v;
        break;
      default: return -EINVAL;
    }
  }
  return 0;
}

/** Decodes with both implementations and requires identical results. */
static void same(const uint8_t *buf, size_t len) {
  PbKv a, r;
  int64_t ra = kv_pb_decode_kv(buf, len, &a);
  int rr = ref_decode(buf, len, &r);
  CHECK(ra == rr);
  if (ra == 0 && rr == 0) {
    CHECK(a.has_key == r.has_key && a.key_len == r.key_len && a.key == r.key);
    CHECK(a.has_value == r.has_value && a.value_len == r.value_len && a.value == r.value);
  }
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  PbKv o;
  /* 1. well-formed */
  const uint8_t m1[] = {0x0A, 0x03, 'a', 'b', 'c'};
  CHECK(kv_pb_decode_kv(m1, sizeof m1, &o) == 0);
  CHECK(o.has_key && o.key_len == 3 && memcmp(o.key, "abc", 3) == 0 && !o.has_value);
  const uint8_t m2[] = {0x12, 0x02, 'v', 'v', 0x0A, 0x01, 'k'}; /* value first */
  CHECK(kv_pb_decode_kv(m2, sizeof m2, &o) == 0);
  CHECK(o.key_len == 1 && o.key[0] == 'k' && o.value_len == 2 && o.value[0] == 'v');
  const uint8_t m3[] = {0x18, 0x96, 0x01,                         /* field 3 varint */
                        0x21, 1, 2, 3, 4, 5, 6, 7, 8,             /* field 4 fixed64 */
                        0x2D, 1, 2, 3, 4,                         /* field 5 fixed32 */
                        0x32, 0x02, 'x', 'y',                     /* field 6 bytes */
                        0x0A, 0x01, 'A', 0x0A, 0x02, 'B', 'C',    /* key twice: last wins */
                        0x08, 0x01};                              /* field 1 as varint: unknown */
  CHECK(kv_pb_decode_kv(m3, sizeof m3, &o) == 0);
  CHECK(o.key_len == 2 && memcmp(o.key, "BC", 2) == 0);
  CHECK(kv_pb_decode_kv(NULL, 0, &o) == 0 && !o.has_key && !o.has_value);
  const uint8_t m4[] = {0x0A, 0x00};
  CHECK(kv_pb_decode_kv(m4, sizeof m4, &o) == 0 && o.has_key && o.key_len == 0);

  /* 2. malformed */
  const uint8_t b1[] = {0x0A};                     /* missing length */
  const uint8_t b2[] = {0x0A, 0x05, 'a'};          /* length beyond end */
  const uint8_t b3[] = {0x0B};                     /* wire type 3 (group) */
  const uint8_t b4[] = {0x0C};                     /* wire type 4 */
  const uint8_t b5[] = {0x0E};                     /* wire type 6 */
  const uint8_t b6[] = {0x02, 0x00};               /* field number 0 */
  const uint8_t b7[] = {0x08, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  const uint8_t b8[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};
  const uint8_t b9[] = {0x80, 0x80, 0x80, 0x80, 0x10}; /* tag = 2^32 */
  const uint8_t b10[] = {0x21, 1, 2, 3};           /* fixed64 truncated */
  const uint8_t b11[] = {0x0A, 0x80};              /* truncated length varint */
  const uint8_t *bad[] = {b1, b2, b3, b4, b5, b6, b7, b8, b9, b10, b11};
  const size_t badlen[] = {sizeof b1, sizeof b2, sizeof b3, sizeof b4, sizeof b5, sizeof b6,
                           sizeof b7, sizeof b8, sizeof b9, sizeof b10, sizeof b11};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(kv_pb_decode_kv(bad[i], badlen[i], &o) == -EINVAL);
  const uint8_t max10[] = {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  CHECK(kv_pb_decode_kv(max10, sizeof max10, &o) == 0); /* 2^64-1: longest legal varint */

  /* 3. differential fuzzing: random, and mutations of valid messages */
  uint64_t s = 0x5EED;
  static uint8_t buf[256];
  for (int i = 0; i < 1000000; i++) {
    size_t len = rng_next(&s) % 64;
    rng_fill(&s, buf, len);
    same(buf, len);
  }
  for (int i = 0; i < 1000000; i++) {
    size_t kl = rng_next(&s) % 40, vl = rng_next(&s) % 40, n = 0;
    buf[n++] = 0x0A; n += kv_pb_write_varint(buf + n, kl); rng_fill(&s, buf + n, kl); n += kl;
    buf[n++] = 0x12; n += kv_pb_write_varint(buf + n, vl); rng_fill(&s, buf + n, vl); n += vl;
    int flips = (int)(rng_next(&s) % 3);
    for (int f = 0; f < flips; f++) buf[rng_next(&s) % n] ^= (uint8_t)(1u << (rng_next(&s) % 8));
    same(buf, n - (rng_next(&s) % 2 ? rng_next(&s) % (n + 1) : 0));
  }

  /* 4. no overread at a guard page */
  for (size_t len = 0; len <= 40; len++) {
    void *base;
    uint8_t *p = guard_alloc(len, &base);
    for (size_t j = 0; j < len; j++) p[j] = 0x80 | (uint8_t)j; /* endless varints */
    same(p, len);
    if (len >= 2) { p[0] = 0x0A; p[1] = 0x7F; same(p, len); } /* length past the end */
    guard_free(base);
  }

  /* 5. varint encoder */
  uint8_t enc[16];
  const uint64_t edges[] = {0, 1, 127, 128, 16383, 16384, UINT32_MAX, UINT64_MAX};
  for (size_t i = 0; i < sizeof edges / sizeof edges[0] + 200000; i++) {
    uint64_t v = i < sizeof edges / sizeof edges[0] ? edges[i] : rng_next(&s) >> (rng_next(&s) % 64);
    uint64_t n = kv_pb_write_varint(enc, v), back = 0;
    const uint8_t *p = enc;
    CHECK(n == kv_pb_varint_len(v));
    CHECK(ref_varint(&p, enc + n, &back) == 0 && back == v && p == enc + n);
  }
  return test_finish("test_proto");
}
