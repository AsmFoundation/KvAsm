/*
 * fuzz_h2.c — black-box fuzzing of a running kvasm's HTTP/2 input path
 * (h2n.asm and hpack.asm, KvAsm's own engine: ADR 0007/0008).
 *
 * Usage: fuzz_h2 HOST PORT ITERATIONS SEED PID
 *        fuzz_h2 --limits HOST PORT      (see limits_matrix; needs a server
 *                                         started with small limits)
 *   ITERATIONS  fuzz connections to run
 *   SEED        PRNG seed (a failure reproduces with the same seed)
 *   PID         the server's pid: its resident memory is sampled from
 *               /proc/PID/smaps_rollup to catch leaks (0 skips the check)
 *
 * Each iteration opens a connection and sends one of:
 *   0  garbage after the client preface
 *   1  a valid gRPC Get (preface, SETTINGS, HEADERS, DATA) with a few bytes
 *      flipped
 *   2  a random frame sequence: random types (incl. unknown ones), flags,
 *      stream ids from a small set, lengths and payloads, mixed with valid
 *      HEADERS blocks
 *   3  the valid request cut at a random byte, then an abrupt close
 *   4  HEADERS frames carrying random HPACK blocks (random indexes, size
 *      updates, Huffman bits, truncated integers and strings)
 *   5  valid requests pipelined, each byte stream split into random-sized
 *      writes (exercises the carry of partial frames and preface)
 *   6  Upload soup: up to three client-streaming Uploads interleaved, each
 *      a valid message stream (up to 64 KiB, crc32c right, wrong, absent or
 *      twice) that is sometimes mutated, truncated or extended with garbage,
 *      cut into random DATA frames (some padded, some empty), sometimes
 *      reset mid-way or left without END_STREAM
 *   7  Download soup: Downloads of pre-uploaded values (0 B .. 300 KB) and
 *      of random keys with random ranges and chunk sizes, under a random
 *      initial window, then random WINDOW_UPDATE / RST_STREAM / SETTINGS /
 *      PING frames — and the connection dropped while responses are still
 *      being generated from pinned records
 * then reads whatever comes back for a short while and closes.
 *
 * Before the iterations, the request matrix (request_matrix): deterministic
 * cases at the frame boundaries of a unary request, each with an exact
 * expected answer — the same three Gets cut into two writes at every byte,
 * equal-length header blocks that differ on one connection, a padded DATA
 * frame, a body in two DATA frames, an empty END_STREAM frame,
 * content-length right and wrong, a padded HEADERS frame with priority
 * fields, DATA after a HEADERS frame that already ended the stream, DATA for
 * another stream, an oversized DATA frame, an Upload in one DATA frame, and
 * 36 MB of full-size DATA frames that must be counted against the connection
 * window. It exists for h2n.asm's
 * fused request path (a DATA frame handled right behind its HEADERS), whose
 * conditions these cases sit on either side of.
 *
 * Checks (any failure exits 1 with the seed and iteration):
 *   * every 50 iterations, and at the end, a fresh connection gets its PING
 *     acknowledged and a real Get answered with grpc-status 0 — the server
 *     is alive, its event loop runs, requests still work;
 *   * the server's resident memory grows by less than 64 MiB between the
 *     first liveness check and the end (per-connection state must be
 *     released);
 *   * run.sh then stops the server, which must exit 0 (a corrupted heap
 *     rarely survives shutdown), and runs the full gRPC suite first.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include "test_common.h"

/* hpack.asm (differentially verified against nghttp2 by test_hpack): decodes
 * the server's response header blocks in the liveness check, whichever HPACK
 * representations the engine chose (literal, indexed, Huffman). */
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
extern const uint64_t kv_pool_struct_size;

static const char *g_host, *g_port;
static uint64_t g_seed;
static long g_iter;

static const uint8_t kPreface[24] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/** Dies with the reproduction coordinates. */
static void fail(const char *what) {
  fprintf(stderr, "fuzz_h2: FAIL at iteration %ld (seed %llu): %s\n", g_iter,
          (unsigned long long)g_seed, what);
  exit(1);
}

/** @return a connected, blocking TCP socket with TCP_NODELAY, or -1. */
static int dial(void) {
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM}, *res;
  if (getaddrinfo(g_host, g_port, &hints, &res) != 0) return -1;
  int fd = -1;
  for (struct addrinfo *a = res; a; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd >= 0) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  }
  return fd;
}

/** Sends everything; a peer that closed early is fine (fuzzing). @return 0/-1. */
static int send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

/** Growable byte buffer for building frame sequences. */
typedef struct {
  uint8_t *p;
  size_t n, cap;
} Buf;

/** Appends bytes. */
static void put(Buf *b, const void *src, size_t n) {
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 256;
    b->p = realloc(b->p, b->cap);
    if (!b->p) fail("out of memory");
  }
  memcpy(b->p + b->n, src, n);
  b->n += n;
}

/** Appends a frame header. */
static void frame_hdr(Buf *b, uint32_t len, uint8_t type, uint8_t flags, uint32_t sid) {
  uint8_t h[9] = {(uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type, flags,
                  (uint8_t)(sid >> 24), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid};
  put(b, h, 9);
}

/** Appends a complete frame. */
static void frame(Buf *b, uint8_t type, uint8_t flags, uint32_t sid, const void *pl, uint32_t len) {
  frame_hdr(b, len, type, flags, sid);
  put(b, pl, len);
}

/** Appends a raw HPACK string literal (length < 127). */
static void hstr(Buf *b, const char *s) {
  uint8_t n = (uint8_t)strlen(s);
  put(b, &n, 1);
  put(b, s, n);
}

/**
 * The header block of a gRPC Get: literals without indexing only, so
 * mutating one block never desynchronizes a later one.
 */
static void get_block(Buf *b) {
  static const struct { const char *n, *v; } f[] = {
      {":method", "POST"}, {":scheme", "http"}, {":path", "/kvasm.v1.KvAsm/Get"},
      {":authority", "fuzz"}, {"content-type", "application/grpc"}, {"te", "trailers"}};
  for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
    uint8_t z = 0;
    put(b, &z, 1);
    hstr(b, f[i].n);
    hstr(b, f[i].v);
  }
}

/** Appends HEADERS + DATA of one Get for key "fuzz-key" on stream sid. */
static void get_request(Buf *b, uint32_t sid) {
  Buf blk = {0};
  get_block(&blk);
  frame(b, 1, 0x4, sid, blk.p, (uint32_t)blk.n);          /* HEADERS, END_HEADERS */
  static const uint8_t msg[] = {0, 0, 0, 0, 10, 0x0A, 8, 'f', 'u', 'z', 'z', '-', 'k', 'e', 'y'};
  frame(b, 0, 0x1, sid, msg, sizeof msg);                 /* DATA, END_STREAM */
  free(blk.p);
}

/** Appends the client preface and an empty SETTINGS frame. */
static void preface(Buf *b) {
  put(b, kPreface, sizeof kPreface);
  frame_hdr(b, 0, 4, 0, 0);
}

/* Defined below, used by the streaming builders that follow. */
static void drain(int fd, int ms);
static void random_bytes(Buf *b, uint64_t *s, size_t n);

/**
 * A gRPC call's header block for any path: literals without indexing only,
 * like get_block, so blocks stay independent of each other.
 * @param b     destination
 * @param path  :path (e.g. "/kvasm.v1.KvAsm/Upload")
 */
static void call_block(Buf *b, const char *path) {
  const struct { const char *n, *v; } f[] = {
      {":method", "POST"}, {":scheme", "http"}, {":path", path},
      {":authority", "fuzz"}, {"content-type", "application/grpc"}, {"te", "trailers"}};
  for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
    uint8_t z = 0;
    put(b, &z, 1);
    hstr(b, f[i].n);
    hstr(b, f[i].v);
  }
}

/** Appends a protobuf varint. */
static void vput(Buf *b, uint64_t v) {
  uint8_t t[10];
  size_t n = 0;
  while (v >= 0x80) {
    t[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  t[n++] = (uint8_t)v;
  put(b, t, n);
}

/**
 * CRC-32C (zlib-style), a plain byte table — this file's own, so the fuzzer
 * shares no code with the server's crc32c.asm beyond the wire format.
 */
static uint32_t crc32c(uint32_t crc, const uint8_t *p, size_t n) {
  static uint32_t tab[256];
  if (!tab[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      tab[i] = c;
    }
  crc = ~crc;
  while (n--) crc = tab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

/** Frames one message body as a gRPC message (prefix flag, 32-bit length). */
static void grpc_msg(Buf *b, uint8_t flag, const Buf *body) {
  uint8_t pfx[5] = {flag, (uint8_t)(body->n >> 24), (uint8_t)(body->n >> 16),
                    (uint8_t)(body->n >> 8), (uint8_t)body->n};
  put(b, pfx, 5);
  if (body->n) put(b, body->p, body->n);
}

/**
 * A valid UploadRequest stream for `key`: the first message with key and
 * total_size (and sometimes the first chunk), then random chunks; the
 * crc32c right (most often), wrong, absent or sent twice.
 * @param b    destination (the message stream, not yet in frames)
 * @param s    PRNG state
 * @param key  key (NUL-terminated)
 * @param max  largest total_size to declare
 */
static void upload_stream(Buf *b, uint64_t *s, const char *key, size_t max) {
  size_t size = rng_next(s) % 3 == 0 ? rng_next(s) % 64 : rng_next(s) % (max + 1);
  Buf val = {0}, m = {0};
  random_bytes(&val, s, size);
  uint32_t crc = crc32c(0, val.p, size);
  int crc_mode = (int)(rng_next(s) % 8);        /* 0-4 right, 5 wrong, 6 absent, 7 twice */
  uint8_t crcf[5] = {0x25, (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16),
                     (uint8_t)(crc >> 24)};
  if (crc_mode == 5) crcf[1] ^= 0x5A;
  put(&m, "\x0A", 1);
  vput(&m, strlen(key));
  put(&m, key, strlen(key));
  put(&m, "\x10", 1);
  vput(&m, size);
  size_t off = 0;
  if (size && rng_next(s) % 2) {                /* the first chunk in the first message */
    size_t n = 1 + rng_next(s) % size;
    put(&m, "\x1A", 1);
    vput(&m, n);
    put(&m, val.p, n);
    off = n;
  }
  if (crc_mode == 7) put(&m, crcf, 5);
  grpc_msg(b, 0, &m);
  while (off < size) {
    size_t n = 1 + rng_next(s) % (size - off);
    m.n = 0;
    put(&m, "\x1A", 1);
    vput(&m, n);
    put(&m, val.p + off, n);
    off += n;
    if (off == size && crc_mode != 6) put(&m, crcf, 5);
    grpc_msg(b, 0, &m);
  }
  if (size == 0 && crc_mode != 6) {             /* an empty value: crc alone */
    m.n = 0;
    put(&m, crcf, 5);
    grpc_msg(b, 0, &m);
  }
  free(val.p);
  free(m.p);
}

/** Keys the Download soup reads, uploaded once at startup (fuzz_setup). */
static const size_t kDlSizes[4] = {0, 1000, 70000, 300000};

/**
 * Appends one DownloadRequest message: a pre-uploaded key (or random bytes),
 * a random offset (sometimes past the end), a length or none, a chunk size
 * from 0 to far beyond the maximum.
 */
static void download_msg(Buf *b, uint64_t *s) {
  Buf m = {0};
  char key[32];
  int k = (int)(rng_next(s) % 5);
  if (k < 4) {
    snprintf(key, sizeof key, "fuzz-dl-%d", k);
    put(&m, "\x0A", 1);
    vput(&m, strlen(key));
    put(&m, key, strlen(key));
  } else {
    size_t n = rng_next(s) % 40;
    put(&m, "\x0A", 1);
    vput(&m, n);
    random_bytes(&m, s, n);
  }
  if (rng_next(s) % 2) {
    put(&m, "\x10", 1);
    vput(&m, rng_next(s) % 3 == 0 ? rng_next(s) : rng_next(s) % 310000);
  }
  if (rng_next(s) % 2) {
    put(&m, "\x18", 1);
    vput(&m, rng_next(s) % 4 == 0 ? rng_next(s) : rng_next(s) % 310000);
  }
  if (rng_next(s) % 2) {
    put(&m, "\x20", 1);
    vput(&m, rng_next(s) % 4 == 0 ? rng_next(s) : rng_next(s) % 100000);
  }
  grpc_msg(b, 0, &m);
  free(m.p);
}

/**
 * Uploads the values the Download soup reads (keys fuzz-dl-0..3), over one
 * raw connection, and gives the server a moment to answer.
 * @remarks Fails the run if the connection cannot be made; the answers are
 *          not parsed (a failed upload only makes those downloads NOT_FOUND,
 *          still valid fuzzing), the liveness checks cover the server.
 */
static void fuzz_setup(void) {
  uint64_t s = 0x5E7;
  Buf b = {0};
  preface(&b);
  for (int i = 0; i < 4; i++) {
    Buf blk = {0}, body = {0}, m = {0}, val = {0};
    call_block(&blk, "/kvasm.v1.KvAsm/Upload");
    frame(&b, 1, 0x4, 1 + 2 * (uint32_t)i, blk.p, (uint32_t)blk.n);
    char key[32];
    snprintf(key, sizeof key, "fuzz-dl-%d", i);
    random_bytes(&val, &s, kDlSizes[i]);
    put(&m, "\x0A", 1);
    vput(&m, strlen(key));
    put(&m, key, strlen(key));
    put(&m, "\x10", 1);
    vput(&m, kDlSizes[i]);
    if (kDlSizes[i]) {
      put(&m, "\x1A", 1);
      vput(&m, kDlSizes[i]);
      put(&m, val.p, kDlSizes[i]);
    }
    grpc_msg(&body, 0, &m);
    for (size_t off = 0; off < body.n; off += 16384) {       /* max frame size */
      size_t n = body.n - off < 16384 ? body.n - off : 16384;
      frame(&b, 0, off + n == body.n ? 0x1 : 0, 1 + 2 * (uint32_t)i, body.p + off, (uint32_t)n);
    }
    free(blk.p);
    free(body.p);
    free(m.p);
    free(val.p);
  }
  int fd = dial();
  if (fd < 0) fail("cannot connect for the setup uploads");
  send_all(fd, b.p, b.n);
  drain(fd, 300);
  close(fd);
  free(b.p);
}

/** Reads and discards input for up to ms milliseconds or until EOF. */
static void drain(int fd, int ms) {
  uint8_t buf[16384];
  uint64_t end = now_ns() + (uint64_t)ms * 1000000ull;
  for (;;) {
    int64_t left = (int64_t)(end - now_ns()) / 1000000;
    if (left <= 0) return;
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, (int)left) <= 0) return;
    ssize_t k = recv(fd, buf, sizeof buf, 0);
    if (k <= 0) return;
  }
}

/** grpc-status seen in one decoded header block (-1: absent). */
static int on_field(void *ctx, const uint8_t *name, size_t nlen, const uint8_t *value, size_t vlen,
                    uint64_t meaning) {
  (void)meaning;
  if (nlen == 11 && memcmp(name, "grpc-status", 11) == 0)
    *(int *)ctx = (vlen == 1 && value[0] == '0') ? 0 : 1;
  return 0;
}

/**
 * Liveness: PING acknowledged and a Get answered with grpc-status 0 on a
 * fresh connection. Frames are parsed just enough: the PING ACK (type 6,
 * flag 1, same opaque data), and every HEADERS block decoded in order (the
 * response's dynamic-table insertions matter for its trailers) until
 * stream 1's END_STREAM block carries grpc-status.
 */
static void check_alive(void) {
  static void *pool;
  static uint8_t scratch_buf[16384];
  const uint64_t scratch[2] = {(uint64_t)(uintptr_t)scratch_buf, sizeof scratch_buf};
  if (!pool) {
    kv_hpack_global_init();
    pool = calloc(1, kv_pool_struct_size);
  }
  HpackDec dec;
  if (kv_hpack_init(&dec, pool, 4096) != 0) fail("liveness: no memory for HPACK");
  int fd = dial();
  if (fd < 0) fail("cannot connect for the liveness check");
  Buf b = {0};
  preface(&b);
  static const uint8_t opaque[8] = "livechek";
  frame(&b, 6, 0, 0, opaque, 8);
  get_request(&b, 1);
  if (send_all(fd, b.p, b.n) != 0) fail("liveness: send failed");
  free(b.p);
  static uint8_t in[1 << 17];
  size_t n = 0;
  int ping_ack = 0, status = -1;
  uint64_t deadline = now_ns() + 3000000000ull;
  while ((!ping_ack || status < 0) && now_ns() < deadline) {
    struct pollfd p = {fd, POLLIN, 0};
    if (poll(&p, 1, 100) <= 0) continue;
    ssize_t k = recv(fd, in + n, sizeof in - n, 0);
    if (k <= 0) break;
    n += (size_t)k;
    size_t off = 0;
    while (n - off >= 9) {
      uint32_t len = (uint32_t)in[off] << 16 | (uint32_t)in[off + 1] << 8 | in[off + 2];
      if (n - off < 9 + len) break;
      uint8_t type = in[off + 3], flags = in[off + 4];
      uint32_t sid = ((uint32_t)in[off + 5] << 24 | (uint32_t)in[off + 6] << 16 |
                      (uint32_t)in[off + 7] << 8 | in[off + 8]) & 0x7FFFFFFF;
      if (type == 6 && (flags & 1) && len == 8 && memcmp(in + off + 9, opaque, 8) == 0) ping_ack = 1;
      if (type == 1) {
        int st = -1;
        if (kv_hpack_decode(&dec, in + off + 9, len, &st, on_field, scratch) != 0)
          fail("liveness: undecodable response header block");
        if (sid == 1 && (flags & 1)) {
          if (st != 0) fail("liveness: Get not answered with grpc-status 0");
          status = st;
        }
      }
      off += 9 + len;
    }
    memmove(in, in + off, n - off);
    n -= off;
  }
  close(fd);
  kv_hpack_free(&dec);
  if (!ping_ack) fail("liveness: PING not acknowledged");
  if (status != 0) fail("liveness: Get not answered");
}

/**
 * @return the server's resident memory in KiB, or 0 when unavailable.
 * @remarks Reads /proc/PID/smaps_rollup ("Rss:"), which walks the page
 *          tables. /proc/PID/status VmRSS is useless here: kernels with
 *          split RSS counting (5.15, the WSL2 kernel) fold each thread's
 *          counter in lazily, and a worker-thread server showed a constant
 *          4 kB against a real 980 kB.
 */
static long rss_kib(long pid) {
  if (pid <= 0) return 0;
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

/** Appends n random bytes. */
static void random_bytes(Buf *b, uint64_t *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = (uint8_t)rng_next(s);
    put(b, &c, 1);
  }
}

/** Builds one fuzz case into b. */
static void build_case(Buf *b, uint64_t *s, int kind) {
  static const uint32_t sids[] = {0, 1, 2, 3, 5, 7, 0x7FFFFFFF, 0x80000001u};
  switch (kind) {
  case 0: /* garbage after the preface */
    put(b, kPreface, sizeof kPreface);
    random_bytes(b, s, 1 + rng_next(s) % 600);
    break;
  case 1: { /* a valid request with flipped bytes */
    preface(b);
    size_t start = b->n;
    get_request(b, 1);
    get_request(b, 3);
    int flips = 1 + (int)(rng_next(s) % 4);
    for (int i = 0; i < flips; i++) b->p[start + rng_next(s) % (b->n - start)] ^= (uint8_t)(1 + rng_next(s) % 255);
    break;
  }
  case 2: { /* random frame soup */
    preface(b);
    int frames = 1 + (int)(rng_next(s) % 24);
    for (int i = 0; i < frames; i++) {
      if (rng_next(s) % 5 == 0) {
        get_request(b, 1 + 2 * (uint32_t)(rng_next(s) % 8));
        continue;
      }
      uint32_t len = (uint32_t)(rng_next(s) % 48);
      uint8_t type = (uint8_t)(rng_next(s) % 12), flags = (uint8_t)rng_next(s);
      frame_hdr(b, len, type, flags, sids[rng_next(s) % 8]);
      random_bytes(b, s, len);
    }
    break;
  }
  case 3: { /* truncated anywhere, then an abrupt close */
    preface(b);
    get_request(b, 1);
    b->n = 1 + rng_next(s) % b->n;
    break;
  }
  case 4: { /* random HPACK blocks */
    preface(b);
    int blocks = 1 + (int)(rng_next(s) % 6);
    for (int i = 0; i < blocks; i++) {
      Buf blk = {0};
      if (rng_next(s) % 2) get_block(&blk);
      random_bytes(&blk, s, rng_next(s) % 64);
      uint8_t flags = (uint8_t)(0x4 | (rng_next(s) % 2));
      frame(b, 1, flags, 1 + 2 * (uint32_t)i, blk.p, (uint32_t)blk.n);
      free(blk.p);
    }
    break;
  }
  case 6: { /* Upload soup: interleaved streams of DATA frames, some broken */
    preface(b);
    enum { MAXS = 3 };
    Buf body[MAXS] = {{0}};
    size_t off[MAXS] = {0};
    int nstreams = 1 + (int)(rng_next(s) % MAXS), cut[MAXS];
    for (int i = 0; i < nstreams; i++) {
      Buf blk = {0};
      call_block(&blk, "/kvasm.v1.KvAsm/Upload");
      frame(b, 1, 0x4, 1 + 2 * (uint32_t)i, blk.p, (uint32_t)blk.n);
      free(blk.p);
      char key[32];
      snprintf(key, sizeof key, "fuzz-up-%d", i);
      upload_stream(&body[i], s, key, 65536);
      switch (rng_next(s) % 6) {
      case 0:                                   /* flipped bytes */
        for (int f = 0; f < 3; f++) body[i].p[rng_next(s) % body[i].n] ^= (uint8_t)(1 + rng_next(s) % 255);
        break;
      case 1:                                   /* truncated */
        body[i].n = rng_next(s) % body[i].n;
        break;
      case 2:                                   /* garbage appended */
        random_bytes(&body[i], s, 1 + rng_next(s) % 40);
        break;
      default:                                  /* valid */
        break;
      }
      /* 1 in 6: reset instead of ending; 1 in 6: never ended */
      cut[i] = (int)(rng_next(s) % 6);
    }
    for (int left = nstreams; left > 0;) {
      int i = (int)(rng_next(s) % (uint64_t)nstreams);
      if (off[i] > body[i].n) continue;         /* this stream is done */
      uint32_t sid = 1 + 2 * (uint32_t)i;
      size_t n = body[i].n - off[i];
      size_t maxf = rng_next(s) % 2 ? 1 + rng_next(s) % 64 : 1 + rng_next(s) % 16384;
      if (n > maxf) n = maxf;
      int last = off[i] + n == body[i].n;
      if (last && cut[i] == 0) {                /* RST_STREAM(CANCEL) before the end */
        static const uint8_t cancel[4] = {0, 0, 0, 8};
        frame(b, 3, 0, sid, cancel, 4);
      } else if (rng_next(s) % 8 == 0 && n < 200) { /* padded DATA */
        uint8_t pad = (uint8_t)(rng_next(s) % 32);
        frame_hdr(b, (uint32_t)(1 + n + pad), 0, (uint8_t)(0x8 | (last && cut[i] != 1)), sid);
        put(b, &pad, 1);
        if (n) put(b, body[i].p + off[i], n);
        for (uint8_t z = 0, k = 0; k < pad; k++) put(b, &z, 1);
      } else {
        frame(b, 0, (uint8_t)(last && cut[i] != 1), sid, body[i].p + off[i], (uint32_t)n);
      }
      off[i] += n;
      if (last) {
        off[i] = body[i].n + 1;                 /* mark done */
        left--;
      }
    }
    for (int i = 0; i < nstreams; i++) free(body[i].p);
    break;
  }
  case 7: { /* Download soup: real and bogus ranges, windows shaken, dropped */
    put(b, kPreface, sizeof kPreface);
    uint8_t set[6] = {0, 4, 0, 0, 0, 0};        /* SETTINGS_INITIAL_WINDOW_SIZE */
    uint32_t win = rng_next(s) % 3 == 0 ? (uint32_t)(rng_next(s) % 100) : 65535;
    set[2] = (uint8_t)(win >> 24);
    set[3] = (uint8_t)(win >> 16);
    set[4] = (uint8_t)(win >> 8);
    set[5] = (uint8_t)win;
    frame(b, 4, 0, 0, set, 6);
    int nstreams = 1 + (int)(rng_next(s) % 4);
    for (int i = 0; i < nstreams; i++) {
      Buf blk = {0}, msg = {0};
      call_block(&blk, "/kvasm.v1.KvAsm/Download");
      frame(b, 1, 0x4, 1 + 2 * (uint32_t)i, blk.p, (uint32_t)blk.n);
      download_msg(&msg, s);
      frame(b, 0, 0x1, 1 + 2 * (uint32_t)i, msg.p, (uint32_t)msg.n);
      free(blk.p);
      free(msg.p);
    }
    int extra = (int)(rng_next(s) % 10);
    for (int i = 0; i < extra; i++) {
      uint32_t sid = rng_next(s) % 3 == 0 ? 0 : 1 + 2 * (uint32_t)(rng_next(s) % 4);
      switch (rng_next(s) % 4) {
      case 0: {                                 /* WINDOW_UPDATE, even 0 or 2^31-1 */
        uint32_t inc = rng_next(s) % 4 == 0 ? (uint32_t)(rng_next(s) & 0x7FFFFFFF)
                                            : (uint32_t)(rng_next(s) % 70000);
        uint8_t p[4] = {(uint8_t)(inc >> 24), (uint8_t)(inc >> 16), (uint8_t)(inc >> 8), (uint8_t)inc};
        frame(b, 8, 0, sid, p, 4);
        break;
      }
      case 1: {                                 /* RST_STREAM mid-response */
        static const uint8_t cancel[4] = {0, 0, 0, 8};
        frame(b, 3, 0, sid ? sid : 1, cancel, 4);
        break;
      }
      case 2: {                                 /* the initial window changes */
        uint32_t w = (uint32_t)(rng_next(s) % 200000);
        uint8_t p[6] = {0, 4, (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w};
        frame(b, 4, 0, 0, p, 6);
        break;
      }
      default: {
        static const uint8_t opaque[8] = "fuzzping";
        frame(b, 6, 0, 0, opaque, 8);
        break;
      }
      }
    }
    break;
  }
  default: /* valid pipelined requests (split into random writes by the caller) */
    preface(b);
    for (uint32_t i = 0; i < 8; i++) get_request(b, 1 + 2 * i);
    break;
  }
}

/* ========================================================================= */
/* The request matrix: frame boundaries of a unary request, deterministic    */
/* ========================================================================= */

/** What one stream of a matrix connection got back. */
typedef struct {
  int status;    /* grpc-status of its last header block that carried one; -1: none */
  int ended;     /* frames with END_STREAM received (more than 1 is a server bug) */
  int rst;       /* RST_STREAM error code; -1: none */
  size_t dlen;   /* DATA payload bytes received */
  uint8_t *data; /* those bytes (heap; matrix_close frees them) */
} StreamAnswer;

/** Streams tracked one by one: ids 1, 3, .. 15 (index = id / 2). */
enum { kMatrixStreams = 8 };

/** What a matrix connection got back. */
typedef struct {
  StreamAnswer st[kMatrixStreams];
  int goaway;         /* GOAWAY error code; -1: none */
  int window_updates; /* WINDOW_UPDATE frames for the connection (stream 0) */
  long ok_ended;      /* streams of any id ended by a block with grpc-status 0 */
  long other_ended;   /* streams ended any other way (another status, RST_STREAM) */
  int ping_ack;       /* the closing PING was acknowledged */
} ConnAnswer;

/** One matrix connection: socket, response HPACK decoder, unparsed input. */
typedef struct {
  int fd;
  HpackDec dec;
  uint8_t *in; /* received bytes not yet parsed into frames */
  size_t n, cap;
  ConnAnswer a;
} MatrixConn;

/** Opaque data of the PING that closes a matrix conversation. */
static const uint8_t kMatrixPing[8] = "matrixpg";

/**
 * Fails the run, naming the matrix case.
 * @param what    the case
 * @param why     what was wrong with the answer
 * @param detail  a number that reproduces it (the split offset; -1: none)
 */
static void matrix_fail(const char *what, const char *why, long detail) {
  char msg[256];
  if (detail >= 0)
    snprintf(msg, sizeof msg, "request matrix, %s (cut at byte %ld): %s", what, detail, why);
  else
    snprintf(msg, sizeof msg, "request matrix, %s: %s", what, why);
  fail(msg);
}

/**
 * @return the pool the matrix decoders allocate from (created on first use).
 * @remarks Calls kv_hpack_global_init again; check_alive has usually done so
 *          already, which is fine — it only recomputes a static table.
 */
static void *matrix_pool(void) {
  static void *pool;
  if (!pool) {
    kv_hpack_global_init();
    pool = calloc(1, kv_pool_struct_size);
    if (!pool) fail("request matrix: out of memory");
  }
  return pool;
}

/** kv_hpack_decode handler: the numeric grpc-status of a block into *ctx. */
static int on_status(void *ctx, const uint8_t *name, size_t nlen, const uint8_t *value,
                     size_t vlen, uint64_t meaning) {
  (void)meaning;
  if (nlen == 11 && memcmp(name, "grpc-status", 11) == 0) {
    int v = 0;
    for (size_t i = 0; i < vlen; i++) v = v * 10 + (value[i] - '0');
    *(int *)ctx = v;
  }
  return 0;
}

/** Opens a matrix connection; fails the run when it cannot. */
static void matrix_open(MatrixConn *c) {
  memset(c, 0, sizeof *c);
  for (int i = 0; i < kMatrixStreams; i++) {
    c->a.st[i].status = -1;
    c->a.st[i].rst = -1;
  }
  c->a.goaway = -1;
  if (kv_hpack_init(&c->dec, matrix_pool(), 4096) != 0) fail("request matrix: no memory for HPACK");
  c->fd = dial();
  if (c->fd < 0) fail("request matrix: cannot connect");
}

/** Closes a matrix connection and frees what it collected. */
static void matrix_close(MatrixConn *c) {
  close(c->fd);
  kv_hpack_free(&c->dec);
  for (int i = 0; i < kMatrixStreams; i++) free(c->a.st[i].data);
  free(c->in);
}

/**
 * Parses every complete frame in c->in into c->a and drops it from the buffer.
 * @remarks Every HEADERS block is decoded, in order: the server's first
 *          response inserts into the dynamic table that its later blocks
 *          index. The server never pads or fragments its frames, so payloads
 *          are taken whole.
 */
static void matrix_parse(MatrixConn *c) {
  static uint8_t scratch_buf[16384];
  const uint64_t scratch[2] = {(uint64_t)(uintptr_t)scratch_buf, sizeof scratch_buf};
  size_t off = 0;
  while (c->n - off >= 9) {
    const uint8_t *f = c->in + off;
    uint32_t len = (uint32_t)f[0] << 16 | (uint32_t)f[1] << 8 | f[2];
    if (c->n - off < 9 + (size_t)len) break;
    uint8_t type = f[3], flags = f[4];
    uint32_t sid = ((uint32_t)f[5] << 24 | (uint32_t)f[6] << 16 | (uint32_t)f[7] << 8 | f[8]) &
                   0x7FFFFFFF;
    const uint8_t *pl = f + 9;
    StreamAnswer *st = (sid & 1) && sid < 2 * kMatrixStreams ? &c->a.st[sid / 2] : NULL;
    int ends = 0, status = -1;
    switch (type) {
    case 0: /* DATA */
      if (st && len) {
        st->data = realloc(st->data, st->dlen + len);
        if (!st->data) fail("request matrix: out of memory");
        memcpy(st->data + st->dlen, pl, len);
        st->dlen += len;
      }
      ends = flags & 1;
      break;
    case 1: /* HEADERS */
      if (kv_hpack_decode(&c->dec, pl, len, &status, on_status, scratch) != 0)
        fail("request matrix: undecodable response header block");
      if (st && status >= 0) st->status = status;
      ends = flags & 1;
      break;
    case 3: /* RST_STREAM */
      if (len == 4) {
        if (st)
          st->rst = (int)((uint32_t)pl[0] << 24 | (uint32_t)pl[1] << 16 | (uint32_t)pl[2] << 8 |
                          pl[3]);
        c->a.other_ended++;
      }
      break;
    case 6: /* PING */
      if ((flags & 1) && len == 8 && memcmp(pl, kMatrixPing, 8) == 0) c->a.ping_ack = 1;
      break;
    case 7: /* GOAWAY */
      if (len >= 8)
        c->a.goaway = (int)((uint32_t)pl[4] << 24 | (uint32_t)pl[5] << 16 | (uint32_t)pl[6] << 8 |
                            pl[7]);
      break;
    case 8: /* WINDOW_UPDATE */
      if (sid == 0) c->a.window_updates++;
      break;
    default: /* SETTINGS and anything else: nothing to record */
      break;
    }
    if (ends) {
      if (st) st->ended++;
      if (type == 1 && status == 0)
        c->a.ok_ended++;
      else
        c->a.other_ended++;
    }
    off += 9 + (size_t)len;
  }
  memmove(c->in, c->in + off, c->n - off);
  c->n -= off;
}

/** matrix_read condition: every stream in the bit mask ended or was reset, or GOAWAY. */
static int until_streams(const MatrixConn *c, long mask) {
  if (c->a.goaway >= 0) return 1;
  for (int i = 0; i < kMatrixStreams; i++)
    if (((mask >> i) & 1) && !c->a.st[i].ended && c->a.st[i].rst < 0) return 0;
  return 1;
}

/** matrix_read condition: a GOAWAY arrived. */
static int until_goaway(const MatrixConn *c, long unused) {
  (void)unused;
  return c->a.goaway >= 0;
}

/** matrix_read condition: the closing PING was acknowledged. */
static int until_ping(const MatrixConn *c, long unused) {
  (void)unused;
  return c->a.ping_ack;
}

/** matrix_read condition: at least n streams ended (any way), or GOAWAY. */
static int until_count(const MatrixConn *c, long n) {
  return c->a.goaway >= 0 || c->a.ok_ended + c->a.other_ended >= n;
}

/**
 * Reads and parses input until a condition holds.
 * @param c      connection
 * @param until  the condition (one of the until_* functions)
 * @param arg    its argument
 * @param ms     how long to wait at most
 * @return 1 when the condition held; 0 when the time ran out or the server
 *         closed the connection first.
 */
static int matrix_read(MatrixConn *c, int (*until)(const MatrixConn *, long), long arg, int ms) {
  uint64_t deadline = now_ns() + (uint64_t)ms * 1000000ull;
  for (;;) {
    if (until(c, arg)) return 1;
    int64_t left = (int64_t)(deadline - now_ns()) / 1000000;
    if (left <= 0) return 0;
    struct pollfd p = {c->fd, POLLIN, 0};
    if (poll(&p, 1, (int)left) <= 0) continue;             /* the deadline is re-checked above */
    if (c->cap - c->n < 65536) {
      c->cap = c->cap * 2 + 131072;
      c->in = realloc(c->in, c->cap);
      if (!c->in) fail("request matrix: out of memory");
    }
    ssize_t k = recv(c->fd, c->in + c->n, c->cap - c->n, 0);
    if (k <= 0) return until(c, arg);                      /* closed: nothing more will come */
    c->n += (size_t)k;
    matrix_parse(c);
  }
}

/**
 * One conversation: sends `b`, waits for the answers, then closes with a PING.
 * @param c      an open matrix connection
 * @param b      preface and frames to send
 * @param split  cut `b` into two writes here, 400 us apart, so the server
 *               reads the first part on its own (0 or >= b->n: one write)
 * @param mask   streams to wait for, bit i = stream 2i + 1; negative: wait
 *               for a GOAWAY instead
 * @param what   the case, for the failure message
 * @remarks The closing PING is what makes "answered exactly once" checkable:
 *          its acknowledgement is queued behind everything the server had
 *          still to say, so a second answer, a late RST_STREAM or a GOAWAY is
 *          in c->a by the time this returns. After a GOAWAY the server
 *          closes, so no PING is sent.
 */
static void matrix_talk(MatrixConn *c, const Buf *b, size_t split, long mask, const char *what) {
  long cut = split > 0 && split < b->n ? (long)split : -1;
  if (cut >= 0) {
    if (send_all(c->fd, b->p, split) != 0) matrix_fail(what, "send failed", cut);
    usleep(400);
    if (send_all(c->fd, b->p + split, b->n - split) != 0) matrix_fail(what, "send failed", cut);
  } else if (send_all(c->fd, b->p, b->n) != 0) {
    matrix_fail(what, "send failed", cut);
  }
  if (!matrix_read(c, mask < 0 ? until_goaway : until_streams, mask, 3000))
    matrix_fail(what, mask < 0 ? "no GOAWAY within 3 s" : "not every stream answered within 3 s", cut);
  if (c->a.goaway >= 0) return;
  Buf p = {0};
  frame(&p, 6, 0, 0, kMatrixPing, 8);
  if (send_all(c->fd, p.p, p.n) != 0) matrix_fail(what, "sending the closing PING failed", cut);
  free(p.p);
  if (!matrix_read(c, until_ping, 0, 3000))
    matrix_fail(what, "the closing PING was not acknowledged", cut);
}

/**
 * Appends a gRPC call's header block: call_block's fields, or — with a
 * content-length — the same fields with content-length in :authority's place.
 * @param b     destination
 * @param path  :path
 * @param clen  content-length to declare; negative: none (and :authority)
 * @remarks Why content-length replaces :authority (which a request may
 *          omit) instead of joining it: the block then stays under the 128
 *          bytes the server's header-block cache keeps, so a second request
 *          with it is answered from the cache and the cached content-length
 *          is exercised too. Literals without indexing, like call_block.
 */
static void matrix_block(Buf *b, const char *path, long clen) {
  char digits[24];
  snprintf(digits, sizeof digits, "%ld", clen);
  const struct { const char *n, *v; } f[] = {
      {":method", "POST"}, {":scheme", "http"}, {":path", path},
      {clen >= 0 ? "content-length" : ":authority", clen >= 0 ? digits : "fuzz"},
      {"content-type", "application/grpc"}, {"te", "trailers"}};
  for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
    uint8_t z = 0;
    put(b, &z, 1);
    hstr(b, f[i].n);
    hstr(b, f[i].v);
  }
}

/**
 * Appends the gRPC message of a Get or Put: {1: key[, 2: value]}.
 * @param b        destination
 * @param key      key (NUL-terminated)
 * @param val      value bytes (ignored unless has_val)
 * @param vlen     value length
 * @param has_val  1 for a Put (field 2 present, even when empty), 0 for a Get
 */
static void kv_message(Buf *b, const char *key, const uint8_t *val, size_t vlen, int has_val) {
  Buf m = {0};
  put(&m, "\x0A", 1);
  vput(&m, strlen(key));
  put(&m, key, strlen(key));
  if (has_val) {
    put(&m, "\x12", 1);
    vput(&m, vlen);
    if (vlen) put(&m, val, vlen);
  }
  grpc_msg(b, 0, &m);
  free(m.p);
}

/**
 * Fails unless stream index i of the connection was answered exactly once,
 * with grpc-status 0 and GetResponse{found = true, value = v}.
 * @param c       the finished conversation
 * @param i       stream index (stream id 2i + 1)
 * @param v       expected value
 * @param vl      its length (> 0)
 * @param what    the case
 * @param detail  the split offset, or -1
 */
static void expect_get(const MatrixConn *c, int i, const uint8_t *v, size_t vl, const char *what,
                       long detail) {
  const StreamAnswer *st = &c->a.st[i];
  Buf want = {0}, m = {0};
  put(&m, "\x08\x01\x12", 3);
  vput(&m, vl);
  put(&m, v, vl);
  grpc_msg(&want, 0, &m);
  int ok = st->status == 0 && st->ended == 1 && st->rst < 0 && st->dlen == want.n &&
           memcmp(st->data, want.p, want.n) == 0;
  free(want.p);
  free(m.p);
  if (!ok) matrix_fail(what, "a Get was not answered exactly once with its value", detail);
}

/**
 * Runs one matrix case that must end in exact Get answers.
 * @param b      preface and frames
 * @param split  as matrix_talk
 * @param gets   streams that must each be a successful Get of v (bit mask)
 * @param v      the value
 * @param vl     its length
 * @param what   the case
 * @remarks For cases whose answers are all successful Gets; a case that
 *          expects an error on some stream drives matrix_talk itself.
 */
static void matrix_gets(const Buf *b, size_t split, long gets, const uint8_t *v, size_t vl,
                        const char *what) {
  MatrixConn c;
  matrix_open(&c);
  matrix_talk(&c, b, split, gets, what);
  long cut = split > 0 && split < b->n ? (long)split : -1;
  if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", cut);
  for (int i = 0; i < kMatrixStreams; i++)
    if ((gets >> i) & 1) expect_get(&c, i, v, vl, what, cut);
  matrix_close(&c);
}

/**
 * The request matrix (see the file header): every case is one or more fresh
 * connections with an exact expected answer.
 * @return the number of conversations held.
 * @remarks Each expectation follows from RFC 9113 and the gRPC mapping, not
 *          from how h2n.asm reaches it, so the matrix holds for an engine
 *          with or without the fused request path — and pins down that the
 *          fused path changes nothing a client can see:
 *            - the cut cases put the boundary between two reads at every
 *              byte, so the DATA frame is whole behind its HEADERS in some,
 *              partly there in others (header cut, payload cut) and absent in
 *              the rest; requests 2 and 3 come through the header-block cache;
 *            - equal-length blocks that differ are the cache's near miss
 *              (same length, same table generation, other bytes): the block
 *              must be decoded, not replayed;
 *            - padded DATA, a body in two frames, an empty END_STREAM frame
 *              and a declared content-length are requests the fused path
 *              must leave to the frame loop;
 *            - a padded HEADERS frame with priority fields is one it must
 *              step over correctly to find the DATA frame;
 *            - HEADERS that already ended the stream must not be fused with
 *              the DATA frame behind them (that would answer twice);
 *            - 36 MB of full-size DATA frames must still make the server top
 *              the connection window up: the fused path counts flow control
 *              itself.
 */
static long request_matrix(void) {
  static const char key[] = "matrix-key";
  static const uint8_t val[] = "matrix-value-0123456789";
  const size_t vl = sizeof val - 1;
  long talks = 0;
  Buf blk_get = {0}, blk_put = {0}, msg_get = {0}, msg_put = {0};
  matrix_block(&blk_get, "/kvasm.v1.KvAsm/Get", -1);
  matrix_block(&blk_put, "/kvasm.v1.KvAsm/Put", -1);
  kv_message(&msg_get, key, NULL, 0, 0);
  kv_message(&msg_put, key, val, vl, 1);

  { /* the value every Get below expects: a Put, HEADERS and DATA in one write */
    MatrixConn c;
    Buf b = {0};
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_put.p, (uint32_t)blk_put.n);
    frame(&b, 0, 0x1, 1, msg_put.p, (uint32_t)msg_put.n);
    matrix_talk(&c, &b, 0, 1, "the Put");
    if (c.a.st[0].status != 0 || c.a.st[0].ended != 1 || c.a.st[0].rst >= 0)
      matrix_fail("the Put", "not answered exactly once with grpc-status 0", -1);
    matrix_close(&c);
    free(b.p);
    talks++;
  }

  { /* three Gets, the byte stream cut into two writes at every offset (0: one write) */
    Buf b = {0};
    preface(&b);
    for (uint32_t i = 0; i < 3; i++) {
      frame(&b, 1, 0x4, 1 + 2 * i, blk_get.p, (uint32_t)blk_get.n);
      frame(&b, 0, 0x1, 1 + 2 * i, msg_get.p, (uint32_t)msg_get.n);
    }
    for (size_t split = 0; split < b.n; split++, talks++)
      matrix_gets(&b, split, 7, val, vl, "three Gets");
    free(b.p);
  }

  { /* header blocks of equal length that differ (.../Get and .../Put) on one connection:
     * the server's header-block cache must tell them apart. Get, Put of a second key,
     * Get, Get of the second key, in one write (a read's Puts are applied before its
     * Gets, so the last Get sees the Put) */
    static const char what[] = "equal-length header blocks that differ";
    static const char key2[] = "matrix-two";
    static const uint8_t val2[] = "second-value";
    MatrixConn c;
    Buf b = {0}, put2 = {0}, get2 = {0};
    kv_message(&put2, key2, val2, sizeof val2 - 1, 1);
    kv_message(&get2, key2, NULL, 0, 0);
    if (blk_get.n != blk_put.n) matrix_fail(what, "the test's two blocks differ in length", -1);
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 1, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4, 3, blk_put.p, (uint32_t)blk_put.n);
    frame(&b, 0, 0x1, 3, put2.p, (uint32_t)put2.n);
    frame(&b, 1, 0x4, 5, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 5, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4, 7, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 7, get2.p, (uint32_t)get2.n);
    matrix_talk(&c, &b, 0, 15, what);
    if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
    expect_get(&c, 0, val, vl, what, -1);
    if (c.a.st[1].status != 0 || c.a.st[1].ended != 1 || c.a.st[1].rst >= 0)
      matrix_fail(what, "the Put was not answered exactly once with grpc-status 0", -1);
    expect_get(&c, 2, val, vl, what, -1);
    expect_get(&c, 3, val2, sizeof val2 - 1, what, -1);
    matrix_close(&c);
    free(b.p);
    free(put2.p);
    free(get2.p);
    talks++;
  }

  { /* a padded DATA frame: pad length 5, the message, 5 bytes of padding */
    Buf b = {0}, pad = {0};
    static const uint8_t five[6] = {5, 0, 0, 0, 0, 0};
    put(&pad, five, 1);
    put(&pad, msg_get.p, msg_get.n);
    put(&pad, five + 1, 5);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1 | 0x8, 1, pad.p, (uint32_t)pad.n);
    matrix_gets(&b, 0, 1, val, vl, "padded DATA");
    free(b.p);
    free(pad.p);
    talks++;
  }

  { /* the body in two DATA frames, and then: a whole body followed by an empty END_STREAM frame */
    Buf b = {0};
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0, 1, msg_get.p, 7);
    frame(&b, 0, 0x1, 1, msg_get.p + 7, (uint32_t)msg_get.n - 7);
    frame(&b, 1, 0x4, 3, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0, 3, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 0, 0x1, 3, "", 0);
    matrix_gets(&b, 0, 3, val, vl, "a body in two DATA frames, an empty END_STREAM frame");
    free(b.p);
    talks++;
  }

  { /* content-length that matches the body */
    Buf b = {0}, blk = {0};
    matrix_block(&blk, "/kvasm.v1.KvAsm/Get", (long)msg_get.n);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk.p, (uint32_t)blk.n);
    frame(&b, 0, 0x1, 1, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4, 3, blk.p, (uint32_t)blk.n);          /* again: now from the block cache */
    frame(&b, 0, 0x1, 3, msg_get.p, (uint32_t)msg_get.n);
    matrix_gets(&b, 0, 3, val, vl, "content-length equal to the body");
    free(b.p);
    free(blk.p);
    talks++;
  }

  { /* content-length one short of the body: that stream is malformed (RFC 9113 8.1.1),
     * RST_STREAM(PROTOCOL_ERROR), unanswered; the connection and the next stream live on */
    static const char what[] = "content-length shorter than the body";
    MatrixConn c;
    Buf b = {0}, blk = {0};
    matrix_block(&blk, "/kvasm.v1.KvAsm/Get", (long)msg_get.n - 1);
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk.p, (uint32_t)blk.n);
    frame(&b, 0, 0x1, 1, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4, 3, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 3, msg_get.p, (uint32_t)msg_get.n);
    matrix_talk(&c, &b, 0, 3, what);
    if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
    if (c.a.st[0].rst != 1 || c.a.st[0].ended != 0 || c.a.st[0].status >= 0)
      matrix_fail(what, "the stream was not reset with PROTOCOL_ERROR and left unanswered", -1);
    expect_get(&c, 1, val, vl, what, -1);
    matrix_close(&c);
    free(b.p);
    free(blk.p);
    talks++;
  }

  { /* HEADERS with padding and priority fields (flags END_HEADERS | PADDED | PRIORITY):
     * pad length 4, stream dependency 0, weight 15, the block, 4 bytes of padding */
    Buf b = {0}, h = {0};
    static const uint8_t lead[6] = {4, 0, 0, 0, 0, 15}, tail[4] = {0, 0, 0, 0};
    put(&h, lead, sizeof lead);
    put(&h, blk_get.p, blk_get.n);
    put(&h, tail, sizeof tail);
    preface(&b);
    frame(&b, 1, 0x4 | 0x8 | 0x20, 1, h.p, (uint32_t)h.n);
    frame(&b, 0, 0x1, 1, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4 | 0x8 | 0x20, 3, h.p, (uint32_t)h.n);
    frame(&b, 0, 0x1, 3, msg_get.p, (uint32_t)msg_get.n);
    matrix_gets(&b, 0, 3, val, vl, "padded HEADERS with priority fields");
    free(b.p);
    free(h.p);
    talks++;
  }

  { /* HEADERS that end the stream (a call without a message: answered with an error
     * status at once), then a DATA frame for it: the stream is closed, so the frame
     * draws RST_STREAM(STREAM_CLOSED) and never a second answer; stream 3 is served */
    static const char what[] = "DATA after HEADERS that ended the stream";
    MatrixConn c;
    Buf b = {0};
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4 | 0x1, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 1, msg_get.p, (uint32_t)msg_get.n);
    frame(&b, 1, 0x4, 3, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 3, msg_get.p, (uint32_t)msg_get.n);
    matrix_talk(&c, &b, 0, 3, what);
    if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
    if (c.a.st[0].ended != 1 || c.a.st[0].status <= 0 || c.a.st[0].dlen != 0)
      matrix_fail(what, "the empty call was not answered once with an error status", -1);
    if (c.a.st[0].rst != 5)
      matrix_fail(what, "the late DATA frame did not draw RST_STREAM(STREAM_CLOSED)", -1);
    expect_get(&c, 1, val, vl, what, -1);
    matrix_close(&c);
    free(b.p);
    talks++;
  }

  { /* DATA for a stream that was never opened, right behind another stream's HEADERS:
     * a connection error, PROTOCOL_ERROR (RFC 9113 5.1) */
    static const char what[] = "DATA for an idle stream behind HEADERS";
    MatrixConn c;
    Buf b = {0};
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 3, msg_get.p, (uint32_t)msg_get.n);
    matrix_talk(&c, &b, 0, -1, what);
    if (c.a.goaway != 1) matrix_fail(what, "GOAWAY did not carry PROTOCOL_ERROR", -1);
    if (c.a.st[0].ended || c.a.st[1].ended) matrix_fail(what, "a stream was answered", -1);
    matrix_close(&c);
    free(b.p);
    talks++;
  }

  { /* a DATA frame of 16,385 bytes (one more than the frame size the server allows):
     * FRAME_SIZE_ERROR on the connection. The whole frame is sent — a valid message,
     * then zeros — so an engine that forgot the limit would have a request to answer */
    static const char what[] = "oversized DATA frame behind HEADERS";
    enum { kOver = 16385 };
    MatrixConn c;
    Buf b = {0};
    uint8_t *zeros = calloc(1, kOver);
    if (!zeros) fail("request matrix: out of memory");
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame_hdr(&b, kOver, 0, 0x1, 1);
    put(&b, msg_get.p, msg_get.n);
    put(&b, zeros, kOver - msg_get.n);
    free(zeros);
    matrix_talk(&c, &b, 0, -1, what);
    if (c.a.goaway != 6) matrix_fail(what, "GOAWAY did not carry FRAME_SIZE_ERROR", -1);
    if (c.a.st[0].ended) matrix_fail(what, "the stream was answered", -1);
    matrix_close(&c);
    free(b.p);
    talks++;
  }

  { /* an Upload whose whole message stream — key, size, the one chunk, crc32c — is a
     * single DATA frame right behind its HEADERS. Client streaming consumes DATA as it
     * arrives, so the engine must not take that frame for a buffered unary body. The
     * value is then read back on a second connection */
    static const char what[] = "Upload in one DATA frame";
    static const char ukey[] = "matrix-up";
    static const uint8_t uval[] = "uploaded-in-one-frame";
    const size_t ul = sizeof uval - 1;
    uint32_t crc = crc32c(0, uval, ul);
    const uint8_t crcf[5] = {0x25, (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16),
                             (uint8_t)(crc >> 24)};
    MatrixConn c;
    Buf b = {0}, blk = {0}, m = {0}, body = {0}, get = {0};
    matrix_block(&blk, "/kvasm.v1.KvAsm/Upload", -1);
    put(&m, "\x0A", 1);
    vput(&m, strlen(ukey));
    put(&m, ukey, strlen(ukey));
    put(&m, "\x10", 1);
    vput(&m, ul);
    put(&m, "\x1A", 1);
    vput(&m, ul);
    put(&m, uval, ul);
    put(&m, crcf, sizeof crcf);
    grpc_msg(&body, 0, &m);
    kv_message(&get, ukey, NULL, 0, 0);
    matrix_open(&c);
    preface(&b);
    frame(&b, 1, 0x4, 1, blk.p, (uint32_t)blk.n);
    frame(&b, 0, 0x1, 1, body.p, (uint32_t)body.n);
    matrix_talk(&c, &b, 0, 1, what);
    if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
    if (c.a.st[0].status != 0 || c.a.st[0].ended != 1 || c.a.st[0].rst >= 0)
      matrix_fail(what, "the Upload was not answered exactly once with grpc-status 0", -1);
    matrix_close(&c);
    b.n = 0;
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 1, get.p, (uint32_t)get.n);
    matrix_gets(&b, 0, 1, uval, ul, what);
    free(b.p);
    free(blk.p);
    free(m.p);
    free(body.p);
    free(get.p);
    talks += 2;
  }

  { /* 2,200 Puts whose DATA frame is exactly the largest frame (16,384 bytes), 20 per
     * write: 36 MB, more than half of the default 64 MiB connection window (which
     * run.sh's fuzz phase leaves alone), so the server must have topped the window
     * up — a second connection WINDOW_UPDATE after the one that announces the window
     * at open. It would not if such frames went uncounted. Then the value is read
     * back whole. */
    static const char what[] = "full-size DATA frames and the connection window";
    static const char bigkey[] = "matrix-big";
    enum { kPerWrite = 20, kWrites = 110, kBig = 16364 };
    uint8_t *big = malloc(kBig);
    if (!big) fail("request matrix: out of memory");
    for (int i = 0; i < kBig; i++) big[i] = (uint8_t)(i * 131 + (i >> 8));
    Buf msg_big = {0}, msg_bigget = {0}, b = {0};
    kv_message(&msg_big, bigkey, big, kBig, 1);
    kv_message(&msg_bigget, bigkey, NULL, 0, 0);
    if (msg_big.n != 16384) matrix_fail(what, "the test's frame is not 16,384 bytes", -1);
    MatrixConn c;
    matrix_open(&c);
    preface(&b);
    if (send_all(c.fd, b.p, b.n) != 0) matrix_fail(what, "send failed", -1);
    for (int w = 0; w < kWrites; w++) {
      b.n = 0;
      for (int i = 0; i < kPerWrite; i++) {
        uint32_t sid = 1 + 2 * (uint32_t)(w * kPerWrite + i);
        frame(&b, 1, 0x4, sid, blk_put.p, (uint32_t)blk_put.n);
        frame(&b, 0, 0x1, sid, msg_big.p, (uint32_t)msg_big.n);
      }
      if (send_all(c.fd, b.p, b.n) != 0) matrix_fail(what, "send failed", -1);
      if (!matrix_read(&c, until_count, (long)(w + 1) * kPerWrite, 5000))
        matrix_fail(what, "a write of 20 Puts was not answered within 5 s", -1);
    }
    if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
    if (c.a.ok_ended != (long)kPerWrite * kWrites || c.a.other_ended != 0)
      matrix_fail(what, "not every Put was answered with grpc-status 0", -1);
    if (c.a.window_updates < 2)
      matrix_fail(what, "36 MB of DATA drew no connection WINDOW_UPDATE after the opening one", -1);
    matrix_close(&c);
    b.n = 0;
    preface(&b);
    frame(&b, 1, 0x4, 1, blk_get.p, (uint32_t)blk_get.n);
    frame(&b, 0, 0x1, 1, msg_bigget.p, (uint32_t)msg_bigget.n);
    matrix_gets(&b, 0, 1, big, kBig, what);
    free(b.p);
    free(msg_big.p);
    free(msg_bigget.p);
    free(big);
    talks += 2;
  }

  free(blk_get.p);
  free(blk_put.p);
  free(msg_get.p);
  free(msg_put.p);
  return talks;
}

/**
 * --limits: the request-size limit at the frame boundary. Needs a server
 * started with KVASM_MAX_KEY_BYTES=16 and KVASM_MAX_VALUE_BYTES=64, so
 * that the largest message it accepts is 16 + 64 + 64 = 144 bytes (149 with
 * the gRPC prefix) — small enough for a single DATA frame to exceed it.
 * Three Puts on one connection, each HEADERS + one DATA frame, in one write:
 *   a value of 64 bytes      grpc-status 0
 *   a value of 100 bytes     the body (116 bytes) is within the limit: the
 *                            message is decoded and its value refused,
 *                            INVALID_ARGUMENT (3)
 *   a value of 1,000 bytes   the body (1,017 bytes) is over the limit: it is
 *                            dropped undecoded, RESOURCE_EXHAUSTED (8)
 * @return 0; any other outcome fails the run.
 * @remarks Why its own mode and server: with the default limits no single
 *          frame can be over max_request, so the engine's choice between
 *          "borrow the frame" and "over the limit" — which the fused request
 *          path makes by itself — would go untested.
 */
static int limits_matrix(void) {
  static const char what[] = "the request-size limit";
  static const char key[] = "lim-key";
  static const size_t sizes[3] = {64, 100, 1000};
  static const int want[3] = {0, 3, 8};
  uint8_t value[1000];
  memset(value, 'v', sizeof value);
  MatrixConn c;
  Buf b = {0}, blk = {0};
  matrix_block(&blk, "/kvasm.v1.KvAsm/Put", -1);
  matrix_open(&c);
  preface(&b);
  for (uint32_t i = 0; i < 3; i++) {
    Buf m = {0};
    kv_message(&m, key, value, sizes[i], 1);
    frame(&b, 1, 0x4, 1 + 2 * i, blk.p, (uint32_t)blk.n);
    frame(&b, 0, 0x1, 1 + 2 * i, m.p, (uint32_t)m.n);
    free(m.p);
  }
  matrix_talk(&c, &b, 0, 7, what);
  if (c.a.goaway >= 0) matrix_fail(what, "the connection was ended with GOAWAY", -1);
  for (int i = 0; i < 3; i++)
    if (c.a.st[i].status != want[i] || c.a.st[i].ended != 1 || c.a.st[i].rst >= 0) {
      char why[96];
      snprintf(why, sizeof why, "the Put of %zu bytes got grpc-status %d, not %d (exactly once)",
               sizes[i], c.a.st[i].status, want[i]);
      matrix_fail(what, why, -1);
    }
  matrix_close(&c);
  free(b.p);
  free(blk.p);
  printf("fuzz_h2 --limits: 64 bytes stored, 100 refused as a value (3), 1,000 as a request (8)\n");
  return 0;
}

/** Entry point. @return 0 when every check passed. */
int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "--limits") == 0) {
    g_host = argv[2];
    g_port = argv[3];
    return limits_matrix();
  }
  if (argc != 6) {
    fprintf(stderr, "usage: %s HOST PORT ITERATIONS SEED PID\n       %s --limits HOST PORT\n",
            argv[0], argv[0]);
    return 2;
  }
  g_host = argv[1];
  g_port = argv[2];
  long iterations = atol(argv[3]);
  g_seed = strtoull(argv[4], NULL, 10);
  long pid = atol(argv[5]);
  uint64_t s = g_seed ? g_seed : 1;
  check_alive();
  uint64_t tm = now_ns();
  long talks = request_matrix();                 /* exact answers at the frame boundaries */
  printf("fuzz_h2: request matrix: %ld conversations in %.1fs, every answer exact\n", talks,
         (double)(now_ns() - tm) / 1e9);
  fuzz_setup();                                  /* the values kind 7 downloads */
  long rss0 = rss_kib(pid);
  long counts[8] = {0};
  uint64_t t0 = now_ns();
  for (g_iter = 1; g_iter <= iterations; g_iter++) {
    int kind = (int)(rng_next(&s) % 8);
    counts[kind]++;
    Buf b = {0};
    build_case(&b, &s, kind);
    int fd = dial();
    if (fd < 0) fail("cannot connect (server gone?)");
    if (kind == 5) { /* random-sized writes: partial frames and prefaces */
      size_t off = 0;
      while (off < b.n) {
        size_t k = 1 + rng_next(&s) % 97;
        if (k > b.n - off) k = b.n - off;
        if (send_all(fd, b.p + off, k) != 0) break;
        off += k;
        if (rng_next(&s) % 4 == 0) usleep(200);
      }
    } else {
      send_all(fd, b.p, b.n);
    }
    free(b.p);
    drain(fd, kind == 3 ? 0 : 5);
    close(fd);
    if (g_iter % 50 == 0) check_alive();
  }
  check_alive();
  long rss1 = rss_kib(pid);
  printf("fuzz_h2: %ld connections in %.1fs (garbage %ld, flipped %ld, soup %ld, truncated %ld, "
         "hpack %ld, split %ld, upload %ld, download %ld); server alive; resident memory "
         "%ld -> %ld KiB\n",
         iterations, (double)(now_ns() - t0) / 1e9, counts[0], counts[1], counts[2], counts[3],
         counts[4], counts[5], counts[6], counts[7], rss0, rss1);
  if (rss0 && rss1 - rss0 > 64 * 1024) fail("server memory grew by more than 64 MiB");
  return 0;
}
