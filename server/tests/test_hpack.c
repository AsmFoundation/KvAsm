/*
 * test_hpack.c — the native HPACK decoder (hpack.asm) against RFC 7541 and,
 * differentially, against nghttp2's own HPACK implementation.
 *
 * Why differential: an HPACK decoder that disagrees with a real encoder by
 * one table entry corrupts every later header on the connection. nghttp2 is
 * linked here only as an oracle: it encodes random header lists (choosing
 * indexing and Huffman coding itself, with random table-size changes) and
 * decodes the RFC vectors; hpack.asm must produce exactly the same fields.
 *
 * Checks:
 *   * RFC 7541 Appendix C.2-C.6 vectors, including the dynamic table size
 *     after each block (C.5/C.6 run with a 256-byte table and exercise
 *     eviction); every vector is also decoded by nghttp2, so a mistyped
 *     vector fails loudly instead of "passing";
 *   * Huffman: RFC strings, 7 vs 8+ padding bits, zero padding, EOS;
 *   * malformed blocks: index 0, index past the table, size update after a
 *     field or above the limit, truncated integers and strings;
 *   * oversize strings: flagged, skipped, and table state still correct;
 *   * classification (meanings) of names and values;
 *   * 30,000 random blocks through nghttp2's deflater and our decoder.
 */
#include <nghttp2/nghttp2.h>

#include "kvasm_abi.h"
#include "test_common.h"

/* ---- hpack.asm (layout mirrored from include/hpack.inc) ---- */
typedef struct {
  void *ents;
  uint64_t ring_mask, head, count, size, max, limit;
  void *pool;
  uint64_t gen; /* table generation (hpack.inc) */
} HpackDec;

typedef int (*FieldHandler)(void *ctx, const uint8_t *name, size_t nlen, const uint8_t *value,
                            size_t vlen, uint64_t meaning);
void kv_hpack_global_init(void);
int64_t kv_hpack_init(HpackDec *d, void *pool, uint64_t limit);
void kv_hpack_free(HpackDec *d);
int64_t kv_hpack_decode(HpackDec *d, const uint8_t *block, size_t len, void *ctx, FieldHandler h,
                        const uint64_t scratch[2]);
int64_t kv_huff_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t cap);
uint32_t kv_hpack_classify(const void *name, size_t nlen, const void *value, size_t vlen);
extern const uint64_t kv_pool_struct_size;
extern const uint64_t kv_hpack_abi[];

enum { HF_OTHER, HF_METHOD, HF_SCHEME, HF_AUTHORITY, HF_PATH, HF_STATUS, HF_PSEUDO_UNKNOWN,
       HF_BAD_NAME, HF_CONTENT_TYPE, HF_TE, HF_GRPC_ENCODING, HF_CONTENT_LENGTH, HF_CONN_SPECIFIC };
enum { VC_EMPTY = 0x80, VC_POST = 1, VC_HEAD = 2, VC_OK = 1 };
enum { M_UNKNOWN, M_GET, M_PUT, M_DELETE, M_HEALTH, M_UPLOAD, M_DOWNLOAD };
#define HF_VALUE_BAD 0x10000u
#define HF_OVERSIZE 0x20000u
#define HPACK_ERR_COMPRESSION (-1)
#define HPACK_ERR_NOMEM (-2)

/* ---- field collection ---- */
enum { MAX_FIELDS = 64, MAX_STR = 4096 };
/** One decoded field, copied out of the decoder's transient buffers. */
typedef struct {
  uint8_t name[MAX_STR], value[MAX_STR];
  size_t nlen, vlen;
  uint64_t meaning;
} Field;
/** The fields of one block, in order. */
typedef struct {
  int n;
  Field f[MAX_FIELDS];
} Fields;

/**
 * Handler that records each decoded field (copying: pointers are transient).
 * @return 0 to continue, 99 (stops decoding) when more than MAX_FIELDS arrive.
 */
static int collect(void *ctx, const uint8_t *name, size_t nlen, const uint8_t *value, size_t vlen,
                   uint64_t meaning) {
  Fields *fs = ctx;
  if (fs->n >= MAX_FIELDS) return 99;
  Field *f = &fs->f[fs->n++];
  f->nlen = nlen;
  f->vlen = vlen;
  f->meaning = meaning;
  if (!(meaning & HF_OVERSIZE)) {
    memcpy(f->name, name, nlen < MAX_STR ? nlen : MAX_STR);
    memcpy(f->value, value, vlen < MAX_STR ? vlen : MAX_STR);
  }
  return 0;
}

static uint8_t g_scratch_buf[16384];
static const uint64_t g_scratch[2] = {(uint64_t)(uintptr_t)g_scratch_buf, sizeof g_scratch_buf};
static void *g_pool;

/** Parses "4082 94e7" style hex (spaces ignored). @return byte count. */
static size_t unhex(const char *s, uint8_t *out) {
  size_t n = 0;
  while (*s) {
    if (*s == ' ') { s++; continue; }
    unsigned v;
    sscanf(s, "%2x", &v);
    out[n++] = (uint8_t)v;
    s += 2;
  }
  return n;
}

/**
 * Encodes an HPACK integer (RFC 7541 §5.1).
 * @param out destination (>= 6 bytes). @param v value.
 * @param prefix N, the prefix width in bits (4..7).
 * @param pattern the representation's high bits of the first byte.
 * @return bytes written.
 */
static size_t enc_int(uint8_t *out, uint64_t v, int prefix, uint8_t pattern) {
  uint64_t max = ((uint64_t)1 << prefix) - 1;
  if (v < max) {
    out[0] = (uint8_t)(pattern | v);
    return 1;
  }
  size_t n = 0;
  out[n++] = (uint8_t)(pattern | max);
  for (v -= max; v >= 128; v >>= 7) out[n++] = (uint8_t)(0x80 | (v & 0x7F));
  out[n++] = (uint8_t)v;
  return n;
}

/**
 * Appends a raw (non-Huffman) string literal.
 * @param out destination. @param s bytes. @param n length.
 * @return bytes written.
 */
static size_t enc_raw(uint8_t *out, const void *s, size_t n) {
  size_t k = enc_int(out, n, 7, 0x00);
  memcpy(out + k, s, n);
  return k + n;
}

/** One expected header field. */
typedef struct { const char *name, *value; } Nv;

/**
 * Decodes one block with nghttp2's inflater into `out`.
 * @return field count, or -1 on an inflate error.
 */
static int oracle_decode(nghttp2_hd_inflater *inf, const uint8_t *in, size_t len, Fields *out) {
  out->n = 0;
  for (;;) {
    nghttp2_nv nv;
    int flags = 0;
    nghttp2_ssize rv = nghttp2_hd_inflate_hd3(inf, &nv, &flags, in, len, 1);
    if (rv < 0) return -1;
    in += rv;
    len -= (size_t)rv;
    if (flags & NGHTTP2_HD_INFLATE_EMIT) {
      if (out->n >= MAX_FIELDS) return -1;
      out->f[out->n].nlen = nv.namelen;
      out->f[out->n].vlen = nv.valuelen;
      memcpy(out->f[out->n].name, nv.name, nv.namelen);
      memcpy(out->f[out->n].value, nv.value, nv.valuelen);
      out->n++;
    }
    if (flags & NGHTTP2_HD_INFLATE_FINAL) {
      nghttp2_hd_inflate_end_headers(inf);
      return out->n;
    }
    if (rv == 0 && len == 0) return -1;
  }
}

/** @return 1 when the collected fields equal the expected list. */
static int same_fields(const Fields *fs, const Nv *want, int nwant) {
  if (fs->n != nwant) return 0;
  for (int i = 0; i < nwant; i++)
    if (fs->f[i].nlen != strlen(want[i].name) || memcmp(fs->f[i].name, want[i].name, fs->f[i].nlen) ||
        fs->f[i].vlen != strlen(want[i].value) || memcmp(fs->f[i].value, want[i].value, fs->f[i].vlen))
      return 0;
  return 1;
}

/** One block of an RFC example: hex, expected fields, table size after. */
typedef struct {
  const char *hex;
  Nv want[8];
  int nwant;
  uint64_t size_after;
} Block;

/**
 * Runs a sequence of RFC blocks through a fresh decoder and a fresh
 * nghttp2 inflater sharing one table-size limit.
 * @remarks The RFC's 256-byte examples (C.5, C.6) assume the size was agreed
 *          out of band. nghttp2's inflater, told a smaller size than the
 *          default, insists that the next block open with a size update (as
 *          a real encoder must send after the SETTINGS change), so the oracle
 *          alone gets that update prepended to the first block. Our decoder
 *          is initialized with the limit as its starting size and gets the
 *          vectors verbatim.
 */
static void run_rfc(const char *label, uint64_t limit, const Block *blocks, int nblocks) {
  HpackDec d;
  CHECK(kv_hpack_init(&d, g_pool, limit) == 0);
  nghttp2_hd_inflater *inf;
  CHECK(nghttp2_hd_inflate_new(&inf) == 0);
  CHECK(nghttp2_hd_inflate_change_table_size(inf, limit) == 0);
  for (int b = 0; b < nblocks; b++) {
    uint8_t buf[512], obuf[520];
    size_t len = unhex(blocks[b].hex, buf), olen = 0;
    if (b == 0 && limit < 4096) olen = enc_int(obuf, limit, 5, 0x20);
    memcpy(obuf + olen, buf, len);
    olen += len;
    static Fields ours, theirs;
    ours.n = 0;
    int64_t rc = kv_hpack_decode(&d, buf, len, &ours, collect, g_scratch);
    int on = oracle_decode(inf, obuf, olen, &theirs);
    if (on < 0 || !same_fields(&theirs, blocks[b].want, blocks[b].nwant))
      fprintf(stderr, "  %s block %d: nghttp2 disagrees with the expected fields (vector typo?)\n", label, b);
    CHECK(on >= 0 && same_fields(&theirs, blocks[b].want, blocks[b].nwant));
    if (rc != 0 || !same_fields(&ours, blocks[b].want, blocks[b].nwant) || d.size != blocks[b].size_after)
      fprintf(stderr, "  %s block %d: rc=%lld fields=%d size=%llu (want %llu)\n", label, b, (long long)rc,
              ours.n, (unsigned long long)d.size, (unsigned long long)blocks[b].size_after);
    CHECK(rc == 0);
    CHECK(same_fields(&ours, blocks[b].want, blocks[b].nwant));
    CHECK_EQ_U64(d.size, blocks[b].size_after);
  }
  nghttp2_hd_inflate_del(inf);
  kv_hpack_free(&d);
}

/** RFC 7541 Appendix C examples. */
static void test_rfc_vectors(void) {
  static const Block c21[] = {{"400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572",
                               {{"custom-key", "custom-header"}}, 1, 55}};
  static const Block c22[] = {{"040c 2f73 616d 706c 652f 7061 7468", {{":path", "/sample/path"}}, 1, 0}};
  static const Block c23[] = {{"1008 7061 7373 776f 7264 0673 6563 7265 74", {{"password", "secret"}}, 1, 0}};
  static const Block c24[] = {{"82", {{":method", "GET"}}, 1, 0}};
  run_rfc("C.2.1", 4096, c21, 1);
  run_rfc("C.2.2", 4096, c22, 1);
  run_rfc("C.2.3", 4096, c23, 1);
  run_rfc("C.2.4", 4096, c24, 1);

  static const Block c3[] = {
      {"8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
       {{":method", "GET"}, {":scheme", "http"}, {":path", "/"}, {":authority", "www.example.com"}}, 4, 57},
      {"8286 84be 5808 6e6f 2d63 6163 6865",
       {{":method", "GET"}, {":scheme", "http"}, {":path", "/"}, {":authority", "www.example.com"},
        {"cache-control", "no-cache"}}, 5, 110},
      {"8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65",
       {{":method", "GET"}, {":scheme", "https"}, {":path", "/index.html"}, {":authority", "www.example.com"},
        {"custom-key", "custom-value"}}, 5, 164},
  };
  run_rfc("C.3", 4096, c3, 3);
  static const Block c4[] = {
      {"8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff",
       {{":method", "GET"}, {":scheme", "http"}, {":path", "/"}, {":authority", "www.example.com"}}, 4, 57},
      {"8286 84be 5886 a8eb 1064 9cbf",
       {{":method", "GET"}, {":scheme", "http"}, {":path", "/"}, {":authority", "www.example.com"},
        {"cache-control", "no-cache"}}, 5, 110},
      {"8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf",
       {{":method", "GET"}, {":scheme", "https"}, {":path", "/index.html"}, {":authority", "www.example.com"},
        {"custom-key", "custom-value"}}, 5, 164},
  };
  run_rfc("C.4", 4096, c4, 3);

  static const Block c5[] = {
      {"4803 3330 3258 0770 7269 7661 7465 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a "
       "3133 3a32 3120 474d 546e 1768 7474 7073 3a2f 2f77 7777 2e65 7861 6d70 6c65 2e63 6f6d",
       {{":status", "302"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"}}, 4, 222},
      {"4803 3330 37c1 c0bf",
       {{":status", "307"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"}}, 4, 222},
      {"88c1 611d 4d6f 6e2c 2032 3120 4f63 7420 3230 3133 2032 303a 3133 3a32 3220 474d 54c0 5a04 "
       "677a 6970 7738 666f 6f3d 4153 444a 4b48 514b 425a 584f 5157 454f 5049 5541 5851 5745 4f49 "
       "553b 206d 6178 2d61 6765 3d33 3630 303b 2076 6572 7369 6f6e 3d31",
       {{":status", "200"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
        {"location", "https://www.example.com"}, {"content-encoding", "gzip"},
        {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}}, 6, 215},
  };
  run_rfc("C.5", 256, c5, 3);
  static const Block c6[] = {
      {"4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6 2d1b ff6e "
       "919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3",
       {{":status", "302"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"}}, 4, 222},
      {"4883 640e ffc1 c0bf",
       {{":status", "307"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"}}, 4, 222},
      {"88c1 6196 d07a be94 1054 d444 a820 0595 040b 8166 e084 a62d 1bff c05a 839b d9ab 77ad 94e7 "
       "821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed "
       "4ee5 b106 3d50 07",
       {{":status", "200"}, {"cache-control", "private"}, {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
        {"location", "https://www.example.com"}, {"content-encoding", "gzip"},
        {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"}}, 6, 215},
  };
  run_rfc("C.6", 256, c6, 3);
}

/** Huffman decoder: RFC strings and every padding rule. */
static void test_huffman(void) {
  uint8_t in[64], out[64];
  size_t n = unhex("f1e3 c2e5 f23a 6ba0 ab90 f4ff", in);
  CHECK(kv_huff_decode(in, n, out, sizeof out) == 15 && memcmp(out, "www.example.com", 15) == 0);
  CHECK(kv_huff_decode(in, n, out, 4) == 15);       /* too small: true length, 4 bytes written */
  CHECK(memcmp(out, "www.", 4) == 0);
  CHECK(kv_huff_decode(in, 0, out, sizeof out) == 0);
  uint8_t a_pad3 = 0x1F;                            /* 'a' = 00011 + 111 */
  CHECK(kv_huff_decode(&a_pad3, 1, out, 8) == 1 && out[0] == 'a');
  uint8_t a_pad11[2] = {0x1F, 0xFF};                /* + 8 more one-bits */
  CHECK(kv_huff_decode(a_pad11, 2, out, 8) == -1);
  uint8_t zero_pad = 0x18;                          /* 'a' + 000 */
  CHECK(kv_huff_decode(&zero_pad, 1, out, 8) == -1);
  uint8_t eos[4] = {0xFF, 0xFF, 0xFF, 0xFF};        /* EOS (30 ones) + 2 */
  CHECK(kv_huff_decode(eos, 4, out, 8) == -1);
}

/** Decodes a hex block with a fresh 4096 table. @return decoder result. */
static int64_t decode_hex(const char *hex, Fields *fs) {
  HpackDec d;
  uint8_t buf[256];
  size_t n = unhex(hex, buf);
  CHECK(kv_hpack_init(&d, g_pool, 4096) == 0);
  fs->n = 0;
  int64_t rc = kv_hpack_decode(&d, buf, n, fs, collect, g_scratch);
  kv_hpack_free(&d);
  return rc;
}

/** Malformed blocks are compression errors; size updates at the start are fine. */
static void test_errors(void) {
  static Fields fs;
  CHECK(decode_hex("80", &fs) == HPACK_ERR_COMPRESSION);          /* index 0 */
  CHECK(decode_hex("be", &fs) == HPACK_ERR_COMPRESSION);          /* 62: empty dynamic table */
  CHECK(decode_hex("ff 80 80 80 80 80", &fs) == HPACK_ERR_COMPRESSION); /* integer too long */
  CHECK(decode_hex("ff", &fs) == HPACK_ERR_COMPRESSION);          /* truncated integer */
  CHECK(decode_hex("400a 6375", &fs) == HPACK_ERR_COMPRESSION);   /* truncated name */
  CHECK(decode_hex("82 3f e1 1f", &fs) == HPACK_ERR_COMPRESSION); /* size update after a field */
  CHECK(decode_hex("3f e2 1f", &fs) == HPACK_ERR_COMPRESSION);    /* 4097 > limit 4096 */
  CHECK(decode_hex("3f e1 1f 20 82", &fs) == 0 && fs.n == 1);     /* 4096 then 0, then a field */
  CHECK(decode_hex("0082 1f", &fs) == HPACK_ERR_COMPRESSION);     /* Huffman name with bad padding */
  CHECK(decode_hex("0f 10 8f ff", &fs) == HPACK_ERR_COMPRESSION); /* Huffman value of 1 byte past end */
}

/**
 * Oversize Huffman strings (decoded length > the scratch buffer): flagged,
 * never classified or stored, and the table stays in sync.
 */
static void test_oversize(void) {
  char big[300];
  memset(big, 'x', sizeof big - 1);
  big[sizeof big - 1] = 0;
  const size_t blen = sizeof big - 1; /* 299 */
  static uint8_t scr256[256], scr64[64];
  const uint64_t s256[2] = {(uint64_t)(uintptr_t)scr256, sizeof scr256};
  const uint64_t s64[2] = {(uint64_t)(uintptr_t)scr64, sizeof scr64};
  static Fields fs;

  /* A "literal with incremental indexing" of x-big with a Huffman value.
     nghttp2 emits it as never-indexed (0x10, new name); only the first
     byte differs from the incremental form (0x40), so patch it. */
  nghttp2_hd_deflater *def;
  CHECK(nghttp2_hd_deflate_new(&def, 4096) == 0);
  nghttp2_nv one = {(uint8_t *)"x-big", (uint8_t *)big, 5, blen, NGHTTP2_NV_FLAG_NO_INDEX};
  uint8_t blk[1024];
  size_t n = 0;
  blk[n++] = 0x40;                                   /* x-a: b, incremental (36 bytes) */
  n += enc_raw(blk + n, "x-a", 3);
  n += enc_raw(blk + n, "b", 1);
  size_t big_at = n;
  nghttp2_ssize dn = nghttp2_hd_deflate_hd2(def, blk + n, sizeof blk - n, &one, 1);
  CHECK(dn > 0 && blk[big_at] == 0x10);
  CHECK(blk[big_at + 1 + 1 + (blk[big_at + 1] & 0x7F)] & 0x80); /* the value is Huffman coded */
  blk[big_at] = 0x40;
  n += (size_t)dn;
  blk[n++] = 0x82;                                   /* :method GET */
  nghttp2_hd_deflate_del(def);

  /* 1. Contract kept (scratch >= limit = 256): 336 bytes cannot fit, so the
        table is emptied as RFC 7541 §4.4 requires; nothing unkept is stored. */
  HpackDec d;
  CHECK(kv_hpack_init(&d, g_pool, 256) == 0);
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, n, &fs, collect, s256) == 0);
  CHECK(fs.n == 3);
  CHECK(fs.f[1].meaning == (HF_OTHER | HF_OVERSIZE) && fs.f[1].nlen == 5 && fs.f[1].vlen == blen);
  CHECK(fs.f[2].meaning == HF_METHOD && fs.f[2].vlen == 3 && memcmp(fs.f[2].value, "GET", 3) == 0);
  CHECK(d.count == 0 && d.size == 0);
  kv_hpack_free(&d);

  /* 2. Contract broken (64-byte scratch, 4096 table): the entry would fit
        but its bytes were never kept, so decoding refuses. */
  CHECK(kv_hpack_init(&d, g_pool, 4096) == 0);
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, n, &fs, collect, s64) == HPACK_ERR_NOMEM);
  kv_hpack_free(&d);

  /* 3. nghttp2 with a 256-byte table: it opens with a size update, does not
        index the 336-byte field, never indexes :path (its policy: paths
        vary per request), and indexes content-type; two blocks in a row,
        the second using the dynamic entry. */
  CHECK(nghttp2_hd_deflate_new(&def, 256) == 0);
  CHECK(kv_hpack_init(&d, g_pool, 256) == 0);
  nghttp2_nv three[3] = {
      {(uint8_t *)"x-big", (uint8_t *)big, 5, blen, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)":path", (uint8_t *)"/kvasm.v1.KvAsm/Get", 5, 19, NGHTTP2_NV_FLAG_NONE},
      {(uint8_t *)"content-type", (uint8_t *)"application/grpc", 12, 16, NGHTTP2_NV_FLAG_NONE},
  };
  for (int round = 0; round < 2; round++) {
    dn = nghttp2_hd_deflate_hd2(def, blk, sizeof blk, three, 3);
    CHECK(dn > 0);
    fs.n = 0;
    CHECK(kv_hpack_decode(&d, blk, (size_t)dn, &fs, collect, s256) == 0);
    CHECK(fs.n == 3 && (fs.f[0].meaning & HF_OVERSIZE) && fs.f[0].vlen == blen);
    CHECK(fs.f[1].meaning == (HF_PATH | M_GET << 8));
    CHECK(fs.f[2].meaning == (HF_CONTENT_TYPE | VC_OK << 8));
    if (d.count != 1 || d.size != 12 + 16 + 32) {
      fprintf(stderr, "  round %d: count=%llu size=%llu block:", round, (unsigned long long)d.count,
              (unsigned long long)d.size);
      for (nghttp2_ssize i = 0; i < dn && i < 12; i++) fprintf(stderr, " %02x", blk[i]);
      fprintf(stderr, "\n");
    }
    CHECK(d.count == 1 && d.size == 12 + 16 + 32);
  }
  CHECK(blk[dn - 1] == 0xBE); /* round 2 sent content-type as dynamic index 62 */
  kv_hpack_free(&d);
  nghttp2_hd_deflate_del(def);
}

/**
 * Regression: a literal whose NAME is a dynamic entry that its own insertion
 * evicts. The handler must still see the intact name (the decoder emits
 * before inserting; a freed pool block has its first 8 bytes overwritten by
 * the free-list link, so reading it afterwards would show garbage).
 */
static void test_evicted_name(void) {
  HpackDec d;
  CHECK(kv_hpack_init(&d, g_pool, 256) == 0);
  static Fields fs;
  uint8_t blk[512];
  size_t n = 0;
  blk[n++] = 0x40;                                   /* x-name: v -> 6 + 1 + 32 = 39 */
  n += enc_raw(blk + n, "x-name", 6);
  n += enc_raw(blk + n, "v", 1);
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, n, &fs, collect, g_scratch) == 0 && d.count == 1 && d.size == 39);

  char val[200];
  memset(val, 'y', sizeof val);
  n = enc_int(blk, 62, 6, 0x40);                     /* name = entry 62 (x-name), incremental */
  n += enc_raw(blk + n, val, sizeof val);            /* 6 + 200 + 32 = 238: evicts x-name */
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, n, &fs, collect, g_scratch) == 0);
  CHECK(fs.n == 1 && fs.f[0].nlen == 6 && memcmp(fs.f[0].name, "x-name", 6) == 0);
  CHECK(fs.f[0].vlen == sizeof val && memcmp(fs.f[0].value, val, sizeof val) == 0);
  CHECK(d.count == 1 && d.size == 238);

  blk[0] = 0xBE;                                     /* index 62: the new entry */
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, 1, &fs, collect, g_scratch) == 0);
  CHECK(fs.n == 1 && fs.f[0].nlen == 6 && memcmp(fs.f[0].name, "x-name", 6) == 0);
  CHECK(fs.f[0].vlen == sizeof val && memcmp(fs.f[0].value, val, sizeof val) == 0);
  kv_hpack_free(&d);
}

/**
 * The table generation (HpackDec.gen) moves exactly when the table may
 * change: callers cache a block's result by (bytes, generation), so a
 * missed increment would replay stale fields and a spurious one only costs
 * a cache miss.
 */
static void test_generation(void) {
  HpackDec d;
  CHECK(kv_hpack_init(&d, g_pool, 4096) == 0);
  static Fields fs;
  CHECK(d.gen == 0);
  uint8_t blk[64];
  size_t n = 0;
  blk[n++] = 0x40;                                   /* insertion */
  n += enc_raw(blk + n, "x-a", 3);
  n += enc_raw(blk + n, "b", 1);
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, blk, n, &fs, collect, g_scratch) == 0 && d.gen == 1);
  /* static entry, the dynamic entry, a literal without indexing: no change */
  uint8_t lit[5] = {0x82, 0xBE, 0x04, 0x01, '/'};
  fs.n = 0;
  CHECK(kv_hpack_decode(&d, lit, sizeof lit, &fs, collect, g_scratch) == 0 && fs.n == 3 && d.gen == 1);
  uint8_t upd[2] = {0x3F, 0x00};                     /* size update to 31, nothing else */
  CHECK(kv_hpack_decode(&d, upd, sizeof upd, &fs, collect, g_scratch) == 0 && d.gen == 2);
  CHECK(d.count == 0);                               /* 31 < 36: the entry was evicted */
  kv_hpack_free(&d);
}

/** Meanings of names and values. */
static void test_classify(void) {
#define CL(n, v) kv_hpack_classify(n, strlen(n), v, strlen(v))
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Get") == (HF_PATH | M_GET << 8));
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Put") == (HF_PATH | M_PUT << 8));
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Delete") == (HF_PATH | M_DELETE << 8));
  CHECK(CL(":path", "/grpc.health.v1.Health/Check") == (HF_PATH | M_HEALTH << 8));
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Upload") == (HF_PATH | M_UPLOAD << 8));
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Download") == (HF_PATH | M_DOWNLOAD << 8));
  CHECK(CL(":path", "/kvasm.v1.KvAsm/upload") == HF_PATH);      /* methods are case-sensitive */
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Uploads") == HF_PATH);
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Downloa") == HF_PATH);
  CHECK(CL(":path", "/kvasm.v1.KvAsm/Gets") == HF_PATH);
  CHECK(CL(":path", "/kvasm.v1.KvAsm/") == HF_PATH);
  CHECK(CL(":path", "") == (HF_PATH | VC_EMPTY << 8));
  CHECK(CL(":method", "POST") == (HF_METHOD | VC_POST << 8));
  CHECK(CL(":method", "HEAD") == (HF_METHOD | VC_HEAD << 8));
  CHECK(CL(":method", "GET") == HF_METHOD);
  CHECK(CL(":scheme", "http") == HF_SCHEME);
  CHECK(CL(":authority", "x") == HF_AUTHORITY);
  CHECK(CL(":status", "200") == HF_STATUS);
  CHECK(CL(":foo", "x") == HF_PSEUDO_UNKNOWN);
  CHECK(CL(":Path", "/") == HF_PSEUDO_UNKNOWN);
  CHECK(CL("content-type", "application/grpc") == (HF_CONTENT_TYPE | VC_OK << 8));
  CHECK(CL("content-type", "application/grpc+proto") == (HF_CONTENT_TYPE | VC_OK << 8));
  CHECK(CL("content-type", "application/grpc;charset=x") == (HF_CONTENT_TYPE | VC_OK << 8));
  CHECK(CL("content-type", "application/grpcx") == HF_CONTENT_TYPE);
  CHECK(CL("content-type", "text/plain") == HF_CONTENT_TYPE);
  CHECK(CL("te", "trailers") == (HF_TE | VC_OK << 8));
  CHECK(CL("te", "gzip") == HF_TE);
  CHECK(CL("grpc-encoding", "identity") == (HF_GRPC_ENCODING | VC_OK << 8));
  CHECK(CL("grpc-encoding", "gzip") == HF_GRPC_ENCODING);
  CHECK(CL("content-length", "12") == HF_CONTENT_LENGTH);
  CHECK(CL("connection", "close") == HF_CONN_SPECIFIC);
  CHECK(CL("keep-alive", "1") == HF_CONN_SPECIFIC);
  CHECK(CL("proxy-connection", "x") == HF_CONN_SPECIFIC);
  CHECK(CL("transfer-encoding", "chunked") == HF_CONN_SPECIFIC);
  CHECK(CL("upgrade", "h2c") == HF_CONN_SPECIFIC);
  CHECK(CL("user-agent", "grpc-dotnet/2.60") == HF_OTHER);
  CHECK(CL("Content-Type", "x") == HF_BAD_NAME);
  CHECK(CL("bad name", "x") == HF_BAD_NAME);
  CHECK(CL("", "x") == HF_BAD_NAME);
  CHECK(CL("x-a", "a\r\nb") == (HF_OTHER | HF_VALUE_BAD));
  CHECK(CL("x-a", " lead") == (HF_OTHER | HF_VALUE_BAD));
  CHECK(CL("x-a", "trail\t") == (HF_OTHER | HF_VALUE_BAD));
  CHECK(CL("x-a", "in\tside") == HF_OTHER);
  CHECK(kv_hpack_classify("x-z", 3, "a\0b", 3) == (HF_OTHER | HF_VALUE_BAD));
#undef CL
}

/** Random printable string of up to `max` bytes from a small alphabet. */
static size_t rand_str(uint64_t *s, char *out, size_t max, const char *alpha) {
  size_t n = rng_next(s) % (max + 1), k = strlen(alpha);
  for (size_t i = 0; i < n; i++) out[i] = alpha[rng_next(s) % k];
  return n;
}

/** nghttp2 encodes random header lists; hpack.asm must decode them exactly. */
static void test_differential(void) {
  static const char *names[] = {":method", ":path", ":scheme", ":authority", "content-type", "te",
                                "user-agent", "grpc-timeout", "grpc-encoding", "x-custom", "cookie",
                                "accept", "cache-control", "set-cookie", "authorization"};
  static const char *vals[] = {"POST", "/kvasm.v1.KvAsm/Get", "http", "localhost:50051",
                               "application/grpc", "trailers", "grpc-c++/1.60", "10S", "identity"};
  nghttp2_hd_deflater *def;
  CHECK(nghttp2_hd_deflate_new(&def, 4096) == 0);
  HpackDec d;
  CHECK(kv_hpack_init(&d, g_pool, 4096) == 0);
  uint64_t s = 0xD1FF;
  static Fields fs;
  static char nbuf[MAX_FIELDS][64], vbuf[MAX_FIELDS][1200];
  uint64_t fields = 0, blocks = 0;
  for (int round = 0; round < 30000; round++) {
    if (rng_next(&s) % 97 == 0) /* the encoder may shrink its table: size updates follow */
      CHECK(nghttp2_hd_deflate_change_table_size(def, rng_next(&s) % 4097) == 0);
    nghttp2_nv nva[12];
    int nn = 1 + (int)(rng_next(&s) % 12);
    for (int i = 0; i < nn; i++) {
      size_t nl, vl;
      if (rng_next(&s) % 3) {
        const char *n = names[rng_next(&s) % (sizeof names / sizeof names[0])];
        nl = strlen(n);
        memcpy(nbuf[i], n, nl);
      } else {
        nl = 1 + rand_str(&s, nbuf[i], 30, "abcdefghijklmnopqrstuvwxyz-0123456789");
        nbuf[i][nl - 1] = 'q';
      }
      if (rng_next(&s) % 2) {
        const char *v = vals[rng_next(&s) % (sizeof vals / sizeof vals[0])];
        vl = strlen(v);
        memcpy(vbuf[i], v, vl);
      } else {
        vl = rand_str(&s, vbuf[i], rng_next(&s) % 8 ? 40 : 1100,
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_.;=/+%~!");
      }
      nva[i] = (nghttp2_nv){(uint8_t *)nbuf[i], (uint8_t *)vbuf[i], nl, vl,
                            (uint8_t)(rng_next(&s) % 5 == 0 ? NGHTTP2_NV_FLAG_NO_INDEX : 0)};
    }
    static uint8_t out[32768];
    nghttp2_ssize len = nghttp2_hd_deflate_hd2(def, out, sizeof out, nva, (size_t)nn);
    CHECK(len >= 0);
    if (len < 0) break;
    fs.n = 0;
    int64_t rc = kv_hpack_decode(&d, out, (size_t)len, &fs, collect, g_scratch);
    int ok = rc == 0 && fs.n == nn;
    for (int i = 0; ok && i < nn; i++)
      ok = fs.f[i].nlen == nva[i].namelen && fs.f[i].vlen == nva[i].valuelen &&
           memcmp(fs.f[i].name, nva[i].name, nva[i].namelen) == 0 &&
           memcmp(fs.f[i].value, nva[i].value, nva[i].valuelen) == 0;
    if (!ok) {
      fprintf(stderr, "  differential mismatch at round %d (rc=%lld, fields %d/%d)\n", round,
              (long long)rc, fs.n, nn);
      CHECK(ok);
      break;
    }
    fields += (uint64_t)nn;
    blocks++;
  }
  printf("  differential: %llu blocks, %llu fields decoded identically to nghttp2\n",
         (unsigned long long)blocks, (unsigned long long)fields);
  kv_hpack_free(&d);
  nghttp2_hd_deflate_del(def);
}

/** Entry point. @return 0 when every check passed. */
int main(void) {
  CHECK(kv_hpack_abi[0] == sizeof(HpackDec));
  CHECK(kv_hpack_abi[1] == offsetof(HpackDec, size));
  CHECK(kv_hpack_abi[2] == 0xABCDEF0123456789ull);
  g_pool = calloc(1, kv_pool_struct_size);
  kv_hpack_global_init();
  test_huffman();
  test_rfc_vectors();
  test_errors();
  test_oversize();
  test_evicted_name();
  test_generation();
  test_classify();
  test_differential();
  return test_finish("test_hpack");
}
