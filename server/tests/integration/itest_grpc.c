/*
 * itest_grpc.c — end-to-end tests of a running kvnode over real HTTP/2.
 *
 * Built on nghttp2's *client* API: the same library the server links, but an
 * independent code path (client session, HPACK encoder, client-side flow
 * control), so this verifies the server's bytes on the wire rather than its
 * internals:
 *   - exact response messages, :status, grpc-status and grpc-message for
 *     every success and error path of Get/Put/Delete;
 *   - values of max_value bytes crossing many DATA frames and flow-control
 *     windows in both directions (the client keeps nghttp2's 64 KiB default
 *     window on the main connection, forcing partial reads in the server's
 *     data provider);
 *   - 300 concurrent streams on one connection (above the server's
 *     max_streams, so the client must queue against the advertised limit);
 *   - a stream cancelled mid-body (RST_STREAM) leaving the connection usable
 *     and nothing stored;
 *   - multi-connection concurrency with torn-value detection: writers replace
 *     "hot" keys with self-describing values while readers verify every value
 *     they receive is internally consistent — atomic replace, end to end.
 *
 * Usage:
 *   itest_grpc HOST PORT MAX_KEY MAX_VALUE     run the suite (exit 0 = pass)
 *   itest_grpc --wait-goaway HOST PORT SECS    hold an idle connection until
 *       the server sends GOAWAY(NO_ERROR) and then closes it (exit 0), or
 *       fail after SECS seconds (exit 1). Used by run.sh to verify graceful
 *       shutdown and idle-timeout handling.
 *   itest_grpc --expect-snapshot HOST PORT N   verify the N records written by
 *       mksnap (snapgen.h) are served byte-exact, and record N is absent.
 *   itest_grpc --drain-check HOST PORT SECS    open a Health/Check stream,
 *       hold its body until the server's GOAWAY (run.sh sends SIGTERM), then
 *       release it: the in-flight request must still be answered, with
 *       NOT_SERVING, before the server closes the connection (exit 0).
 *
 * MAX_KEY/MAX_VALUE must equal the server's KVNODE_MAX_KEY_BYTES and
 * KVNODE_MAX_VALUE_BYTES: the boundary cases are computed from them.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>

#include <nghttp2/nghttp2.h>

#include "snapgen.h"
#include "test_common.h"

#define PATH_GET    "/kvnode.v1.KvNode/Get"
#define PATH_PUT    "/kvnode.v1.KvNode/Put"
#define PATH_DELETE "/kvnode.v1.KvNode/Delete"
#define PATH_HEALTH "/grpc.health.v1.Health/Check"
#define GRPC_CT     "application/grpc"
#define CALL_TIMEOUT_MS 20000

/* Per-run key prefix: makes keys unique even against a long-lived server. */
static char g_nonce[32];
static uint64_t g_max_key, g_max_value;
static const char *g_host, *g_port;

/* ========================================================================= */
/* Protobuf / gRPC framing helpers                                           */
/* ========================================================================= */

/**
 * Writes a protobuf base-128 varint.
 * @param p  destination with at least 10 writable bytes
 * @param v  value to encode
 * @returns bytes written (1..10)
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
 * Reads a protobuf varint without reading past end.
 * @param p    cursor, advanced past the varint on success
 * @param end  one past the last readable byte
 * @param out  receives the decoded value
 * @returns 1 on success, 0 if truncated or longer than 10 bytes
 */
static int get_varint(const uint8_t **p, const uint8_t *end, uint64_t *out) {
  uint64_t v = 0;
  for (int shift = 0; shift < 70 && *p < end; shift += 7) {
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
 * Writes the 5-byte gRPC message prefix.
 * @param p     destination (5 bytes)
 * @param flag  compressed flag byte (0 = uncompressed; other values are
 *              used deliberately by the framing tests)
 * @param len   message length, encoded big-endian
 */
static void set_prefix(uint8_t *p, uint8_t flag, uint32_t len) {
  p[0] = flag;
  p[1] = (uint8_t)(len >> 24);
  p[2] = (uint8_t)(len >> 16);
  p[3] = (uint8_t)(len >> 8);
  p[4] = (uint8_t)len;
}

/** A heap-allocated request body (gRPC prefix + protobuf). Free p. */
typedef struct {
  uint8_t *p;
  size_t len;
} Msg;

/**
 * Builds a gRPC-framed KV request {1: key, 2: value}.
 * @param key   key bytes, or NULL to omit field 1 entirely
 * @param klen  key length (ignored when key is NULL)
 * @param val   value bytes, or NULL to omit field 2 ("" = present but empty)
 * @param vlen  value length (ignored when val is NULL)
 * @returns the message; exits the process on allocation failure
 */
static Msg msg_kv(const void *key, size_t klen, const void *val, size_t vlen) {
  Msg m;
  m.p = malloc(5 + 11 + klen + 11 + vlen);
  if (!m.p) { perror("malloc"); exit(2); }
  size_t n = 5;
  if (key) {
    m.p[n++] = 0x0A;                    /* field 1, length-delimited */
    n += put_varint(m.p + n, klen);
    memcpy(m.p + n, key, klen);
    n += klen;
  }
  if (val) {
    m.p[n++] = 0x12;                    /* field 2, length-delimited */
    n += put_varint(m.p + n, vlen);
    if (vlen) memcpy(m.p + n, val, vlen);
    n += vlen;
  }
  set_prefix(m.p, 0, (uint32_t)(n - 5));
  m.len = n;
  return m;
}

/**
 * Copies arbitrary bytes into a Msg (for malformed-framing cases).
 * @param bytes  body bytes exactly as they should go on the wire
 * @param len    number of bytes
 * @returns the message; exits the process on allocation failure
 */
static Msg msg_raw(const void *bytes, size_t len) {
  Msg m = {malloc(len ? len : 1), len};
  if (!m.p) { perror("malloc"); exit(2); }
  if (len) memcpy(m.p, bytes, len);
  return m;
}

/* ========================================================================= */
/* Client connection                                                         */
/* ========================================================================= */

/** One gRPC call: the request to send and the response collected for it. */
typedef struct {
  int32_t stream_id;
  const uint8_t *req;       /**< request body (caller-owned), NULL = none */
  size_t req_len;
  size_t req_off;           /**< bytes handed to nghttp2 so far */
  size_t defer_at;          /**< >0: stall the body at this offset (cancel test) */
  int http_status;          /**< :status, 0 until received */
  int grpc_status;          /**< grpc-status, -1 when absent */
  char grpc_message[160];
  char accept_encoding[64];
  char content_type[48];    /**< response content-type */
  char allow[16];           /**< allow (405 responses) */
  char accept[40];          /**< accept (415 responses) */
  uint8_t *body;            /**< response DATA (malloc'd) */
  size_t body_len, body_cap;
  int closed;               /**< on_stream_close ran */
  uint32_t close_error;     /**< RST_STREAM / close error code */
} Call;

/** One HTTP/2 connection, driven synchronously by pump(). */
typedef struct {
  int fd;
  nghttp2_session *session;
  uint8_t *out;             /**< serialized frames not yet accepted by send() */
  size_t out_len, out_off, out_cap;
  int open_calls;           /**< submitted calls whose stream has not closed */
  int eof;                  /**< the server closed (or reset) the connection */
  int goaway;               /**< a GOAWAY frame was received */
  uint32_t goaway_error;
  int32_t goaway_last_stream;
} Client;

/**
 * Parses a small decimal header value (not NUL-terminated).
 * @param v  value bytes
 * @param n  value length
 * @returns the number, or -1 if empty, too long or not all digits
 */
static int parse_small_int(const uint8_t *v, size_t n) {
  if (n == 0 || n > 6) return -1;
  int x = 0;
  for (size_t i = 0; i < n; i++) {
    if (v[i] < '0' || v[i] > '9') return -1;
    x = x * 10 + (v[i] - '0');
  }
  return x;
}

/**
 * Copies a header value into a fixed buffer, truncating and NUL-terminating.
 * @param dst  destination buffer
 * @param cap  destination capacity (> 0)
 * @param v    value bytes
 * @param n    value length
 */
static void copy_header(char *dst, size_t cap, const uint8_t *v, size_t n) {
  if (n >= cap) n = cap - 1;
  memcpy(dst, v, n);
  dst[n] = 0;
}

/**
 * nghttp2 on_header callback: records the headers the tests assert on, both
 * from the response HEADERS and from the trailer HEADERS (trailers-only
 * errors put grpc-status in the first and only HEADERS frame).
 * @returns 0 (never fails the session)
 */
static int on_header(nghttp2_session *s, const nghttp2_frame *frame, const uint8_t *name,
                     size_t namelen, const uint8_t *value, size_t valuelen, uint8_t flags,
                     void *ud) {
  (void)flags;
  (void)ud;
  if (frame->hd.type != NGHTTP2_HEADERS) return 0;
  Call *c = nghttp2_session_get_stream_user_data(s, frame->hd.stream_id);
  if (!c) return 0;
  if (namelen == 7 && memcmp(name, ":status", 7) == 0)
    c->http_status = parse_small_int(value, valuelen);
  else if (namelen == 11 && memcmp(name, "grpc-status", 11) == 0)
    c->grpc_status = parse_small_int(value, valuelen);
  else if (namelen == 12 && memcmp(name, "grpc-message", 12) == 0)
    copy_header(c->grpc_message, sizeof c->grpc_message, value, valuelen);
  else if (namelen == 20 && memcmp(name, "grpc-accept-encoding", 20) == 0)
    copy_header(c->accept_encoding, sizeof c->accept_encoding, value, valuelen);
  else if (namelen == 12 && memcmp(name, "content-type", 12) == 0)
    copy_header(c->content_type, sizeof c->content_type, value, valuelen);
  else if (namelen == 5 && memcmp(name, "allow", 5) == 0)
    copy_header(c->allow, sizeof c->allow, value, valuelen);
  else if (namelen == 6 && memcmp(name, "accept", 6) == 0)
    copy_header(c->accept, sizeof c->accept, value, valuelen);
  return 0;
}

/**
 * nghttp2 on_data_chunk_recv callback: appends response DATA to its call.
 * @returns 0, or NGHTTP2_ERR_CALLBACK_FAILURE when out of memory (fatal for
 *          the test connection, which is the right outcome in a test)
 */
static int on_data(nghttp2_session *s, uint8_t flags, int32_t stream_id, const uint8_t *data,
                   size_t len, void *ud) {
  (void)flags;
  (void)ud;
  Call *c = nghttp2_session_get_stream_user_data(s, stream_id);
  if (!c) return 0;
  if (c->body_len + len > c->body_cap) {
    size_t cap = (c->body_len + len) * 2;
    uint8_t *p = realloc(c->body, cap);
    if (!p) return NGHTTP2_ERR_CALLBACK_FAILURE;
    c->body = p;
    c->body_cap = cap;
  }
  memcpy(c->body + c->body_len, data, len);
  c->body_len += len;
  return 0;
}

/**
 * nghttp2 on_stream_close callback: marks the call finished.
 * @returns 0
 */
static int on_close(nghttp2_session *s, int32_t stream_id, uint32_t error_code, void *ud) {
  Client *cl = ud;
  Call *c = nghttp2_session_get_stream_user_data(s, stream_id);
  if (!c) return 0;
  c->closed = 1;
  c->close_error = error_code;
  cl->open_calls--;
  return 0;
}

/**
 * nghttp2 on_frame_recv callback: records GOAWAY for the shutdown tests.
 * @returns 0
 */
static int on_frame(nghttp2_session *s, const nghttp2_frame *frame, void *ud) {
  (void)s;
  Client *cl = ud;
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    cl->goaway = 1;
    cl->goaway_error = frame->goaway.error_code;
    cl->goaway_last_stream = frame->goaway.last_stream_id;
  }
  return 0;
}

/**
 * Request body provider: streams Call.req, optionally stalling at defer_at.
 * @returns bytes copied, or NGHTTP2_ERR_DEFERRED while stalled (the cancel
 *          test resets the stream instead of ever resuming it)
 */
static nghttp2_ssize read_body(nghttp2_session *s, int32_t stream_id, uint8_t *buf,
                               size_t length, uint32_t *data_flags, nghttp2_data_source *src,
                               void *ud) {
  (void)s;
  (void)stream_id;
  (void)ud;
  Call *c = src->ptr;
  size_t limit = c->defer_at ? c->defer_at : c->req_len;
  if (c->req_off >= limit && c->req_off < c->req_len) return NGHTTP2_ERR_DEFERRED;
  size_t n = limit - c->req_off;
  if (n > length) n = length;
  memcpy(buf, c->req + c->req_off, n);
  c->req_off += n;
  if (c->req_off == c->req_len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  return (nghttp2_ssize)n;
}

/**
 * Connects and starts an HTTP/2 client session.
 * @param cl      connection to initialize
 * @param host    server host
 * @param port    server port (string)
 * @param window  initial stream receive window advertised to the server;
 *                65535 keeps DATA frames small and exercises the server's
 *                partial data-provider reads, larger values speed up load
 * @returns 0 on success, -1 on failure (reason printed)
 * @remarks The socket is non-blocking: pump() interleaves reads and writes,
 *          so a large request and a large response can never deadlock on
 *          full socket buffers.
 */
static int client_open(Client *cl, const char *host, const char *port, uint32_t window) {
  memset(cl, 0, sizeof *cl);
  cl->fd = -1;
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(rc));
    return -1;
  }
  for (struct addrinfo *a = res; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
      cl->fd = fd;
      break;
    }
    close(fd);
  }
  freeaddrinfo(res);
  if (cl->fd < 0) {
    fprintf(stderr, "connect %s:%s: %s\n", host, port, strerror(errno));
    return -1;
  }
  int one = 1;
  setsockopt(cl->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  fcntl(cl->fd, F_SETFL, fcntl(cl->fd, F_GETFL) | O_NONBLOCK);

  nghttp2_session_callbacks *cbs;
  if (nghttp2_session_callbacks_new(&cbs) != 0) return -1;
  nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data);
  nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_close);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame);
  rc = nghttp2_session_client_new(&cl->session, cbs, cl);
  nghttp2_session_callbacks_del(cbs);   /* the session keeps its own copy */
  if (rc != 0) return -1;
  /* nghttp2 sends the client preface itself; SETTINGS must be submitted. */
  nghttp2_settings_entry iv[] = {{NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
                                 {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, window}};
  if (nghttp2_submit_settings(cl->session, NGHTTP2_FLAG_NONE, iv, 2) != 0) return -1;
  if (window > 65535)   /* connection window too, or it caps the stream windows */
    nghttp2_session_set_local_window_size(cl->session, NGHTTP2_FLAG_NONE, 0, 64 << 20);
  return 0;
}

/**
 * Closes the socket and frees the session.
 * @param cl  connection (may be partially opened)
 */
static void client_close(Client *cl) {
  if (cl->session) nghttp2_session_del(cl->session);
  if (cl->fd >= 0) close(cl->fd);
  free(cl->out);
  memset(cl, 0, sizeof *cl);
  cl->fd = -1;
}

/**
 * Appends serialized frames to the pending output.
 * @param cl    connection
 * @param data  bytes from nghttp2_session_mem_send2 (only valid until the
 *              next mem_send2 call, hence the copy)
 * @param n     byte count
 * @returns 0, or -1 when out of memory
 */
static int out_append(Client *cl, const uint8_t *data, size_t n) {
  if (cl->out_len + n > cl->out_cap) {
    size_t cap = (cl->out_len + n) * 2;
    uint8_t *p = realloc(cl->out, cap);
    if (!p) return -1;
    cl->out = p;
    cl->out_cap = cap;
  }
  memcpy(cl->out + cl->out_len, data, n);
  cl->out_len += n;
  return 0;
}

/**
 * Serializes pending frames into the output buffer without sending them.
 * @param cl  connection
 * @returns 0, or -1 on a session error or out of memory
 * @remarks Separate from pump() so a test can queue frames from several
 *          submissions and put them on the wire in a single write.
 */
static int collect(Client *cl) {
  /* Bound the copy so a huge pending request cannot balloon memory. */
  while (cl->out_len - cl->out_off < (1u << 20)) {
    const uint8_t *data;
    nghttp2_ssize n = nghttp2_session_mem_send2(cl->session, &data);
    if (n < 0) {
      fprintf(stderr, "mem_send2: %s\n", nghttp2_strerror((int)n));
      return -1;
    }
    if (n == 0) break;
    if (out_append(cl, data, (size_t)n) != 0) return -1;
  }
  return 0;
}

/**
 * Moves bytes in both directions once: serializes pending frames, writes
 * what the socket accepts, waits up to timeout_ms, feeds received bytes to
 * nghttp2.
 * @param cl          connection
 * @param timeout_ms  poll timeout
 * @returns 0 on progress or timeout, -1 on a fatal session/socket error
 * @remarks EOF and ECONNRESET are not errors here: they set cl->eof, and the
 *          caller decides whether that was expected.
 */
static int pump(Client *cl, int timeout_ms) {
  if (collect(cl) != 0) return -1;
  while (cl->out_off < cl->out_len && !cl->eof) {
    ssize_t w = send(cl->fd, cl->out + cl->out_off, cl->out_len - cl->out_off, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      if (errno == EPIPE || errno == ECONNRESET) { cl->eof = 1; break; }
      perror("send");
      return -1;
    }
    cl->out_off += (size_t)w;
  }
  if (cl->out_off == cl->out_len) cl->out_off = cl->out_len = 0;
  if (cl->eof) return 0;

  struct pollfd p = {.fd = cl->fd, .events = POLLIN | (cl->out_len ? POLLOUT : 0)};
  int r = poll(&p, 1, timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  if (r == 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) return 0;
  uint8_t buf[65536];
  ssize_t n = recv(cl->fd, buf, sizeof buf, 0);
  if (n == 0) { cl->eof = 1; return 0; }
  if (n < 0) {
    if (errno == EAGAIN || errno == EINTR) return 0;
    if (errno == ECONNRESET) { cl->eof = 1; return 0; }
    perror("recv");
    return -1;
  }
  nghttp2_ssize rc = nghttp2_session_mem_recv2(cl->session, buf, (size_t)n);
  if (rc < 0) {
    fprintf(stderr, "mem_recv2: %s\n", nghttp2_strerror((int)rc));
    return -1;
  }
  return 0;
}

/**
 * Pumps until every submitted call has closed.
 * @param cl          connection
 * @param timeout_ms  overall deadline
 * @returns 0 when all calls closed, -1 on connection failure/EOF or timeout
 */
static int run_until_idle(Client *cl, int timeout_ms) {
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  while (cl->open_calls > 0) {
    if (cl->eof) {
      fprintf(stderr, "connection closed with %d call(s) open\n", cl->open_calls);
      return -1;
    }
    if (now_ns() > deadline) {
      fprintf(stderr, "timeout with %d call(s) open\n", cl->open_calls);
      return -1;
    }
    if (pump(cl, 50) < 0) return -1;
  }
  return 0;
}

/**
 * Builds a header entry; nghttp2 copies names and values at submission.
 * @param name   header name (lowercase)
 * @param value  header value
 * @returns the entry
 */
static nghttp2_nv mknv(const char *name, const char *value) {
  nghttp2_nv nv = {(uint8_t *)name, (uint8_t *)value, strlen(name), strlen(value),
                   NGHTTP2_NV_FLAG_NONE};
  return nv;
}

/**
 * Submits one request without waiting for it.
 * @param cl        connection
 * @param c         call record, reset here; must stay alive until it closes
 * @param method    HTTP method
 * @param path      request path
 * @param ctype     content-type, or NULL to omit the header
 * @param encoding  grpc-encoding, or NULL to omit the header
 * @param body      request body, or NULL for a HEADERS-only request
 * @param len       body length
 * @returns 0 on success, -1 if nghttp2 refused the submission
 */
static int call_submit(Client *cl, Call *c, const char *method, const char *path,
                       const char *ctype, const char *encoding, const uint8_t *body,
                       size_t len) {
  size_t defer_at = c->defer_at;
  memset(c, 0, sizeof *c);
  c->grpc_status = -1;
  c->req = body;
  c->req_len = len;
  c->defer_at = defer_at;
  nghttp2_nv nva[8];
  size_t n = 0;
  nva[n++] = mknv(":method", method);
  nva[n++] = mknv(":scheme", "http");
  nva[n++] = mknv(":authority", "kvnode");
  nva[n++] = mknv(":path", path);
  if (ctype) nva[n++] = mknv("content-type", ctype);
  nva[n++] = mknv("te", "trailers");
  if (encoding) nva[n++] = mknv("grpc-encoding", encoding);
  nghttp2_data_provider2 prd = {.source = {.ptr = c}, .read_callback = read_body};
  int32_t id = nghttp2_submit_request2(cl->session, NULL, nva, n, body ? &prd : NULL, c);
  if (id < 0) {
    fprintf(stderr, "submit_request2: %s\n", nghttp2_strerror(id));
    return -1;
  }
  c->stream_id = id;
  cl->open_calls++;
  return 0;
}

/**
 * Runs one request to completion.
 * @returns 0 when the stream closed, -1 on submission/connection failure
 * @see call_submit for the parameters
 */
static int call_run(Client *cl, Call *c, const char *method, const char *path,
                    const char *ctype, const char *encoding, const uint8_t *body, size_t len) {
  c->defer_at = 0;
  if (call_submit(cl, c, method, path, ctype, encoding, body, len) != 0) return -1;
  return run_until_idle(cl, CALL_TIMEOUT_MS);
}

/**
 * Frees a call's response buffer.
 * @param c  call
 */
static void call_free(Call *c) {
  free(c->body);
  c->body = NULL;
  c->body_len = c->body_cap = 0;
}

/** A decoded response message: {1: bool, 2: bytes}. */
typedef struct {
  int ok;                   /**< the body was exactly one well-formed message */
  int flag;                 /**< field 1 (found/created/deleted) */
  int has_value;            /**< field 2 present */
  const uint8_t *value;     /**< points into Call.body */
  size_t value_len;
} Reply;

/**
 * Decodes a successful call's response body.
 * @param c  finished call
 * @returns the reply; ok = 0 when framing or protobuf is invalid, or when the
 *          message contains a field the contract does not define (the server
 *          must never send unknown fields)
 */
static Reply parse_reply(const Call *c) {
  Reply r = {0};
  if (c->body_len < 5 || c->body[0] != 0) return r;
  uint32_t mlen = (uint32_t)c->body[1] << 24 | (uint32_t)c->body[2] << 16 |
                  (uint32_t)c->body[3] << 8 | c->body[4];
  if ((size_t)mlen + 5 != c->body_len) return r;
  const uint8_t *p = c->body + 5, *end = p + mlen;
  while (p < end) {
    uint64_t tag, v;
    if (!get_varint(&p, end, &tag)) return r;
    if (tag == 0x08) {
      if (!get_varint(&p, end, &v) || v > 1) return r;
      r.flag = (int)v;
    } else if (tag == 0x12) {
      if (!get_varint(&p, end, &v) || v > (uint64_t)(end - p)) return r;
      r.has_value = 1;
      r.value = p;
      r.value_len = (size_t)v;
      p += v;
    } else {
      return r;
    }
  }
  r.ok = 1;
  return r;
}

/**
 * Checks that a call succeeded at every layer.
 * @param what  label for diagnostics
 * @param c     finished call
 * @returns 1 when HTTP 200, grpc-status 0, clean close; 0 otherwise (printed)
 */
static int call_ok(const char *what, const Call *c) {
  if (c->closed && c->close_error == 0 && c->http_status == 200 && c->grpc_status == 0)
    return 1;
  fprintf(stderr, "  %s: closed=%d err=%u :status=%d grpc-status=%d grpc-message=\"%s\"\n",
          what, c->closed, c->close_error, c->http_status, c->grpc_status, c->grpc_message);
  return 0;
}

/**
 * Put through gRPC.
 * @param cl  connection
 * @param k   key bytes
 * @param kl  key length
 * @param v   value bytes (NULL = omit the field, i.e. empty value)
 * @param vl  value length
 * @returns 1 created, 0 replaced, -1 on any transport/status/format error
 */
static int op_put(Client *cl, const void *k, size_t kl, const void *v, size_t vl) {
  Msg m = msg_kv(k, kl, v, vl);
  Call c;
  int result = -1;
  if (call_run(cl, &c, "POST", PATH_PUT, GRPC_CT, NULL, m.p, m.len) == 0 && call_ok("Put", &c)) {
    Reply r = parse_reply(&c);
    if (r.ok && !r.has_value) result = r.flag;
    else fprintf(stderr, "  Put: malformed reply (%zu bytes)\n", c.body_len);
  }
  call_free(&c);
  free(m.p);
  return result;
}

/**
 * Get through gRPC.
 * @param cl   connection
 * @param k    key bytes
 * @param kl   key length
 * @param out  receives a malloc'd copy of the value when found (NULL for an
 *             empty value), else NULL; caller frees
 * @param ol   receives the value length (0 when not found)
 * @returns 1 found, 0 not found, -1 on any error
 */
static int op_get(Client *cl, const void *k, size_t kl, uint8_t **out, size_t *ol) {
  Msg m = msg_kv(k, kl, NULL, 0);
  Call c;
  int result = -1;
  *out = NULL;
  *ol = 0;
  if (call_run(cl, &c, "POST", PATH_GET, GRPC_CT, NULL, m.p, m.len) == 0 && call_ok("Get", &c)) {
    Reply r = parse_reply(&c);
    /* not found must be the empty message: no found=false, no value field */
    if (r.ok && (r.flag || (!r.has_value && c.body_len == 5))) {
      result = r.flag;
      if (r.flag && r.value_len) {
        *out = malloc(r.value_len);
        memcpy(*out, r.value, r.value_len);
        *ol = r.value_len;
      }
    } else {
      fprintf(stderr, "  Get: malformed reply (%zu bytes)\n", c.body_len);
    }
  }
  call_free(&c);
  free(m.p);
  return result;
}

/**
 * Delete through gRPC.
 * @param cl  connection
 * @param k   key bytes
 * @param kl  key length
 * @returns 1 deleted, 0 absent, -1 on any error
 */
static int op_delete(Client *cl, const void *k, size_t kl) {
  Msg m = msg_kv(k, kl, NULL, 0);
  Call c;
  int result = -1;
  if (call_run(cl, &c, "POST", PATH_DELETE, GRPC_CT, NULL, m.p, m.len) == 0 &&
      call_ok("Delete", &c)) {
    Reply r = parse_reply(&c);
    if (r.ok && !r.has_value) result = r.flag;
  }
  call_free(&c);
  free(m.p);
  return result;
}

/**
 * Sends one request and asserts its error answer.
 * @param name      case label for diagnostics
 * @param cl        connection
 * @param method    HTTP method
 * @param path      request path
 * @param ctype     content-type, or NULL to omit
 * @param encoding  grpc-encoding, or NULL to omit
 * @param m         request body (freed here), or a Msg with p == NULL for none
 * @param http      expected :status
 * @param grpc      expected grpc-status, or -1 when it must be absent
 * @param msg       substring required in grpc-message, or NULL
 * @remarks gRPC errors must be Trailers-Only (a single HEADERS frame, no
 *          DATA). Plain HTTP errors must carry a text/plain body plus the
 *          header RFC 9110 asks for: `allow: POST` on 405 (a MUST) and
 *          `accept: application/grpc` on 415.
 */
static void expect_error(const char *name, Client *cl, const char *method, const char *path,
                         const char *ctype, const char *encoding, Msg m, int http, int grpc,
                         const char *msg) {
  Call c;
  int rc = call_run(cl, &c, method, path, ctype, encoding, m.p, m.len);
  int body_ok = grpc >= 0 ? c.body_len == 0
                          : c.body_len > 0 && strncmp(c.content_type, "text/plain", 10) == 0;
  if (http == 405) body_ok = body_ok && strcmp(c.allow, "POST") == 0;
  if (http == 415) body_ok = body_ok && strcmp(c.accept, "application/grpc") == 0;
  int ok = rc == 0 && c.closed && c.close_error == 0 && c.http_status == http &&
           c.grpc_status == grpc && body_ok && (!msg || strstr(c.grpc_message, msg));
  if (!ok)
    fprintf(stderr,
            "  case %-22s rc=%d :status=%d (want %d) grpc-status=%d (want %d) "
            "message=\"%s\" body=%zu err=%u\n",
            name, rc, c.http_status, http, c.grpc_status, grpc, c.grpc_message, c.body_len,
            c.close_error);
  CHECK(ok);
  call_free(&c);
  free(m.p);
}

/**
 * Formats a run-unique key.
 * @param buf     destination
 * @param cap     destination capacity
 * @param kind    short label ("crud", "pipe", ...)
 * @param i       index within the kind
 * @returns key length
 */
static size_t mkkey(char *buf, size_t cap, const char *kind, uint64_t i) {
  return (size_t)snprintf(buf, cap, "%s-%s-%" PRIu64, g_nonce, kind, i);
}

/* ========================================================================= */
/* Single-connection cases                                                   */
/* ========================================================================= */

/**
 * Put/Get/Delete life cycle, empty values, binary keys, key-length edges.
 * @param cl  connection
 */
static void test_crud(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "crud", 1);
  uint8_t *v;
  size_t vl;
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);
  CHECK(op_delete(cl, k, kl) == 0);
  CHECK(op_put(cl, k, kl, "value-1", 7) == 1);
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == 7 && memcmp(v, "value-1", 7) == 0);
  free(v);
  CHECK(op_put(cl, k, kl, "v2", 2) == 0);                  /* replace */
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == 2 && memcmp(v, "v2", 2) == 0);
  free(v);
  CHECK(op_delete(cl, k, kl) == 1);
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);
  CHECK(op_delete(cl, k, kl) == 0);

  /* empty value: sent as a present-but-empty field and as an omitted field */
  kl = mkkey(k, sizeof k, "crud", 2);
  CHECK(op_put(cl, k, kl, "", 0) == 1);
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == 0 && v == NULL);
  kl = mkkey(k, sizeof k, "crud", 3);
  CHECK(op_put(cl, k, kl, NULL, 0) == 1);
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == 0);

  /* binary key: NULs and 0xFF must be compared exactly, not as C strings;
   * the whole run nonce follows the binary part so reruns never collide */
  uint8_t bk[12 + sizeof g_nonce] = {0, 0xFF, 0, 1, 0x80, 0x7F, 0, 0, 'b', 'i', 'n', 0};
  size_t bkl = 12 + strlen(g_nonce);
  memcpy(bk + 12, g_nonce, bkl - 12);
  CHECK(op_put(cl, bk, bkl, "B", 1) == 1);
  CHECK(op_get(cl, bk, 11, &v, &vl) == 0);                 /* a prefix is another key */
  CHECK(op_get(cl, bk, bkl, &v, &vl) == 1 && vl == 1 && v[0] == 'B');
  free(v);

  /* key of exactly max_key bytes is accepted */
  uint8_t *big = malloc(g_max_key);
  memset(big, 'K', g_max_key);
  memcpy(big, g_nonce, strlen(g_nonce));
  CHECK(op_put(cl, big, g_max_key, "max", 3) == 1);
  CHECK(op_get(cl, big, g_max_key, &v, &vl) == 1 && vl == 3);
  free(v);
  CHECK(op_delete(cl, big, g_max_key) == 1);
  free(big);
}

/**
 * A value of exactly max_value bytes round-trips byte-exact. With the
 * default 64 KiB client window the response spans many DATA frames and
 * WINDOW_UPDATEs, and the request exceeds the server's 1 MiB stream window.
 * @param cl  connection
 */
static void test_large_value(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "large", 1);
  uint8_t *val = malloc(g_max_value);
  uint64_t s = 0x1234;
  rng_fill(&s, val, g_max_value);
  CHECK(op_put(cl, k, kl, val, g_max_value) == 1);
  uint8_t *v;
  size_t vl;
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == g_max_value &&
        memcmp(v, val, g_max_value) == 0);
  free(v);
  CHECK(op_delete(cl, k, kl) == 1);
  free(val);
}

/**
 * Every HTTP- and gRPC-level error path, each asserting the exact status and
 * message, and that the connection stays usable afterwards.
 * @param cl  connection
 */
static void test_errors(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "err", 1);
  Msg none = {NULL, 0};

  /* HTTP level: not a POST / not gRPC */
  expect_error("GET method", cl, "GET", PATH_GET, GRPC_CT, NULL, none, 405, -1, NULL);
  expect_error("json content-type", cl, "POST", PATH_GET, "application/json", NULL,
               msg_kv(k, kl, NULL, 0), 415, -1, NULL);
  expect_error("grpcx content-type", cl, "POST", PATH_GET, "application/grpcx", NULL,
               msg_kv(k, kl, NULL, 0), 415, -1, NULL);
  expect_error("no content-type", cl, "POST", PATH_GET, NULL, NULL, msg_kv(k, kl, NULL, 0),
               415, -1, NULL);

  /* routing */
  expect_error("unknown method", cl, "POST", "/kvnode.v1.KvNode/Scan", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("unknown service", cl, "POST", "/other.v1.Svc/Get", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("service prefix only", cl, "POST", "/kvnode.v1.KvNode/", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("method case", cl, "POST", "/kvnode.v1.KvNode/get", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");

  /* framing: exactly one uncompressed message */
  expect_error("empty body", cl, "POST", PATH_GET, GRPC_CT, NULL, none, 200, 13,
               "exactly one");
  expect_error("short prefix", cl, "POST", PATH_GET, GRPC_CT, NULL, msg_raw("\0\0\0", 3),
               200, 13, "exactly one");
  Msg m = msg_kv(k, kl, NULL, 0);
  set_prefix(m.p, 0, (uint32_t)(m.len - 5 + 1));          /* claims one byte more */
  expect_error("length too long", cl, "POST", PATH_GET, GRPC_CT, NULL, m, 200, 13,
               "exactly one");
  Msg one = msg_kv(k, kl, NULL, 0);
  Msg two = {malloc(one.len * 2), one.len * 2};
  memcpy(two.p, one.p, one.len);
  memcpy(two.p + one.len, one.p, one.len);
  free(one.p);
  expect_error("two messages", cl, "POST", PATH_GET, GRPC_CT, NULL, two, 200, 13,
               "exactly one");
  m = msg_kv(k, kl, NULL, 0);
  m.p[0] = 2;
  expect_error("flag 2", cl, "POST", PATH_GET, GRPC_CT, NULL, m, 200, 13, "exactly one");

  /* compression: grpc-go/grpc-core semantics */
  m = msg_kv(k, kl, NULL, 0);
  m.p[0] = 1;
  expect_error("flag 1, no encoding", cl, "POST", PATH_GET, GRPC_CT, NULL, m, 200, 13,
               "compressed flag set without a grpc-encoding");
  m = msg_kv(k, kl, NULL, 0);
  m.p[0] = 1;
  expect_error("flag 1, identity", cl, "POST", PATH_GET, GRPC_CT, "identity", m, 200, 13,
               "compressed flag set without a grpc-encoding");
  m = msg_kv(k, kl, NULL, 0);
  m.p[0] = 1;
  Call c;
  CHECK(call_run(cl, &c, "POST", PATH_GET, GRPC_CT, "gzip", m.p, m.len) == 0);
  CHECK(c.http_status == 200 && c.grpc_status == 12 && c.body_len == 0);
  CHECK(strcmp(c.accept_encoding, "identity") == 0);       /* tells the client what works */
  call_free(&c);
  free(m.p);
  m = msg_kv(k, kl, NULL, 0);                              /* flag 0 + gzip header: fine */
  CHECK(call_run(cl, &c, "POST", PATH_GET, GRPC_CT, "gzip", m.p, m.len) == 0 &&
        call_ok("flag 0 + gzip", &c));
  call_free(&c);
  free(m.p);

  /* protobuf payload */
  expect_error("truncated varint", cl, "POST", PATH_GET, GRPC_CT, NULL,
               msg_raw("\0\0\0\0\2\x0a\xff", 7), 200, 3, "malformed");
  expect_error("length past end", cl, "POST", PATH_GET, GRPC_CT, NULL,
               msg_raw("\0\0\0\0\3\x0a\x05k", 8), 200, 3, "malformed");
  expect_error("missing key", cl, "POST", PATH_PUT, GRPC_CT, NULL, msg_kv(NULL, 0, "v", 1),
               200, 3, "key length");
  expect_error("empty key", cl, "POST", PATH_GET, GRPC_CT, NULL, msg_kv("", 0, NULL, 0), 200,
               3, "key length");
  uint8_t *big = malloc(g_max_key + 1);
  memset(big, 'K', g_max_key + 1);
  expect_error("key over limit", cl, "POST", PATH_PUT, GRPC_CT, NULL,
               msg_kv(big, g_max_key + 1, "v", 1), 200, 3, "key length");
  free(big);
  uint8_t *val = calloc(1, g_max_value + g_max_key + 128);
  expect_error("value over limit", cl, "POST", PATH_PUT, GRPC_CT, NULL,
               msg_kv(k, kl, val, g_max_value + 1), 200, 3, "value too large");
  expect_error("message too large", cl, "POST", PATH_PUT, GRPC_CT, NULL,
               msg_kv(k, kl, val, g_max_value + g_max_key + 128), 200, 8,
               "request message too large");
  free(val);

  /* unknown fields are skipped, as protobuf requires (forward compatibility):
   * field 15 varint, field 16 fixed64, field 17 fixed32, then the key */
  uint8_t fwd[64] = {0, 0, 0, 0, 0, 0x78, 0x2A, 0x81, 0x01, 1, 2, 3, 4, 5, 6, 7, 8,
                     0x8D, 0x01, 1, 2, 3, 4};
  size_t fl = 23;
  fwd[fl++] = 0x0A;
  fwd[fl++] = (uint8_t)kl;
  memcpy(fwd + fl, k, kl);
  fl += kl;
  set_prefix(fwd, 0, (uint32_t)(fl - 5));
  CHECK(call_run(cl, &c, "POST", PATH_GET, GRPC_CT, NULL, fwd, fl) == 0 &&
        call_ok("unknown fields", &c) && c.body_len == 5);  /* not found, empty reply */
  call_free(&c);

  /* the connection survived every error */
  CHECK(op_put(cl, k, kl, "alive", 5) == 1);
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * 300 requests in flight on one connection (the server allows 100
 * concurrent streams; nghttp2 queues the rest client-side), then 300 Gets.
 * @param cl  connection
 */
static void test_pipelined(Client *cl) {
  enum { M = 300 };
  static Call calls[M];
  static Msg msgs[M];
  char k[64], v[64];
  for (int i = 0; i < M; i++) {
    size_t kl = mkkey(k, sizeof k, "pipe", (uint64_t)i);
    size_t vl = (size_t)snprintf(v, sizeof v, "value-%d", i * 7);
    msgs[i] = msg_kv(k, kl, v, vl);
    calls[i].defer_at = 0;
    CHECK(call_submit(cl, &calls[i], "POST", PATH_PUT, GRPC_CT, NULL, msgs[i].p,
                      msgs[i].len) == 0);
  }
  CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0);
  int created = 0;
  for (int i = 0; i < M; i++) {
    Reply r = parse_reply(&calls[i]);
    if (call_ok("pipelined Put", &calls[i]) && r.ok && r.flag == 1) created++;
    call_free(&calls[i]);
    free(msgs[i].p);
  }
  CHECK(created == M);

  for (int i = 0; i < M; i++) {
    size_t kl = mkkey(k, sizeof k, "pipe", (uint64_t)i);
    msgs[i] = msg_kv(k, kl, NULL, 0);
    CHECK(call_submit(cl, &calls[i], "POST", PATH_GET, GRPC_CT, NULL, msgs[i].p,
                      msgs[i].len) == 0);
  }
  CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0);
  int exact = 0;
  for (int i = 0; i < M; i++) {
    size_t vl = (size_t)snprintf(v, sizeof v, "value-%d", i * 7);
    Reply r = parse_reply(&calls[i]);
    if (call_ok("pipelined Get", &calls[i]) && r.ok && r.flag && r.value_len == vl &&
        memcmp(r.value, v, vl) == 0)
      exact++;
    call_free(&calls[i]);
    free(msgs[i].p);
  }
  CHECK(exact == M);
}

/**
 * A Put cancelled with RST_STREAM after 32 KiB of a 256 KiB body: the server
 * must release the partial request, store nothing, and keep the connection.
 * @param cl  connection
 */
static void test_cancel(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "cancel", 1);
  size_t vlen = 256 * 1024;
  uint8_t *val = malloc(vlen);
  memset(val, 'c', vlen);
  Msg m = msg_kv(k, kl, val, vlen);
  static Call c;
  c.defer_at = 32 * 1024;
  CHECK(call_submit(cl, &c, "POST", PATH_PUT, GRPC_CT, NULL, m.p, m.len) == 0);
  /* pump until the provider has stalled and those bytes left the client */
  uint64_t deadline = now_ns() + 5000000000ull;
  while ((c.req_off < c.defer_at || cl->out_len) && now_ns() < deadline && !cl->eof)
    CHECK(pump(cl, 20) == 0);
  for (int i = 0; i < 5; i++) pump(cl, 20);               /* let the server read them */
  CHECK(c.req_off == c.defer_at && !c.closed);
  CHECK(nghttp2_submit_rst_stream(cl->session, NGHTTP2_FLAG_NONE, c.stream_id,
                                  NGHTTP2_CANCEL) == 0);
  CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0);
  CHECK(c.closed && c.close_error == NGHTTP2_CANCEL);
  uint8_t *v;
  size_t vl;
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);                  /* nothing stored */
  CHECK(op_put(cl, k, kl, "after", 5) == 1);               /* connection still fine */
  CHECK(op_delete(cl, k, kl) == 1);
  call_free(&c);
  free(m.p);
  free(val);
}

/**
 * GETs reset by the client in the same write as their requests. The server
 * queues parsed GETs per read and answers them after the read, so a
 * RST_STREAM arriving in that same read must free the queued stream instead
 * of answering it, without disturbing the GETs around it.
 * @param cl  connection
 * @remarks All 32 requests are serialized first, then RST_STREAM frames for
 *          the odd ones are queued behind them, and everything goes out in
 *          one write (about 1.5 KB — one server read).
 */
static void test_reset_in_batch(Client *cl) {
  enum { M = 32 };
  static Call calls[M];
  static Msg msgs[M];
  char k[64];
  size_t kl = mkkey(k, sizeof k, "rib", 0);
  CHECK(op_put(cl, k, kl, "batch-value", 11) == 1);
  for (int i = 0; i < M; i++) {
    msgs[i] = msg_kv(k, kl, NULL, 0);
    calls[i].defer_at = 0;
    CHECK(call_submit(cl, &calls[i], "POST", PATH_GET, GRPC_CT, NULL, msgs[i].p, msgs[i].len) == 0);
  }
  CHECK(collect(cl) == 0); /* HEADERS + DATA(END_STREAM) of all 32, not sent yet */
  for (int i = 1; i < M; i += 2)
    CHECK(nghttp2_submit_rst_stream(cl->session, NGHTTP2_FLAG_NONE, calls[i].stream_id,
                                    NGHTTP2_CANCEL) == 0);
  CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0); /* the first pump sends it all at once */
  int answered = 0, cancelled = 0;
  for (int i = 0; i < M; i++) {
    Reply r = parse_reply(&calls[i]);
    if (i % 2 == 0)
      answered += call_ok("batched GET", &calls[i]) && r.ok && r.flag && r.value_len == 11 &&
                  memcmp(r.value, "batch-value", 11) == 0;
    else
      cancelled += calls[i].closed && calls[i].close_error == NGHTTP2_CANCEL;
    call_free(&calls[i]);
    free(msgs[i].p);
  }
  CHECK(answered == M / 2);
  CHECK(cancelled == M / 2);
  CHECK(op_delete(cl, k, kl) == 1); /* the connection is still healthy */
}

/** HealthCheckResponse{status: SERVING}, gRPC-framed. */
static const uint8_t kServing[7] = {0, 0, 0, 0, 2, 0x08, 0x01};
/** HealthCheckResponse{status: NOT_SERVING}, gRPC-framed. */
static const uint8_t kNotServing[7] = {0, 0, 0, 0, 2, 0x08, 0x02};

/**
 * grpc.health.v1.Health: Check answers SERVING for the whole server ("" or
 * an empty request) and for kvnode.v1.KvNode, NOT_FOUND for any other
 * service; Watch is not implemented (allowed by the health protocol).
 * @param cl  connection
 */
static void test_health(Client *cl) {
  static const char *const services[] = {"", "kvnode.v1.KvNode"};
  Call c;
  for (int i = 0; i < 3; i++) {
    Msg m = i < 2 ? msg_kv(services[i], strlen(services[i]), NULL, 0)
                  : msg_kv(NULL, 0, NULL, 0);             /* no field at all */
    CHECK(call_run(cl, &c, "POST", PATH_HEALTH, GRPC_CT, NULL, m.p, m.len) == 0 &&
          call_ok("Health/Check", &c) && c.body_len == 7 && memcmp(c.body, kServing, 7) == 0);
    call_free(&c);
    free(m.p);
  }
  expect_error("unknown health service", cl, "POST", PATH_HEALTH, GRPC_CT, NULL,
               msg_kv("kvnode.v2.Other", 15, NULL, 0), 200, 5, "unknown service");
  expect_error("health Watch", cl, "POST", "/grpc.health.v1.Health/Watch", GRPC_CT, NULL,
               msg_kv(NULL, 0, NULL, 0), 200, 12, "unknown method");
}

/* ========================================================================= */
/* Multi-connection concurrency with torn-value detection                   */
/* ========================================================================= */

#define HOT_KEYS  8
#define HOT_MAGIC 0x484F544Bu           /* "KTOH" */
#define THREADS   8

/** Per-thread state and results of the concurrency test. */
typedef struct {
  pthread_t thread;
  int id;
  uint64_t duration_ns;
  uint64_t ops, errors, torn, hot_reads, hot_writes, lifecycles;
} Worker;

/**
 * Length of the hot value written by (writer, seq): a pure function, so a
 * reader can verify the length too.
 * @param writer  writer id
 * @param seq     writer's sequence number
 * @returns payload length after the 16-byte header (0..2048)
 */
static uint32_t hot_len(uint32_t writer, uint32_t seq) {
  return (writer * 2654435761u ^ seq * 40503u) % 2049;
}

/**
 * Builds a self-describing value: {magic, writer, seq, len} + pattern bytes
 * derived from (writer, seq). Any mix of two writes (a torn read) breaks the
 * header/pattern relation.
 * @param out     destination (16 + 2048 bytes)
 * @param writer  writer id
 * @param seq     writer's sequence number
 * @returns total value length
 */
static size_t hot_value(uint8_t *out, uint32_t writer, uint32_t seq) {
  uint32_t len = hot_len(writer, seq);
  uint32_t hdr[4] = {HOT_MAGIC, writer, seq, len};
  memcpy(out, hdr, 16);
  for (uint32_t i = 0; i < len; i++) out[16 + i] = (uint8_t)(writer * 31 + seq * 7 + i);
  return 16 + len;
}

/**
 * Verifies a value produced by hot_value.
 * @param v  value bytes
 * @param n  value length
 * @returns 1 if internally consistent, 0 if torn or foreign
 */
static int hot_valid(const uint8_t *v, size_t n) {
  if (n < 16) return 0;
  uint32_t hdr[4];
  memcpy(hdr, v, 16);
  if (hdr[0] != HOT_MAGIC || hdr[3] != n - 16 || hdr[3] != hot_len(hdr[1], hdr[2])) return 0;
  for (uint32_t i = 0; i < hdr[3]; i++)
    if (v[16 + i] != (uint8_t)(hdr[1] * 31 + hdr[2] * 7 + i)) return 0;
  return 1;
}

/**
 * Thread body: a random mix of hot-key replaces, hot-key reads (validated)
 * and private-key life cycles (exact created/deleted flags) on its own
 * connection — which the kernel's SO_REUSEPORT hashing spreads across the
 * server's workers.
 * @param arg  Worker*
 * @returns NULL
 */
static void *worker_main(void *arg) {
  Worker *w = arg;
  Client cl;
  if (client_open(&cl, g_host, g_port, 1u << 20) != 0) {
    w->errors++;
    return NULL;
  }
  uint64_t s = 0x9E3779B97F4A7C15ull * (uint64_t)(w->id + 1);
  uint8_t val[16 + 2049];
  uint32_t seq = 0;
  char k[96];
  uint64_t end = now_ns() + w->duration_ns;
  while (now_ns() < end && w->errors < 10) {
    uint64_t r = rng_next(&s);
    uint8_t *v;
    size_t vl, kl;
    switch (r % 4) {
    case 0:                                                  /* hot replace */
      kl = mkkey(k, sizeof k, "hot", (r >> 8) % HOT_KEYS);
      vl = hot_value(val, (uint32_t)w->id, ++seq);
      if (op_put(&cl, k, kl, val, vl) != 0) w->errors++;     /* pre-populated: replace */
      w->hot_writes++;
      break;
    case 1:
    case 2:                                                  /* hot read */
      kl = mkkey(k, sizeof k, "hot", (r >> 8) % HOT_KEYS);
      if (op_get(&cl, k, kl, &v, &vl) != 1) w->errors++;
      else if (!hot_valid(v, vl)) w->torn++;
      free(v);
      w->hot_reads++;
      break;
    default: {                                               /* private life cycle */
      int n = snprintf(k, sizeof k, "%s-priv-%d-%" PRIu64, g_nonce, w->id, w->ops);
      kl = (size_t)n;
      uint8_t pv[48];
      rng_fill(&s, pv, sizeof pv);
      if (op_put(&cl, k, kl, pv, sizeof pv) != 1) w->errors++;
      if (op_put(&cl, k, kl, pv, 24) != 0) w->errors++;
      if (op_get(&cl, k, kl, &v, &vl) != 1 || vl != 24 || memcmp(v, pv, 24) != 0) w->errors++;
      free(v);
      if (op_delete(&cl, k, kl) != 1) w->errors++;
      if (op_get(&cl, k, kl, &v, &vl) != 0) w->errors++;
      if (op_delete(&cl, k, kl) != 0) w->errors++;
      w->lifecycles++;
      break;
    }
    }
    w->ops++;
  }
  client_close(&cl);
  return NULL;
}

/**
 * Runs THREADS connections concurrently for duration_ms and checks that no
 * operation failed and no reader ever saw a torn value.
 * @param cl           main connection (pre-populates and re-checks hot keys)
 * @param duration_ms  run time
 */
static void test_concurrency(Client *cl, int duration_ms) {
  char k[64];
  uint8_t val[16 + 2049];
  for (int h = 0; h < HOT_KEYS; h++) {
    size_t kl = mkkey(k, sizeof k, "hot", (uint64_t)h);
    size_t vl = hot_value(val, 999, (uint32_t)h);
    CHECK(op_put(cl, k, kl, val, vl) == 1);
  }
  static Worker w[THREADS];
  uint64_t t0 = now_ns();
  for (int i = 0; i < THREADS; i++) {
    w[i] = (Worker){.id = i, .duration_ns = (uint64_t)duration_ms * 1000000ull};
    CHECK(pthread_create(&w[i].thread, NULL, worker_main, &w[i]) == 0);
  }
  uint64_t ops = 0, errors = 0, torn = 0, reads = 0, writes = 0, cycles = 0;
  for (int i = 0; i < THREADS; i++) {
    pthread_join(w[i].thread, NULL);
    ops += w[i].ops;
    errors += w[i].errors;
    torn += w[i].torn;
    reads += w[i].hot_reads;
    writes += w[i].hot_writes;
    cycles += w[i].lifecycles;
  }
  double secs = (double)(now_ns() - t0) / 1e9;
  printf("  concurrency: %d connections %.1fs: %" PRIu64 " hot reads, %" PRIu64
         " hot replaces, %" PRIu64 " private life cycles (%.0f rpc/s); errors %" PRIu64
         ", torn %" PRIu64 "\n",
         THREADS, secs, reads, writes, cycles, (double)(reads + writes + cycles * 6) / secs,
         errors, torn);
  CHECK(ops > 0);
  CHECK_EQ_U64(errors, 0);
  CHECK_EQ_U64(torn, 0);
  for (int h = 0; h < HOT_KEYS; h++) {
    size_t kl = mkkey(k, sizeof k, "hot", (uint64_t)h);
    uint8_t *v;
    size_t vl;
    CHECK(op_get(cl, k, kl, &v, &vl) == 1 && hot_valid(v, vl));
    free(v);
    CHECK(op_delete(cl, k, kl) == 1);
  }
}

/* ========================================================================= */
/* Entry points                                                              */
/* ========================================================================= */

/**
 * --wait-goaway: holds an idle connection until the server sends GOAWAY and
 * then closes the connection.
 * @param secs  give up after this many seconds
 * @returns process exit code: 0 on GOAWAY(NO_ERROR) followed by EOF, 1 on
 *          anything else, 2 if the connection could not be made
 */
static int wait_goaway(int secs) {
  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  uint64_t deadline = now_ns() + (uint64_t)secs * 1000000000ull;
  while (!cl.eof && now_ns() < deadline)
    if (pump(&cl, 100) < 0) break;
  int ok = cl.goaway && cl.goaway_error == NGHTTP2_NO_ERROR && cl.eof;
  printf("wait-goaway: goaway=%d error=%u last_stream=%d eof=%d => %s\n", cl.goaway,
         cl.goaway_error, cl.goaway_last_stream, cl.eof, ok ? "ok" : "FAILED");
  client_close(&cl);
  return ok ? 0 : 1;
}

/**
 * --drain-check: proves graceful shutdown completes in-flight requests.
 * A Health/Check stream is opened and its body held back after 1 byte; once
 * the server's GOAWAY arrives (run.sh sends SIGTERM meanwhile) the body is
 * released. The server must accept the stream (GOAWAY last_stream_id covers
 * it), answer NOT_SERVING (its worker is draining), then close the
 * connection.
 * @param secs  overall time limit
 * @returns exit code: 0 on the behavior above, 1 otherwise, 2 if the
 *          connection or submission failed
 */
static int drain_check(int secs) {
  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  Msg m = msg_kv("", 0, NULL, 0);
  static Call c;
  c.defer_at = 1;                        /* HEADERS + 1 body byte, then stall */
  if (call_submit(&cl, &c, "POST", PATH_HEALTH, GRPC_CT, NULL, m.p, m.len) != 0) return 2;
  uint64_t deadline = now_ns() + (uint64_t)secs * 1000000000ull;
  while (!cl.goaway && !cl.eof && now_ns() < deadline)
    if (pump(&cl, 50) < 0) break;
  int got_goaway = cl.goaway;
  c.defer_at = 0;                        /* release the rest of the body */
  nghttp2_session_resume_data(cl.session, c.stream_id);
  int finished = run_until_idle(&cl, 10000) == 0;
  while (!cl.eof && now_ns() < deadline)
    if (pump(&cl, 50) < 0) break;
  int answered = finished && call_ok("drain Check", &c) && c.body_len == 7 &&
                 memcmp(c.body, kNotServing, 7) == 0;
  int ok = got_goaway && cl.goaway_error == NGHTTP2_NO_ERROR &&
           cl.goaway_last_stream >= c.stream_id && answered && cl.eof;
  printf("drain-check: goaway=%d last_stream=%d stream=%d answered NOT_SERVING=%d eof=%d => %s\n",
         got_goaway, cl.goaway_last_stream, c.stream_id, answered, cl.eof,
         ok ? "ok" : "FAILED");
  call_free(&c);
  free(m.p);
  client_close(&cl);
  return ok ? 0 : 1;
}

/**
 * --expect-snapshot: verifies every record mksnap wrote is served byte-exact.
 * Requests are pipelined in batches of 200 streams, so a 20k-record check
 * takes a fraction of a second and also exercises many concurrent streams.
 * @param count  number of records in the snapshot
 * @returns exit code: 0 all exact and record `count` absent, 1 on any
 *          mismatch, 2 if the connection could not be made
 */
static int expect_snapshot(uint64_t count) {
  enum { BATCH = 200 };
  static Call calls[BATCH];
  static Msg msgs[BATCH];
  Client cl;
  if (client_open(&cl, g_host, g_port, 1u << 20) != 0) return 2;
  uint64_t bad = 0;
  char k[SNAP_KEY_CAP];
  uint8_t want[SNAP_VALUE_MAX];
  for (uint64_t base = 0; base < count && bad < 10; base += BATCH) {
    int n = (int)(count - base < BATCH ? count - base : BATCH);
    for (int i = 0; i < n; i++) {
      size_t kl = snap_key(k, base + (uint64_t)i);
      msgs[i] = msg_kv(k, kl, NULL, 0);
      calls[i].defer_at = 0;
      if (call_submit(&cl, &calls[i], "POST", PATH_GET, GRPC_CT, NULL, msgs[i].p,
                      msgs[i].len) != 0)
        bad++;
    }
    if (run_until_idle(&cl, CALL_TIMEOUT_MS) != 0) bad++;
    for (int i = 0; i < n; i++) {
      size_t wl = snap_value(want, base + (uint64_t)i);
      Reply r = parse_reply(&calls[i]);
      int exact = calls[i].closed && call_ok("snapshot Get", &calls[i]) && r.ok && r.flag &&
                  r.value_len == wl && (wl == 0 || memcmp(r.value, want, wl) == 0);
      if (!exact) {
        fprintf(stderr, "  snapshot record %" PRIu64 " not served exactly\n", base + (uint64_t)i);
        bad++;
      }
      call_free(&calls[i]);
      free(msgs[i].p);
    }
  }
  uint8_t *v;
  size_t vl;
  size_t kl = snap_key(k, count);
  if (op_get(&cl, k, kl, &v, &vl) != 0) {                  /* one past the end */
    fprintf(stderr, "  record %" PRIu64 " exists but the snapshot has only %" PRIu64 "\n",
            count, count);
    bad++;
  }
  free(v);
  client_close(&cl);
  printf("expect-snapshot: %" PRIu64 " records checked, %" PRIu64 " bad\n", count, bad);
  return bad ? 1 : 0;
}

/**
 * Entry point.
 * @param argc  argument count
 * @param argv  see the file header for the three usages
 * @returns 0 when every check passed, 1 on failures, 2 on usage/connection
 *          errors
 */
int main(int argc, char **argv) {
  if (argc == 5 && strcmp(argv[1], "--wait-goaway") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return wait_goaway(atoi(argv[4]));
  }
  if (argc == 5 && strcmp(argv[1], "--expect-snapshot") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return expect_snapshot(strtoull(argv[4], NULL, 10));
  }
  if (argc == 5 && strcmp(argv[1], "--drain-check") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return drain_check(atoi(argv[4]));
  }
  if (argc != 5) {
    fprintf(stderr, "usage: %s HOST PORT MAX_KEY MAX_VALUE\n"
                    "       %s --wait-goaway HOST PORT SECS\n"
                    "       %s --expect-snapshot HOST PORT COUNT\n"
                    "       %s --drain-check HOST PORT SECS\n",
            argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }
  g_host = argv[1];
  g_port = argv[2];
  g_max_key = strtoull(argv[3], NULL, 10);
  g_max_value = strtoull(argv[4], NULL, 10);
  if (g_max_key < 32 || g_max_value < 4096) {
    fprintf(stderr, "MAX_KEY must be >= 32 and MAX_VALUE >= 4096 for these cases\n");
    return 2;
  }
  snprintf(g_nonce, sizeof g_nonce, "it%lx%x", (unsigned long)time(NULL),
           (unsigned)getpid());

  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  uint64_t t0 = now_ns();
  test_crud(&cl);
  test_large_value(&cl);
  test_errors(&cl);
  test_health(&cl);
  test_pipelined(&cl);
  test_cancel(&cl);
  test_reset_in_batch(&cl);
  test_concurrency(&cl, 3000);
  CHECK(!cl.goaway && !cl.eof);                            /* never dropped by the server */
  client_close(&cl);
  printf("  %.2fs total\n", (double)(now_ns() - t0) / 1e9);
  return test_finish("itest_grpc");
}
