/*
 * grpc.c — gRPC over HTTP/2 cleartext (KvNode), on nghttp2's client API.
 *
 * Each connection owns one nghttp2 client session and a fixed pool of stream
 * slots (one per allowed in-flight request), so steady state allocates
 * nothing in kvbench itself. Requests are unary gRPC calls; a response is
 * judged when its stream closes: HTTP 200, grpc-status 0 and a well-formed
 * message, with GET values verified byte for byte like every other protocol.
 *
 * Client-side choices are the efficient ones a production gRPC client makes:
 * large flow-control windows (no WINDOW_UPDATE stalls on the response path)
 * and HPACK-indexed headers after the first request.
 *
 * Streaming calls (KvNode's Upload and Download) move one whole value per
 * call without ever holding it: an Upload's request stream is generated
 * straight into the connection's output buffer as nghttp2 asks for each DATA
 * frame (NGHTTP2_DATA_FLAG_NO_COPY, so the bytes are copied once, not three
 * times), and a Download's response is parsed and compared with the expected
 * value where nghttp2 hands it over, in the receive buffer. The server's
 * CRC-32C of the value is checked against value_crc (O(1) per call), so a
 * client thread's cost is the transport, not verification.
 */
#include <stdio.h>
#include <stdlib.h>

#include <nghttp2/nghttp2.h>

#include "kvbench.h"

/** One in-flight gRPC call. */
typedef struct {
  int32_t stream_id;
  uint8_t op;
  uint64_t key, t0;
  uint8_t *req;            /**< gRPC-framed request message (unary calls, Download) */
  /**
   * Unary: request bytes copied to nghttp2 so far. Upload: request bytes
   * promised to nghttp2 by read_upload (send_upload generates them later).
   */
  size_t req_len, req_off;
  uint8_t *resp;           /**< accumulated response DATA (unary calls, Upload) */
  size_t resp_len, resp_cap;
  int http_status, grpc_status;
  UpGen up;                /**< OP_UPLOAD: the request stream generator */
  DlParse dl;              /**< OP_DOWNLOAD: the response parser */
} Slot;

/** Per-connection gRPC state. */
typedef struct {
  Conn *c;
  nghttp2_session *s;
  Slot *slots;
  uint32_t *free_list;
  uint32_t nfree, nslots;
  uint64_t now;            /**< receive time of the batch being parsed */
} GrpcConn;

#define NV(n, v)                                                                 \
  {                                                                              \
    (uint8_t *)(n), (uint8_t *)(v), sizeof(n) - 1, sizeof(v) - 1,                \
        NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE             \
  }
#define REQUEST_HEADERS(path)                                                    \
  {NV(":method", "POST"), NV(":scheme", "http"), NV(":path", path),              \
   NV(":authority", "kvnode"), NV("content-type", "application/grpc"),           \
   NV("te", "trailers")}

static nghttp2_nv g_nv_get[] = REQUEST_HEADERS("/kvnode.v1.KvNode/Get");
static nghttp2_nv g_nv_put[] = REQUEST_HEADERS("/kvnode.v1.KvNode/Put");
static nghttp2_nv g_nv_ping[] = REQUEST_HEADERS("/grpc.health.v1.Health/Check");
static nghttp2_nv g_nv_upload[] = REQUEST_HEADERS("/kvnode.v1.KvNode/Upload");
static nghttp2_nv g_nv_download[] = REQUEST_HEADERS("/kvnode.v1.KvNode/Download");
static nghttp2_session_callbacks *g_callbacks;

/**
 * grpc_collect stops serializing once this much output is pending, so the
 * engine sends it before more is generated: a 1 MiB-per-stream upload window
 * would otherwise stage megabytes (evicting them from cache) before the first
 * send(). Unary benchmarks rarely reach it, and when they do only the number
 * of send() calls changes, never the bytes.
 */
#define COLLECT_CAP (256 * 1024)

/** Upload data bytes per request message when --chunk is not given. */
#define UPLOAD_CHUNK_DEFAULT 65536

/* ------------------------------------------------------------------------- */
/* Streaming codec                                                            */
/* ------------------------------------------------------------------------- */

/* UpGen.stage: emitting piece[] (a message header), data, the crc field. */
enum { UP_PIECE, UP_DATA, UP_TAIL, UP_END };

/* DlParse.state: what the next byte of the body is. */
enum { DL_PREFIX, DL_TAG, DL_TOTAL, DL_LEN, DL_DATA, DL_CRC };

/**
 * Data bytes per Upload message for this run.
 * @returns --chunk, or UPLOAD_CHUNK_DEFAULT when it is 0
 */
static uint32_t upload_chunk(void) { return g_opt.chunk ? g_opt.chunk : UPLOAD_CHUNK_DEFAULT; }

/**
 * Bytes of a protobuf varint.
 * @param v  value
 * @returns 1..10
 */
static size_t varint_len(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    n++;
  }
  return n;
}

/**
 * Writes a protobuf varint.
 * @param p  at least varint_len(v) writable bytes
 * @param v  value
 * @returns bytes written
 */
static size_t put_varint(uint8_t *p, uint64_t v) {
  size_t n = 0;
  while (v >= 0x80) {
    p[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  p[n++] = (uint8_t)v;
  return n;
}

/**
 * Stream length of an Upload; see kvbench.h. Closed form of what up_emit
 * produces (the unit tests compare the two).
 * @returns bytes
 */
uint64_t upload_stream_len(void) {
  uint64_t n = g_opt.value_size, c = upload_chunk();
  /* Every stream: the first message's key (0A 10 + 16 bytes) and total_size
   * (10 + varint), the last message's crc32c (25 + 4 bytes). */
  uint64_t fixed = 2 + KEY_BYTES + 1 + varint_len(n) + 5;
  if (n == 0) return 5 + fixed; /* one message, no data field */
  uint64_t msgs = (n + c - 1) / c, last = n - (msgs - 1) * c;
  return 5 * msgs + fixed + n + (msgs - 1) * (1 + varint_len(c)) + 1 + varint_len(last);
}

/**
 * Prepares the header piece of the message whose data starts at u->off:
 * gRPC prefix, then (first message) key and total_size, then the data
 * field's tag and length.
 * @param u      generator
 * @param first  1 for the stream's first message
 */
static void up_message(UpGen *u, int first) {
  uint64_t n = g_opt.value_size, c = upload_chunk();
  uint64_t len = n - u->off < c ? n - u->off : c;
  int last = u->off + len == n;
  uint64_t mlen = (first ? 2 + KEY_BYTES + 1 + varint_len(n) : 0) +
                  (len ? 1 + varint_len(len) + len : 0) + (last ? 5 : 0);
  uint8_t *p = u->piece;
  size_t k = 0;
  p[k++] = 0; /* uncompressed */
  p[k++] = (uint8_t)(mlen >> 24);
  p[k++] = (uint8_t)(mlen >> 16);
  p[k++] = (uint8_t)(mlen >> 8);
  p[k++] = (uint8_t)mlen;
  if (first) {
    p[k++] = 0x0A; /* key, length-delimited */
    p[k++] = KEY_BYTES;
    key_format((char *)p + k, u->key);
    k += KEY_BYTES;
    p[k++] = 0x10; /* total_size, varint */
    k += put_varint(p + k, n);
  }
  if (len) {
    p[k++] = 0x1A; /* data, length-delimited */
    k += put_varint(p + k, len);
  }
  u->piece_len = (uint8_t)k;
  u->piece_off = 0;
  u->data_left = (uint32_t)len;
  u->stage = UP_PIECE;
}

/**
 * Prepares the last message's closing field: crc32c (field 4, fixed32).
 * @param u  generator
 */
static void up_tail(UpGen *u) {
  u->piece[0] = 0x25;
  memcpy(u->piece + 1, &u->crc, 4); /* little-endian, as fixed32 is */
  u->piece_len = 5;
  u->piece_off = 0;
  u->stage = UP_TAIL;
}

/**
 * Starts an Upload stream; see kvbench.h.
 * @param u    generator (fully overwritten)
 * @param key  key id
 */
void up_start(UpGen *u, uint64_t key) {
  memset(u, 0, sizeof *u);
  u->key = key;
  u->crc = value_crc(key);
  up_message(u, 1);
}

/**
 * Emits the next bytes of an Upload stream; see kvbench.h.
 * @param u    generator
 * @param dst  n writable bytes
 * @param n    bytes wanted
 * @returns bytes written (fewer than n only at the end of the stream)
 */
size_t up_emit(UpGen *u, uint8_t *dst, size_t n) {
  size_t done = 0;
  while (done < n && u->stage != UP_END) {
    if (u->stage == UP_DATA) {
      size_t k = n - done < u->data_left ? n - done : u->data_left;
      value_copy(dst + done, u->key, u->off, k);
      u->off += k;
      u->data_left -= (uint32_t)k;
      done += k;
      if (u->data_left == 0) {
        if (u->off == g_opt.value_size) up_tail(u);
        else up_message(u, 0);
      }
      continue;
    }
    /* UP_PIECE or UP_TAIL: copy what is left of the small piece. */
    size_t k = n - done < (size_t)(u->piece_len - u->piece_off) ? n - done
                                                                : (size_t)(u->piece_len - u->piece_off);
    memcpy(dst + done, u->piece + u->piece_off, k);
    u->piece_off += (uint8_t)k;
    done += k;
    if (u->piece_off < u->piece_len) break; /* n exhausted mid-piece */
    if (u->stage == UP_TAIL) u->stage = UP_END;
    else if (u->data_left) u->stage = UP_DATA;
    else up_tail(u); /* the empty value: one message, no data field */
  }
  return done;
}

/**
 * Starts parsing a Download body; see kvbench.h.
 * @param d    parser (fully overwritten)
 * @param key  key id
 */
void dl_start(DlParse *d, uint64_t key) {
  memset(d, 0, sizeof *d);
  d->key = key;
  d->total = UINT64_MAX;
  d->state = DL_PREFIX;
}

/**
 * Consumes a piece of a Download body; see kvbench.h. Every rule of the
 * DownloadResponse contract is checked: total_size in the first message
 * only, data in order and within each message, crc32c as the very last
 * field of the stream.
 * @param d  parser
 * @param p  bytes
 * @param n  count
 */
void dl_feed(DlParse *d, const uint8_t *p, size_t n) {
  while (n && !d->bad) {
    if (d->state == DL_PREFIX) {
      if (d->have_crc) { /* a message after the one that carried crc32c */
        d->bad = 1;
        return;
      }
      size_t k = n < (size_t)(5 - d->tok_len) ? n : (size_t)(5 - d->tok_len);
      memcpy(d->tok + d->tok_len, p, k);
      d->tok_len += (uint8_t)k;
      p += k;
      n -= k;
      if (d->tok_len < 5) return;
      d->tok_len = 0;
      d->msg_left = (uint32_t)d->tok[1] << 24 | (uint32_t)d->tok[2] << 16 |
                    (uint32_t)d->tok[3] << 8 | d->tok[4];
      /* Compressed messages were never negotiated; an empty message carries
       * none of the fields every DownloadResponse must have. */
      if (d->tok[0] != 0 || d->msg_left == 0) {
        d->bad = 1;
        return;
      }
      if (d->messages < 255) d->messages++;
      d->state = DL_TAG;
      continue;
    }
    /* Inside a message: every remaining state consumes message bytes, so a
     * field that would cross the message's end is caught here. */
    if (d->msg_left == 0) {
      d->bad = 1;
      return;
    }
    if (d->state == DL_DATA) {
      size_t k = n;
      if (k > d->field_left) k = (size_t)d->field_left;
      if (d->received + k > g_opt.value_size) d->mismatch = 1; /* longer than the value */
      else if (!d->mismatch && !value_matches(p, d->key, d->received, k)) d->mismatch = 1;
      d->received += k;
      d->field_left -= k;
      d->msg_left -= (uint32_t)k;
      p += k;
      n -= k;
      if (d->field_left == 0) d->state = d->msg_left ? DL_TAG : DL_PREFIX;
      continue;
    }
    uint8_t b = *p++;
    n--;
    d->msg_left--;
    switch (d->state) {
    case DL_TAG:
      d->acc = 0;
      d->tok_len = 0;
      if (b == 0x08 && d->messages == 1 && d->total == UINT64_MAX) d->state = DL_TOTAL;
      else if (b == 0x12) d->state = DL_LEN;
      else if (b == 0x1D) d->state = DL_CRC;
      else d->bad = 1; /* unknown field, or total_size outside the first message */
      break;
    case DL_TOTAL:
    case DL_LEN:
      d->acc |= (uint64_t)(b & 0x7F) << (7 * d->tok_len);
      if (b & 0x80) {
        if (++d->tok_len == 10) d->bad = 1; /* overlong varint */
        break;
      }
      d->tok_len = 0;
      if (d->state == DL_TOTAL) {
        d->total = d->acc;
        if (d->total != g_opt.value_size) d->mismatch = 1;
        d->state = d->msg_left ? DL_TAG : DL_PREFIX;
      } else if (d->acc > d->msg_left) {
        d->bad = 1; /* the data field claims more than its message holds */
      } else {
        d->field_left = d->acc;
        d->state = d->acc ? DL_DATA : d->msg_left ? DL_TAG : DL_PREFIX;
      }
      break;
    case DL_CRC:
      d->acc |= (uint64_t)b << (8 * d->tok_len);
      if (++d->tok_len < 4) break;
      d->tok_len = 0;
      d->crc = (uint32_t)d->acc;
      d->have_crc = 1;
      if (d->crc != value_crc(d->key)) d->mismatch = 1;
      if (d->msg_left != 0) d->bad = 1; /* crc32c must be the stream's last field */
      d->state = DL_PREFIX;
      break;
    }
  }
}

/**
 * Judges a cleanly ended Download; see kvbench.h.
 * @param d  parser
 * @returns ST_OK, ST_BAD_VALUE or ST_ERROR
 */
int dl_judge(const DlParse *d) {
  /* Structure first: truncated, malformed, no total_size, no crc32c. */
  if (d->bad || d->state != DL_PREFIX || d->tok_len != 0 || !d->have_crc ||
      d->total == UINT64_MAX)
    return ST_ERROR;
  if (d->mismatch || d->total != g_opt.value_size || d->received != g_opt.value_size)
    return ST_BAD_VALUE;
  return ST_OK;
}

/**
 * Parses a small decimal header value.
 * @param v  value bytes
 * @param n  length
 * @returns the number, or -1 if not a short decimal
 */
static int small_int(const uint8_t *v, size_t n) {
  if (n == 0 || n > 6) return -1;
  int x = 0;
  for (size_t i = 0; i < n; i++) {
    if (v[i] < '0' || v[i] > '9') return -1;
    x = x * 10 + (v[i] - '0');
  }
  return x;
}

/**
 * Reads a protobuf varint without passing end.
 * @param p    cursor, advanced on success
 * @param end  one past the last byte
 * @param out  receives the value
 * @returns 1 on success, 0 if truncated or overlong
 */
static int varint(const uint8_t **p, const uint8_t *end, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 64 && *p < end; shift += 7) {
    uint8_t b = *(*p)++;
    v |= (uint64_t)(b & 0x7F) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return 1;
    }
  }
  return 0;
}

/**
 * Judges a finished call: transport status, gRPC status and message content.
 * @param sl          the call
 * @param error_code  HTTP/2 stream close code
 * @returns ST_OK, ST_MISS, ST_BAD_VALUE or ST_ERROR
 */
static int judge(const Slot *sl, uint32_t error_code) {
  if (sl->op == OP_DOWNLOAD) {
    /* NOT_FOUND is a miss (every key was preloaded, so still a failed run);
     * the body was verified as it arrived. */
    if (error_code == 0 && sl->http_status == 200 && sl->grpc_status == 5) return ST_MISS;
    if (error_code != 0 || sl->http_status != 200 || sl->grpc_status != 0) return ST_ERROR;
    return dl_judge(&sl->dl);
  }
  if (error_code != 0 || sl->http_status != 200 || sl->grpc_status != 0) return ST_ERROR;
  if (sl->resp_len < 5 || sl->resp[0] != 0) return ST_ERROR;
  uint32_t mlen = (uint32_t)sl->resp[1] << 24 | (uint32_t)sl->resp[2] << 16 |
                  (uint32_t)sl->resp[3] << 8 | sl->resp[4];
  if ((size_t)mlen + 5 != sl->resp_len) return ST_ERROR;
  const uint8_t *m = sl->resp + 5, *end = m + mlen;
  switch (sl->op) {
  case OP_GET: { /* GetResponse{found = true (08 01), value (12 len bytes)} */
    if (mlen == 0) return ST_MISS;
    uint64_t vlen;
    if (end - m < 3 || m[0] != 0x08 || m[1] != 0x01 || m[2] != 0x12) return ST_BAD_VALUE;
    m += 3;
    if (!varint(&m, end, &vlen) || vlen != (uint64_t)(end - m)) return ST_BAD_VALUE;
    return value_ok(sl->key, m, (size_t)vlen) ? ST_OK : ST_BAD_VALUE;
  }
  case OP_SET: /* PutResponse: created (08 01) or replaced (empty) */
    return mlen == 0 || (mlen == 2 && m[0] == 0x08 && m[1] == 0x01) ? ST_OK : ST_ERROR;
  case OP_UPLOAD: { /* UploadResponse{created (08 01)?, crc32c (15 + fixed32)?} */
    if (end - m >= 2 && m[0] == 0x08 && m[1] == 0x01) m += 2;
    uint32_t crc = 0; /* proto3 omits a zero crc32c */
    if (end - m == 5 && m[0] == 0x15) memcpy(&crc, m + 1, 4);
    else if (end != m) return ST_ERROR;
    /* The server's CRC of what it stored must be the CRC of what was sent. */
    return crc == value_crc(sl->key) ? ST_OK : ST_BAD_VALUE;
  }
  default: /* HealthCheckResponse{status = SERVING} */
    return mlen == 2 && m[0] == 0x08 && m[1] == 0x01 ? ST_OK : ST_ERROR;
  }
}

/**
 * nghttp2 data source: streams the prepared request message.
 * @returns bytes copied; sets EOF on the last chunk
 */
static nghttp2_ssize read_body(nghttp2_session *s, int32_t sid, uint8_t *buf, size_t len,
                               uint32_t *flags, nghttp2_data_source *src, void *ud) {
  (void)s;
  (void)sid;
  (void)ud;
  Slot *sl = src->ptr;
  size_t n = sl->req_len - sl->req_off;
  if (n > len) n = len;
  memcpy(buf, sl->req + sl->req_off, n);
  sl->req_off += n;
  if (sl->req_off == sl->req_len) *flags |= NGHTTP2_DATA_FLAG_EOF;
  return (nghttp2_ssize)n;
}

/**
 * nghttp2 data source of an Upload: only sizes the next DATA frame. The
 * bytes are generated by send_upload when nghttp2 serializes the frame
 * (NGHTTP2_DATA_FLAG_NO_COPY), directly into the connection's output buffer.
 * @param s      session (unused)
 * @param sid    stream id (unused)
 * @param buf    nghttp2's frame buffer (unused: no copy)
 * @param len    largest payload flow control and the frame size allow
 * @param flags  receives NO_COPY, plus EOF on the stream's last frame
 * @param src    the call's Slot
 * @param ud     GrpcConn (unused)
 * @returns the frame's payload length
 * @remarks nghttp2 prepares and sends one frame at a time, so the bytes
 *          promised here are exactly the bytes the next send_upload call for
 *          this stream emits; req_off counts promises, UpGen counts output.
 */
static nghttp2_ssize read_upload(nghttp2_session *s, int32_t sid, uint8_t *buf, size_t len,
                                 uint32_t *flags, nghttp2_data_source *src, void *ud) {
  (void)s;
  (void)sid;
  (void)buf;
  (void)ud;
  Slot *sl = src->ptr;
  size_t n = sl->req_len - sl->req_off;
  if (n > len) n = len;
  sl->req_off += n;
  *flags |= NGHTTP2_DATA_FLAG_NO_COPY;
  if (sl->req_off == sl->req_len) *flags |= NGHTTP2_DATA_FLAG_EOF;
  return (nghttp2_ssize)n;
}

/**
 * nghttp2 no-copy DATA callback: writes one complete DATA frame (the 9-byte
 * header nghttp2 serialized, then the generated payload) to the output
 * buffer.
 * @param s        session (unused)
 * @param f        the DATA frame (padding is never requested: no padding callback)
 * @param framehd  9 serialized header bytes
 * @param length   payload bytes promised by read_upload
 * @param src      the call's Slot
 * @param ud       GrpcConn
 * @returns 0, or NGHTTP2_ERR_CALLBACK_FAILURE if the generator ran dry (a
 *          kvbench bug: the connection then fails loudly)
 * @remarks Runs inside nghttp2_session_mem_send2, after every byte that
 *          call returned earlier was appended by grpc_collect, so frames stay
 *          in order in the output buffer.
 */
static int send_upload(nghttp2_session *s, nghttp2_frame *f, const uint8_t *framehd, size_t length,
                       nghttp2_data_source *src, void *ud) {
  (void)s;
  (void)f;
  GrpcConn *g = ud;
  Slot *sl = src->ptr;
  Conn *c = g->c;
  out_reserve(c, 9 + length);
  memcpy(c->out + c->out_len, framehd, 9);
  if (up_emit(&sl->up, c->out + c->out_len + 9, length) != length) return NGHTTP2_ERR_CALLBACK_FAILURE;
  c->out_len += 9 + length;
  return 0;
}

/**
 * nghttp2 header callback: keeps :status and grpc-status (from the response
 * HEADERS or the trailers).
 * @returns 0
 */
static int on_header(nghttp2_session *s, const nghttp2_frame *f, const uint8_t *name,
                     size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                     void *ud) {
  (void)flags;
  (void)ud;
  if (f->hd.type != NGHTTP2_HEADERS) return 0;
  Slot *sl = nghttp2_session_get_stream_user_data(s, f->hd.stream_id);
  if (!sl) return 0;
  if (namelen == 7 && memcmp(name, ":status", 7) == 0) sl->http_status = small_int(value, valuelen);
  else if (namelen == 11 && memcmp(name, "grpc-status", 11) == 0) sl->grpc_status = small_int(value, valuelen);
  return 0;
}

/**
 * nghttp2 DATA callback: a Download's bytes are parsed and verified where
 * they lie; every other call's are appended to its response buffer.
 * @returns 0, or NGHTTP2_ERR_CALLBACK_FAILURE when out of memory
 */
static int on_data(nghttp2_session *s, uint8_t flags, int32_t sid, const uint8_t *data,
                   size_t len, void *ud) {
  (void)flags;
  (void)ud;
  Slot *sl = nghttp2_session_get_stream_user_data(s, sid);
  if (!sl) return 0;
  if (sl->op == OP_DOWNLOAD) {
    dl_feed(&sl->dl, data, len);
    return 0;
  }
  if (sl->resp_len + len > sl->resp_cap) {
    size_t cap = (sl->resp_len + len) * 2;
    uint8_t *p = realloc(sl->resp, cap);
    if (!p) return NGHTTP2_ERR_CALLBACK_FAILURE;
    sl->resp = p;
    sl->resp_cap = cap;
  }
  memcpy(sl->resp + sl->resp_len, data, len);
  sl->resp_len += len;
  return 0;
}

/**
 * nghttp2 stream-close callback: completes the call and frees its slot.
 * @returns 0
 */
static int on_close(nghttp2_session *s, int32_t sid, uint32_t error_code, void *ud) {
  GrpcConn *g = ud;
  Slot *sl = nghttp2_session_get_stream_user_data(s, sid);
  if (!sl) return 0;
  int status = judge(sl, error_code);
  if (status == ST_ERROR && !g->c->w->error_text[0]) {
    if (sl->op == OP_DOWNLOAD && error_code == 0 && sl->grpc_status == 0)
      snprintf(g->c->w->error_text, sizeof g->c->w->error_text,
               "grpc: malformed Download body (state %d, %u messages, %llu of %u bytes, crc %d)",
               sl->dl.state, sl->dl.messages, (unsigned long long)sl->dl.received,
               g_opt.value_size, sl->dl.have_crc);
    else
      snprintf(g->c->w->error_text, sizeof g->c->w->error_text,
               "grpc: stream close %u, :status %d, grpc-status %d", error_code, sl->http_status,
               sl->grpc_status);
  }
  complete(g->c, sl->op, sl->key, sl->t0, g->now, status);
  g->free_list[g->nfree++] = (uint32_t)(sl - g->slots);
  return 0;
}

/**
 * nghttp2 frame callback: a GOAWAY ends the run (the server is shutting down
 * or rejected the connection).
 * @returns 0
 */
static int on_frame(nghttp2_session *s, const nghttp2_frame *f, void *ud) {
  (void)s;
  GrpcConn *g = ud;
  if (f->hd.type == NGHTTP2_GOAWAY) conn_fail(g->c, "grpc: server sent GOAWAY");
  return 0;
}

/**
 * Builds the shared callback table once (nghttp2 copies it into each session).
 * @returns 0, or -1 when out of memory
 */
static int callbacks_init(void) {
  if (g_callbacks) return 0;
  if (nghttp2_session_callbacks_new(&g_callbacks) != 0) return -1;
  nghttp2_session_callbacks_set_on_header_callback(g_callbacks, on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(g_callbacks, on_data);
  nghttp2_session_callbacks_set_on_stream_close_callback(g_callbacks, on_close);
  nghttp2_session_callbacks_set_on_frame_recv_callback(g_callbacks, on_frame);
  nghttp2_session_callbacks_set_send_data_callback(g_callbacks, send_upload);
  return 0;
}

static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Creates the client session and the slot pool for a connected socket.
 * @param c  connection
 * @returns 0, or -1 on failure (reason recorded)
 */
static int grpc_attach(Conn *c) {
  pthread_mutex_lock(&g_init_lock);
  int rc = callbacks_init();
  pthread_mutex_unlock(&g_init_lock);
  if (rc != 0) return -1;
  GrpcConn *g = calloc(1, sizeof *g);
  if (!g) return -1;
  g->c = c;
  g->nslots = (uint32_t)g_opt.depth;
  g->slots = calloc(g->nslots, sizeof *g->slots);
  g->free_list = calloc(g->nslots, sizeof *g->free_list);
  if (!g->slots || !g->free_list) return -1;
  /* Streaming workloads (and their Upload preload) never hold a value: their
   * requests are generated or tiny, and their responses verified in place or
   * tiny, so a slot needs only small buffers even for 64 MiB values. */
  int streaming = g_opt.workload == WL_UPLOAD || g_opt.workload == WL_DOWNLOAD;
  size_t req_cap = streaming ? 64 : 5 + 2 + KEY_BYTES + 1 + 10 + g_opt.value_size;
  for (uint32_t i = 0; i < g->nslots; i++) {
    g->slots[i].req = malloc(req_cap);
    g->slots[i].resp_cap = streaming ? 64 : 5 + 16 + g_opt.value_size;
    g->slots[i].resp = malloc(g->slots[i].resp_cap);
    if (!g->slots[i].req || !g->slots[i].resp) return -1;
    g->free_list[g->nfree++] = g->nslots - 1 - i;
  }
  if (nghttp2_session_client_new(&g->s, g_callbacks, g) != 0) return -1;
  nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
                                 {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 16 << 20}};
  if (nghttp2_submit_settings(g->s, NGHTTP2_FLAG_NONE, iv, 2) != 0) return -1;
  nghttp2_session_set_local_window_size(g->s, NGHTTP2_FLAG_NONE, 0, 1 << 30);
  c->proto_state = g;
  return 0;
}

/**
 * Frees the session and slot pool.
 * @param c  connection
 */
static void grpc_detach(Conn *c) {
  GrpcConn *g = c->proto_state;
  if (!g) return;
  if (g->s) nghttp2_session_del(g->s);
  for (uint32_t i = 0; g->slots && i < g->nslots; i++) {
    free(g->slots[i].req);
    free(g->slots[i].resp);
  }
  free(g->slots);
  free(g->free_list);
  free(g);
  c->proto_state = NULL;
}

/**
 * Starts one call. The engine never issues more than depth requests per
 * connection, so a free slot always exists.
 * @param c    connection
 * @param op   OP_GET / OP_SET / OP_PING (unary), OP_UPLOAD or OP_DOWNLOAD
 * @param key  key id
 * @param t0   start time
 */
static void grpc_issue(Conn *c, int op, uint64_t key, uint64_t t0) {
  GrpcConn *g = c->proto_state;
  Slot *sl = &g->slots[g->free_list[--g->nfree]];
  sl->op = (uint8_t)op;
  sl->key = key;
  sl->t0 = t0;
  sl->resp_len = 0;
  sl->req_off = 0;
  sl->http_status = sl->grpc_status = -1;

  if (op == OP_UPLOAD) {
    /* No request buffer: read_upload sizes each DATA frame and send_upload
     * generates it in place. */
    up_start(&sl->up, key);
    sl->req_len = upload_stream_len();
    nghttp2_data_provider2 body = {.source = {.ptr = sl}, .read_callback = read_upload};
    int32_t sid = nghttp2_submit_request2(g->s, NULL, g_nv_upload, 6, &body, sl);
    if (sid < 0) {
      g->free_list[g->nfree++] = (uint32_t)(sl - g->slots);
      complete(c, op, key, t0, now_ns(), ST_ERROR);
      conn_fail(c, "grpc: submit_request2 failed");
      return;
    }
    sl->stream_id = sid;
    return;
  }

  uint8_t *m = sl->req + 5;
  size_t mlen = 0;
  if (op != OP_PING) {
    m[0] = 0x0A; /* field 1 (key), length-delimited */
    m[1] = KEY_BYTES;
    key_format((char *)m + 2, key);
    mlen = 2 + KEY_BYTES;
    if (op == OP_DOWNLOAD) {
      /* DownloadRequest{key, chunk_size (field 4) when --chunk is given}:
       * the whole value, verified as it streams in. */
      if (g_opt.chunk) {
        m[mlen++] = 0x20;
        mlen += put_varint(m + mlen, g_opt.chunk);
      }
      dl_start(&sl->dl, key);
    }
    if (op == OP_SET) {
      m[mlen++] = 0x12; /* field 2 (value) */
      uint32_t v = g_opt.value_size;
      while (v >= 0x80) {
        m[mlen++] = (uint8_t)(v | 0x80);
        v >>= 7;
      }
      m[mlen++] = (uint8_t)v;
      value_fill(m + mlen, key);
      mlen += g_opt.value_size;
    }
  }
  sl->req[0] = 0; /* uncompressed */
  sl->req[1] = (uint8_t)(mlen >> 24);
  sl->req[2] = (uint8_t)(mlen >> 16);
  sl->req[3] = (uint8_t)(mlen >> 8);
  sl->req[4] = (uint8_t)mlen;
  sl->req_len = 5 + mlen;

  nghttp2_data_provider2 body = {.source = {.ptr = sl}, .read_callback = read_body};
  nghttp2_nv *nva = op == OP_GET        ? g_nv_get
                    : op == OP_SET      ? g_nv_put
                    : op == OP_DOWNLOAD ? g_nv_download
                                        : g_nv_ping;
  int32_t sid = nghttp2_submit_request2(g->s, NULL, nva, 6, &body, sl);
  if (sid < 0) {
    g->free_list[g->nfree++] = (uint32_t)(sl - g->slots);
    complete(c, op, key, t0, now_ns(), ST_ERROR);
    conn_fail(c, "grpc: submit_request2 failed");
    return;
  }
  sl->stream_id = sid;
}

/**
 * Feeds received bytes to the session (callbacks complete calls).
 * @param c     connection
 * @param data  received bytes
 * @param len   count
 * @param now   receive time
 * @returns 0, or -1 when the session reports a protocol error
 */
static int grpc_input(Conn *c, const uint8_t *data, size_t len, uint64_t now) {
  GrpcConn *g = c->proto_state;
  g->now = now;
  nghttp2_ssize rc = nghttp2_session_mem_recv2(g->s, data, len);
  if (rc < 0) {
    conn_fail(c, nghttp2_strerror((int)rc));
    return -1;
  }
  return 0;
}

/**
 * Serializes pending frames (requests, WINDOW_UPDATE, SETTINGS ACK) into the
 * connection's output buffer, stopping once COLLECT_CAP bytes are pending.
 * @param c  connection
 * @returns 0 when everything is serialized, 1 when it stopped at the cap
 *          (the engine sends, then calls again), -1 on a session error
 */
static int grpc_collect(Conn *c) {
  GrpcConn *g = c->proto_state;
  while (c->out_len - c->out_off < COLLECT_CAP) {
    const uint8_t *data;
    nghttp2_ssize n = nghttp2_session_mem_send2(g->s, &data);
    if (n < 0) return -1;
    if (n == 0) return 0;
    out_put(c, data, (size_t)n);
  }
  return 1;
}

const Proto proto_grpc = {
    .name = "grpc",
    .attach = grpc_attach,
    .detach = grpc_detach,
    .issue = grpc_issue,
    .input = grpc_input,
    .collect = grpc_collect,
    .parses_in_place = 0,
};
