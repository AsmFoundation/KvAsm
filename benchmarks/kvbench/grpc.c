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
  uint8_t *req;            /**< gRPC-framed request message */
  size_t req_len, req_off;
  uint8_t *resp;           /**< accumulated response DATA */
  size_t resp_len, resp_cap;
  int http_status, grpc_status;
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
static nghttp2_session_callbacks *g_callbacks;

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
 * nghttp2 DATA callback: appends to the call's response buffer.
 * @returns 0, or NGHTTP2_ERR_CALLBACK_FAILURE when out of memory
 */
static int on_data(nghttp2_session *s, uint8_t flags, int32_t sid, const uint8_t *data,
                   size_t len, void *ud) {
  (void)flags;
  (void)ud;
  Slot *sl = nghttp2_session_get_stream_user_data(s, sid);
  if (!sl) return 0;
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
  if (status == ST_ERROR && !g->c->w->error_text[0])
    snprintf(g->c->w->error_text, sizeof g->c->w->error_text,
             "grpc: stream close %u, :status %d, grpc-status %d", error_code, sl->http_status,
             sl->grpc_status);
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
  size_t req_cap = 5 + 2 + KEY_BYTES + 1 + 10 + g_opt.value_size;
  for (uint32_t i = 0; i < g->nslots; i++) {
    g->slots[i].req = malloc(req_cap);
    g->slots[i].resp_cap = 5 + 16 + g_opt.value_size;
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
 * Starts one unary call. The engine never issues more than depth requests per
 * connection, so a free slot always exists.
 * @param c    connection
 * @param op   OP_GET / OP_SET / OP_PING
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

  uint8_t *m = sl->req + 5;
  size_t mlen = 0;
  if (op != OP_PING) {
    m[0] = 0x0A; /* field 1 (key), length-delimited */
    m[1] = KEY_BYTES;
    key_format((char *)m + 2, key);
    mlen = 2 + KEY_BYTES;
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
  nghttp2_nv *nva = op == OP_GET ? g_nv_get : op == OP_SET ? g_nv_put : g_nv_ping;
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
 * connection's output buffer.
 * @param c  connection
 * @returns 0, or -1 on a session error
 */
static int grpc_collect(Conn *c) {
  GrpcConn *g = c->proto_state;
  for (;;) {
    const uint8_t *data;
    nghttp2_ssize n = nghttp2_session_mem_send2(g->s, &data);
    if (n < 0) return -1;
    if (n == 0) return 0;
    out_put(c, data, (size_t)n);
  }
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
