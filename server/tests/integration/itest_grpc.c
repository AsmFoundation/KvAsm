/*
 * itest_grpc.c — end-to-end tests of a running kvasm over real HTTP/2.
 *
 * Built on nghttp2's *client* API: an HTTP/2 implementation that shares no
 * code with the server (whose engine is its own h2n.asm/hpack.asm, ADR
 * 0008), so this verifies the server's bytes on the wire against an
 * independent client session, HPACK encoder and client-side flow control
 * rather than against the server's own understanding of the protocol:
 *   - exact response messages, :status, grpc-status and grpc-message for
 *     every success and error path of Get/Put/Delete;
 *   - values of max_value bytes crossing many DATA frames and flow-control
 *     windows in both directions (the client keeps nghttp2's 64 KiB default
 *     window on the main connection, forcing the server to frame responses
 *     from its pend list as window updates arrive);
 *   - 300 concurrent streams on one connection (above the server's
 *     max_streams, so the client must queue against the advertised limit);
 *   - a stream cancelled mid-body (RST_STREAM) leaving the connection usable
 *     and nothing stored;
 *   - multi-connection concurrency with torn-value detection: writers replace
 *     "hot" keys with self-describing values while readers verify every value
 *     they receive is internally consistent — atomic replace, end to end;
 *   - the streaming calls (Upload: client streaming, Download: server
 *     streaming): every refusal with its exact status and message; a matrix
 *     of sizes x chunkings x crc32c placements, round-tripped byte-exact;
 *     the request split at EVERY byte and in 1..7-byte and random DATA
 *     frames; refusals answered while the client still holds its body;
 *     cancels at every stage (nothing stored, the old value standing) and
 *     abrupt TCP resets; ranges and chunking, each response parsed message by
 *     message against the contract (total_size first, exact chunks, the
 *     crc32c of exactly the range last); 1- and 13-byte flow-control windows;
 *     a download stalled at exactly one window while other calls proceed;
 *     one version per Download while the key is replaced and deleted under
 *     it (pins); an 8-connection race of Upload replaces against Downloads
 *     checked for torn values;
 *   - with dedicated servers (run.sh): memory/budget/capacity limits and
 *     every reservation and pin coming back however a call ends
 *     (--stream-limits), graceful shutdown mid-transfer (--drain-transfers),
 *     the idle timeout against stalled, slow and socket-frozen transfers
 *     (--stall), and one worker serving unary calls beside bulk downloads
 *     (--fairness).
 *
 * The CRC-32C used to check the server's is this file's own byte-table
 * implementation, independent of the server's crc32c.asm.
 *
 * Usage:
 *   itest_grpc HOST PORT MAX_KEY MAX_VALUE [MAX_UPLOAD]   run the suite
 *       (exit 0 = pass); MAX_UPLOAD = the server's KVASM_MAX_UPLOAD_BYTES
 *       (default 64 MiB), at least 3145735
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
 *   itest_grpc --drain-transfers HOST PORT SECS   the same for an Upload and
 *       a Download in flight: both must complete exactly after the GOAWAY.
 *   itest_grpc --stream-limits HOST PORT   limits of streaming calls; the
 *       server runs with KVASM_MAX_MEMORY_MB=24 KVASM_CONN_BUFFER_MB=16
 *       KVASM_MAX_UPLOAD_BYTES=16777216 KVASM_CAPACITY=64.
 *   itest_grpc --stall HOST PORT   idle timeout against transfers; the server
 *       runs with KVASM_IDLE_TIMEOUT_SEC=1 KVASM_MAX_MEMORY_MB=24
 *       KVASM_CONN_BUFFER_MB=16 KVASM_MAX_UPLOAD_BYTES=16777216.
 *   itest_grpc --fairness HOST PORT   one worker (KVASM_WORKERS=1) serving
 *       unary calls beside 32 MiB downloads.
 *   itest_grpc --bench-transfer HOST PORT PID   not a test: Upload/Download
 *       throughput against unary Put/Get on one connection, and the server's
 *       memory (PID's RSS) while a 64 MiB value is stored and downloaded.
 *
 * MAX_KEY/MAX_VALUE must equal the server's KVASM_MAX_KEY_BYTES and
 * KVASM_MAX_VALUE_BYTES: the boundary cases are computed from them.
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

#define PATH_GET    "/kvasm.v1.KvAsm/Get"
#define PATH_PUT    "/kvasm.v1.KvAsm/Put"
#define PATH_DELETE "/kvasm.v1.KvAsm/Delete"
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
  /* DATA frame shaping for the streaming tests (0 = nghttp2's choice); each
   * read_body call becomes exactly one DATA frame, so these control where
   * the server's decoder sees frame boundaries. Preserved by call_submit. */
  size_t split_at;          /**< the first DATA frame ends exactly here */
  size_t frame_max;         /**< no DATA frame carries more than this */
  uint64_t frame_seed;      /**< non-zero: random frame sizes in [1, frame_max] */
  size_t unconsumed;        /**< manual-window clients: DATA received, not yet consumed */
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
  int ping_acks;            /**< PING ACKs received (readiness handshake) */
  int manual_window;        /**< 1: no automatic WINDOW_UPDATE; the test consumes */
  size_t conn_unconsumed;   /**< manual-window clients: DATA not yet given back to
                                 the connection window (consume_conn) */
  size_t recv_max;          /**< >0: pump() reads at most this many bytes per call
                                 (a slow reader, paced by the test's own sleeps) */
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
  Client *cl = ud;
  Call *c = nghttp2_session_get_stream_user_data(s, stream_id);
  if (!c) return 0;
  if (cl->manual_window) {          /* the test decides when to open the windows */
    c->unconsumed += len;
    cl->conn_unconsumed += len;
  }
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
 * nghttp2 on_frame_recv callback: records GOAWAY for the shutdown tests and
 * counts PING ACKs for the readiness handshake (await_ping_ack).
 * @returns 0
 */
static int on_frame(nghttp2_session *s, const nghttp2_frame *frame, void *ud) {
  (void)s;
  Client *cl = ud;
  if (frame->hd.type == NGHTTP2_GOAWAY) {
    cl->goaway = 1;
    cl->goaway_error = frame->goaway.error_code;
    cl->goaway_last_stream = frame->goaway.last_stream_id;
  } else if (frame->hd.type == NGHTTP2_PING && (frame->hd.flags & NGHTTP2_FLAG_ACK)) {
    cl->ping_acks++;
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
  /* Frame shaping (streaming tests): one call = one DATA frame. */
  if (c->split_at && c->req_off < c->split_at && n > c->split_at - c->req_off)
    n = c->split_at - c->req_off;
  if (c->frame_max && n > c->frame_max) {
    n = c->frame_max;
    if (c->frame_seed) n = 1 + (size_t)(rng_next(&c->frame_seed) % c->frame_max);
  }
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
/** Socket options applied before connect() (0 = the kernel's default). */
typedef struct {
  int rcvbuf;   /**< SO_RCVBUF: a small TCP window, so a client that stops
                     calling pump() stalls the server at the TCP level */
  int mss;      /**< TCP_MAXSEG: the MSS advertised in the SYN. The server
                     sizes its send buffer from the MSS (~20 segments), and
                     loopback's 64 KiB MSS gives ~1.3 MB — so large that a
                     frozen reader never makes a write fail there. A WAN-like
                     1200 gives ~46 KB, which a flush overruns, as on a real
                     network: the output the server cannot send then stays
                     queued in user space, in front of anything sent later */
} SockOpts;

static int client_open_ex(Client *cl, const char *host, const char *port, uint32_t window,
                          int manual_window, const SockOpts *so);

static int client_open(Client *cl, const char *host, const char *port, uint32_t window) {
  return client_open_ex(cl, host, port, window, 0, NULL);
}

/**
 * client_open with a choice of flow-control mode and socket options.
 * @param manual_window  1: nghttp2 sends no WINDOW_UPDATE by itself; received
 *                       DATA accumulates in Call.unconsumed and
 *                       Client.conn_unconsumed until the test calls
 *                       consume_call / consume_conn — how the streaming tests
 *                       make a reader stall (per stream or per connection)
 *                       and resume on purpose
 * @param so             socket options set before connect(), or NULL
 * @see client_open for the other parameters and the return value
 */
static int client_open_ex(Client *cl, const char *host, const char *port, uint32_t window,
                          int manual_window, const SockOpts *so) {
  memset(cl, 0, sizeof *cl);
  cl->fd = -1;
  cl->manual_window = manual_window;
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
  int rc = getaddrinfo(host, port, &hints, &res);
  if (rc != 0) {
    fprintf(stderr, "getaddrinfo %s:%s: %s\n", host, port, gai_strerror(rc));
    return -1;
  }
  for (struct addrinfo *a = res; a; a = a->ai_next) {
    int fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    /* Before connect(): the window scale and the MSS are negotiated in the
     * SYN, so options set afterwards would not change what the server sees. */
    if (so && so->rcvbuf > 0) setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &so->rcvbuf, sizeof so->rcvbuf);
    if (so && so->mss > 0) setsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &so->mss, sizeof so->mss);
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
  nghttp2_option *opt = NULL;
  if (nghttp2_option_new(&opt) != 0) return -1;
  if (manual_window) nghttp2_option_set_no_auto_window_update(opt, 1);
  rc = nghttp2_session_client_new2(&cl->session, cbs, cl, opt);
  nghttp2_option_del(opt);
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
  /* A client whose client_open failed (the server is gone) has no session:
   * fail instead of letting nghttp2 dereference NULL — the tests keep going
   * after a failed CHECK, and a crash would hide every later diagnosis. */
  if (!cl->session) return -1;
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
  size_t cap = cl->recv_max && cl->recv_max < sizeof buf ? cl->recv_max : sizeof buf;
  ssize_t n = recv(cl->fd, buf, cap, 0);
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
  size_t defer_at = c->defer_at, split_at = c->split_at, frame_max = c->frame_max;
  uint64_t frame_seed = c->frame_seed;
  memset(c, 0, sizeof *c);
  c->grpc_status = -1;
  if (!cl->session) return -1;             /* never opened: see pump() */
  c->req = body;
  c->req_len = len;
  c->defer_at = defer_at;
  c->split_at = split_at;     /* frame shaping survives the reset, like defer_at */
  c->frame_max = frame_max;
  c->frame_seed = frame_seed;
  nghttp2_nv nva[8];
  size_t n = 0;
  nva[n++] = mknv(":method", method);
  nva[n++] = mknv(":scheme", "http");
  nva[n++] = mknv(":authority", "kvasm");
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
 * Manual-window clients: gives every byte received so far back to the
 * CONNECTION window only. A test that stalls one stream keeps calling this,
 * so the stalled stream's unconsumed bytes do not also block every other
 * stream of the connection — the stall is then per stream, as HTTP/2 allows.
 * @param cl  connection opened with manual_window = 1
 */
static void consume_conn(Client *cl) {
  if (cl->conn_unconsumed && cl->session) {
    nghttp2_session_consume_connection(cl->session, cl->conn_unconsumed);
    cl->conn_unconsumed = 0;
  }
}

/**
 * Manual-window clients: gives a call's received bytes back to its STREAM
 * window (nghttp2 queues the WINDOW_UPDATE; the next pump sends it), and all
 * received bytes to the connection window (consume_conn).
 * @param cl  connection opened with manual_window = 1
 * @param c   the call; a stream nghttp2 already closed is ignored by it
 */
static void consume_call(Client *cl, Call *c) {
  if (c->unconsumed && cl->session) {
    nghttp2_session_consume_stream(cl->session, c->stream_id, c->unconsumed);
    c->unconsumed = 0;
  }
  consume_conn(cl);
}

/**
 * Pumps until one call has closed, whatever the others do.
 * @param cl          connection
 * @param c           the call to wait for
 * @param timeout_ms  deadline
 * @returns 0 once c closed, -1 on EOF, timeout or a session error (printed)
 * @remarks On a manual-window client this call's own window and the
 *          connection window are given back as its data arrives (it is the
 *          one being waited for), while every OTHER stream keeps its stream
 *          window closed — so a test can run ordinary calls beside a
 *          deliberately stalled download.
 */
static int wait_closed(Client *cl, Call *c, int timeout_ms) {
  uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
  while (!c->closed) {
    if (cl->eof) {
      fprintf(stderr, "connection closed before stream %d\n", c->stream_id);
      return -1;
    }
    if (now_ns() > deadline) {
      fprintf(stderr, "timeout waiting for stream %d\n", c->stream_id);
      return -1;
    }
    if (pump(cl, 20) < 0) return -1;
    if (cl->manual_window) consume_call(cl, c);
  }
  return 0;
}

/**
 * Runs one request to completion.
 * @returns 0 when the stream closed, -1 on submission/connection failure
 * @see call_submit for the parameters
 * @remarks Waits for this call only (wait_closed): other calls may stay in
 *          flight — stalled on purpose by the streaming tests.
 */
static int call_run(Client *cl, Call *c, const char *method, const char *path,
                    const char *ctype, const char *encoding, const uint8_t *body, size_t len) {
  c->defer_at = 0;
  c->split_at = c->frame_max = 0; /* a plain call: nghttp2 shapes its own frames */
  c->frame_seed = 0;
  if (call_submit(cl, c, method, path, ctype, encoding, body, len) != 0) return -1;
  return wait_closed(cl, c, CALL_TIMEOUT_MS);
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

#define BURST 40
static void sleep_ms(int ms);

/**
 * Submits n Gets of one key back to back on one connection, none awaited.
 * @param c      connection
 * @param calls  n call records (reset here); must stay alive until they close
 * @param req    the Get request message, shared by all n calls (read-only)
 * @param n      number of calls
 * @returns the number of calls submitted (n, or fewer if nghttp2 refused one)
 * @remarks The requests are tiny, so one write carries them all and the
 *          server usually finds the whole burst in one read — which is what
 *          makes that read's replies outgrow one output buffer.
 */
static int burst_submit(Client *c, Call *calls, const Msg *req, int n) {
  for (int i = 0; i < n; i++) {
    memset(&calls[i], 0, sizeof calls[i]);   /* no stalls, no frame shaping */
    if (call_submit(c, &calls[i], "POST", PATH_GET, GRPC_CT, NULL, req->p, req->len) != 0)
      return i;
  }
  return n;
}

/**
 * Counts the calls of a finished burst that returned exactly the value.
 * @param calls  the n call records of burst_submit (their bodies are freed here)
 * @param n      number of calls
 * @param v      expected value bytes
 * @param vl     expected value length
 * @returns how many calls ended OK with a found value identical to v
 */
static int burst_exact(Call *calls, int n, const uint8_t *v, size_t vl) {
  int exact = 0;
  for (int i = 0; i < n; i++) {
    Reply r = parse_reply(&calls[i]);
    if (call_ok("burst Get", &calls[i]) && r.ok && r.flag && r.value_len == vl &&
        memcmp(r.value, v, vl) == 0)
      exact++;
    call_free(&calls[i]);
  }
  return exact;
}

/**
 * Bursts of Gets whose replies do not fit one DATA frame, or one output
 * buffer: every reply must still arrive whole and byte-exact, whichever way
 * the server framed it.
 *
 * The server frames a reply straight from the store record when it can
 * (h2n.asm reply_fast): in one DATA frame, or split over several when the
 * message is longer than a frame; and whenever the queued output reaches
 * 64 KiB it is sent before more is framed, also in the middle of a long
 * message. When it cannot — a window is too small, the socket is full — the
 * reply, or what is left of it, is buffered and sent as windows and room
 * allow. Each part below drives one of those paths:
 *   (a) fast reader, wide windows: values whose messages land on every
 *       frame-count boundary — 16,374 bytes is the last value in one full
 *       frame, 16,375 the first in two, 32,757 fills exactly two, 49,141
 *       exactly three, 65,525 makes a 65,536-byte message (one output
 *       buffer) and 65,526 one byte more, 262,133 fills exactly sixteen
 *       frames — up to the server's largest value; 40 Gets in flight for
 *       the small sizes and 8 for the large, so one read's replies are 0.6
 *       to 8 MB and the early send runs many times, between replies and
 *       inside them;
 *   (b) a reader that stops reading (a small TCP window, a WAN-sized MSS:
 *       the server's send buffer then holds ~46 KB): with 16 KiB values the
 *       early send is refused between two replies and the rest of the burst
 *       is buffered; with 300,000-byte values it is refused in the middle of
 *       the first reply, whose remainder alone is buffered. Everything
 *       arrives once the reader drains its socket slowly; another
 *       connection is served meanwhile;
 *   (c) default 65,535-byte HTTP/2 windows on the main connection: the
 *       connection window closes after four replies, so the same burst mixes
 *       replies framed at once with replies that waited for WINDOW_UPDATE.
 * @param cl  the main connection (65,535-byte windows)
 */
static void test_get_bursts(Client *cl) {
  static const size_t kSizes[] = {16373, 16374, 16375, 16384, 32757, 32758, 49141, 65525,
                                  65526, 100000, 262133, 262134, 300000, 1048576};
  enum { NSIZES = (int)(sizeof kSizes / sizeof kSizes[0]), BIG = 100000 };
  static Call calls[BURST];
  size_t cap = g_max_value < 1048576 ? g_max_value : 1048576;
  uint8_t *val = malloc(cap);
  char k[64];
  uint8_t *v;
  size_t vl;

  /* (a) every boundary, read at full speed over 16 MiB windows */
  Client w;
  if (client_open(&w, g_host, g_port, 16u << 20) != 0) {
    CHECK(0);
    free(val);
    return;
  }
  for (int i = 0; i < NSIZES; i++) {
    size_t n = kSizes[i];
    if (n > g_max_value) continue;            /* a server configured below these sizes */
    int burst = n > BIG ? 8 : BURST;          /* 8 x 1 MiB is burst enough */
    size_t kl = mkkey(k, sizeof k, "burst", (uint64_t)n);
    uint64_t seed = 0xB0057 + n;
    rng_fill(&seed, val, n);
    CHECK(op_put(cl, k, kl, val, n) == 1);
    Msg req = msg_kv(k, kl, NULL, 0);
    CHECK(burst_submit(&w, calls, &req, burst) == burst);
    CHECK(run_until_idle(&w, CALL_TIMEOUT_MS) == 0);
    int exact = burst_exact(calls, burst, val, n);
    if (exact != burst) fprintf(stderr, "  burst of %zu-byte values: %d/%d exact\n", n, exact, burst);
    CHECK(exact == burst);
    free(req.p);
    if (n != 16384 && n != 300000) CHECK(op_delete(cl, k, kl) == 1);   /* those two serve (b), (c) */
  }
  CHECK(op_get(&w, "burst-absent", 12, &v, &vl) == 0);   /* the connection survived it all */
  client_close(&w);

  /* (b) the reader freezes, then drains slowly. recv_max = 1 lets pump()
   * send the burst while taking at most one byte per call off the socket. */
  static const struct { size_t n; int burst; } kFrozen[] = {{16384, BURST}, {300000, 8}};
  for (int f = 0; f < 2; f++) {
    size_t n = kFrozen[f].n;
    int burst = kFrozen[f].burst;
    if (n > g_max_value) continue;
    size_t kl = mkkey(k, sizeof k, "burst", (uint64_t)n);
    uint64_t seed = 0xB0057 + n;
    rng_fill(&seed, val, n);
    Msg req = msg_kv(k, kl, NULL, 0);
    Client s;
    const SockOpts frozen = {.rcvbuf = 4096, .mss = 1200};
    if (client_open_ex(&s, g_host, g_port, 16u << 20, 0, &frozen) == 0) {
      s.recv_max = 1;
      CHECK(burst_submit(&s, calls, &req, burst) == burst);
      for (int i = 0; i < 20; i++) CHECK(pump(&s, 5) == 0);   /* the requests leave */
      sleep_ms(200);                           /* the server fills the socket and stalls */
      CHECK(op_get(cl, "burst-absent", 12, &v, &vl) == 0);   /* other connections are served */
      s.recv_max = 4096;
      CHECK(run_until_idle(&s, CALL_TIMEOUT_MS) == 0);
      int exact = burst_exact(calls, burst, val, n);
      if (exact != burst)
        fprintf(stderr, "  burst of %zu-byte values to a frozen reader: %d/%d exact\n", n, exact, burst);
      CHECK(exact == burst);
      s.recv_max = 0;
      CHECK(op_get(&s, k, kl, &v, &vl) == 1 && vl == n && memcmp(v, val, n) == 0);
      free(v);                                 /* and the stalled connection recovered */
      client_close(&s);
    } else {
      CHECK(0);
    }

    /* (c) the main connection's default windows: replies of 16 KiB (8 of
     * them) or 300,000 bytes (2) against a 65,535-byte connection window */
    int few = n > BIG ? 2 : 8;
    CHECK(burst_submit(cl, calls, &req, few) == few);
    CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0);
    CHECK(burst_exact(calls, few, val, n) == few);
    free(req.p);
    CHECK(op_delete(cl, k, kl) == 1);
  }
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
  expect_error("unknown method", cl, "POST", "/kvasm.v1.KvAsm/Scan", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("unknown service", cl, "POST", "/other.v1.Svc/Get", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("service prefix only", cl, "POST", "/kvasm.v1.KvAsm/", GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 12, "unknown method");
  expect_error("method case", cl, "POST", "/kvasm.v1.KvAsm/get", GRPC_CT, NULL,
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
 * Writes are applied in arrival order within a batch (ADR 0011).
 *
 * Seven calls on one key go out back to back on one connection, none
 * awaited: Put v1, Put v2, Get, Delete, Get, Put v3, Get. The server batches
 * the calls one read carries and applies the writes of a batch, in order,
 * before its Gets — and a read boundary can fall anywhere. So:
 *   * the writes' answers are fixed whatever the reads: created, replaced,
 *     deleted, created (any reordering of writes would change one of them);
 *   * a Get answers from a state at or after every write sent before it —
 *     possibly also after writes sent after it in the same read, which is
 *     valid because they were in flight together;
 *   * the last Get sees v3.
 * Repeated with fresh keys so different read splits get a chance to occur.
 * @param cl  connection
 */
static void test_batch_order(Client *cl) {
  enum { N = 7, ROUNDS = 50 };
  static const char *const kPath[N] = {PATH_PUT, PATH_PUT, PATH_GET, PATH_DELETE,
                                       PATH_GET, PATH_PUT, PATH_GET};
  static const char *const kVal[N] = {"v1", "v2", NULL, NULL, NULL, "v3", NULL};
  static Call calls[N];
  int writes_ok = 0, gets_ok = 0;
  for (int round = 0; round < ROUNDS; round++) {
    char k[64];
    size_t kl = mkkey(k, sizeof k, "order", (uint64_t)round);
    Msg msgs[N];
    for (int i = 0; i < N; i++) {
      msgs[i] = msg_kv(k, kl, kVal[i], kVal[i] ? 2 : 0);
      calls[i].defer_at = 0;
      CHECK(call_submit(cl, &calls[i], "POST", kPath[i], GRPC_CT, NULL, msgs[i].p,
                        msgs[i].len) == 0);
    }
    CHECK(run_until_idle(cl, CALL_TIMEOUT_MS) == 0);
    Reply r[N];
    for (int i = 0; i < N; i++) {
      CHECK(call_ok("batch order", &calls[i]));
      r[i] = parse_reply(&calls[i]);
      CHECK(r[i].ok);
    }
    /* Writes: exactly the sequential outcomes. */
    if (r[0].flag == 1 && r[1].flag == 0 && r[3].flag == 1 && r[5].flag == 1) writes_ok++;
    /* Get 2: v2, absent (after the Delete) or v3. Get 4: absent or v3.
     * Get 6: v3. Never v1: Put v2 was sent before every Get. */
#define IS_VAL(rr, s) ((rr).flag && (rr).value_len == 2 && memcmp((rr).value, (s), 2) == 0)
    int g2 = IS_VAL(r[2], "v2") || !r[2].flag || IS_VAL(r[2], "v3");
    int g4 = !r[4].flag || IS_VAL(r[4], "v3");
    int g6 = IS_VAL(r[6], "v3");
#undef IS_VAL
    if (g2 && g4 && g6) gets_ok++;
    for (int i = 0; i < N; i++) {
      call_free(&calls[i]);
      free(msgs[i].p);
    }
    CHECK(op_delete(cl, k, kl) == 1);
  }
  CHECK(writes_ok == ROUNDS);
  CHECK(gets_ok == ROUNDS);
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
 * an empty request) and for kvasm.v1.KvAsm, NOT_FOUND for any other
 * service; Watch is not implemented (allowed by the health protocol).
 * @param cl  connection
 */
static void test_health(Client *cl) {
  static const char *const services[] = {"", "kvasm.v1.KvAsm"};
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
               msg_kv("kvasm.v2.Other", 14, NULL, 0), 200, 5, "unknown service");
  expect_error("health Watch", cl, "POST", "/grpc.health.v1.Health/Watch", GRPC_CT, NULL,
               msg_kv(NULL, 0, NULL, 0), 200, 12, "unknown method");
}

/* ========================================================================= */
/* Streaming: Upload / Download                                             */
/* ========================================================================= */

#define PATH_UPLOAD   "/kvasm.v1.KvAsm/Upload"
#define PATH_DOWNLOAD "/kvasm.v1.KvAsm/Download"
#define DL_DEFAULT_CHUNK 65536u
#define DL_MAX_CHUNK     1048576u

static uint64_t g_max_upload;   /* the server's KVASM_MAX_UPLOAD_BYTES */
static uint64_t g_conn_budget = 64u << 20; /* its KVASM_CONN_BUFFER_MB (default) */

/* CRC-32C, independent of the server's crc32c.asm: a plain byte table. */
static uint32_t g_crc_tab[256];

/** Builds the CRC-32C byte table (reflected Castagnoli polynomial 0x82F63B78). */
static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
    g_crc_tab[i] = c;
  }
}

/**
 * CRC-32C, zlib-style (preset and final inversion inside).
 * @param crc  0 for a new checksum, else the previous result to continue
 * @param p    bytes   @param n  byte count
 * @returns the continued checksum
 */
static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) crc = g_crc_tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

/** A growable byte buffer for request streams. */
typedef struct {
  uint8_t *p;
  size_t n, cap;
} Buf;

/** Appends bytes (exits on allocation failure: this is a test). */
static void buf_put(Buf *b, const void *src, size_t n) {
  if (b->n + n > b->cap) {
    size_t cap = (b->n + n) * 2 + 64;
    uint8_t *p = realloc(b->p, cap);
    if (!p) { perror("realloc"); exit(2); }
    b->p = p;
    b->cap = cap;
  }
  if (n) memcpy(b->p + b->n, src, n);
  b->n += n;
}

/** Appends one byte. */
static void buf_byte(Buf *b, uint8_t v) { buf_put(b, &v, 1); }

/** Appends a protobuf varint. */
static void buf_varint(Buf *b, uint64_t v) {
  uint8_t t[10];
  buf_put(b, t, put_varint(t, v));
}

/** Frees a buffer and resets it. */
static void buf_free(Buf *b) {
  free(b->p);
  memset(b, 0, sizeof *b);
}

/** One UploadRequest message; each field is optional (NULL or has_* = 0: absent). */
typedef struct {
  const void *key;
  size_t klen;
  int has_size;
  uint64_t size;
  const void *data;       /**< NULL: no data field; non-NULL with dlen 0: an empty one */
  size_t dlen;
  int has_crc;
  uint32_t crc;
  const void *raw;        /**< bytes appended verbatim inside the message (unknown fields) */
  size_t rawlen;
  int data_first;         /**< the data field before key and total_size (a contract violation) */
  uint8_t flag;           /**< the prefix's compressed flag (0; 1 or 2 for framing cases) */
} UpMsg;

/**
 * Appends one gRPC-framed UploadRequest: key (0x0A), total_size (0x10),
 * data (0x1A), crc32c (0x25, fixed32 little-endian), then any raw bytes.
 * @param b  stream buffer
 * @param m  the message
 */
static void up_msg(Buf *b, const UpMsg *m) {
  Buf body = {0};
  if (m->data && m->data_first) {
    buf_byte(&body, 0x1A);
    buf_varint(&body, m->dlen);
    buf_put(&body, m->data, m->dlen);
  }
  if (m->key) {
    buf_byte(&body, 0x0A);
    buf_varint(&body, m->klen);
    buf_put(&body, m->key, m->klen);
  }
  if (m->has_size) {
    buf_byte(&body, 0x10);
    buf_varint(&body, m->size);
  }
  if (m->data && !m->data_first) {
    buf_byte(&body, 0x1A);
    buf_varint(&body, m->dlen);
    buf_put(&body, m->data, m->dlen);
  }
  if (m->has_crc) {
    uint8_t f[5] = {0x25, (uint8_t)m->crc, (uint8_t)(m->crc >> 8), (uint8_t)(m->crc >> 16),
                    (uint8_t)(m->crc >> 24)};
    buf_put(&body, f, 5);
  }
  if (m->raw) buf_put(&body, m->raw, m->rawlen);
  uint8_t pfx[5];
  set_prefix(pfx, m->flag, (uint32_t)body.n);
  buf_put(b, pfx, 5);
  buf_put(b, body.p, body.n);
  buf_free(&body);
}

/** Where up_stream puts the crc32c. */
enum { CRC_NONE, CRC_FIRST, CRC_LAST, CRC_ALONE };

/**
 * A well-formed upload of a whole value: the first message carries key and
 * total_size (plus the first chunk when first_data), then one message per
 * chunk.
 * @param crc_where  CRC_NONE, CRC_FIRST (in the first message), CRC_LAST (in
 *                   the last message, whichever it is) or CRC_ALONE (in a
 *                   final message of its own)
 * @returns the stream (caller frees)
 */
static Buf up_stream(const void *key, size_t klen, const uint8_t *v, size_t vlen, size_t chunk,
                     int crc_where, int first_data) {
  Buf b = {0};
  uint32_t crc = crc32c(0, v, vlen);
  size_t off = 0;
  UpMsg m = {.key = key, .klen = klen, .has_size = 1, .size = vlen};
  if (first_data && vlen) {
    off = vlen < chunk ? vlen : chunk;
    m.data = v;
    m.dlen = off;
  }
  if (crc_where == CRC_FIRST || (crc_where == CRC_LAST && off == vlen)) {
    m.has_crc = 1;
    m.crc = crc;
  }
  up_msg(&b, &m);
  while (off < vlen) {
    size_t n = vlen - off < chunk ? vlen - off : chunk;
    UpMsg d = {.data = v + off, .dlen = n};
    off += n;
    if (crc_where == CRC_LAST && off == vlen) {
      d.has_crc = 1;
      d.crc = crc;
    }
    up_msg(&b, &d);
  }
  if (crc_where == CRC_ALONE) {
    UpMsg t = {.has_crc = 1, .crc = crc};
    up_msg(&b, &t);
  }
  return b;
}

/**
 * Decodes an UploadResponse {1: created (varint), 2: crc32c (fixed32)} and
 * requires canonical proto3: created only when true, crc32c only when not 0,
 * nothing else.
 * @returns 1 when well-formed (created/crc set), 0 otherwise
 */
static int parse_upload_reply(const Call *c, int *created, uint32_t *crc) {
  *created = 0;
  *crc = 0;
  if (c->body_len < 5 || c->body[0] != 0) return 0;
  uint32_t mlen = (uint32_t)c->body[1] << 24 | (uint32_t)c->body[2] << 16 |
                  (uint32_t)c->body[3] << 8 | c->body[4];
  if ((size_t)mlen + 5 != c->body_len) return 0;
  const uint8_t *p = c->body + 5, *end = p + mlen;
  while (p < end) {
    uint64_t tag, v;
    if (!get_varint(&p, end, &tag)) return 0;
    if (tag == 0x08) {
      if (!get_varint(&p, end, &v) || v != 1) return 0;
      *created = 1;
    } else if (tag == 0x15) {
      if (end - p < 4) return 0;
      *crc = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
      p += 4;
      if (*crc == 0) return 0;
    } else {
      return 0;
    }
  }
  return 1;
}

/**
 * Sends one prepared upload stream on a call (its frame shaping fields are
 * used) and waits until the call closes.
 * @returns 0 when it closed, -1 on submission/connection failure or timeout
 */
static int upload_run(Client *cl, Call *c, const Buf *s) {
  if (call_submit(cl, c, "POST", PATH_UPLOAD, GRPC_CT, NULL, s->p, s->n) != 0) return -1;
  return wait_closed(cl, c, CALL_TIMEOUT_MS);   /* this call only: others may be held */
}

/**
 * Upload of a whole value.
 * @param chunk       data bytes per message (> 0)
 * @param crc_where   see up_stream
 * @param first_data  1: the first message carries the first chunk too
 * @param crc_out     receives the server's crc32c (may be NULL); it must
 *                    equal the value's, or the call counts as failed
 * @returns 1 created, 0 replaced, -1 on any error (printed)
 */
static int op_upload(Client *cl, const void *k, size_t kl, const uint8_t *v, size_t vl,
                     size_t chunk, int crc_where, int first_data, uint32_t *crc_out) {
  Buf s = up_stream(k, kl, v, vl, chunk, crc_where, first_data);
  Call c;
  memset(&c, 0, sizeof c);
  int result = -1;
  if (upload_run(cl, &c, &s) == 0 && call_ok("Upload", &c)) {
    int created;
    uint32_t crc;
    if (parse_upload_reply(&c, &created, &crc) && crc == crc32c(0, v, vl)) {
      result = created;
      if (crc_out) *crc_out = crc;
    } else {
      fprintf(stderr, "  Upload: bad reply (%zu bytes)\n", c.body_len);
    }
  }
  call_free(&c);
  buf_free(&s);
  return result;
}

/**
 * Builds a gRPC-framed DownloadRequest {1: key, 2: offset, 3: length, 4:
 * chunk_size}; zero offset/chunk are omitted (proto3 defaults).
 * @param key      key bytes, or NULL to omit the field
 * @param has_len  1: send the length field (even when 0)
 */
static Msg msg_download(const void *key, size_t klen, uint64_t off, int has_len, uint64_t len,
                        uint64_t chunk) {
  Buf b = {0};
  if (key) {
    buf_byte(&b, 0x0A);
    buf_varint(&b, klen);
    buf_put(&b, key, klen);
  }
  if (off) {
    buf_byte(&b, 0x10);
    buf_varint(&b, off);
  }
  if (has_len) {
    buf_byte(&b, 0x18);
    buf_varint(&b, len);
  }
  if (chunk) {
    buf_byte(&b, 0x20);
    buf_varint(&b, chunk);
  }
  Msg m = {malloc(5 + b.n + 1), 5 + b.n};
  if (!m.p) { perror("malloc"); exit(2); }
  set_prefix(m.p, 0, (uint32_t)b.n);
  if (b.n) memcpy(m.p + 5, b.p, b.n);
  buf_free(&b);
  return m;
}

/** A parsed Download response: every message, checked against the contract. */
typedef struct {
  int ok;                 /**< every message well-formed and in the documented shape */
  char why[160];          /**< the first violation when !ok */
  size_t msgs;
  uint64_t total;         /**< total_size of the first message */
  uint8_t *data;          /**< every data field, concatenated (malloc'd) */
  size_t len;
  uint32_t crc;           /**< crc32c of the last message */
  size_t *sizes;          /**< data bytes per message (malloc'd) */
} Dl;

/** Frees a parsed download. */
static void dl_free(Dl *d) {
  free(d->data);
  free(d->sizes);
  memset(d, 0, sizeof *d);
}

/**
 * Parses a Download response body: a sequence of gRPC messages, each
 * {1: total_size (first message only, always there), 2: data (omitted when
 * empty), 3: crc32c (fixed32, last message only)}, nothing else.
 * @param c  the finished call
 * @returns the parse (ok = 0 with the reason in why on any violation)
 */
static Dl parse_download(const Call *c) {
  Dl d = {0};
  size_t cap = 0, dcap = 0, crcs = 0, crc_at = 0;
  const uint8_t *p = c->body, *end = c->body + c->body_len;
  if (!c->body_len) snprintf(d.why, sizeof d.why, "no message at all");
  while (p < end && !d.why[0]) {
    if (end - p < 5 || p[0] != 0) {
      snprintf(d.why, sizeof d.why, "bad prefix at message %zu", d.msgs);
      break;
    }
    size_t mlen = (size_t)p[1] << 24 | (size_t)p[2] << 16 | (size_t)p[3] << 8 | p[4];
    p += 5;
    if (mlen > (size_t)(end - p)) {
      snprintf(d.why, sizeof d.why, "message %zu runs past the body", d.msgs);
      break;
    }
    const uint8_t *q = p, *mend = p + mlen;
    p = mend;
    int has_total = 0;
    size_t dl = 0;
    while (q < mend && !d.why[0]) {
      uint64_t tag, v;
      if (!get_varint(&q, mend, &tag)) {
        snprintf(d.why, sizeof d.why, "bad tag in message %zu", d.msgs);
      } else if (tag == 0x08) {
        if (!get_varint(&q, mend, &v)) {
          snprintf(d.why, sizeof d.why, "bad total_size");
        } else {
          if (d.msgs != 0) snprintf(d.why, sizeof d.why, "total_size in message %zu", d.msgs);
          d.total = v;
          has_total = 1;
        }
      } else if (tag == 0x12) {
        if (!get_varint(&q, mend, &v) || v > (uint64_t)(mend - q)) {
          snprintf(d.why, sizeof d.why, "bad data field in message %zu", d.msgs);
        } else if (v == 0) {
          snprintf(d.why, sizeof d.why, "empty data field present in message %zu", d.msgs);
        } else {
          if (d.len + v > dcap) {         /* geometric: 1-byte chunks stay linear */
            dcap = (d.len + v) * 2;
            uint8_t *nd = realloc(d.data, dcap);
            if (!nd) { perror("realloc"); exit(2); }
            d.data = nd;
          }
          memcpy(d.data + d.len, q, v);
          d.len += v;
          dl += v;
          q += v;
        }
      } else if (tag == 0x1D) {
        if (mend - q < 4) {
          snprintf(d.why, sizeof d.why, "short crc32c");
        } else {
          d.crc = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
          q += 4;
          crcs++;
          crc_at = d.msgs;
        }
      } else {
        snprintf(d.why, sizeof d.why, "unexpected tag 0x%" PRIx64 " in message %zu", tag, d.msgs);
      }
    }
    if (d.msgs == 0 && !has_total && !d.why[0])
      snprintf(d.why, sizeof d.why, "the first message lacks total_size");
    if (d.msgs == cap) {
      cap = cap ? cap * 2 : 64;
      d.sizes = realloc(d.sizes, cap * sizeof *d.sizes);
      if (!d.sizes) { perror("realloc"); exit(2); }
    }
    d.sizes[d.msgs++] = dl;
  }
  if (!d.why[0] && (crcs != 1 || crc_at != d.msgs - 1))
    snprintf(d.why, sizeof d.why, "%zu crc32c fields, want exactly one in the last message", crcs);
  d.ok = !d.why[0];
  return d;
}

/**
 * The server's chunk size for a requested chunk_size (0 = default, above
 * the maximum = the maximum).
 */
static size_t eff_chunk(uint64_t chunk) {
  return chunk == 0 ? DL_DEFAULT_CHUNK : chunk > DL_MAX_CHUNK ? DL_MAX_CHUNK : (size_t)chunk;
}

/**
 * Checks a parsed download against what it must be.
 * @param what    label for diagnostics
 * @param d       the parse
 * @param expect  the expected bytes (the range)   @param elen  their count
 * @param total   the whole value's size (total_size)
 * @param chunk   the effective chunk size: max(1, ceil(elen / chunk))
 *                messages, all but the last exactly chunk bytes
 * @returns 1 when everything matches, else 0 (the reason printed)
 */
static int dl_expect(const char *what, const Dl *d, const uint8_t *expect, size_t elen,
                     uint64_t total, size_t chunk) {
  char why[200] = "";
  size_t want = elen ? (elen + chunk - 1) / chunk : 1;
  if (!d->ok) snprintf(why, sizeof why, "%s", d->why);
  else if (d->len != elen) snprintf(why, sizeof why, "%zu data bytes, want %zu", d->len, elen);
  else if (elen && memcmp(d->data, expect, elen) != 0) snprintf(why, sizeof why, "data differs");
  else if (d->total != total) snprintf(why, sizeof why, "total_size %" PRIu64 ", want %" PRIu64, d->total, total);
  else if (d->crc != crc32c(0, expect, elen)) snprintf(why, sizeof why, "crc32c 0x%08x is wrong", d->crc);
  else if (d->msgs != want) snprintf(why, sizeof why, "%zu messages, want %zu", d->msgs, want);
  else
    for (size_t i = 0; i + 1 < d->msgs && !why[0]; i++)
      if (d->sizes[i] != chunk) snprintf(why, sizeof why, "message %zu carries %zu bytes, want %zu", i, d->sizes[i], chunk);
  if (why[0]) {
    fprintf(stderr, "  %s: %s\n", what, why);
    return 0;
  }
  return 1;
}

/**
 * Runs one Download to completion.
 * @param c  receives the call (caller call_free's it)   @param d  its parse
 * @returns call_run's result
 */
static int op_download(Client *cl, const void *k, size_t kl, uint64_t off, int has_len,
                       uint64_t len, uint64_t chunk, Call *c, Dl *d) {
  Msg m = msg_download(k, kl, off, has_len, len, chunk);
  memset(c, 0, sizeof *c);
  int rc = call_run(cl, c, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, m.p, m.len);
  free(m.p);
  *d = parse_download(c);
  return rc;
}

/**
 * Downloads a whole value and checks it completely.
 * @returns 1 when the download succeeded and matched v exactly
 */
static int download_equals(Client *cl, const void *k, size_t kl, const uint8_t *v, size_t vl,
                           uint64_t chunk) {
  Call c;
  Dl d;
  int ok = op_download(cl, k, kl, 0, 0, 0, chunk, &c, &d) == 0 && call_ok("Download", &c) &&
           dl_expect("Download", &d, v, vl, vl, eff_chunk(chunk));
  dl_free(&d);
  call_free(&c);
  return ok;
}

/** Closes a connection abruptly: SO_LINGER 0 makes close() send RST, like a crash. */
static void client_abort(Client *cl) {
  struct linger lg = {1, 0};
  setsockopt(cl->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  client_close(cl);
}

/** String literal as protobuf bytes (the CRC and buffer helpers take uint8_t). */
#define U8(s) ((const uint8_t *)(s))

/* Defined with the entry points below; the streaming tests use it as a barrier. */
static int await_ping_ack(Client *cl, int secs);

/**
 * Submits a call whose body stalls at c->defer_at and waits until the server
 * has provably read everything up to there.
 * @param cl    connection
 * @param c     call with defer_at set (> 0); reset by call_submit otherwise
 * @param path  request path
 * @param body  the whole request body (caller-owned, alive until the call closes)
 * @param len   its length
 * @returns 0 when the body stalled at defer_at, the call is still open and a
 *          PING sent afterwards was answered; -1 otherwise (the server
 *          answered or dropped the call first, or the transport failed)
 * @remarks The PING is the barrier: the server processes one connection's
 *          frames in order and answers a PING only after everything before
 *          it, so from here on the server has seen exactly defer_at body
 *          bytes — no sleeping and hoping (await_ping_ack flushes the queued
 *          frames before submitting it). A stall point beyond the server's
 *          stream window (1 MiB) is first pumped to: nghttp2 hands bytes to
 *          the provider only as WINDOW_UPDATEs arrive, and a PING sent
 *          earlier would overtake body bytes still waiting for window.
 */
static int submit_stalled(Client *cl, Call *c, const char *path, const uint8_t *body,
                          size_t len) {
  if (call_submit(cl, c, "POST", path, GRPC_CT, NULL, body, len) != 0) return -1;
  uint64_t deadline = now_ns() + 10000000000ull;
  while (c->req_off < c->defer_at && !c->closed && !cl->eof && now_ns() < deadline)
    if (pump(cl, 20) < 0) return -1;
  if (await_ping_ack(cl, 10) != 0) return -1;
  return c->req_off == c->defer_at && !c->closed ? 0 : -1;
}

/**
 * Cancels a call with RST_STREAM(CANCEL) and waits until nghttp2 closed it.
 * @returns 0 when the stream closed with CANCEL (our reset won: the server
 *          had not finished it), -1 otherwise
 */
static int cancel_call(Client *cl, Call *c) {
  if (!cl->session || nghttp2_submit_rst_stream(cl->session, NGHTTP2_FLAG_NONE, c->stream_id,
                                                NGHTTP2_CANCEL) != 0)
    return -1;
  if (wait_closed(cl, c, CALL_TIMEOUT_MS) != 0) return -1;
  return c->close_error == NGHTTP2_CANCEL ? 0 : -1;
}

/**
 * Pumps a manual-window call until it has received exactly `want` body
 * bytes (a stream window's worth), then proves nothing more is coming.
 * @param cl    manual-window connection
 * @param c     the stalled call (its stream window is never given back here)
 * @param want  expected body bytes: the client's initial stream window
 * @returns 0 when c holds exactly `want` bytes, is still open, and a PING
 *          round trip added nothing; -1 otherwise
 * @remarks The connection window IS given back throughout, so the stall is
 *          the server honoring this one stream's window — the check that it
 *          sends exactly a window, not a byte more.
 */
static int stall_at_window(Client *cl, Call *c, size_t want) {
  uint64_t deadline = now_ns() + 10000000000ull;
  while (c->body_len < want && !c->closed && !cl->eof && now_ns() < deadline) {
    if (pump(cl, 20) < 0) return -1;
    consume_conn(cl);
  }
  if (await_ping_ack(cl, 10) != 0) return -1;
  consume_conn(cl);
  return c->body_len == want && !c->closed ? 0 : -1;
}

/**
 * Sends one prepared upload stream and asserts its answer is the given error.
 * @param name    case label for diagnostics
 * @param cl      connection
 * @param method  HTTP method ("POST"; "GET" for the 405 case)
 * @param ctype   content-type, or NULL to omit it
 * @param enc     grpc-encoding, or NULL to omit it
 * @param s       the request stream, consumed here (freed and reset); an
 *                empty one sends HEADERS only
 * @param http    expected :status
 * @param grpc    expected grpc-status (-1: a plain HTTP error)
 * @param msg     substring required in grpc-message, or NULL
 * @remarks expect_error's rules apply: a gRPC error is Trailers-Only (one
 *          HEADERS frame, no DATA) and the stream must end with NO_ERROR —
 *          also when the server answered before the client finished sending,
 *          which it closes with RST_STREAM(NO_ERROR) (RFC 9113 §8.1).
 */
static void up_expect(const char *name, Client *cl, const char *method, const char *ctype,
                      const char *enc, Buf *s, int http, int grpc, const char *msg) {
  Msg m = {s->p, s->n};
  memset(s, 0, sizeof *s);              /* expect_error frees the bytes */
  expect_error(name, cl, method, PATH_UPLOAD, ctype, enc, m, http, grpc, msg);
}

/** Appends the usual first message: key and total_size, nothing else. */
static void up_first(Buf *b, const void *k, size_t kl, uint64_t size) {
  UpMsg m = {.key = k, .klen = kl, .has_size = 1, .size = size};
  up_msg(b, &m);
}

/** Appends a message carrying only a data field (n may be 0: an empty field). */
static void up_data(Buf *b, const void *d, size_t n) {
  UpMsg m = {.data = d ? d : "", .dlen = n};
  up_msg(b, &m);
}

/**
 * Get of a key whose value is known.
 * @returns 1 when Get answers the value exactly — or, for a value above
 *          max_value (possible only through Upload), when it refuses with
 *          RESOURCE_EXHAUSTED pointing to Download as a Trailers-Only error
 */
static int get_matches(Client *cl, const void *k, size_t kl, const uint8_t *v, size_t vl) {
  if (vl > g_max_value) {
    Msg m = msg_kv(k, kl, NULL, 0);
    Call c;
    int ok = call_run(cl, &c, "POST", PATH_GET, GRPC_CT, NULL, m.p, m.len) == 0 && c.closed &&
             c.close_error == 0 && c.http_status == 200 && c.grpc_status == 8 &&
             c.body_len == 0 && strstr(c.grpc_message, "read it with Download");
    if (!ok)
      fprintf(stderr, "  Get of a %zu-byte value: grpc-status=%d \"%s\"\n", vl, c.grpc_status,
              c.grpc_message);
    call_free(&c);
    free(m.p);
    return ok;
  }
  uint8_t *got;
  size_t gl;
  int ok = op_get(cl, k, kl, &got, &gl) == 1 && gl == vl && (vl == 0 || memcmp(got, v, vl) == 0);
  free(got);
  return ok;
}

/**
 * Every refusal Upload has, each with its exact status and message, and
 * nothing stored by any of them; the connection must survive them all.
 * @param cl  connection
 */
static void test_upload_errors(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "uperr", 1);
  Buf s = {0};
  const char *missing = "the first message must carry key and total_size";
  const char *order = "key and total_size must come before any data";
  const char *first_only = "key and total_size are only allowed in the first message";
  const char *over = "more data than total_size";
  const char *shortv = "the upload ended before total_size bytes";
  const char *trunc = "the request stream ended inside a message";

  /* the first message must carry key and total_size */
  up_expect("no body", cl, "POST", GRPC_CT, NULL, &s, 200, 3, missing);
  up_msg(&s, &(UpMsg){0});
  up_expect("empty first message", cl, "POST", GRPC_CT, NULL, &s, 200, 3, missing);
  up_msg(&s, &(UpMsg){.key = k, .klen = kl});
  up_expect("key only", cl, "POST", GRPC_CT, NULL, &s, 200, 3, missing);
  up_msg(&s, &(UpMsg){.has_size = 1, .size = 3});
  up_expect("total_size only", cl, "POST", GRPC_CT, NULL, &s, 200, 3, missing);
  up_msg(&s, &(UpMsg){.key = k, .klen = kl});
  up_msg(&s, &(UpMsg){.has_size = 1, .size = 1, .data = "x", .dlen = 1});
  up_expect("key, then size in msg 2", cl, "POST", GRPC_CT, NULL, &s, 200, 3, missing);

  /* key length (refused as soon as the length is known) */
  up_msg(&s, &(UpMsg){.key = "", .klen = 0, .has_size = 1, .size = 0});
  up_expect("empty key", cl, "POST", GRPC_CT, NULL, &s, 200, 3, "key length out of range");
  uint8_t *bigk = malloc(g_max_key + 1);
  memset(bigk, 'K', g_max_key + 1);
  up_msg(&s, &(UpMsg){.key = bigk, .klen = g_max_key + 1, .has_size = 1, .size = 1});
  up_expect("key over limit", cl, "POST", GRPC_CT, NULL, &s, 200, 3, "key length out of range");
  free(bigk);

  /* total_size limit: refused before any data is accepted */
  up_first(&s, k, kl, g_max_upload + 1);
  up_data(&s, "x", 1);
  up_expect("total_size over limit", cl, "POST", GRPC_CT, NULL, &s, 200, 3, "value too large");
  up_first(&s, k, kl, UINT64_MAX);
  up_expect("total_size 2^64-1", cl, "POST", GRPC_CT, NULL, &s, 200, 3, "value too large");

  /* order: key and total_size first, and only in the first message */
  UpMsg m = {.key = k, .klen = kl, .has_size = 1, .size = 1, .data = "x", .dlen = 1,
             .data_first = 1};
  up_msg(&s, &m);
  up_expect("data before key", cl, "POST", GRPC_CT, NULL, &s, 200, 3, order);
  m = (UpMsg){.key = k, .klen = kl, .data = "x", .dlen = 1, .raw = "\x10\x01", .rawlen = 2};
  up_msg(&s, &m);
  up_expect("data before total_size", cl, "POST", GRPC_CT, NULL, &s, 200, 3, order);
  m = (UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 2, .data = "x", .dlen = 1,
              .raw = "\x0A\x01k", .rawlen = 3};
  up_msg(&s, &m);
  up_expect("key again after data", cl, "POST", GRPC_CT, NULL, &s, 200, 3, order);
  m.raw = "\x10\x02";
  m.rawlen = 2;
  up_msg(&s, &m);
  up_expect("size again after data", cl, "POST", GRPC_CT, NULL, &s, 200, 3, order);
  up_first(&s, k, kl, 1);
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .data = "x", .dlen = 1});
  up_expect("key in message 2", cl, "POST", GRPC_CT, NULL, &s, 200, 3, first_only);
  up_first(&s, k, kl, 1);
  up_msg(&s, &(UpMsg){.has_size = 1, .size = 1, .data = "x", .dlen = 1});
  up_expect("total_size in message 2", cl, "POST", GRPC_CT, NULL, &s, 200, 3, first_only);
  up_first(&s, k, kl, 1);
  up_data(&s, "x", 1);
  up_msg(&s, &(UpMsg){.has_size = 1, .size = 1});
  up_expect("total_size after the value", cl, "POST", GRPC_CT, NULL, &s, 200, 3, first_only);

  /* exactly total_size bytes: too many is refused at the first extra byte */
  up_first(&s, k, kl, 10);
  up_data(&s, "0123456789A", 11);
  up_expect("one byte too many", cl, "POST", GRPC_CT, NULL, &s, 200, 3, over);
  up_first(&s, k, kl, 10);
  up_data(&s, "012345", 6);
  up_data(&s, "6789AB", 6);
  up_expect("too many across messages", cl, "POST", GRPC_CT, NULL, &s, 200, 3, over);
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 0, .data = "x", .dlen = 1});
  up_expect("data for an empty value", cl, "POST", GRPC_CT, NULL, &s, 200, 3, over);
  up_first(&s, k, kl, 10);
  up_data(&s, "01234", 5);
  up_expect("five bytes short", cl, "POST", GRPC_CT, NULL, &s, 200, 3, shortv);
  up_first(&s, k, kl, 10);
  up_expect("no data at all", cl, "POST", GRPC_CT, NULL, &s, 200, 3, shortv);

  /* the request stream must end between messages */
  up_first(&s, k, kl, 3);
  up_data(&s, "abc", 3);
  buf_put(&s, "\0\0\0", 3);
  up_expect("ends inside a prefix", cl, "POST", GRPC_CT, NULL, &s, 200, 13, trunc);
  up_first(&s, k, kl, 95);
  uint8_t cut[5 + 2 + 45];
  set_prefix(cut, 0, 97);                  /* a 95-byte data field: 45 bytes arrive */
  cut[5] = 0x1A;
  cut[6] = 95;
  memset(cut + 7, 'd', 45);
  buf_put(&s, cut, sizeof cut);
  up_expect("ends inside a data field", cl, "POST", GRPC_CT, NULL, &s, 200, 13, trunc);
  uint8_t cut1[5 + 4] = {0, 0, 0, 0, 20, 0x0A, 18, 'a', 'b'};
  buf_put(&s, cut1, sizeof cut1);          /* the first message's key, cut short */
  up_expect("ends inside message 1", cl, "POST", GRPC_CT, NULL, &s, 200, 13, trunc);

  /* crc32c: at most once, and it must match */
  uint32_t cx = crc32c(0, U8("x"), 1), cabc = crc32c(0, U8("abc"), 3);
  uint8_t crc_field[5] = {0x25, (uint8_t)cx, (uint8_t)(cx >> 8), (uint8_t)(cx >> 16),
                          (uint8_t)(cx >> 24)};
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 1, .data = "x", .dlen = 1,
                      .has_crc = 1, .crc = cx, .raw = crc_field, .rawlen = 5});
  up_expect("crc32c twice in a message", cl, "POST", GRPC_CT, NULL, &s, 200, 3,
            "crc32c sent more than once");
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 1, .has_crc = 1, .crc = cx});
  up_msg(&s, &(UpMsg){.data = "x", .dlen = 1, .has_crc = 1, .crc = cx});
  up_expect("crc32c in two messages", cl, "POST", GRPC_CT, NULL, &s, 200, 3,
            "crc32c sent more than once");
  UpMsg badcrc = {.key = k, .klen = kl, .has_size = 1, .size = 3, .data = "abc", .dlen = 3,
                  .has_crc = 1, .crc = cabc ^ 1};
  up_msg(&s, &badcrc);
  up_expect("crc32c mismatch", cl, "POST", GRPC_CT, NULL, &s, 200, 15,
            "crc32c mismatch: nothing was stored");
  uint8_t *v;
  size_t vl;
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);                   /* nothing stored */
  CHECK(op_put(cl, k, kl, "old", 3) == 1);
  up_msg(&s, &badcrc);
  up_expect("crc32c mismatch, replace", cl, "POST", GRPC_CT, NULL, &s, 200, 15,
            "crc32c mismatch: nothing was stored");
  CHECK(op_get(cl, k, kl, &v, &vl) == 1 && vl == 3 && memcmp(v, "old", 3) == 0);
  free(v);                                                  /* the old value stands */
  CHECK(op_delete(cl, k, kl) == 1);

  /* message framing and compression: the unary rules, applied per message */
  UpMsg fl = {.key = k, .klen = kl, .has_size = 1, .size = 1, .data = "x", .dlen = 1, .flag = 1};
  up_msg(&s, &fl);
  up_expect("flag 1, no encoding", cl, "POST", GRPC_CT, NULL, &s, 200, 13,
            "compressed flag set without a grpc-encoding");
  up_msg(&s, &fl);
  up_expect("flag 1, identity", cl, "POST", GRPC_CT, "identity", &s, 200, 13,
            "compressed flag set without a grpc-encoding");
  up_msg(&s, &fl);
  Call c;
  CHECK(call_run(cl, &c, "POST", PATH_UPLOAD, GRPC_CT, "gzip", s.p, s.n) == 0);
  CHECK(c.closed && c.close_error == 0 && c.http_status == 200 && c.grpc_status == 12 &&
        c.body_len == 0 && strstr(c.grpc_message, "compressed messages are not supported") &&
        strcmp(c.accept_encoding, "identity") == 0);
  call_free(&c);
  buf_free(&s);
  fl.flag = 2;
  up_msg(&s, &fl);
  up_expect("flag 2", cl, "POST", GRPC_CT, NULL, &s, 200, 13, "invalid gRPC message prefix");
  fl.flag = 0x80;
  up_msg(&s, &fl);
  up_expect("flag 0x80", cl, "POST", GRPC_CT, NULL, &s, 200, 13, "invalid gRPC message prefix");
  up_first(&s, k, kl, 1);
  up_msg(&s, &(UpMsg){.data = "x", .dlen = 1, .flag = 1});
  up_expect("flag 1 in message 2", cl, "POST", GRPC_CT, NULL, &s, 200, 13,
            "compressed flag set without a grpc-encoding");
  up_first(&s, k, kl, 1);
  up_data(&s, "x", 1);
  up_msg(&s, &(UpMsg){.flag = 2});
  up_expect("flag 2 after the value", cl, "POST", GRPC_CT, NULL, &s, 200, 13,
            "invalid gRPC message prefix");

  /* malformed protobuf, after a valid key and total_size */
  static const struct {
    const char *name, *raw;
    size_t len;
  } bad[] = {
      {"field number 0", "\x00\x01", 2},
      {"wire type 3", "\x0B", 1},
      {"wire type 4", "\x0C", 1},
      {"wire type 6", "\x0E\x00", 2},
      {"wire type 7", "\x0F\x00", 2},
      {"11-byte varint", "\x10\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x01", 12},
      {"tag of 2^32", "\x80\x80\x80\x80\x10\x00", 6},
      {"varint cut by the message end", "\x78\xFF", 2},
      {"bytes past the message end", "\x4A\x05" "ab", 4},
      {"fixed32 past the message end", "\x8D\x01\x01\x02", 4},
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 0, .raw = bad[i].raw,
                        .rawlen = bad[i].len});
    up_expect(bad[i].name, cl, "POST", GRPC_CT, NULL, &s, 200, 3, "malformed request message");
  }

  /* HTTP-level refusals come before any of the body is decoded */
  up_first(&s, k, kl, 1);
  up_data(&s, "x", 1);
  up_expect("upload with GET", cl, "GET", GRPC_CT, NULL, &s, 405, -1, NULL);
  up_expect("GET, no body", cl, "GET", GRPC_CT, NULL, &s, 405, -1, NULL);
  up_first(&s, k, kl, 1);
  up_data(&s, "x", 1);
  up_expect("json upload", cl, "POST", "application/json", NULL, &s, 415, -1, NULL);
  up_first(&s, k, kl, 1);
  up_data(&s, "x", 1);
  up_expect("no content-type", cl, "POST", NULL, NULL, &s, 415, -1, NULL);

  /* nothing above stored anything, and the connection survived it all */
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);
  CHECK(op_upload(cl, k, kl, U8("alive"), 5, 2, CRC_LAST, 1, NULL) == 1);
  CHECK(get_matches(cl, k, kl, U8("alive"), 5));
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * The accepted variations of the request stream: unknown fields and known
 * numbers of another wire type (skipped), empty messages and empty data
 * fields, last-wins inside the first message, the crc32c anywhere, empty
 * values, the key-length boundary, and Upload/Put interplay.
 * @param cl  connection
 */
static void test_upload_forms(Client *cl) {
  char k[64], junk[64], e[64];
  size_t kl = mkkey(k, sizeof k, "upform", 1), jl = mkkey(junk, sizeof junk, "upform-junk", 1);
  size_t el = mkkey(e, sizeof e, "upform-empty", 1);
  Buf s = {0};
  Call c;
  int created;
  uint32_t crc;
  uint8_t *v;
  size_t vl;

  /* 1. unknown fields and wrong wire types, in every kind of message */
  static const uint8_t unknown[] = {
      0x78, 0x2A,                                   /* 15: varint 42 */
      0x81, 0x01, 1, 2, 3, 4, 5, 6, 7, 8,           /* 16: fixed64 */
      0x8D, 0x01, 1, 2, 3, 4,                       /* 17: fixed32 */
      0x4A, 0x02, 'z', 'z',                         /* 9: bytes */
      0x08, 0x01,                                   /* 1 (key) as a varint */
      0x12, 0x01, 'w',                              /* 2 (total_size) as bytes */
      0x18, 0x05,                                   /* 3 (data) as a varint */
      0x22, 0x01, 'q',                              /* 4 (crc32c) as bytes */
      0x21, 1, 2, 3, 4, 5, 6, 7, 8,                 /* 4 (crc32c) as fixed64 */
  };
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 6, .raw = unknown,
                      .rawlen = sizeof unknown});
  up_msg(&s, &(UpMsg){.data = "abc", .dlen = 3, .raw = unknown, .rawlen = sizeof unknown});
  up_msg(&s, &(UpMsg){0});                              /* an empty message */
  up_data(&s, "", 0);                                   /* an empty data field */
  up_msg(&s, &(UpMsg){.raw = unknown, .rawlen = sizeof unknown});
  up_msg(&s, &(UpMsg){.data = "def", .dlen = 3, .has_crc = 1, .crc = crc32c(0, U8("abcdef"), 6)});
  memset(&c, 0, sizeof c);
  CHECK(upload_run(cl, &c, &s) == 0 && call_ok("unknown fields", &c) &&
        parse_upload_reply(&c, &created, &crc) && created == 1 &&
        crc == crc32c(0, U8("abcdef"), 6));
  call_free(&c);
  buf_free(&s);
  CHECK(get_matches(cl, k, kl, U8("abcdef"), 6));

  /* 2. inside the first message the last key and the last total_size win */
  Buf tail = {0};
  buf_byte(&tail, 0x0A);
  buf_varint(&tail, kl);
  buf_put(&tail, k, kl);
  buf_byte(&tail, 0x10);
  buf_varint(&tail, 3);
  up_msg(&s, &(UpMsg){.key = junk, .klen = jl, .has_size = 1, .size = 99, .raw = tail.p,
                      .rawlen = tail.n});
  buf_free(&tail);
  up_data(&s, "xyz", 3);
  memset(&c, 0, sizeof c);
  CHECK(upload_run(cl, &c, &s) == 0 && call_ok("last wins", &c) &&
        parse_upload_reply(&c, &created, &crc) && created == 0);   /* replaced k */
  call_free(&c);
  buf_free(&s);
  CHECK(get_matches(cl, k, kl, U8("xyz"), 3));
  CHECK(op_get(cl, junk, jl, &v, &vl) == 0);            /* the first key was dropped */

  /* 3. an empty value, its crc32c (0) sent before the key */
  Buf body = {0};
  uint8_t zcrc[5] = {0x25, 0, 0, 0, 0};
  buf_put(&body, zcrc, 5);
  buf_byte(&body, 0x0A);
  buf_varint(&body, el);
  buf_put(&body, e, el);
  buf_byte(&body, 0x10);
  buf_byte(&body, 0);
  up_msg(&s, &(UpMsg){.raw = body.p, .rawlen = body.n});
  buf_free(&body);
  memset(&c, 0, sizeof c);
  CHECK(upload_run(cl, &c, &s) == 0 && call_ok("empty value", &c) &&
        parse_upload_reply(&c, &created, &crc) && created == 1 && crc == 0 &&
        c.body_len == 7);                               /* 00 00000002 08 01: no crc field */
  call_free(&c);
  buf_free(&s);
  CHECK(get_matches(cl, e, el, NULL, 0));
  CHECK(download_equals(cl, e, el, NULL, 0, 0));

  /* 4. an empty value with an explicit empty data field; replace: empty reply */
  up_msg(&s, &(UpMsg){.key = e, .klen = el, .has_size = 1, .size = 0, .data = "", .dlen = 0});
  memset(&c, 0, sizeof c);
  CHECK(upload_run(cl, &c, &s) == 0 && call_ok("empty field", &c) &&
        parse_upload_reply(&c, &created, &crc) && created == 0 && crc == 0 && c.body_len == 5);
  call_free(&c);
  buf_free(&s);
  CHECK(op_delete(cl, e, el) == 1);

  /* 5. Upload replaces a Put value; Put replaces an uploaded value */
  CHECK(op_put(cl, e, el, "put", 3) == 1);
  CHECK(op_upload(cl, e, el, U8("upload"), 6, 4, CRC_FIRST, 0, NULL) == 0);
  CHECK(get_matches(cl, e, el, U8("upload"), 6));
  CHECK(op_put(cl, e, el, "put2", 4) == 0);
  CHECK(download_equals(cl, e, el, U8("put2"), 4, 0));
  CHECK(op_delete(cl, e, el) == 1);

  /* 6. a key of exactly max_key bytes (Upload and Download) */
  uint8_t *bigk = malloc(g_max_key);
  memset(bigk, 'U', g_max_key);
  memcpy(bigk, g_nonce, strlen(g_nonce));
  CHECK(op_upload(cl, bigk, g_max_key, U8("maxkey"), 6, 6, CRC_ALONE, 1, NULL) == 1);
  CHECK(download_equals(cl, bigk, g_max_key, U8("maxkey"), 6, 0));
  CHECK(op_delete(cl, bigk, g_max_key) == 1);
  free(bigk);
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * Upload/Download round trips over a matrix of value sizes (every boundary
 * around 1, 64, 128, 4 KiB, 16 KiB, 64 KiB, max_value and beyond), upload
 * chunkings (1 byte to 1 MiB per message), every crc32c placement, data in
 * the first message or not, and download chunk sizes — each value checked
 * byte-exact through Download, and through Get where it fits max_value.
 * @param cl  connection
 */
static void test_upload_basic(Client *cl) {
  static const size_t sizes[] = {0,     1,     2,     5,     63,    64,      65,      127,
                                 128,   129,   4095,  4096,  4097,  16383,   16384,   16385,
                                 65535, 65536, 65537, 1048575, 1048576, 1048577, 3145735};
  static const size_t chunks[] = {1, 3, 1000, 16384, 65536, 1048576};
  static const uint64_t dl_chunks[] = {0, 7, 4096, 65536, 1048576, 2097152};
  size_t vmax = 3145735;
  uint8_t *val = malloc(vmax);
  uint64_t seed = 0x5EED0001;
  int ops = 0, bad = 0;
  uint64_t bytes = 0, t0 = now_ns();
  for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
    size_t n = sizes[si];
    if (n > g_max_upload) continue;
    char k[64];
    size_t kl = mkkey(k, sizeof k, "upbasic", si);
    int first = 1;
    for (size_t ci = 0; ci < sizeof chunks / sizeof chunks[0]; ci++) {
      size_t chunk = chunks[ci];
      if (n / chunk > 4096) continue;           /* bounded message counts */
      for (int where = CRC_NONE; where <= CRC_ALONE; where++) {
        int first_data = (int)((ci + (size_t)where + si) % 2);
        rng_fill(&seed, val, n);                /* every upload differs: replaces show */
        uint32_t crc = 0;
        int r = op_upload(cl, k, kl, val, n, chunk, where, first_data, &crc);
        if (r != (first ? 1 : 0)) {
          fprintf(stderr, "  upload %zu bytes chunk %zu crc@%d: %d, want %d\n", n, chunk, where,
                  r, first ? 1 : 0);
          bad++;
        }
        first = 0;
        uint64_t dc = dl_chunks[(si + ci + (size_t)where) % 6];
        if (!download_equals(cl, k, kl, val, n, dc)) {
          fprintf(stderr, "  download %zu bytes chunk %" PRIu64 " (upload chunk %zu crc@%d)\n",
                  n, dc, chunk, where);
          bad++;
        }
        if (!get_matches(cl, k, kl, val, n)) bad++;
        ops++;
        bytes += n;
      }
    }
    CHECK(op_delete(cl, k, kl) == 1);           /* an uploaded key deletes like any other */
  }
  CHECK_EQ_U64(bad, 0);

  /* the size limit's boundary: exactly KVASM_MAX_UPLOAD_BYTES is stored */
  char k[64];
  size_t kl = mkkey(k, sizeof k, "upmax", 1);
  uint8_t *big = malloc(g_max_upload ? g_max_upload : 1);
  rng_fill(&seed, big, g_max_upload);
  CHECK(op_upload(cl, k, kl, big, g_max_upload, 1 << 20, CRC_LAST, 1, NULL) == 1);
  CHECK(download_equals(cl, k, kl, big, g_max_upload, 0));
  CHECK(get_matches(cl, k, kl, big, g_max_upload));
  CHECK(op_delete(cl, k, kl) == 1);
  bytes += g_max_upload;
  free(big);
  free(val);
  printf("  upload/download matrix: %d round trips, %.1f MiB each way, %.2fs\n", ops + 1,
         (double)bytes / 1048576.0, (double)(now_ns() - t0) / 1e9);
}

/**
 * The server's decoder must not care where DATA frames split the request:
 * one fixed stream (prefixes, tags, multi-byte varints, key bytes, unknown
 * fields, an empty message, data pieces and the crc32c) is sent with its
 * first frame ending at EVERY byte position, then as 1..7-byte frames, and
 * a 200 KB upload with random frame sizes under several seeds.
 * @param cl  connection
 */
static void test_upload_splits(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "split", 1);
  uint8_t val[300];
  uint64_t seed = 0x5B117;
  rng_fill(&seed, val, sizeof val);
  uint32_t vcrc = crc32c(0, val, sizeof val);
  Buf s = {0};
  up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = 300, .raw = "\x78\xAC\x02",
                      .rawlen = 3});
  up_data(&s, val, 1);
  up_msg(&s, &(UpMsg){0});
  up_msg(&s, &(UpMsg){.data = val + 1, .dlen = 200, .raw = "\x4A\x03" "abc", .rawlen = 5});
  up_msg(&s, &(UpMsg){.data = val + 201, .dlen = 99, .has_crc = 1, .crc = vcrc});
  int bad = 0, runs = 0;
  for (size_t split = 1; split < s.n; split++, runs++) {
    Call c;
    memset(&c, 0, sizeof c);
    c.split_at = split;
    int created;
    uint32_t crc;
    if (upload_run(cl, &c, &s) != 0 || !call_ok("split upload", &c) ||
        !parse_upload_reply(&c, &created, &crc) || crc != vcrc || created != (split == 1)) {
      fprintf(stderr, "  split at %zu failed\n", split);
      bad++;
    }
    call_free(&c);
    if (!download_equals(cl, k, kl, val, sizeof val, (split % 5) * 50)) bad++;
  }
  for (size_t fm = 1; fm <= 7; fm++, runs++) {
    Call c;
    memset(&c, 0, sizeof c);
    c.frame_max = fm;                           /* every frame at most fm bytes */
    int created;
    uint32_t crc;
    if (upload_run(cl, &c, &s) != 0 || !call_ok("tiny frames", &c) ||
        !parse_upload_reply(&c, &created, &crc) || crc != vcrc || created != 0) {
      fprintf(stderr, "  %zu-byte frames failed\n", fm);
      bad++;
    }
    call_free(&c);
  }
  buf_free(&s);
  CHECK(download_equals(cl, k, kl, val, sizeof val, 0));

  size_t n = 200000;
  uint8_t *big = malloc(n);
  rng_fill(&seed, big, n);
  Buf b = up_stream(k, kl, big, n, 777, CRC_LAST, 1);
  for (uint64_t fs = 1; fs <= 6; fs++, runs++) {
    Call c;
    memset(&c, 0, sizeof c);
    c.frame_max = 1 + fs * 1500;
    c.frame_seed = fs * 0x9E3779B97F4A7C15ull;  /* random frames in [1, frame_max] */
    int created;
    uint32_t crc;
    if (upload_run(cl, &c, &b) != 0 || !call_ok("random frames", &c) ||
        !parse_upload_reply(&c, &created, &crc) || crc != crc32c(0, big, n) || created != 0) {
      fprintf(stderr, "  random frames (seed %" PRIu64 ") failed\n", fs);
      bad++;
    }
    call_free(&c);
  }
  CHECK(download_equals(cl, k, kl, big, n, 0));
  buf_free(&b);
  free(big);
  CHECK(op_delete(cl, k, kl) == 1);
  CHECK_EQ_U64(bad, 0);
  printf("  upload framing: %d uploads with shifted frame boundaries\n", runs);
}

/**
 * Refusals are answered at once, while the client still holds most of its
 * body back: a too-large total_size after the first message, more data than
 * declared in the middle of a data field, an invalid prefix flag after five
 * bytes. Each call must close with its error (NO_ERROR reset) while the
 * client's body is still stalled — the proof the server did not wait for
 * the rest.
 * @param cl  connection
 */
static void test_upload_early(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "early", 1);
  size_t pad = 256 * 1024;
  uint8_t *junk = calloc(1, pad);
  struct {
    const char *name;
    uint64_t size;
    uint8_t flag;
    size_t extra;            /* data bytes sent after the first message */
    int grpc;
    const char *msg;
  } cases[] = {
      {"too large, held back", g_max_upload + 1, 0, 0, 3, "value too large"},
      {"overflow, mid-field", 1000, 0, 2000, 3, "more data than total_size"},
      {"flag 2, held back", 10, 2, 0, 13, "invalid gRPC message prefix"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    Buf s = {0};
    up_msg(&s, &(UpMsg){.key = k, .klen = kl, .has_size = 1, .size = cases[i].size,
                        .flag = cases[i].flag});
    size_t stop = cases[i].flag ? 5 : s.n;   /* a bad flag: only its prefix goes out */
    up_data(&s, junk, pad);
    stop += cases[i].extra ? 5 + 1 + 3 + cases[i].extra : 0; /* into the data field */
    Call c;
    memset(&c, 0, sizeof c);
    c.defer_at = stop;
    CHECK(call_submit(cl, &c, "POST", PATH_UPLOAD, GRPC_CT, NULL, s.p, s.n) == 0);
    int rc = wait_closed(cl, &c, CALL_TIMEOUT_MS);
    int ok = rc == 0 && c.close_error == 0 && c.http_status == 200 &&
             c.grpc_status == cases[i].grpc && strstr(c.grpc_message, cases[i].msg) &&
             c.body_len == 0 && c.req_off == stop && stop < s.n;
    if (!ok)
      fprintf(stderr, "  early refusal \"%s\": rc=%d err=%u grpc=%d \"%s\" sent %zu of %zu\n",
              cases[i].name, rc, c.close_error, c.grpc_status, c.grpc_message, c.req_off, s.n);
    CHECK(ok);
    call_free(&c);
    buf_free(&s);
  }
  free(junk);
  uint8_t *v;
  size_t vl;
  CHECK(op_get(cl, k, kl, &v, &vl) == 0);                   /* none stored anything */
  CHECK(op_put(cl, k, kl, "fine", 4) == 1);                 /* the connection is fine */
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * Uploads that never finish store nothing and leave the old value standing:
 * RST_STREAM(CANCEL) inside the first prefix, right after the first message
 * (the record reserved), in the middle of the data, and after EVERY data
 * byte and a matching crc32c with only END_STREAM missing; then an abrupt
 * TCP reset of a connection in the middle of an upload.
 * @param cl  connection
 */
static void test_upload_cancel(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "upcancel", 1);
  CHECK(op_put(cl, k, kl, "old", 3) == 1);
  size_t n = 256 * 1024;
  uint8_t *v = malloc(n);
  uint64_t seed = 0xCA9CE1;
  rng_fill(&seed, v, n);
  Buf s = {0};
  up_first(&s, k, kl, n);
  size_t first_len = s.n;
  for (size_t off = 0; off < n; off += 65536) {
    UpMsg d = {.data = v + off, .dlen = 65536};
    if (off + 65536 == n) {
      d.has_crc = 1;
      d.crc = crc32c(0, v, n);
    }
    up_msg(&s, &d);
  }
  size_t all_data = s.n;
  up_msg(&s, &(UpMsg){0});      /* a trailing empty message holds END_STREAM back */
  const size_t stops[] = {3, first_len, first_len + 100000, all_data};
  for (size_t i = 0; i < 4; i++) {
    Call c;
    memset(&c, 0, sizeof c);
    c.defer_at = stops[i];
    CHECK(submit_stalled(cl, &c, PATH_UPLOAD, s.p, s.n) == 0);
    CHECK(cancel_call(cl, &c) == 0);
    call_free(&c);
    CHECK(get_matches(cl, k, kl, U8("old"), 3));          /* nothing published */
  }

  /* an abrupt disconnect in the middle of an upload, on a connection of its own */
  Client x;
  CHECK(client_open(&x, g_host, g_port, 65535) == 0);
  Call cx;
  memset(&cx, 0, sizeof cx);
  cx.defer_at = first_len + 100000;
  CHECK(submit_stalled(&x, &cx, PATH_UPLOAD, s.p, s.n) == 0);
  client_abort(&x);
  CHECK(get_matches(cl, k, kl, U8("old"), 3));

  /* the whole stream, uninterrupted, replaces the value */
  Call c;
  memset(&c, 0, sizeof c);
  int created;
  uint32_t crc;
  CHECK(upload_run(cl, &c, &s) == 0 && call_ok("full upload", &c) &&
        parse_upload_reply(&c, &created, &crc) && created == 0 && crc == crc32c(0, v, n));
  call_free(&c);
  CHECK(download_equals(cl, k, kl, v, n, 0));
  CHECK(op_delete(cl, k, kl) == 1);
  buf_free(&s);
  free(v);
}

/**
 * Download's refusals: NOT_FOUND, key validation, protobuf and framing
 * errors (the unary rules), HTTP-level 405/415, OUT_OF_RANGE for an offset
 * past the end — and the accepted forms: unknown fields and wrong wire
 * types skipped, the last of a repeated field winning.
 * @param cl  connection
 */
static void test_download_errors(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "dlerr", 1);
  Msg none = {NULL, 0};
  expect_error("download: not found", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(k, kl, 0, 0, 0, 0), 200, 5, "key not found");
  expect_error("download: no key", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(NULL, 0, 5, 0, 0, 0), 200, 3, "key length out of range");
  expect_error("download: empty key", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download("", 0, 0, 0, 0, 0), 200, 3, "key length out of range");
  uint8_t *bigk = malloc(g_max_key + 1);
  memset(bigk, 'D', g_max_key + 1);
  expect_error("download: long key", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(bigk, g_max_key + 1, 0, 0, 0, 0), 200, 3, "key length out of range");
  free(bigk);
  expect_error("download: malformed", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_raw("\0\0\0\0\2\x0a\xff", 7), 200, 3, "malformed");
  expect_error("download: empty body", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, none, 200, 13,
               "exactly one");
  Msg one = msg_download(k, kl, 0, 0, 0, 0);
  Msg two = {malloc(one.len * 2), one.len * 2};
  memcpy(two.p, one.p, one.len);
  memcpy(two.p + one.len, one.p, one.len);
  free(one.p);
  expect_error("download: two messages", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, two, 200, 13,
               "exactly one");
  Msg m = msg_download(k, kl, 0, 0, 0, 0);
  m.p[0] = 2;
  expect_error("download: flag 2", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, m, 200, 13,
               "exactly one");
  m = msg_download(k, kl, 0, 0, 0, 0);
  m.p[0] = 1;
  expect_error("download: flag 1", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, m, 200, 13,
               "compressed flag set without a grpc-encoding");
  expect_error("download: GET", cl, "GET", PATH_DOWNLOAD, GRPC_CT, NULL, none, 405, -1, NULL);
  expect_error("download: json", cl, "POST", PATH_DOWNLOAD, "application/json", NULL,
               msg_download(k, kl, 0, 0, 0, 0), 415, -1, NULL);

  /* ranges starting past the end */
  CHECK(op_put(cl, k, kl, "0123456789", 10) == 1);
  expect_error("download: offset 11 of 10", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(k, kl, 11, 0, 0, 0), 200, 11, "offset is beyond the end of the value");
  expect_error("download: offset 2^64-1", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(k, kl, UINT64_MAX, 1, 5, 0), 200, 11,
               "offset is beyond the end of the value");

  /* unknown field, offset as bytes (skipped), offset 9 then 3 (last wins), chunk 2 */
  Buf b = {0};
  buf_put(&b, "\x78\x07", 2);
  buf_byte(&b, 0x0A);
  buf_varint(&b, kl);
  buf_put(&b, k, kl);
  buf_put(&b, "\x12\x01x\x10\x09\x10\x03\x20\x02", 9);
  Msg fwd = {malloc(5 + b.n), 5 + b.n};
  set_prefix(fwd.p, 0, (uint32_t)b.n);
  memcpy(fwd.p + 5, b.p, b.n);
  buf_free(&b);
  Call c;
  CHECK(call_run(cl, &c, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, fwd.p, fwd.len) == 0 &&
        call_ok("download forms", &c));
  Dl d = parse_download(&c);
  CHECK(dl_expect("download forms", &d, U8("3456789"), 7, 10, 2));
  dl_free(&d);
  call_free(&c);
  free(fwd.p);
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * Ranges and chunking of Download, each response parsed message by message:
 * total_size in the first message only, data fields exactly chunk bytes but
 * the last, the crc32c of exactly the range in the last message, empty
 * ranges as one message. Fixed edge cases, then 200 random ranges.
 * @param cl  connection
 */
static void test_download_ranges(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "range", 1);
  const size_t n = 200003;
  uint8_t *val = malloc(n);
  uint64_t seed = 0xA46E;
  rng_fill(&seed, val, n);
  CHECK(op_upload(cl, k, kl, val, n, 65536, CRC_LAST, 1, NULL) == 1);
  const struct {
    uint64_t off;
    int has_len;
    uint64_t len, chunk;
  } cases[] = {
      {0, 0, 0, 0},         {1, 0, 0, 0},          {n - 1, 0, 0, 0},    {n, 0, 0, 0},
      {0, 1, 0, 0},         {n, 1, 0, 0},          {n, 1, 5, 0},        {0, 1, 1, 0},
      {n - 1, 1, 1, 0},     {7, 1, n - 7, 0},      {7, 1, n, 0},        {7, 1, UINT64_MAX, 0},
      {100, 1, 65536, 0},   {100, 1, 65537, 0},    {n - 5000, 0, 0, 1}, {0, 0, 0, 7},
      {0, 0, 0, 4096},      {0, 0, 0, 65535},      {0, 0, 0, 65537},    {0, 0, 0, 1 << 20},
      {0, 0, 0, (1 << 20) + 1}, {0, 0, 0, 1ull << 32}, {0, 0, 0, UINT64_MAX}, {3, 1, 200000, 3},
  };
  int bad = 0;
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    uint64_t off = cases[i].off, end = n;
    if (cases[i].has_len && cases[i].len < n - off) end = off + cases[i].len;
    Call c;
    Dl d;
    char what[96];
    snprintf(what, sizeof what, "range case %zu", i);
    if (op_download(cl, k, kl, off, cases[i].has_len, cases[i].len, cases[i].chunk, &c, &d) != 0 ||
        !call_ok(what, &c) ||
        !dl_expect(what, &d, val + off, (size_t)(end - off), n, eff_chunk(cases[i].chunk)))
      bad++;
    dl_free(&d);
    call_free(&c);
  }
  for (int i = 0; i < 200; i++) {
    uint64_t r = rng_next(&seed);
    uint64_t off = r % (n + 1);
    int has_len = (int)((r >> 20) % 3 != 0);
    uint64_t len = (r >> 24) % 4 == 0 ? UINT64_MAX : (r >> 30) % (n + 10);
    uint64_t chunk = (r >> 40) % 5 == 0 ? 0 : 50 + (r >> 44) % 90000;
    uint64_t end = has_len && len < n - off ? off + len : n;
    Call c;
    Dl d;
    if (op_download(cl, k, kl, off, has_len, len, chunk, &c, &d) != 0 ||
        !call_ok("random range", &c) ||
        !dl_expect("random range", &d, val + off, (size_t)(end - off), n, eff_chunk(chunk)))
      bad++;
    dl_free(&d);
    call_free(&c);
  }
  CHECK_EQ_U64(bad, 0);

  /* an empty value: offset 0 is an empty range, offset 1 is out of range */
  CHECK(op_upload(cl, k, kl, NULL, 0, 1, CRC_NONE, 1, NULL) == 0);
  CHECK(download_equals(cl, k, kl, NULL, 0, 0));
  expect_error("offset 1 of an empty value", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(k, kl, 1, 0, 0, 0), 200, 11, "offset is beyond the end");
  CHECK(op_delete(cl, k, kl) == 1);
  free(val);
}

/**
 * Flow control of generated response bodies: (a) 1- and 13-byte stream
 * windows, so every DATA frame boundary falls somewhere else inside the
 * prefixes, fields and crc32c tails; (b) a download stalled on its stream
 * window must hold at exactly one window while other calls on the same
 * connection are answered, then complete when the window reopens; (c) 40
 * downloads in flight on one connection with 16 KiB windows.
 * @param cl  main connection (uploads the values)
 */
static void test_download_flow(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "flow", 1);
  uint64_t seed = 0xF10;
  size_t n = 3 * 1048576 + 11;
  uint8_t *val = malloc(n);
  rng_fill(&seed, val, n);

  /* (a) tiny windows: 2003 bytes in 7-byte chunks through a 1-byte window,
   *     70000 bytes in 1000-byte chunks through a 13-byte window */
  const struct {
    uint32_t window;
    size_t len;
    uint64_t chunk;
  } tiny[] = {{1, 2003, 7}, {13, 70000, 1000}};
  for (size_t i = 0; i < 2; i++) {
    Client t;
    CHECK(client_open_ex(&t, g_host, g_port, tiny[i].window, 0, NULL) == 0);
    CHECK(op_upload(cl, k, kl, val, tiny[i].len, 4096, CRC_LAST, 1, NULL) >= 0);
    CHECK(download_equals(&t, k, kl, val, tiny[i].len, tiny[i].chunk));
    client_close(&t);
  }

  /* (b) stall on the stream window, other calls proceed, then resume */
  CHECK(op_upload(cl, k, kl, val, n, 1 << 20, CRC_LAST, 1, NULL) == 0);
  char sk[64];
  size_t skl = mkkey(sk, sizeof sk, "flowsmall", 1);
  CHECK(op_put(cl, sk, skl, "small", 5) == 1);
  Client m;
  CHECK(client_open_ex(&m, g_host, g_port, 65535, 1, NULL) == 0);
  Msg dm = msg_download(k, kl, 0, 0, 0, 0);
  Call d;
  memset(&d, 0, sizeof d);
  CHECK(call_submit(&m, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d, 65535) == 0);
  for (int i = 0; i < 20; i++) {
    uint8_t *v;
    size_t vl;
    CHECK(op_get(&m, sk, skl, &v, &vl) == 1 && vl == 5);
    free(v);
  }
  CHECK(await_ping_ack(&m, 10) == 0);
  CHECK(d.body_len == 65535 && !d.closed);   /* still held at exactly one window */
  uint64_t deadline = now_ns() + 20000000000ull;
  while (!d.closed && !m.eof && now_ns() < deadline) {
    consume_call(&m, &d);
    CHECK(pump(&m, 20) == 0);
  }
  CHECK(call_ok("stalled download", &d));
  Dl p = parse_download(&d);
  CHECK(dl_expect("stalled download", &p, val, n, n, DL_DEFAULT_CHUNK));
  dl_free(&p);
  call_free(&d);
  free(dm.p);
  client_close(&m);
  CHECK(op_delete(cl, sk, skl) == 1);

  /* (c) 40 downloads in flight on one connection, 16 KiB stream windows */
  enum { M = 40 };
  static Call calls[M];
  static Msg msgs[M];
  static size_t lens[M];
  static uint64_t chunks[M];
  char mk[M][64];
  size_t mkl[M];
  for (int i = 0; i < M; i++) {
    mkl[i] = mkkey(mk[i], sizeof mk[i], "flowmany", (uint64_t)i);
    lens[i] = (size_t)(rng_next(&seed) % 150001);
    chunks[i] = rng_next(&seed) % 3 == 0 ? 0 : 1 + rng_next(&seed) % 40000;
    CHECK(op_upload(cl, mk[i], mkl[i], val + i, lens[i], 30000, CRC_FIRST, 1, NULL) == 1);
  }
  Client w;
  CHECK(client_open(&w, g_host, g_port, 16384) == 0);
  for (int i = 0; i < M; i++) {
    msgs[i] = msg_download(mk[i], mkl[i], 0, 0, 0, chunks[i]);
    memset(&calls[i], 0, sizeof calls[i]);
    CHECK(call_submit(&w, &calls[i], "POST", PATH_DOWNLOAD, GRPC_CT, NULL, msgs[i].p,
                      msgs[i].len) == 0);
  }
  CHECK(run_until_idle(&w, CALL_TIMEOUT_MS) == 0);
  int exact = 0;
  for (int i = 0; i < M; i++) {
    Dl q = parse_download(&calls[i]);
    exact += call_ok("concurrent download", &calls[i]) &&
             dl_expect("concurrent download", &q, val + i, lens[i], lens[i], eff_chunk(chunks[i]));
    dl_free(&q);
    call_free(&calls[i]);
    free(msgs[i].p);
    CHECK(op_delete(cl, mk[i], mkl[i]) == 1);
  }
  CHECK(exact == M);
  client_close(&w);
  CHECK(op_delete(cl, k, kl) == 1);
  free(val);
}

/**
 * One Download call reads one version, whatever happens to the key
 * meanwhile: downloads stalled on their windows while the key is replaced
 * by Upload, replaced by Put and deleted — with enough writes and time
 * (> 1 s of housekeeping) that the grace periods of the old records end —
 * must each complete with exactly the version they started on. This is the
 * pin (kv_store_pin) end to end: an unpinned old record would be freed
 * under the stalled reader.
 * @param cl  main connection
 */
static void test_download_consistency(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "pin", 1);
  uint64_t seed = 0x9127;
  size_t n1 = 2 * 1048576 + 3, n2 = 1536 * 1024, n3 = 1000;
  uint8_t *v1 = malloc(n1), *v2 = malloc(n2), *v3 = malloc(n3);
  rng_fill(&seed, v1, n1);
  rng_fill(&seed, v2, n2);
  rng_fill(&seed, v3, n3);
  Client m;
  CHECK(client_open_ex(&m, g_host, g_port, 65535, 1, NULL) == 0);
  Msg dm = msg_download(k, kl, 0, 0, 0, 0);
  static Call d1, d2, d3, d4;
  memset(&d1, 0, sizeof d1);
  memset(&d2, 0, sizeof d2);
  memset(&d3, 0, sizeof d3);
  memset(&d4, 0, sizeof d4);

  CHECK(op_upload(cl, k, kl, v1, n1, 1 << 20, CRC_LAST, 1, NULL) == 1);
  CHECK(call_submit(&m, &d1, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d1, 65535) == 0);
  CHECK(op_upload(cl, k, kl, v2, n2, 100000, CRC_NONE, 0, NULL) == 0);   /* replace */
  CHECK(call_submit(&m, &d2, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d2, 65535) == 0);
  CHECK(op_put(cl, k, kl, v3, n3) == 0);                                  /* replace again */
  CHECK(call_submit(&m, &d3, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(wait_closed(&m, &d3, CALL_TIMEOUT_MS) == 0);                      /* fits the window */
  CHECK(op_delete(cl, k, kl) == 1);
  CHECK(call_submit(&m, &d4, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(wait_closed(&m, &d4, CALL_TIMEOUT_MS) == 0);
  CHECK(d4.grpc_status == 5 && d4.body_len == 0);                         /* gone for new calls */

  /* let the old records' grace periods end under the stalled readers */
  char wk[64];
  for (int i = 0; i < 300; i++) {
    size_t wl = mkkey(wk, sizeof wk, "pinchurn", (uint64_t)i);
    CHECK(op_put(cl, wk, wl, "churn", 5) == 1);
    CHECK(op_delete(cl, wk, wl) == 1);
  }
  uint64_t until = now_ns() + 1300000000ull;
  while (now_ns() < until) {
    CHECK(pump(&m, 50) == 0);
    consume_conn(&m);
  }
  /* Then records of exactly v1's and v2's sizes, on the same connection —
   * the same worker, so the same malloc arena — each written in full: had
   * v1 or v2 been freed despite its pin, malloc would hand that very memory
   * back here (an exact fit) and the stalled downloads would resume on
   * these bytes. Without this, a premature free goes unnoticed: freed
   * memory that nobody reuses still holds the old value. */
  uint8_t *junk = malloc(n1);
  for (int i = 0; i < 6; i++) {
    size_t wl = mkkey(wk, sizeof wk, "pinreuse", (uint64_t)i), jn = i % 2 ? n2 : n1;
    memset(junk, 0xA5 ^ i, jn);
    CHECK(op_upload(cl, wk, wl, junk, jn, 1 << 20, CRC_NONE, 1, NULL) == 1);
  }
  for (int i = 0; i < 6; i++) {
    size_t wl = mkkey(wk, sizeof wk, "pinreuse", (uint64_t)i);
    CHECK(op_delete(cl, wk, wl) == 1);
  }
  free(junk);
  CHECK(d1.body_len == 65535 && d2.body_len == 65535 && !d1.closed && !d2.closed);

  uint64_t deadline = now_ns() + 20000000000ull;
  while ((!d1.closed || !d2.closed) && !m.eof && now_ns() < deadline) {
    consume_call(&m, &d1);
    consume_call(&m, &d2);
    CHECK(pump(&m, 20) == 0);
  }
  Dl p1 = parse_download(&d1), p2 = parse_download(&d2), p3 = parse_download(&d3);
  CHECK(call_ok("pinned v1", &d1) && dl_expect("pinned v1", &p1, v1, n1, n1, DL_DEFAULT_CHUNK));
  CHECK(call_ok("pinned v2", &d2) && dl_expect("pinned v2", &p2, v2, n2, n2, DL_DEFAULT_CHUNK));
  CHECK(call_ok("v3", &d3) && dl_expect("v3", &p3, v3, n3, n3, DL_DEFAULT_CHUNK));
  dl_free(&p1);
  dl_free(&p2);
  dl_free(&p3);
  call_free(&d1);
  call_free(&d2);
  call_free(&d3);
  call_free(&d4);
  free(dm.p);
  client_close(&m);
  expect_error("download after delete", cl, "POST", PATH_DOWNLOAD, GRPC_CT, NULL,
               msg_download(k, kl, 0, 0, 0, 0), 200, 5, "key not found");
  free(v1);
  free(v2);
  free(v3);
}

/**
 * Downloads that end early: RST_STREAM(CANCEL) while stalled; a cancel after
 * the record was replaced (its last unpin frees it); an abrupt TCP reset of
 * the connection with a stalled download of a deleted record. The server
 * must keep serving; that every pin was released is verified when run.sh
 * stops the server (the "stopped" line must say pinned=0 deferred=0).
 * @param cl  main connection
 */
static void test_download_cancel(Client *cl) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "dlcancel", 1);
  uint64_t seed = 0xDC;
  size_t n = 1048576 + 17;
  uint8_t *v = malloc(n);
  rng_fill(&seed, v, n);
  CHECK(op_upload(cl, k, kl, v, n, 1 << 20, CRC_LAST, 1, NULL) == 1);
  Msg dm = msg_download(k, kl, 0, 0, 0, 0);
  Client m;
  CHECK(client_open_ex(&m, g_host, g_port, 65535, 1, NULL) == 0);
  static Call d;
  memset(&d, 0, sizeof d);
  CHECK(call_submit(&m, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d, 65535) == 0);
  CHECK(cancel_call(&m, &d) == 0);
  call_free(&d);

  memset(&d, 0, sizeof d);
  CHECK(call_submit(&m, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d, 65535) == 0);
  rng_fill(&seed, v, n);
  CHECK(op_upload(cl, k, kl, v, n, 1 << 20, CRC_NONE, 1, NULL) == 0);  /* replaced meanwhile */
  CHECK(cancel_call(&m, &d) == 0);
  call_free(&d);

  memset(&d, 0, sizeof d);                 /* the connection still works for downloads */
  CHECK(call_submit(&m, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  uint64_t deadline = now_ns() + 20000000000ull;
  while (!d.closed && !m.eof && now_ns() < deadline) {
    consume_call(&m, &d);
    CHECK(pump(&m, 20) == 0);
  }
  Dl p = parse_download(&d);
  CHECK(call_ok("after cancels", &d) && dl_expect("after cancels", &p, v, n, n, DL_DEFAULT_CHUNK));
  dl_free(&p);
  call_free(&d);

  memset(&d, 0, sizeof d);
  CHECK(call_submit(&m, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) == 0);
  CHECK(stall_at_window(&m, &d, 65535) == 0);
  CHECK(op_delete(cl, k, kl) == 1);        /* deleted under the stalled reader */
  client_abort(&m);                        /* TCP RST: the server must unpin on teardown */
  call_free(&d);
  free(dm.p);
  free(v);
  CHECK(op_put(cl, k, kl, "next", 4) == 1);
  CHECK(op_delete(cl, k, kl) == 1);
}

/**
 * The conformance vectors of docs/PROTOCOL.md, byte for byte: the documented
 * request bodies must produce exactly the documented response bodies, so the
 * page other client implementations test against can never drift from the
 * server.
 * @param cl  connection
 * @remarks Uses the vectors' fixed key "k" (deleted first, so "created"
 *          holds on a server that saw it before).
 */
static void test_conformance_vectors(Client *cl) {
  static const struct {
    const char *what, *path, *req, *resp;
  } v[] = {
      {"upload abc (created)", PATH_UPLOAD, "000000000f0a016b10031a03616263" "25b73f4b36",
       "00000000070801" "15b73f4b36"},
      {"upload abc (replaced)", PATH_UPLOAD, "000000000f0a016b10031a03616263" "25b73f4b36",
       "0000000005" "15b73f4b36"},
      {"download, chunk 2", PATH_DOWNLOAD, "00000000050a016b2002",
       "000000000608031202" "6162" "0000000008120163" "1db73f4b36"},
      {"download, empty range", PATH_DOWNLOAD, "00000000050a016b1003",
       "000000000708031d00000000"},
  };
  op_delete(cl, "k", 1);
  for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
    uint8_t req[64], resp[64];
    size_t rl = strlen(v[i].req) / 2, pl = strlen(v[i].resp) / 2;
    for (size_t j = 0; j < rl; j++) sscanf(v[i].req + 2 * j, "%2hhx", &req[j]);
    for (size_t j = 0; j < pl; j++) sscanf(v[i].resp + 2 * j, "%2hhx", &resp[j]);
    Call c;
    int ok = call_run(cl, &c, "POST", v[i].path, GRPC_CT, NULL, req, rl) == 0 &&
             call_ok(v[i].what, &c) && c.body_len == pl && memcmp(c.body, resp, pl) == 0;
    if (!ok) {
      fprintf(stderr, "  conformance \"%s\": got", v[i].what);
      for (size_t j = 0; j < c.body_len && j < 40; j++) fprintf(stderr, " %02x", c.body[j]);
      fprintf(stderr, "\n");
    }
    CHECK(ok);
    call_free(&c);
  }
  CHECK(op_delete(cl, "k", 1) == 1);
}

/**
 * Unary Get of a value only Upload can store (above max_value) is refused
 * with RESOURCE_EXHAUSTED naming Download, which serves it; unary Put of the
 * same size stays refused (the unary limit is unchanged by Upload).
 * @param cl  connection
 */
static void test_get_too_big(Client *cl) {
  if (g_max_upload <= g_max_value) return;   /* no such value can exist */
  char k[64];
  size_t kl = mkkey(k, sizeof k, "toobig", 1);
  size_t n = g_max_value + 1;
  uint8_t *v = malloc(n + g_max_key + 64);
  uint64_t seed = 0xB16;
  rng_fill(&seed, v, n);
  CHECK(op_upload(cl, k, kl, v, n, 65536, CRC_LAST, 1, NULL) == 1);
  expect_error("Get of an uploaded big value", cl, "POST", PATH_GET, GRPC_CT, NULL,
               msg_kv(k, kl, NULL, 0), 200, 8,
               "value exceeds KVASM_MAX_VALUE_BYTES: read it with Download");
  CHECK(download_equals(cl, k, kl, v, n, 0));
  expect_error("Put above max_value", cl, "POST", PATH_PUT, GRPC_CT, NULL,
               msg_kv(k, kl, v, n), 200, 3, "value too large");
  CHECK(download_equals(cl, k, kl, v, n, 0));    /* untouched by the refused Put */
  CHECK(op_delete(cl, k, kl) == 1);
  free(v);
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
/* Multi-connection streaming: Upload replaces vs Download readers           */
/* ========================================================================= */

#define BIG_HOT_KEYS 4
#define BIG_HOT_MAX  150000
#define BIG_MAGIC    0x5452454Du        /* "MERT" */

/**
 * Length of the streamed hot value written by (writer, seq): a pure
 * function, so a reader can verify the length too.
 * @returns payload length after the 16-byte header (0..BIG_HOT_MAX)
 */
static uint32_t big_len(uint32_t writer, uint32_t seq) {
  return (writer * 2654435761u ^ seq * 40503u) % (BIG_HOT_MAX + 1);
}

/**
 * Builds a self-describing value of up to ~150 KB — several download chunks
 * and many DATA frames — so a download mixing two versions (a torn read
 * across chunks or frames) breaks the header/pattern relation.
 * @param out     destination (16 + BIG_HOT_MAX bytes)
 * @param writer  writer id   @param seq  writer's sequence number
 * @returns total value length
 */
static size_t big_value(uint8_t *out, uint32_t writer, uint32_t seq) {
  uint32_t len = big_len(writer, seq);
  uint32_t hdr[4] = {BIG_MAGIC, writer, seq, len};
  memcpy(out, hdr, 16);
  for (uint32_t i = 0; i < len; i++) out[16 + i] = (uint8_t)(writer * 31 + seq * 7 + i + (i >> 9));
  return 16 + len;
}

/**
 * Verifies a value produced by big_value.
 * @returns 1 if internally consistent, 0 if torn or foreign
 */
static int big_valid(const uint8_t *v, size_t n) {
  if (n < 16) return 0;
  uint32_t hdr[4];
  memcpy(hdr, v, 16);
  if (hdr[0] != BIG_MAGIC || hdr[3] != n - 16 || hdr[3] != big_len(hdr[1], hdr[2])) return 0;
  for (uint32_t i = 0; i < hdr[3]; i++)
    if (v[16 + i] != (uint8_t)(hdr[1] * 31 + hdr[2] * 7 + i + (i >> 9))) return 0;
  return 1;
}

/** Per-thread state and results of the streaming race. */
typedef struct {
  pthread_t thread;
  int id;
  uint64_t duration_ns;
  uint64_t uploads, downloads, privates, errors, torn, bytes;
} StreamWorker;

/**
 * Thread body: on its own connection, a random mix of hot-key replaces by
 * Upload (random chunking, crc32c placement and DATA frame sizes), hot-key
 * Downloads (random chunk sizes; each must be ONE version: self-consistent,
 * and matching the crc32c the server computed while sending it) and private
 * life cycles (Upload, a random range Download checked byte-exact, Delete).
 * @param arg  StreamWorker*
 * @returns NULL
 * @remarks Never CHECKs (the counters are read after join): CHECK's failure
 *          count is not thread-safe.
 */
static void *stream_worker_main(void *arg) {
  StreamWorker *w = arg;
  Client cl;
  if (client_open(&cl, g_host, g_port, 1u << 20) != 0) {
    w->errors++;
    return NULL;
  }
  uint64_t s = 0xD1B54A32D192ED03ull * (uint64_t)(w->id + 1);
  uint8_t *val = malloc(16 + BIG_HOT_MAX + 1), *priv = malloc(200001);
  uint32_t seq = 0;
  char k[96];
  uint64_t end = now_ns() + w->duration_ns;
  while (now_ns() < end && w->errors < 10) {
    uint64_t r = rng_next(&s);
    size_t kl;
    switch (r % 4) {
    case 0: {                                                /* hot replace by Upload */
      kl = mkkey(k, sizeof k, "bighot", (r >> 8) % BIG_HOT_KEYS);
      size_t vl = big_value(val, (uint32_t)w->id, ++seq);
      size_t chunk = 1 + (size_t)((r >> 16) % 50000);
      if (vl / chunk > 2000) chunk = vl / 2000 + 1;
      Buf b = up_stream(k, kl, val, vl, chunk, (int)((r >> 32) % 4), (int)((r >> 34) & 1));
      Call c;
      memset(&c, 0, sizeof c);
      if ((r >> 35) & 1) {
        c.frame_max = 1 + (size_t)((r >> 36) % 20000);
        c.frame_seed = r | 1;
      }
      int created;
      uint32_t crc;
      if (upload_run(&cl, &c, &b) != 0 || !call_ok("hot upload", &c) ||
          !parse_upload_reply(&c, &created, &crc) || created != 0 || crc != crc32c(0, val, vl))
        w->errors++;
      call_free(&c);
      buf_free(&b);
      w->uploads++;
      w->bytes += vl;
      break;
    }
    case 1:
    case 2: {                                                /* hot read by Download */
      kl = mkkey(k, sizeof k, "bighot", (r >> 8) % BIG_HOT_KEYS);
      uint64_t chunk = (r >> 16) % 3 == 0 ? 0 : 1 + (r >> 20) % 70000;
      Call c;
      Dl d;
      if (op_download(&cl, k, kl, 0, 0, 0, chunk, &c, &d) != 0 || !call_ok("hot download", &c) ||
          !d.ok || d.total != d.len || d.crc != crc32c(0, d.data, d.len))
        w->errors++;
      else if (!big_valid(d.data, d.len))
        w->torn++;
      w->bytes += d.len;
      dl_free(&d);
      call_free(&c);
      w->downloads++;
      break;
    }
    default: {                                               /* private life cycle */
      int n = snprintf(k, sizeof k, "%s-spriv-%d-%" PRIu64, g_nonce, w->id, w->privates);
      kl = (size_t)n;
      size_t vl = (size_t)((r >> 8) % 200001);
      rng_fill(&s, priv, vl);
      size_t chunk = 1 + (size_t)((r >> 30) % 70000);
      if (vl / chunk > 2000) chunk = vl / 2000 + 1;
      if (op_upload(&cl, k, kl, priv, vl, chunk, (int)((r >> 50) % 4), (int)((r >> 52) & 1),
                    NULL) != 1)
        w->errors++;
      uint64_t off = rng_next(&s) % (vl + 1), len = rng_next(&s) % (vl + 2);
      size_t endr = len < vl - off ? (size_t)(off + len) : vl;
      Call c;
      Dl d;
      if (op_download(&cl, k, kl, off, 1, len, 0, &c, &d) != 0 ||
          !call_ok("private download", &c) ||
          !dl_expect("private download", &d, priv + off, endr - (size_t)off, vl, DL_DEFAULT_CHUNK))
        w->errors++;
      w->bytes += vl + d.len;
      dl_free(&d);
      call_free(&c);
      if (op_delete(&cl, k, kl) != 1) w->errors++;
      w->privates++;
      break;
    }
    }
  }
  free(val);
  free(priv);
  client_close(&cl);
  return NULL;
}

/**
 * Runs THREADS connections of stream_worker_main for duration_ms: no call
 * may fail and no Download may ever mix two versions of a value.
 * @param cl           main connection (pre-populates and re-checks the hot keys)
 * @param duration_ms  run time
 */
static void test_stream_concurrency(Client *cl, int duration_ms) {
  char k[64];
  uint8_t *val = malloc(16 + BIG_HOT_MAX + 1);
  for (int h = 0; h < BIG_HOT_KEYS; h++) {
    size_t kl = mkkey(k, sizeof k, "bighot", (uint64_t)h);
    size_t vl = big_value(val, 999, (uint32_t)h);
    CHECK(op_upload(cl, k, kl, val, vl, 65536, CRC_LAST, 1, NULL) == 1);
  }
  static StreamWorker w[THREADS];
  uint64_t t0 = now_ns();
  for (int i = 0; i < THREADS; i++) {
    w[i] = (StreamWorker){.id = i, .duration_ns = (uint64_t)duration_ms * 1000000ull};
    CHECK(pthread_create(&w[i].thread, NULL, stream_worker_main, &w[i]) == 0);
  }
  uint64_t ups = 0, downs = 0, privs = 0, errors = 0, torn = 0, bytes = 0;
  for (int i = 0; i < THREADS; i++) {
    pthread_join(w[i].thread, NULL);
    ups += w[i].uploads;
    downs += w[i].downloads;
    privs += w[i].privates;
    errors += w[i].errors;
    torn += w[i].torn;
    bytes += w[i].bytes;
  }
  double secs = (double)(now_ns() - t0) / 1e9;
  printf("  streaming race: %d connections %.1fs: %" PRIu64 " hot uploads, %" PRIu64
         " hot downloads, %" PRIu64 " private cycles, %.0f MiB/s; errors %" PRIu64
         ", torn %" PRIu64 "\n",
         THREADS, secs, ups, downs, privs, (double)bytes / 1048576.0 / secs, errors, torn);
  CHECK(ups > 0 && downs > 0 && privs > 0);
  CHECK_EQ_U64(errors, 0);
  CHECK_EQ_U64(torn, 0);
  for (int h = 0; h < BIG_HOT_KEYS; h++) {
    size_t kl = mkkey(k, sizeof k, "bighot", (uint64_t)h);
    Call c;
    Dl d;
    CHECK(op_download(cl, k, kl, 0, 0, 0, 0, &c, &d) == 0 && call_ok("final hot", &c) && d.ok &&
          big_valid(d.data, d.len));
    dl_free(&d);
    call_free(&c);
    CHECK(op_delete(cl, k, kl) == 1);
  }
  free(val);
}

/* ========================================================================= */
/* Entry points                                                              */
/* ========================================================================= */

/**
 * Readiness handshake: sends a PING and pumps until its ACK arrives.
 * @param cl    an open client whose frames so far (preface, SETTINGS, any
 *              submitted HEADERS) are queued ahead of the PING
 * @param secs  give up after this many seconds
 * @returns 0 once the ACK arrived, -1 on timeout, EOF or a session error
 * @remarks The server processes one connection's frames in order, so its ACK
 *          proves every frame sent before the PING was parsed — for
 *          --drain-check, that the Health/Check stream is open and counted
 *          in the server's last stream id. run.sh waits for the "ready" line
 *          printed after this instead of sleeping and hoping the client got
 *          there first (a fixed 0.3 s sleep lost that race on a host with 30
 *          of 32 CPUs busy).
 */
static int await_ping_ack(Client *cl, int secs) {
  if (!cl->session) return -1;             /* never opened: see pump() */
  uint64_t deadline = now_ns() + (uint64_t)secs * 1000000000ull;
  /* Put everything already queued on the wire FIRST. nghttp2 serializes
   * PING from its urgent queue ahead of queued HEADERS and DATA, so a PING
   * submitted while the drain check's HEADERS still waited would be sent —
   * and ACKed — before the stream even existed. (Deferred DATA does not
   * count as wanting to write, so this terminates with the body stalled.) */
  while ((nghttp2_session_want_write(cl->session) || cl->out_off < cl->out_len) && !cl->eof) {
    if (pump(cl, 0) < 0 || now_ns() >= deadline) return -1;
  }
  int before = cl->ping_acks;
  if (nghttp2_submit_ping(cl->session, NGHTTP2_FLAG_NONE, NULL) != 0) return -1;
  while (cl->ping_acks == before && !cl->eof && now_ns() < deadline)
    if (pump(cl, 50) < 0) return -1;
  return cl->ping_acks > before ? 0 : -1;
}

/**
 * --wait-goaway: holds an idle connection until the server sends GOAWAY and
 * then closes the connection.
 * @param secs  give up after this many seconds
 * @returns process exit code: 0 on GOAWAY(NO_ERROR) followed by EOF, 1 on
 *          anything else, 2 if the connection could not be made
 * @remarks Prints "wait-goaway: ready" (flushed) once the server has
 *          answered a PING on this connection, i.e. once the connection is
 *          accepted and served — the point after which run.sh may stop the
 *          server.
 */
static int wait_goaway(int secs) {
  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  if (await_ping_ack(&cl, secs) != 0) return 2;
  printf("wait-goaway: ready\n");
  fflush(stdout);
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
 * @remarks Prints "drain-check: ready" (flushed) once a PING sent after the
 *          stream's HEADERS has been answered: from then on the server has
 *          accepted the stream, so run.sh's SIGTERM tests draining an
 *          in-flight call rather than racing the client's connect.
 */
static int drain_check(int secs) {
  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  Msg m = msg_kv("", 0, NULL, 0);
  static Call c;
  c.defer_at = 1;                        /* HEADERS + 1 body byte, then stall */
  if (call_submit(&cl, &c, "POST", PATH_HEALTH, GRPC_CT, NULL, m.p, m.len) != 0) return 2;
  if (await_ping_ack(&cl, secs) != 0) return 2;   /* the stream is accepted now */
  printf("drain-check: ready\n");
  fflush(stdout);
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

/* ========================================================================= */
/* Streaming modes: limits, drain, stalls, fairness (each on its own server) */
/* ========================================================================= */

#define MIB (1024u * 1024u)

/** Common setup of the streaming modes: run nonce and CRC table. */
static void mode_init(void) {
  snprintf(g_nonce, sizeof g_nonce, "it%lx%x", (unsigned long)time(NULL), (unsigned)getpid());
  crc_init();
  g_max_value = 1048576;                 /* the servers of these modes keep the default */
}

/**
 * Declares an upload of `size` bytes and holds its body back after the
 * first message: the server either refuses the reservation at once, or
 * holds it for as long as the call stays open.
 * @param cl    connection
 * @param c     the call; stays open while held — cancel_call releases it
 * @param s     receives the request stream; free it once the call is closed
 * @param k     key   @param kl  key length   @param size  declared total_size
 * @returns the grpc-status of an immediate refusal (8: memory or budget),
 *          0 when the reservation is held, -1 on a transport failure
 * @remarks The PING barrier (await_ping_ack) makes this deterministic: the
 *          server reserves at the end of the first message and answers a
 *          refusal before it answers the PING that follows.
 */
static int hold_upload(Client *cl, Call *c, Buf *s, const void *k, size_t kl, uint64_t size) {
  memset(s, 0, sizeof *s);
  up_first(s, k, kl, size);
  size_t first = s->n;
  up_data(s, NULL, 0);                   /* bytes past the stall point: never sent */
  memset(c, 0, sizeof *c);
  c->defer_at = first;
  if (call_submit(cl, c, "POST", PATH_UPLOAD, GRPC_CT, NULL, s->p, s->n) != 0) return -1;
  if (await_ping_ack(cl, 10) != 0) return -1;
  return c->closed ? c->grpc_status : 0;
}

/**
 * Whether the server would reserve `size` bytes for an upload right now.
 * @param cl    connection whose own budget counts
 * @param msg   substring the refusal's grpc-message must contain, or NULL
 * @returns 0 when it would (the probe is then cancelled: nothing stays
 *          reserved), the refusal's grpc-status, or -1 on a failure
 */
static int probe_reserve(Client *cl, uint64_t size, const char *msg) {
  char k[64];
  size_t kl = mkkey(k, sizeof k, "probe", size);
  Call c;
  Buf s;
  int rc = hold_upload(cl, &c, &s, k, kl, size);
  if (rc == 0 && cancel_call(cl, &c) != 0) rc = -1;
  if (rc > 0 && msg && !strstr(c.grpc_message, msg)) {
    fprintf(stderr, "  probe of %" PRIu64 " bytes: \"%s\" lacks \"%s\"\n", size, c.grpc_message,
            msg);
    rc = -1;
  }
  call_free(&c);
  buf_free(&s);
  return rc;
}

/**
 * probe_reserve on a fresh connection that is closed right after — for the
 * idle-timeout mode, where a connection left idle would itself be closed.
 */
static int probe_fresh(uint64_t size, const char *msg) {
  Client p;
  if (client_open(&p, g_host, g_port, 65535) != 0) return -1;
  int rc = probe_reserve(&p, size, msg);
  client_close(&p);
  return rc;
}

/**
 * Polls until a reservation of `size` succeeds: memory released by a closed
 * stream or connection comes back at its last unpin, or at the next
 * reclamation (at least once a second).
 * @param fresh   1: every probe on a fresh connection (probe_fresh)
 * @returns milliseconds waited, or -1 when it never succeeded within max_ms
 */
static int await_reserve(Client *cl, uint64_t size, int max_ms, int fresh) {
  uint64_t t0 = now_ns(), deadline = t0 + (uint64_t)max_ms * 1000000ull;
  do {
    int rc = fresh ? probe_fresh(size, NULL) : probe_reserve(cl, size, NULL);
    if (rc == 0) return (int)((now_ns() - t0) / 1000000);
    if (rc < 0) return -1;
    struct timespec ts = {0, 100 * 1000000};
    nanosleep(&ts, NULL);
  } while (now_ns() < deadline);
  return -1;
}

/**
 * Starts a Download on a manual-window client and stalls it at one window.
 * @returns 0 when stalled, -1 otherwise
 */
static int start_stalled_download(Client *m, Call *d, Msg *req, const void *k, size_t kl) {
  *req = msg_download(k, kl, 0, 0, 0, 0);
  memset(d, 0, sizeof *d);
  if (call_submit(m, d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, req->p, req->len) != 0) return -1;
  return stall_at_window(m, d, 65535);
}

/**
 * Lets the grace periods of retired records end while a manual-window
 * client keeps its connection (not its stalled streams) flowing: writes on
 * `cl` (reclamation runs on writes) and 1.3 s of housekeeping (once a
 * second). Afterwards a record deleted under a stalled Download is deferred
 * to its pin, not in limbo.
 */
static void let_grace_end(Client *cl, Client *m) {
  char wk[64];
  for (int i = 0; i < 200; i++) {
    size_t wl = mkkey(wk, sizeof wk, "grace", (uint64_t)i);
    CHECK(op_put(cl, wk, wl, "g", 1) == 1);
    CHECK(op_delete(cl, wk, wl) == 1);
  }
  uint64_t until = now_ns() + 1300000000ull;
  while (now_ns() < until) {
    CHECK(pump(m, 50) == 0);
    consume_conn(m);
  }
}

/**
 * --stream-limits: memory, budget and capacity limits of streaming calls,
 * and that every reservation or pin comes back however its call ends.
 * Server: KVASM_MAX_MEMORY_MB=24 KVASM_CONN_BUFFER_MB=16
 * KVASM_MAX_UPLOAD_BYTES=16777216 KVASM_CAPACITY=64 (run.sh phase 8).
 * @returns exit code: 0 all checks passed, 1 failures, 2 no connection
 * @remarks Observes the server's memory accounting from outside: a 16 MiB
 *          (or 15 MiB) reservation fits exactly when nothing else big is
 *          counted, so "the reservation fits again" proves a record or
 *          reservation was freed. Steps: (1) memory limit; (2) a stalled
 *          Download keeps its deleted record counted until it ends; (3) the
 *          per-connection budget, exactly full, per connection; (4) a
 *          published upload returns its budget; (5) a connection lost
 *          mid-upload returns its reservation; (6) a connection lost
 *          mid-download releases its pin; (7) a full store refuses a new
 *          key at END_STREAM (RESOURCE_EXHAUSTED) but not a replace.
 */
static int stream_limits(void) {
  mode_init();
  Client cl, c2;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  if (client_open(&c2, g_host, g_port, 65535) != 0) return 2;
  uint8_t *big = malloc(16 * MIB);
  uint64_t seed = 0x11A175;
  rng_fill(&seed, big, 16 * MIB);
  char ka[64], kx[64], ky[64], kz[64];
  size_t kal = mkkey(ka, sizeof ka, "lim-a", 1), kxl = mkkey(kx, sizeof kx, "lim-x", 1);
  size_t kyl = mkkey(ky, sizeof ky, "lim-y", 1), kzl = mkkey(kz, sizeof kz, "lim-z", 1);
  const char *mem = "store memory limit reached", *budget = "exceed this connection's budget";

  /* (1) 10 MiB stored: 15 MiB more exceeds 24 MiB, 13 MiB fits */
  CHECK(op_upload(&cl, ka, kal, big, 10 * MIB, MIB, CRC_LAST, 1, NULL) == 1);
  CHECK(probe_reserve(&cl, 15 * MIB, mem) == 8);
  CHECK(probe_reserve(&cl, 13 * MIB, NULL) == 0);

  /* (2) a Download stalled on a deleted record keeps it counted until it ends */
  Client m;
  CHECK(client_open_ex(&m, g_host, g_port, 65535, 1, NULL) == 0);
  static Call d;
  Msg dreq;
  CHECK(start_stalled_download(&m, &d, &dreq, ka, kal) == 0);
  CHECK(op_delete(&cl, ka, kal) == 1);
  let_grace_end(&cl, &m);
  CHECK(probe_reserve(&cl, 15 * MIB, mem) == 8);
  CHECK(wait_closed(&m, &d, CALL_TIMEOUT_MS) == 0);         /* resume: it completes */
  Dl p = parse_download(&d);
  CHECK(call_ok("pinned download", &d) && dl_expect("pinned download", &p, big, 10 * MIB,
                                                    10 * MIB, DL_DEFAULT_CHUNK));
  dl_free(&p);
  call_free(&d);
  free(dreq.p);
  int ms = await_reserve(&cl, 15 * MIB, 3000, 0);
  printf("  (2) deleted record under a stalled download: freed %d ms after it ended\n", ms);
  CHECK(ms >= 0);

  /* (3) the per-connection budget (16 MiB): 10 held + 7 refused here, fits there */
  Call x;
  Buf xs;
  CHECK(hold_upload(&cl, &x, &xs, kx, kxl, 10 * MIB) == 0);
  CHECK(probe_reserve(&cl, 7 * MIB, budget) == 8);
  CHECK(probe_reserve(&c2, 7 * MIB, NULL) == 0);
  CHECK(cancel_call(&cl, &x) == 0);                          /* released on RST */
  call_free(&x);
  buf_free(&xs);
  CHECK(probe_reserve(&cl, 16 * MIB, NULL) == 0);            /* exactly the budget */
  CHECK(hold_upload(&cl, &x, &xs, kx, kxl, 16 * MIB) == 0);
  CHECK(op_upload(&cl, kz, kzl, NULL, 0, 1, CRC_NONE, 1, NULL) == 1);   /* 16 + 0 fits */
  CHECK(probe_reserve(&cl, 1, budget) == 8);                 /* 16 + 1 does not */
  CHECK(cancel_call(&cl, &x) == 0);
  call_free(&x);
  buf_free(&xs);
  CHECK(op_delete(&cl, kz, kzl) == 1);

  /* (4) a published upload returns its budget at once */
  CHECK(op_upload(&cl, kx, kxl, big, 16 * MIB, MIB, CRC_LAST, 1, NULL) == 1);
  CHECK(op_upload(&cl, ky, kyl, U8("y"), 1, 1, CRC_NONE, 1, NULL) == 1);
  CHECK(download_equals(&cl, kx, kxl, big, 16 * MIB, 0));
  CHECK(op_delete(&cl, kx, kxl) == 1);
  CHECK(op_delete(&cl, ky, kyl) == 1);
  CHECK(await_reserve(&cl, 16 * MIB, 3000, 0) >= 0);

  /* (5) a connection lost in the middle of an upload returns its reservation */
  Client x2;
  CHECK(client_open(&x2, g_host, g_port, 65535) == 0);
  Buf s16 = up_stream(kx, kxl, big, 16 * MIB, MIB, CRC_LAST, 0);
  Call cx;
  memset(&cx, 0, sizeof cx);
  cx.defer_at = 64 + 3 * MIB;                                /* 3 MiB of data in the record */
  CHECK(submit_stalled(&x2, &cx, PATH_UPLOAD, s16.p, s16.n) == 0);
  CHECK(probe_reserve(&cl, 16 * MIB, mem) == 8);
  client_abort(&x2);
  call_free(&cx);
  buf_free(&s16);
  ms = await_reserve(&cl, 16 * MIB, 3000, 0);
  printf("  (5) upload reservation of a reset connection: back after %d ms\n", ms);
  CHECK(ms >= 0);

  /* (6) a connection lost in the middle of a download of a deleted record */
  CHECK(op_upload(&cl, ka, kal, big, 10 * MIB, MIB, CRC_FIRST, 1, NULL) == 1);
  Client y;
  CHECK(client_open_ex(&y, g_host, g_port, 65535, 1, NULL) == 0);
  CHECK(start_stalled_download(&y, &d, &dreq, ka, kal) == 0);
  CHECK(op_delete(&cl, ka, kal) == 1);
  let_grace_end(&cl, &y);
  CHECK(probe_reserve(&cl, 15 * MIB, mem) == 8);
  client_abort(&y);
  call_free(&d);
  free(dreq.p);
  ms = await_reserve(&cl, 15 * MIB, 3000, 0);
  printf("  (6) pinned record of a reset connection: back after %d ms\n", ms);
  CHECK(ms >= 0);

  /* (7) capacity: a new key is refused at END_STREAM, a replace is not */
  char fk[64];
  int filled = 0, full = 0;
  for (int i = 0; i < 200 && !full; i++) {
    size_t fl = mkkey(fk, sizeof fk, "fill", (uint64_t)i);
    Msg fm = msg_kv(fk, fl, "f", 1);
    Call c;
    CHECK(call_run(&cl, &c, "POST", PATH_PUT, GRPC_CT, NULL, fm.p, fm.len) == 0);
    if (c.grpc_status == 8 && strstr(c.grpc_message, "store is at capacity")) full = 1;
    else if (c.grpc_status == 0) filled++;
    else CHECK(0);
    call_free(&c);
    free(fm.p);
  }
  CHECK(full && filled > 0);
  Buf s = up_stream(kz, kzl, big, 1000, 300, CRC_LAST, 1);
  up_expect("upload into a full store", &cl, "POST", GRPC_CT, NULL, &s, 200, 8,
            "store is at capacity");
  size_t fl = mkkey(fk, sizeof fk, "fill", 0);
  CHECK(op_upload(&cl, fk, fl, big, 1000, 300, CRC_LAST, 1, NULL) == 0);
  CHECK(download_equals(&cl, fk, fl, big, 1000, 0));
  for (int i = 0; i < filled; i++) {
    fl = mkkey(fk, sizeof fk, "fill", (uint64_t)i);
    CHECK(op_delete(&cl, fk, fl) == 1);
  }
  CHECK(await_reserve(&cl, 16 * MIB, 3000, 0) >= 0);         /* nothing big left anywhere */
  free(big);
  client_close(&c2);
  client_close(&cl);
  return test_finish("itest_grpc --stream-limits");
}

/**
 * --drain-transfers: graceful shutdown completes streaming calls in flight.
 * An Upload with its body held back and a Download stalled on its window
 * are both open when run.sh sends SIGTERM; after the GOAWAY arrives both are
 * released and must complete exactly (the upload stored, the download
 * byte-exact), then the server closes the connection.
 * @param secs  overall time limit
 * @returns exit code: 0 on the behavior above, 1 otherwise, 2 no connection
 * @remarks Prints "drain-transfers: ready" (flushed) once both calls are
 *          provably in the server's hands (a PING round trip), like
 *          --drain-check.
 */
static int drain_transfers(int secs) {
  mode_init();
  Client cl;
  if (client_open_ex(&cl, g_host, g_port, 65535, 1, NULL) != 0) return 2;
  char kd[64], ku[64];
  size_t kdl = mkkey(kd, sizeof kd, "drain-dl", 1), kul = mkkey(ku, sizeof ku, "drain-up", 1);
  size_t n = 3 * MIB + 5, un = 2 * MIB;
  uint8_t *val = malloc(n);
  uint64_t seed = 0xD7A1;
  rng_fill(&seed, val, n);
  if (op_upload(&cl, kd, kdl, val, n, MIB, CRC_LAST, 1, NULL) != 1) return 2;
  Buf us = up_stream(ku, kul, val, un, 65536, CRC_LAST, 0);
  static Call u, d;
  memset(&u, 0, sizeof u);
  u.defer_at = 100000;                          /* first message + ~100 KB, then held */
  if (call_submit(&cl, &u, "POST", PATH_UPLOAD, GRPC_CT, NULL, us.p, us.n) != 0) return 2;
  Msg dreq;
  if (start_stalled_download(&cl, &d, &dreq, kd, kdl) != 0) return 2;  /* PING barrier too */
  if (u.closed || u.req_off != u.defer_at) return 2;
  printf("drain-transfers: ready\n");
  fflush(stdout);
  uint64_t deadline = now_ns() + (uint64_t)secs * 1000000000ull;
  while (!cl.goaway && !cl.eof && now_ns() < deadline) {
    if (pump(&cl, 50) < 0) break;
    consume_conn(&cl);
  }
  int got_goaway = cl.goaway;
  u.defer_at = 0;                               /* release the upload's body */
  nghttp2_session_resume_data(cl.session, u.stream_id);
  while ((!u.closed || !d.closed) && !cl.eof && now_ns() < deadline) {
    consume_call(&cl, &d);
    consume_call(&cl, &u);
    if (pump(&cl, 20) < 0) break;
  }
  while (!cl.eof && now_ns() < deadline)
    if (pump(&cl, 50) < 0) break;
  int created = 0;
  uint32_t crc = 0;
  int up_ok = u.closed && call_ok("drained upload", &u) && parse_upload_reply(&u, &created, &crc) &&
              created == 1 && crc == crc32c(0, val, un);
  Dl p = parse_download(&d);
  int dl_ok = d.closed && call_ok("drained download", &d) &&
              dl_expect("drained download", &p, val, n, n, DL_DEFAULT_CHUNK);
  int ok = got_goaway && cl.goaway_error == NGHTTP2_NO_ERROR &&
           cl.goaway_last_stream >= u.stream_id && cl.goaway_last_stream >= d.stream_id &&
           up_ok && dl_ok && cl.eof;
  printf("drain-transfers: goaway=%d last_stream=%d upload=%d (stream %d) download=%d "
         "(stream %d) eof=%d => %s\n",
         got_goaway, cl.goaway_last_stream, up_ok, u.stream_id, dl_ok, d.stream_id, cl.eof,
         ok ? "ok" : "FAILED");
  dl_free(&p);
  call_free(&u);
  call_free(&d);
  free(dreq.p);
  buf_free(&us);
  free(val);
  client_close(&cl);
  return ok ? 0 : 1;
}

/**
 * Uploads a value on a fresh connection (closed right after): the idle
 * mode's controller must not leave connections idle.
 * @returns op_upload's result
 */
static int upload_fresh(const void *k, size_t kl, const uint8_t *v, size_t vl) {
  Client p;
  if (client_open(&p, g_host, g_port, 65535) != 0) return -1;
  int r = op_upload(&p, k, kl, v, vl, MIB, CRC_LAST, 1, NULL);
  client_close(&p);
  return r;
}

/** Delete on a fresh connection. @returns op_delete's result */
static int delete_fresh(const void *k, size_t kl) {
  Client p;
  if (client_open(&p, g_host, g_port, 65535) != 0) return -1;
  int r = op_delete(&p, k, kl);
  client_close(&p);
  return r;
}

/** Sleeps for ms milliseconds. */
static void sleep_ms(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000};
  nanosleep(&ts, NULL);
}

/**
 * --stall: the idle timeout against transfers. Server:
 * KVASM_IDLE_TIMEOUT_SEC=1 KVASM_MAX_MEMORY_MB=24 KVASM_CONN_BUFFER_MB=16
 * KVASM_MAX_UPLOAD_BYTES=16777216 (run.sh phase 3).
 *   (a) a Download stalled by its reader's HTTP/2 window (the socket is
 *       still read) is ended by GOAWAY(NO_ERROR) after ~1 s, and its pinned
 *       (deleted) record's memory comes back;
 *   (b) a Download read slowly but steadily for > 2.5 s, during which the
 *       client sends nothing at all, is NOT cut: bytes the server manages to
 *       send count as activity;
 *   (c) a Download whose reader stops reading the SOCKET (TCP window 0, so
 *       not even a GOAWAY can leave) is destroyed by the server while the
 *       reader is still frozen — observed as its pinned record's memory
 *       coming back; when the reader wakes up it finds the connection gone,
 *       without a GOAWAY (with a WAN-like MSS: on loopback's 64 KiB MSS the
 *       kernel's send buffer is so large that a frozen reader never makes
 *       the server's writes fail, see SockOpts);
 *   (d) an Upload sent slowly (1 KiB every 150 ms for > 2 s) completes;
 *   (e) an Upload whose sender goes silent is ended after ~1 s and its
 *       reservation comes back.
 * @returns exit code: 0 all checks passed, 1 failures, 2 no connection
 */
static int stall_checks(void) {
  mode_init();
  uint8_t *big = malloc(8 * MIB);
  uint64_t seed = 0x57A11;
  rng_fill(&seed, big, 8 * MIB);
  char k[64];
  size_t kl = mkkey(k, sizeof k, "stall", 1);
  const char *mem = "store memory limit reached";

  /* (a) stalled at the HTTP/2 level */
  CHECK(upload_fresh(k, kl, big, 8 * MIB) == 1);
  Client m;
  if (client_open_ex(&m, g_host, g_port, 65535, 1, NULL) != 0) return 2;
  static Call d;
  Msg dreq;
  CHECK(start_stalled_download(&m, &d, &dreq, k, kl) == 0);
  CHECK(delete_fresh(k, kl) == 1);
  CHECK(probe_fresh(16 * MIB, mem) == 8);                  /* 8 MiB pinned + 16 > 24 */
  uint64_t t0 = now_ns(), deadline = t0 + 8000000000ull;
  while (!m.eof && now_ns() < deadline) {
    CHECK(pump(&m, 50) == 0);
    consume_conn(&m);                                        /* never the stream's window */
  }
  int ms = (int)((now_ns() - t0) / 1000000);
  printf("  (a) HTTP/2-stalled download: goaway=%d error=%u eof=%d after %d ms\n", m.goaway,
         m.goaway_error, m.eof, ms);
  CHECK(m.goaway && m.goaway_error == NGHTTP2_NO_ERROR && m.eof && !d.closed);
  CHECK(ms < 4000);
  call_free(&d);
  free(dreq.p);
  client_close(&m);
  ms = await_reserve(NULL, 16 * MIB, 4000, 1);
  printf("      its pinned record's memory came back after %d ms\n", ms);
  CHECK(ms >= 0);

  /* (b) read slowly for > 2.5 s, the client sending nothing meanwhile */
  CHECK(upload_fresh(k, kl, big, 8 * MIB) == 1);
  Client s;
  const SockOpts slow = {.rcvbuf = 16384};
  if (client_open_ex(&s, g_host, g_port, 1u << 30, 0, &slow) != 0) return 2;
  s.recv_max = 16384;
  Msg req = msg_download(k, kl, 0, 0, 0, 0);
  memset(&d, 0, sizeof d);
  CHECK(call_submit(&s, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, req.p, req.len) == 0);
  t0 = now_ns();
  deadline = t0 + 30000000000ull;
  while (!d.closed && !s.eof && now_ns() < deadline) {
    CHECK(pump(&s, 20) == 0);
    sleep_ms(5);                                             /* <= 16 KiB per 5 ms */
  }
  ms = (int)((now_ns() - t0) / 1000000);
  Dl p = parse_download(&d);
  printf("  (b) slow steady download of 8 MiB: %d ms, complete=%d\n", ms, d.closed);
  CHECK(call_ok("slow download", &d) &&
        dl_expect("slow download", &p, big, 8 * MIB, 8 * MIB, DL_DEFAULT_CHUNK));
  CHECK(ms > 2000);                                          /* the timeout was really exceeded */
  dl_free(&p);
  call_free(&d);
  free(req.p);
  client_close(&s);

  /* (c) the reader stops reading its socket. The WAN-like MSS keeps the
   * server's send buffer small, so the flush in progress overruns it and
   * the rest stays queued in the server — the idle GOAWAY lands behind it
   * and can never leave (see SockOpts.mss). */
  Client t;
  const SockOpts frozen = {.rcvbuf = 4096, .mss = 1200};
  if (client_open_ex(&t, g_host, g_port, 1u << 30, 0, &frozen) != 0) return 2;
  req = msg_download(k, kl, 0, 0, 0, 0);
  memset(&d, 0, sizeof d);
  CHECK(call_submit(&t, &d, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, req.p, req.len) == 0);
  deadline = now_ns() + 10000000000ull;
  while (d.body_len == 0 && !t.eof && now_ns() < deadline) CHECK(pump(&t, 20) == 0);
  CHECK(d.body_len > 0);                                     /* the download is running */
  /* from here on t is frozen: nobody reads its socket */
  CHECK(delete_fresh(k, kl) == 1);
  CHECK(probe_fresh(16 * MIB, mem) == 8);
  t0 = now_ns();
  ms = await_reserve(NULL, 16 * MIB, 8000, 1);
  printf("  (c) socket-stalled download: pinned record freed %d ms into the freeze\n",
         ms < 0 ? -1 : (int)((now_ns() - t0) / 1000000));
  CHECK(ms >= 0);
  deadline = now_ns() + 10000000000ull;
  while (!t.eof && now_ns() < deadline) CHECK(pump(&t, 20) == 0);
  printf("      the reader, awake again: eof=%d goaway=%d, %zu of %u bytes\n", t.eof, t.goaway,
         d.body_len, 8 * MIB);
  /* No GOAWAY: it was still queued behind unsendable output when the server
   * gave up — the connection was destroyed, not closed after the frame (a
   * server that waited for the frame would have sent it now, on waking). */
  CHECK(t.eof && !d.closed && !t.goaway);
  call_free(&d);
  free(req.p);
  client_close(&t);

  /* (d) a slow but steady upload: 14 KiB, 1 KiB every 150 ms */
  Client u;
  if (client_open(&u, g_host, g_port, 65535) != 0) return 2;
  Buf us = up_stream(k, kl, big, 14 * 1024, 1024, CRC_LAST, 0);
  memset(&d, 0, sizeof d);
  d.defer_at = 64;                                           /* ~the first message */
  CHECK(call_submit(&u, &d, "POST", PATH_UPLOAD, GRPC_CT, NULL, us.p, us.n) == 0);
  t0 = now_ns();
  deadline = t0 + 30000000000ull;
  while (!d.closed && !u.eof && now_ns() < deadline) {
    uint64_t next = now_ns() + 150000000ull;
    while (now_ns() < next && !d.closed) CHECK(pump(&u, 10) == 0);
    if (d.defer_at) {
      d.defer_at += 1024;
      if (d.defer_at >= us.n) d.defer_at = 0;                /* the rest, END_STREAM */
      nghttp2_session_resume_data(u.session, d.stream_id);
    }
  }
  ms = (int)((now_ns() - t0) / 1000000);
  int created = -1;
  uint32_t crc = 0;
  printf("  (d) slow steady upload of 14 KiB: %d ms\n", ms);
  CHECK(call_ok("slow upload", &d) && parse_upload_reply(&d, &created, &crc) && created == 1 &&
        crc == crc32c(0, big, 14 * 1024));
  CHECK(ms > 1500);
  call_free(&d);
  buf_free(&us);
  client_close(&u);
  CHECK(delete_fresh(k, kl) == 1);

  /* (e) an upload whose sender goes silent after 64 KiB of a declared 8 MiB */
  Client e;
  if (client_open(&e, g_host, g_port, 65535) != 0) return 2;
  Buf es = up_stream(k, kl, big, 8 * MIB, 65536, CRC_NONE, 0);
  memset(&d, 0, sizeof d);
  d.defer_at = 64 + 65536;
  CHECK(submit_stalled(&e, &d, PATH_UPLOAD, es.p, es.n) == 0);
  CHECK(probe_fresh(16 * MIB, mem) == 8);                    /* 8 MiB reserved + 16 > 24 */
  t0 = now_ns();
  deadline = t0 + 8000000000ull;
  while (!e.eof && now_ns() < deadline) CHECK(pump(&e, 50) == 0);
  ms = (int)((now_ns() - t0) / 1000000);
  printf("  (e) silent uploader: goaway=%d eof=%d after %d ms\n", e.goaway, e.eof, ms);
  CHECK(e.goaway && e.eof && !d.closed && ms < 4000);
  call_free(&d);
  buf_free(&es);
  client_close(&e);
  CHECK(await_reserve(NULL, 16 * MIB, 4000, 1) >= 0);
  free(big);
  return test_finish("itest_grpc --stall");
}

/** The downloader thread of --fairness: repeated whole-value Downloads. */
typedef struct {
  const char *key;
  size_t klen, len, rounds;
  const uint8_t *want;
  size_t ok;
  uint64_t bytes, ns;
  volatile int done;
} BulkReader;

/**
 * Thread body: downloads the value `rounds` times on its own connection
 * with 1 GiB windows (the server is never waiting for window), each checked
 * byte-exact.
 * @param arg  BulkReader*
 * @returns NULL
 */
static void *bulk_reader_main(void *arg) {
  BulkReader *b = arg;
  Client c;
  if (client_open(&c, g_host, g_port, 1u << 30) == 0) {
    uint64_t t0 = now_ns();
    for (size_t i = 0; i < b->rounds; i++) {
      Call call;
      Dl d;
      if (op_download(&c, b->key, b->klen, 0, 0, 0, DL_MAX_CHUNK, &call, &d) == 0 &&
          call_ok("bulk download", &call) &&
          dl_expect("bulk download", &d, b->want, b->len, b->len, DL_MAX_CHUNK))
        b->ok++;
      b->bytes += d.len;
      dl_free(&d);
      call_free(&call);
    }
    b->ns = now_ns() - t0;
    client_close(&c);
  }
  b->done = 1;
  return NULL;
}

/**
 * --fairness: with ONE worker, a connection streaming large Downloads as
 * fast as it can read must not starve another connection's unary calls:
 * the worker's flush stops at FLUSH_BUDGET and yields.
 * Server: KVASM_WORKERS=1 (run.sh phase 9).
 * @returns exit code: 0 all checks passed, 1 failures, 2 no connection
 * @remarks A liveness bound, not a benchmark: every Get must be answered
 *          within 500 ms (the host may be shared), at least 20 of them
 *          while the downloads run; the numbers are printed.
 */
static int fairness(void) {
  mode_init();
  Client cl;
  if (client_open(&cl, g_host, g_port, 1u << 20) != 0) return 2;
  char kb[64], ks[64];
  size_t kbl = mkkey(kb, sizeof kb, "fair-big", 1), ksl = mkkey(ks, sizeof ks, "fair-small", 1);
  size_t n = 32 * MIB;
  uint8_t *val = malloc(n);
  uint64_t seed = 0xFA1;
  rng_fill(&seed, val, n);
  CHECK(op_upload(&cl, kb, kbl, val, n, MIB, CRC_LAST, 1, NULL) == 1);
  CHECK(op_put(&cl, ks, ksl, "small", 5) == 1);
  BulkReader b = {.key = kb, .klen = kbl, .len = n, .rounds = 8, .want = val};
  pthread_t th;
  CHECK(pthread_create(&th, NULL, bulk_reader_main, &b) == 0);
  enum { MAXG = 100000 };
  static uint64_t lat[MAXG];
  size_t gets = 0, bad = 0;
  while (!b.done && gets < MAXG) {
    uint64_t t0 = now_ns();
    uint8_t *v;
    size_t vl;
    if (op_get(&cl, ks, ksl, &v, &vl) != 1 || vl != 5) bad++;
    free(v);
    lat[gets++] = now_ns() - t0;
  }
  pthread_join(th, NULL);
  uint64_t max = 0, sum = 0;
  for (size_t i = 0; i < gets; i++) {
    sum += lat[i];
    if (lat[i] > max) max = lat[i];
  }
  printf("  fairness (1 worker): %zu of %zu downloads of 32 MiB exact, %.0f MiB/s; meanwhile %zu "
         "Gets, mean %.2f ms, max %.2f ms\n",
         b.ok, b.rounds, (double)b.bytes / 1048576.0 / ((double)b.ns / 1e9 + 1e-9), gets,
         gets ? (double)sum / (double)gets / 1e6 : 0.0, (double)max / 1e6);
  CHECK(b.ok == b.rounds);
  CHECK_EQ_U64(bad, 0);
  CHECK(gets >= 20);
  CHECK(max < 500000000ull);
  CHECK(op_delete(&cl, kb, kbl) == 1);
  CHECK(op_delete(&cl, ks, ksl) == 1);
  free(val);
  client_close(&cl);
  return test_finish("itest_grpc --fairness");
}

/**
 * The server's resident memory in KiB (0 when unavailable).
 * @param pid  server pid
 * @remarks /proc/PID/smaps_rollup walks the page tables; /proc/PID/status
 *          VmRSS lags on kernels with split RSS counting (see fuzz_h2.c).
 */
static long server_rss_kib(long pid) {
  char path[64], line[256];
  snprintf(path, sizeof path, "/proc/%ld/smaps_rollup", pid);
  FILE *f = fopen(path, "r");
  if (!f) return 0;
  long kib = 0;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "Rss: %ld", &kib) == 1) break;
  fclose(f);
  return kib;
}

/** Sorts doubles ascending (qsort). */
static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

/**
 * --bench-transfer: throughput of Upload/Download against unary Put/Get on
 * one connection, and the server's memory while values move. The client
 * does the least it can (streams built before the clock starts, responses
 * only length-checked, no client-side CRC), so the numbers bound the server
 * from below; the nghttp2 client itself is usually the limit on loopback.
 * @param pid  the server's pid (its RSS is sampled from /proc)
 * @returns exit code: 0 (numbers printed), 1 on a failed transfer, 2 no connection
 * @remarks Medians of 7. Needs KVASM_MAX_UPLOAD_BYTES >= 64 MiB (the
 *          default) and nothing else loading the host.
 */
static int bench_transfer(long pid) {
  mode_init();
  Client cl;
  if (client_open(&cl, g_host, g_port, 1u << 30) != 0) return 2;
  enum { RUNS = 7 };
  static const size_t sizes[] = {64 * 1024, 1 * MIB, 16 * MIB, 64 * MIB};
  uint8_t *val = malloc(64 * MIB);
  uint64_t seed = 0xBE7C;
  rng_fill(&seed, val, 64 * MIB);
  char k[64];
  size_t kl = mkkey(k, sizeof k, "bench", 1);
  int bad = 0;
  printf("  size       unary Put  unary Get   Upload   Download  (MB/s, 1 connection, median of %d)\n", RUNS);
  for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
    size_t n = sizes[si];
    double up[RUNS], down[RUNS], put[RUNS], get[RUNS];
    Buf s = up_stream(k, kl, val, n, n < MIB ? n : MIB, CRC_LAST, 1);
    Msg dm = msg_download(k, kl, 0, 0, 0, DL_MAX_CHUNK);
    for (int r = 0; r < RUNS; r++) {
      Call c;
      memset(&c, 0, sizeof c);
      uint64_t t0 = now_ns();
      if (upload_run(&cl, &c, &s) != 0 || !call_ok("bench upload", &c)) bad++;
      up[r] = (double)n / ((double)(now_ns() - t0) / 1e9) / 1e6;
      call_free(&c);
      t0 = now_ns();
      if (call_run(&cl, &c, "POST", PATH_DOWNLOAD, GRPC_CT, NULL, dm.p, dm.len) != 0 ||
          !call_ok("bench download", &c) || c.body_len < n)
        bad++;
      down[r] = (double)n / ((double)(now_ns() - t0) / 1e9) / 1e6;
      call_free(&c);
      if (n <= g_max_value) {
        Msg pm = msg_kv(k, kl, val, n), gm = msg_kv(k, kl, NULL, 0);
        t0 = now_ns();
        if (call_run(&cl, &c, "POST", PATH_PUT, GRPC_CT, NULL, pm.p, pm.len) != 0 || !call_ok("bench put", &c)) bad++;
        put[r] = (double)n / ((double)(now_ns() - t0) / 1e9) / 1e6;
        call_free(&c);
        t0 = now_ns();
        if (call_run(&cl, &c, "POST", PATH_GET, GRPC_CT, NULL, gm.p, gm.len) != 0 || !call_ok("bench get", &c) ||
            c.body_len < n)
          bad++;
        get[r] = (double)n / ((double)(now_ns() - t0) / 1e9) / 1e6;
        call_free(&c);
        free(pm.p);
        free(gm.p);
      }
    }
    qsort(up, RUNS, sizeof up[0], cmp_double);
    qsort(down, RUNS, sizeof down[0], cmp_double);
    qsort(put, RUNS, sizeof put[0], cmp_double);
    qsort(get, RUNS, sizeof get[0], cmp_double);
    if (n <= g_max_value)
      printf("  %6zu KiB  %9.0f  %9.0f  %8.0f  %8.0f\n", n / 1024, put[RUNS / 2], get[RUNS / 2],
             up[RUNS / 2], down[RUNS / 2]);
    else
      printf("  %6zu KiB  %9s  %9s  %8.0f  %8.0f\n", n / 1024, "-", "-", up[RUNS / 2], down[RUNS / 2]);
    buf_free(&s);
    free(dm.p);
  }

  /* Memory: a stored 64 MiB value costs ~64 MiB (the upload's record is its
   * only buffer); a download of it costs nothing more (generated frames). */
  CHECK(op_delete(&cl, k, kl) == 1);
  sleep_ms(1500);                                  /* reclamation + housekeeping */
  long before = server_rss_kib(pid);
  CHECK(op_upload(&cl, k, kl, val, 64 * MIB, MIB, CRC_LAST, 1, NULL) == 1);
  long stored = server_rss_kib(pid);
  Client m;
  if (client_open_ex(&m, g_host, g_port, 65535, 1, NULL) != 0) return 2;
  static Call d;
  Msg dreq;
  CHECK(start_stalled_download(&m, &d, &dreq, k, kl) == 0);
  long during = server_rss_kib(pid);
  CHECK(wait_closed(&m, &d, CALL_TIMEOUT_MS) == 0 && d.body_len > 64 * MIB);
  long after = server_rss_kib(pid);
  printf("  server RSS: %ld KiB idle, +%ld KiB with a 64 MiB value stored, +%ld KiB more during a "
         "(stalled) download of it, +%ld KiB after it\n",
         before, stored - before, during - stored, after - stored);
  call_free(&d);
  free(dreq.p);
  client_close(&m);
  CHECK(op_delete(&cl, k, kl) == 1);
  free(val);
  client_close(&cl);
  if (bad) fprintf(stderr, "  %d transfers failed\n", bad);
  return bad ? 1 : test_finish("itest_grpc --bench-transfer");
}

/**
 * Entry point.
 * @param argc  argument count
 * @param argv  see the file header for the usages
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
  if (argc == 5 && strcmp(argv[1], "--drain-transfers") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return drain_transfers(atoi(argv[4]));
  }
  if (argc == 4 && strcmp(argv[1], "--stream-limits") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return stream_limits();
  }
  if (argc == 4 && strcmp(argv[1], "--stall") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return stall_checks();
  }
  if (argc == 4 && strcmp(argv[1], "--fairness") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return fairness();
  }
  if (argc == 5 && strcmp(argv[1], "--bench-transfer") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return bench_transfer(atol(argv[4]));
  }
  /* --streaming: only the Upload/Download tests of the main suite, for the
   * phase that repeats them under each CRC-32C path (run.sh phase 2c). */
  int streaming_only = 0;
  if (argc >= 2 && strcmp(argv[1], "--streaming") == 0) {
    streaming_only = 1;
    argc--;
    argv++;
  }
  if (argc != 5 && argc != 6) {
    fprintf(stderr, "usage: %s [--streaming] HOST PORT MAX_KEY MAX_VALUE [MAX_UPLOAD]\n"
                    "       %s --wait-goaway HOST PORT SECS\n"
                    "       %s --expect-snapshot HOST PORT COUNT\n"
                    "       %s --drain-check HOST PORT SECS\n"
                    "       %s --drain-transfers HOST PORT SECS\n"
                    "       %s --stream-limits HOST PORT\n"
                    "       %s --stall HOST PORT\n"
                    "       %s --fairness HOST PORT\n"
                    "       %s --bench-transfer HOST PORT PID\n",
            argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }
  g_host = argv[1];
  g_port = argv[2];
  g_max_key = strtoull(argv[3], NULL, 10);
  g_max_value = strtoull(argv[4], NULL, 10);
  /* KVASM_MAX_UPLOAD_BYTES; the server's default when not given */
  g_max_upload = argc == 6 ? strtoull(argv[5], NULL, 10) : 67108864;
  if (g_max_key < 32 || g_max_value < 4096 || g_max_upload < 3145735 ||
      g_max_upload > g_conn_budget) {
    fprintf(stderr, "MAX_KEY must be >= 32, MAX_VALUE >= 4096 and MAX_UPLOAD in "
                    "[3145735, 64 MiB] for these cases\n");
    return 2;
  }
  snprintf(g_nonce, sizeof g_nonce, "it%lx%x", (unsigned long)time(NULL),
           (unsigned)getpid());
  crc_init();

  Client cl;
  if (client_open(&cl, g_host, g_port, 65535) != 0) return 2;
  uint64_t t0 = now_ns();
  /* The main connection must never be dropped by the server: checked after
   * every test, so a GOAWAY or a close is pinned to the test that caused it
   * and reported with its error code and last stream id. */
#define RUN_ON_MAIN(call)                                                            \
  do {                                                                               \
    call;                                                                            \
    if (cl.goaway || cl.eof) {                                                       \
      fprintf(stderr, "  main connection lost during %s: goaway=%d error=%u "        \
                      "last_stream=%d eof=%d\n",                                     \
              #call, cl.goaway, cl.goaway_error, cl.goaway_last_stream, cl.eof);     \
      CHECK(!cl.goaway && !cl.eof);                                                  \
      client_close(&cl);                                                             \
      return test_finish("itest_grpc");                                              \
    }                                                                                \
  } while (0)
  if (!streaming_only) {
    RUN_ON_MAIN(test_crud(&cl));
    RUN_ON_MAIN(test_large_value(&cl));
    RUN_ON_MAIN(test_get_bursts(&cl));
    RUN_ON_MAIN(test_errors(&cl));
    RUN_ON_MAIN(test_health(&cl));
    RUN_ON_MAIN(test_pipelined(&cl));
    RUN_ON_MAIN(test_batch_order(&cl));
    RUN_ON_MAIN(test_cancel(&cl));
    RUN_ON_MAIN(test_reset_in_batch(&cl));
  }
  RUN_ON_MAIN(test_upload_errors(&cl));
  RUN_ON_MAIN(test_upload_forms(&cl));
  RUN_ON_MAIN(test_upload_basic(&cl));
  RUN_ON_MAIN(test_upload_splits(&cl));
  RUN_ON_MAIN(test_upload_early(&cl));
  RUN_ON_MAIN(test_upload_cancel(&cl));
  RUN_ON_MAIN(test_download_errors(&cl));
  RUN_ON_MAIN(test_download_ranges(&cl));
  RUN_ON_MAIN(test_download_flow(&cl));
  RUN_ON_MAIN(test_download_consistency(&cl));
  RUN_ON_MAIN(test_download_cancel(&cl));
  RUN_ON_MAIN(test_conformance_vectors(&cl));
  if (!streaming_only) {
    RUN_ON_MAIN(test_get_too_big(&cl));
    RUN_ON_MAIN(test_concurrency(&cl, 3000));
  }
  RUN_ON_MAIN(test_stream_concurrency(&cl, 2000));
#undef RUN_ON_MAIN
  client_close(&cl);
  printf("  %.2fs total\n", (double)(now_ns() - t0) / 1e9);
  return test_finish("itest_grpc");
}
