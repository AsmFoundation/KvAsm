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
 *  6. DownloadRequest decoder (kv_pb_decode_download): fixed cases, then 2M
 *     random and mutated messages against an independent C reference.
 *  7. Streaming decoder (kv_pb_stream_feed, the Upload path): golden event
 *     sequences for hand-built streams; then random and mutated gRPC
 *     message streams, each fed to the assembly in random pieces (down to
 *     1 byte), whose event list must equal an independent byte-at-a-time C
 *     decoder's — which proves both the rules and split invariance; handler
 *     abort codes; and pieces that end at a guard page.
 */
#include "kvasm_abi.h"
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

/** Mirror of PbDl (proto.asm): the decoded DownloadRequest. */
typedef struct {
  const uint8_t *key;
  uint64_t key_len, offset, length, chunk;
  uint32_t has_key, has_length;
} PbDl;

int64_t kv_pb_decode_download(const void *buf, size_t len, PbDl *out);

/** Mirror of PbStream (include/pbstream.inc). */
typedef struct {
  uint64_t state, mleft, tag, fleft, hlen, msgs;
  uint8_t hbuf[16];
} PbStream;

/** Event codes and errors of include/pbstream.inc. */
enum {
  PBE_MSG_BEGIN = 1, PBE_VARINT, PBE_FIXED32, PBE_FIXED64, PBE_BYTES_BEGIN, PBE_BYTES,
  PBE_BYTES_END, PBE_MSG_END
};
enum { PB_ERR_MALFORMED = -1, PB_ERR_COMPRESSED = -2, PB_ERR_FLAG = -3 };

typedef int (*PbHandler)(void *ctx, int event, uint64_t tag, uint64_t a1, uint64_t a2);
int64_t kv_pb_stream_feed(PbStream *ps, const void *p, size_t n, void *ctx, PbHandler h);
int kv_pb_stream_idle(const PbStream *ps);

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

/**
 * Decodes a copy of a message that ends exactly at a PROT_NONE page, with
 * both implementations, and requires identical results: a decoder that read
 * one byte past the message would crash here.
 * @param msg  message bytes
 * @param len  message length (any: test_common.h's guard_alloc stops at one
 *             page, and the fast path's interesting lengths are longer)
 */
static void same_at_guard(const uint8_t *msg, size_t len);

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

static void same_at_guard(const uint8_t *msg, size_t len) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t body = (len + page - 1) / page * page;          /* whole pages holding the message */
  uint8_t *base = mmap(NULL, body + page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) { perror("mmap"); exit(2); }
  if (mprotect(base + body, page, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
  uint8_t *p = base + body - len;                         /* the message ends at the guard */
  if (len) memcpy(p, msg, len);
  same(p, len);
  munmap(base, body + page);
}

/* ========================================================================= */
/* 6. DownloadRequest                                                        */
/* ========================================================================= */

/**
 * Independent C decoder with kv_pb_decode_download's contract: key = field 1
 * (bytes), offset/length/chunk = fields 2/3/4 (varints), length's presence
 * recorded, everything else skipped, last value wins.
 * @param buf  message bytes
 * @param len  message length
 * @param o    receives the decoded fields
 * @returns 0 or -EINVAL, like the assembly
 */
static int ref_decode_dl(const uint8_t *buf, size_t len, PbDl *o) {
  memset(o, 0, sizeof *o);
  const uint8_t *p = buf, *end = buf + len;
  while (p < end) {
    uint64_t tag, v;
    if (ref_varint(&p, end, &tag) || (tag >> 3) == 0 || tag > 0xFFFFFFFFull) return -EINVAL;
    uint64_t field = tag >> 3;
    switch (tag & 7) {
      case 0:
        if (ref_varint(&p, end, &v)) return -EINVAL;
        if (field == 2) o->offset = v;
        if (field == 3) { o->length = v; o->has_length = 1; }
        if (field == 4) o->chunk = v;
        break;
      case 1: if ((size_t)(end - p) < 8) return -EINVAL; p += 8; break;
      case 5: if ((size_t)(end - p) < 4) return -EINVAL; p += 4; break;
      case 2:
        if (ref_varint(&p, end, &v) || v > (uint64_t)(end - p)) return -EINVAL;
        if (field == 1) { o->key = p; o->key_len = v; o->has_key = 1; }
        p += v;
        break;
      default: return -EINVAL;
    }
  }
  return 0;
}

/** Decodes a DownloadRequest with both implementations; they must agree. */
static void same_dl(const uint8_t *buf, size_t len) {
  PbDl a, r;
  int64_t ra = kv_pb_decode_download(buf, len, &a);
  int rr = ref_decode_dl(buf, len, &r);
  CHECK(ra == rr);
  if (ra == 0 && rr == 0) {
    CHECK(a.has_key == r.has_key && a.key_len == r.key_len && a.key == r.key);
    CHECK(a.offset == r.offset && a.length == r.length && a.has_length == r.has_length);
    CHECK(a.chunk == r.chunk);
  }
}

/** Section 6: fixed cases, malformed cases, differential fuzzing, guard page. */
static void test_download_request(void) {
  PbDl o;
  const uint8_t m1[] = {0x0A, 0x02, 'k', '1', 0x10, 0x80, 0x01, 0x18, 0x00, 0x20, 0x90, 0x4E};
  CHECK(kv_pb_decode_download(m1, sizeof m1, &o) == 0);
  CHECK(o.has_key && o.key_len == 2 && memcmp(o.key, "k1", 2) == 0);
  CHECK(o.offset == 128 && o.has_length && o.length == 0 && o.chunk == 10000);
  const uint8_t m2[] = {0x20, 0x05, 0x0A, 0x01, 'x'};   /* no length field: "to the end" */
  CHECK(kv_pb_decode_download(m2, sizeof m2, &o) == 0);
  CHECK(!o.has_length && o.length == 0 && o.offset == 0 && o.chunk == 5 && o.key_len == 1);
  const uint8_t m3[] = {0x12, 0x01, 'z',                 /* offset sent as bytes: skipped */
                        0x08, 0x07,                      /* key sent as a varint: skipped */
                        0x18, 0x05, 0x18, 0x09,          /* length twice: the last wins */
                        0x2D, 1, 2, 3, 4,                /* fixed32 field 5: skipped */
                        0x31, 1, 2, 3, 4, 5, 6, 7, 8};   /* fixed64 field 6: skipped */
  CHECK(kv_pb_decode_download(m3, sizeof m3, &o) == 0);
  CHECK(!o.has_key && o.offset == 0 && o.has_length && o.length == 9);
  CHECK(kv_pb_decode_download(NULL, 0, &o) == 0 && !o.has_key && !o.has_length);
  const uint8_t mx[] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01};
  CHECK(kv_pb_decode_download(mx, sizeof mx, &o) == 0 && o.offset == UINT64_MAX);

  const uint8_t b1[] = {0x10};                           /* offset value missing */
  const uint8_t b2[] = {0x0A, 0x03, 'a'};                /* key runs past the end */
  const uint8_t b3[] = {0x1B};                           /* wire type 3 (group) */
  const uint8_t b4[] = {0x02, 0x00};                     /* field number 0 */
  const uint8_t b5[] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02};
  const uint8_t b6[] = {0x80, 0x80, 0x80, 0x80, 0x10}; /* tag = 2^32 */
  const uint8_t b7[] = {0x25, 1, 2};                     /* fixed32 truncated */
  const uint8_t *bad[] = {b1, b2, b3, b4, b5, b6, b7};
  const size_t badlen[] = {sizeof b1, sizeof b2, sizeof b3, sizeof b4, sizeof b5, sizeof b6,
                           sizeof b7};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    CHECK(kv_pb_decode_download(bad[i], badlen[i], &o) == -EINVAL);

  uint64_t s = 0xD0D0;
  static uint8_t buf[256];
  for (int i = 0; i < 1000000; i++) {
    size_t len = rng_next(&s) % 64;
    rng_fill(&s, buf, len);
    same_dl(buf, len);
  }
  for (int i = 0; i < 1000000; i++) {
    size_t kl = rng_next(&s) % 40, n = 0;
    buf[n++] = 0x0A; n += kv_pb_write_varint(buf + n, kl); rng_fill(&s, buf + n, kl); n += kl;
    buf[n++] = 0x10; n += kv_pb_write_varint(buf + n, rng_next(&s) >> (rng_next(&s) % 64));
    if (rng_next(&s) % 2) { buf[n++] = 0x18; n += kv_pb_write_varint(buf + n, rng_next(&s) % 100000); }
    buf[n++] = 0x20; n += kv_pb_write_varint(buf + n, rng_next(&s) >> (rng_next(&s) % 64));
    int flips = (int)(rng_next(&s) % 3);
    for (int f = 0; f < flips; f++) buf[rng_next(&s) % n] ^= (uint8_t)(1u << (rng_next(&s) % 8));
    same_dl(buf, n - (rng_next(&s) % 2 ? rng_next(&s) % (n + 1) : 0));
  }
  for (size_t len = 0; len <= 40; len++) {
    void *base;
    uint8_t *p = guard_alloc(len, &base);
    for (size_t j = 0; j < len; j++) p[j] = 0x80 | (uint8_t)j; /* endless varints */
    same_dl(p, len);
    if (len >= 2) { p[0] = 0x0A; p[1] = 0x7F; same_dl(p, len); } /* key length past the end */
    guard_free(base);
  }
}

/* ========================================================================= */
/* 7. Streaming decoder                                                      */
/* ========================================================================= */

/** One decoder event. Consecutive PBE_BYTES pieces of a payload are joined. */
typedef struct {
  int type;
  uint64_t tag, a1;   /**< a1: length or value where the event defines one, else 0 */
  uint8_t *data;      /**< PBE_BYTES: the payload bytes (malloc'd) */
  size_t len;
} Ev;

/** A growable list of events. */
typedef struct {
  Ev *v;
  size_t n, cap;
} EvList;

/** Frees every event's data and the list. */
static void ev_free(EvList *l) {
  for (size_t i = 0; i < l->n; i++) free(l->v[i].data);
  free(l->v);
  memset(l, 0, sizeof *l);
}

/**
 * Appends an event, joining a PBE_BYTES piece to the previous one of the
 * same field so that split points do not show in the list.
 * @param l     list
 * @param type  PBE_* code
 * @param tag   field tag (0 for message events)
 * @param a1    length or value (see Ev)
 * @param data  PBE_BYTES bytes, else NULL
 * @param len   PBE_BYTES length, else 0
 */
static void ev_add(EvList *l, int type, uint64_t tag, uint64_t a1, const uint8_t *data, size_t len) {
  if (type == PBE_BYTES && l->n && l->v[l->n - 1].type == PBE_BYTES && l->v[l->n - 1].tag == tag) {
    Ev *e = &l->v[l->n - 1];
    e->data = realloc(e->data, e->len + len);
    if (!e->data) { perror("realloc"); exit(2); }
    memcpy(e->data + e->len, data, len);
    e->len += len;
    return;
  }
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 16;
    l->v = realloc(l->v, l->cap * sizeof *l->v);
    if (!l->v) { perror("realloc"); exit(2); }
  }
  Ev e = {type, tag, a1, NULL, 0};
  if (type == PBE_BYTES) {
    e.data = malloc(len ? len : 1);
    if (!e.data) { perror("malloc"); exit(2); }
    memcpy(e.data, data, len);
    e.len = len;
  }
  l->v[l->n++] = e;
}

/**
 * Handler that records events (the argument each event defines, 0 for the
 * others, whose registers carry nothing meaningful).
 * @returns 0 (continue), or -99 when a PBE_BYTES piece is empty — the
 *          contract promises length > 0, so an empty piece is a bug.
 */
static int rec_handler(void *ctx, int event, uint64_t tag, uint64_t a1, uint64_t a2) {
  EvList *l = ctx;
  switch (event) {
    case PBE_BYTES:
      if (a2 == 0) return -99;
      ev_add(l, event, tag, 0, (const uint8_t *)(uintptr_t)a1, a2);
      break;
    case PBE_MSG_BEGIN: case PBE_VARINT: case PBE_FIXED32: case PBE_FIXED64: case PBE_BYTES_BEGIN:
      ev_add(l, event, event == PBE_MSG_BEGIN ? 0 : tag, a1, NULL, 0);
      break;
    default: /* PBE_BYTES_END (tag only), PBE_MSG_END (nothing) */
      ev_add(l, event, event == PBE_BYTES_END ? tag : 0, 0, NULL, 0);
      break;
  }
  return 0;
}

/**
 * Independent reference: decodes a whole stream one byte at a time with the
 * rules stated in include/pbstream.inc and proto.asm's header (written from
 * that specification, not from the assembly). At the end of the input it
 * stops where the assembly would wait for more.
 * @param p     stream bytes
 * @param n     stream length
 * @param out   receives the events
 * @param idle  receives 1 when the stream ended between messages
 * @returns 0, or the PB_ERR_* code of the first violation
 */
static int ref_stream(const uint8_t *p, size_t n, EvList *out, int *idle) {
  enum { PFX, TAG, VAR, LEN, BYT, FIX } st = PFX;
  uint8_t h[16];
  size_t hl = 0;
  uint64_t mleft = 0, tag = 0, fleft = 0;
  *idle = 0;
  for (size_t i = 0; i < n;) {
    if (st == PFX) {
      h[hl++] = p[i++];
      if (hl < 5) continue;
      hl = 0;
      if (h[0] == 1) return PB_ERR_COMPRESSED;
      if (h[0] > 1) return PB_ERR_FLAG;
      mleft = (uint64_t)h[1] << 24 | (uint64_t)h[2] << 16 | (uint64_t)h[3] << 8 | h[4];
      ev_add(out, PBE_MSG_BEGIN, 0, mleft, NULL, 0);
      if (mleft == 0) ev_add(out, PBE_MSG_END, 0, 0, NULL, 0);
      else st = TAG;
      continue;
    }
    if (st == BYT) {
      ev_add(out, PBE_BYTES, tag, 0, p + i, 1);
      i++;
      fleft--;
      mleft--;
      if (fleft) continue;
      ev_add(out, PBE_BYTES_END, tag, 0, NULL, 0);
    } else if (st == FIX) {
      h[hl++] = p[i++];
      fleft--;
      mleft--;
      if (fleft) continue;
      uint64_t v = 0;
      for (size_t k = 0; k < hl; k++) v |= (uint64_t)h[k] << (8 * k);
      ev_add(out, (tag & 7) == 5 ? PBE_FIXED32 : PBE_FIXED64, tag, v, NULL, 0);
      hl = 0;
    } else { /* TAG, VAR, LEN: one varint byte, inside the message */
      if (mleft == 0) return PB_ERR_MALFORMED;
      uint8_t b = p[i++];
      mleft--;
      h[hl++] = b;
      if (b & 0x80) {
        if (hl >= 10) return PB_ERR_MALFORMED;
        if (mleft == 0) return PB_ERR_MALFORMED;  /* the message ends inside it: now */
        continue;
      }
      uint64_t v = 0;
      for (size_t k = 0; k < hl; k++) {
        uint64_t d = h[k] & 0x7F;
        if (k == 9 && d > 1) return PB_ERR_MALFORMED;
        v |= d << (7 * k);
      }
      hl = 0;
      if (st == TAG) {
        if ((v >> 3) == 0 || v > 0xFFFFFFFFull) return PB_ERR_MALFORMED;
        tag = v;
        switch (v & 7) {       /* a tag ending its message has no room for a value */
          case 0: st = VAR; if (mleft == 0) return PB_ERR_MALFORMED; continue;
          case 2: st = LEN; if (mleft == 0) return PB_ERR_MALFORMED; continue;
          case 1: fleft = 8; break;
          case 5: fleft = 4; break;
          default: return PB_ERR_MALFORMED;
        }
        if (fleft > mleft) return PB_ERR_MALFORMED;
        st = FIX;
        continue;
      }
      if (st == VAR) {
        ev_add(out, PBE_VARINT, tag, v, NULL, 0);
      } else { /* LEN */
        if (v > mleft) return PB_ERR_MALFORMED;
        ev_add(out, PBE_BYTES_BEGIN, tag, v, NULL, 0);
        fleft = v;
        if (fleft) { st = BYT; continue; }
        ev_add(out, PBE_BYTES_END, tag, 0, NULL, 0);
      }
    }
    /* a field is complete */
    if (mleft == 0) {
      ev_add(out, PBE_MSG_END, 0, 0, NULL, 0);
      st = PFX;
    } else {
      st = TAG;
    }
  }
  *idle = st == PFX && hl == 0;
  return 0;
}

/**
 * Feeds a stream to the assembly in random pieces (sometimes 1 byte each,
 * sometimes whole, plus empty pieces) and records its events.
 * @param p     stream bytes
 * @param n     stream length
 * @param s     PRNG state (chooses the split points)
 * @param out   receives the events
 * @param idle  receives kv_pb_stream_idle after the last piece
 * @returns the first non-zero feed result, else 0
 */
static int asm_stream(const uint8_t *p, size_t n, uint64_t *s, EvList *out, int *idle) {
  PbStream ps;
  memset(&ps, 0, sizeof ps);
  static const size_t maxes[] = {1, 2, 3, 7, 17, 64, 1u << 30};
  size_t max = maxes[rng_next(s) % (sizeof maxes / sizeof maxes[0])];
  size_t off = 0;
  *idle = 0;
  while (off < n) {
    size_t k = 1 + rng_next(s) % (max < n - off ? max : n - off);
    if (rng_next(s) % 16 == 0) {                          /* an empty piece changes nothing */
      int64_t rc0 = kv_pb_stream_feed(&ps, p + off, 0, out, rec_handler);
      if (rc0 != 0) return (int)rc0;
    }
    int64_t rc = kv_pb_stream_feed(&ps, p + off, k, out, rec_handler);
    if (rc != 0) return (int)rc;
    off += k;
  }
  *idle = kv_pb_stream_idle(&ps);
  return 0;
}

/**
 * Compares two event lists.
 * @returns 1 when identical (types, tags, arguments and payload bytes)
 */
static int ev_equal(const EvList *a, const EvList *b) {
  if (a->n != b->n) return 0;
  for (size_t i = 0; i < a->n; i++) {
    const Ev *x = &a->v[i], *y = &b->v[i];
    if (x->type != y->type || x->tag != y->tag || x->a1 != y->a1 || x->len != y->len) return 0;
    if (x->len && memcmp(x->data, y->data, x->len) != 0) return 0;
  }
  return 1;
}

/**
 * Appends a protobuf field with a random value of the given wire type.
 * @param b     output buffer (room for 2 * 256 bytes)
 * @param n     current length, advanced
 * @param s     PRNG state
 * @param tag   tag to write (any value: invalid ones are the point sometimes)
 */
static void gen_field(uint8_t *b, size_t *n, uint64_t *s, uint64_t tag) {
  *n += kv_pb_write_varint(b + *n, tag);
  switch (tag & 7) {
    case 0: *n += kv_pb_write_varint(b + *n, rng_next(s) >> (rng_next(s) % 64)); break;
    case 1: rng_fill(s, b + *n, 8); *n += 8; break;
    case 5: rng_fill(s, b + *n, 4); *n += 4; break;
    case 2: {
      size_t l = rng_next(s) % 120;
      *n += kv_pb_write_varint(b + *n, l);
      rng_fill(s, b + *n, l);
      *n += l;
      break;
    }
    default: break; /* invalid wire types have no payload */
  }
}

/**
 * Builds a random gRPC message stream, usually well-formed, sometimes with a
 * compressed/undefined flag, a wrong length prefix, an invalid tag, bit
 * flips or a truncated end.
 * @param s    PRNG state
 * @param buf  output (room for 4 * (5 + 8 * 256) bytes)
 * @returns the stream length
 */
static size_t gen_stream(uint64_t *s, uint8_t *buf) {
  size_t n = 0;
  int msgs = (int)(rng_next(s) % 5);
  for (int m = 0; m < msgs; m++) {
    uint8_t body[8 * 256];
    size_t bl = 0;
    int fields = (int)(rng_next(s) % 7);
    for (int f = 0; f < fields; f++) {
      uint64_t field = 1 + rng_next(s) % 6;
      static const int wts[] = {0, 1, 2, 5, 2, 0, 3, 7};
      int wt = wts[rng_next(s) % (rng_next(s) % 10 == 0 ? 8 : 6)];
      uint64_t tag = field << 3 | (uint64_t)wt;
      if (rng_next(s) % 50 == 0) tag = rng_next(s) % 2 ? 0x02 /* field 0 */ : 0x100000000ull;
      gen_field(body, &bl, s, tag);
    }
    uint8_t flag = rng_next(s) % 40 == 0 ? (uint8_t)(1 + rng_next(s) % 3) : 0;
    uint32_t len = (uint32_t)bl;
    if (rng_next(s) % 20 == 0) len += (uint32_t)(rng_next(s) % 5) - 2; /* lie by -2..+2 */
    buf[n++] = flag;
    buf[n++] = (uint8_t)(len >> 24);
    buf[n++] = (uint8_t)(len >> 16);
    buf[n++] = (uint8_t)(len >> 8);
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, body, bl);
    n += bl;
  }
  if (n && rng_next(s) % 8 == 0) buf[rng_next(s) % n] ^= (uint8_t)(1u << (rng_next(s) % 8));
  if (n && rng_next(s) % 6 == 0) n = rng_next(s) % n;     /* stream cut short */
  return n;
}

/** Handler that aborts with -42 at its third event (checks code propagation). */
static int abort_handler(void *ctx, int event, uint64_t tag, uint64_t a1, uint64_t a2) {
  (void)event; (void)tag; (void)a1; (void)a2;
  int *count = ctx;
  return ++*count == 3 ? -42 : 0;
}

/** Section 7: golden sequences, differential split fuzzing, aborts, guard pages. */
static void test_stream_decoder(void) {
  /* golden: {1:"ab", 2:5, 3:"hello"} then {4: fixed32 0x11223344} then {} */
  const uint8_t g[] = {0, 0, 0, 0, 13, 0x0A, 2, 'a', 'b', 0x10, 5, 0x1A, 5, 'h', 'e', 'l', 'l', 'o',
                       0, 0, 0, 0, 5, 0x25, 0x44, 0x33, 0x22, 0x11,
                       0, 0, 0, 0, 0};
  EvList want = {0}, got = {0};
  ev_add(&want, PBE_MSG_BEGIN, 0, 13, NULL, 0);
  ev_add(&want, PBE_BYTES_BEGIN, 0x0A, 2, NULL, 0);
  ev_add(&want, PBE_BYTES, 0x0A, 0, (const uint8_t *)"ab", 2);
  ev_add(&want, PBE_BYTES_END, 0x0A, 0, NULL, 0);
  ev_add(&want, PBE_VARINT, 0x10, 5, NULL, 0);
  ev_add(&want, PBE_BYTES_BEGIN, 0x1A, 5, NULL, 0);
  ev_add(&want, PBE_BYTES, 0x1A, 0, (const uint8_t *)"hello", 5);
  ev_add(&want, PBE_BYTES_END, 0x1A, 0, NULL, 0);
  ev_add(&want, PBE_MSG_END, 0, 0, NULL, 0);
  ev_add(&want, PBE_MSG_BEGIN, 0, 5, NULL, 0);
  ev_add(&want, PBE_FIXED32, 0x25, 0x11223344, NULL, 0);
  ev_add(&want, PBE_MSG_END, 0, 0, NULL, 0);
  ev_add(&want, PBE_MSG_BEGIN, 0, 0, NULL, 0);
  ev_add(&want, PBE_MSG_END, 0, 0, NULL, 0);
  uint64_t s = 0x57AE;
  for (int split = 0; split < 200; split++) {             /* every split pattern agrees */
    int idle;
    CHECK(asm_stream(g, sizeof g, &s, &got, &idle) == 0 && idle);
    CHECK(ev_equal(&got, &want));
    ev_free(&got);
  }
  ev_free(&want);

  /* flags, fields crossing their message, idle states */
  PbStream ps;
  EvList sink = {0};
  const uint8_t c1[] = {1, 0, 0, 0, 0};
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, c1, sizeof c1, &sink, rec_handler) == PB_ERR_COMPRESSED);
  const uint8_t c2[] = {7, 0, 0, 0, 0};
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, c2, sizeof c2, &sink, rec_handler) == PB_ERR_FLAG);
  const uint8_t x1[] = {0, 0, 0, 0, 3, 0x0A, 0x05, 'a'}; /* 5-byte payload in a 3-byte message */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x1, sizeof x1, &sink, rec_handler) == PB_ERR_MALFORMED);
  const uint8_t x2[] = {0, 0, 0, 0, 4, 0x09, 1, 2, 3};   /* fixed64 in 3 remaining bytes */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x2, sizeof x2, &sink, rec_handler) == PB_ERR_MALFORMED);
  const uint8_t x3[] = {0, 0, 0, 0, 1, 0x08, 0x08};      /* the varint's value is outside */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x3, sizeof x3, &sink, rec_handler) == PB_ERR_MALFORMED);
  /* A message whose LAST byte leaves a field unfinished is malformed at that
   * byte, even with nothing after it: waiting would misreport it as a stream
   * cut inside a message at END_STREAM (or hang a client awaiting a reply). */
  const uint8_t x4[] = {0, 0, 0, 0, 2, 0x08, 0x80};      /* a varint wanting more */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x4, sizeof x4, &sink, rec_handler) == PB_ERR_MALFORMED);
  const uint8_t x5[] = {0, 0, 0, 0, 1, 0x08};            /* a varint tag, no value */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x5, sizeof x5, &sink, rec_handler) == PB_ERR_MALFORMED);
  const uint8_t x6[] = {0, 0, 0, 0, 1, 0x0A};            /* a bytes tag, no length */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x6, sizeof x6, &sink, rec_handler) == PB_ERR_MALFORMED);
  const uint8_t x7[] = {0, 0, 0, 0, 3, 0x78, 0x80, 0x80}; /* value varint cut, 2 pieces */
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, x7, 7, &sink, rec_handler) == 0);   /* 1 byte of it left */
  CHECK(kv_pb_stream_feed(&ps, x7 + 7, 1, &sink, rec_handler) == PB_ERR_MALFORMED);
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_idle(&ps));
  CHECK(kv_pb_stream_feed(&ps, g, 3, &sink, rec_handler) == 0 && !kv_pb_stream_idle(&ps));
  CHECK(kv_pb_stream_feed(&ps, g + 3, 12, &sink, rec_handler) == 0 && !kv_pb_stream_idle(&ps));
  CHECK(kv_pb_stream_feed(&ps, g + 15, sizeof g - 15, &sink, rec_handler) == 0);
  CHECK(kv_pb_stream_idle(&ps) && ps.msgs == 3);
  ev_free(&sink);

  /* the handler's negative code stops the feed and is returned as is */
  int count = 0;
  memset(&ps, 0, sizeof ps);
  CHECK(kv_pb_stream_feed(&ps, g, sizeof g, &count, abort_handler) == -42 && count == 3);

  /* differential: random streams, random pieces, independent reference */
  static uint8_t buf[4 * (5 + 8 * 256)];
  int mism = 0;
  for (int i = 0; i < 200000 && mism < 5; i++) {
    size_t n = gen_stream(&s, buf);
    EvList a = {0}, r = {0};
    int ia, ir;
    int ra = asm_stream(buf, n, &s, &a, &ia);
    int rr = ref_stream(buf, n, &r, &ir);
    int ok = ra == rr && ev_equal(&a, &r) && (ra != 0 || ia == ir);
    if (!ok) {
      fprintf(stderr, "  stream case %d: asm %d/%zu events/idle %d, ref %d/%zu/%d (len %zu)\n", i,
              ra, a.n, ia, rr, r.n, ir, n);
      mism++;
    }
    CHECK(ok);
    ev_free(&a);
    ev_free(&r);
  }

  /* pieces that end at a guard page: never read past a piece */
  for (int i = 0; i < 2000; i++) {
    size_t n = gen_stream(&s, buf);
    if (n == 0 || n > 4000) continue;
    size_t cut = rng_next(&s) % n;
    void *b1, *b2;
    uint8_t *p1 = guard_alloc(cut, &b1), *p2 = guard_alloc(n - cut, &b2);
    memcpy(p1, buf, cut);
    memcpy(p2, buf + cut, n - cut);
    EvList a = {0}, r = {0};
    memset(&ps, 0, sizeof ps);
    int ra = (int)kv_pb_stream_feed(&ps, p1, cut, &a, rec_handler);
    if (ra == 0) ra = (int)kv_pb_stream_feed(&ps, p2, n - cut, &a, rec_handler);
    int ir, rr = ref_stream(buf, n, &r, &ir);
    CHECK(ra == rr && ev_equal(&a, &r));
    ev_free(&a);
    ev_free(&r);
    guard_free(b1);
    guard_free(b2);
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

  /* 3b. the decoder's fast path, shape by shape: {1: key} and {1: key,
   * 2: value} with every key length around its one-byte limit and value
   * lengths around each varint size, the lengths also written non-minimally
   * (a longer varint for the same number), and every such message truncated
   * at every byte — each ending exactly at a guard page, so an accepted
   * shape that read one byte too far would crash. */
  {
    static const size_t kKeyLens[] = {0, 1, 2, 16, 64, 126, 127, 128, 129, 300};
    static const size_t kValLens[] = {0, 1, 2, 100, 126, 127, 128, 129, 1000, 16383, 16384, 16385,
                                      70000};
    static uint8_t msg[72000];
    for (size_t ki = 0; ki < sizeof kKeyLens / sizeof kKeyLens[0]; ki++)
      for (size_t vi = 0; vi <= sizeof kValLens / sizeof kValLens[0]; vi++)   /* last: no value */
        for (int pad = 0; pad < 3; pad++) {                                  /* extra varint bytes */
          size_t kl = kKeyLens[ki], n = 0;
          int with_value = vi < sizeof kValLens / sizeof kValLens[0];
          size_t vl = with_value ? kValLens[vi] : 0;
          msg[n++] = 0x0A;
          n += kv_pb_write_varint(msg + n, kl);
          rng_fill(&s, msg + n, kl);
          n += kl;
          if (with_value) {
            msg[n++] = 0x12;
            n += kv_pb_write_varint(msg + n, vl);
            for (int e = 0; e < pad; e++) {       /* non-minimal: 0x80 continuation + 0x00 */
              msg[n - 1] |= 0x80;
              msg[n++] = 0x00;
            }
            rng_fill(&s, msg + n, vl);
            n += vl;
          } else if (pad) {
            continue;                             /* nothing to pad without a value */
          }
          /* whole, and cut at every byte for the short ones (every header
           * byte and a few payload bytes for the long ones) */
          size_t cuts = n < 400 ? n : 12;
          for (size_t c = 0; c <= cuts; c++) {
            size_t len = c == 0 ? n : (n < 400 ? n - c : (c <= 6 ? c : n - (c - 6)));
            same_at_guard(msg, len);
          }
        }
    /* trailing bytes after a canonical pair: another field, and garbage */
    uint8_t tail[] = {0x0A, 0x01, 'k', 0x12, 0x01, 'v', 0x18, 0x05};
    same(tail, sizeof tail);
    same(tail, sizeof tail - 1);
    uint8_t twice[] = {0x0A, 0x01, 'k', 0x0A, 0x01, 'j'};   /* a repeated key: the last wins */
    same(twice, sizeof twice);
    uint8_t value_first[] = {0x12, 0x01, 'v', 0x0A, 0x01, 'k'};
    same(value_first, sizeof value_first);
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

  /* 6. DownloadRequest */
  test_download_request();
  /* 7. streaming decoder (Upload) */
  test_stream_decoder();
  return test_finish("test_proto");
}
