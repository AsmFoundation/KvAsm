/*
 * fuzz_h2.c — black-box fuzzing of a running kvnode's HTTP/2 input path
 * (written for the native engine, h2n.asm; works against either engine).
 *
 * Usage: fuzz_h2 HOST PORT ITERATIONS SEED PID
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
 * then reads whatever comes back for a short while and closes.
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
      {":method", "POST"}, {":scheme", "http"}, {":path", "/kvnode.v1.KvNode/Get"},
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
  default: /* valid pipelined requests (split into random writes by the caller) */
    preface(b);
    for (uint32_t i = 0; i < 8; i++) get_request(b, 1 + 2 * i);
    break;
  }
}

/** Entry point. @return 0 when every check passed. */
int main(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr, "usage: %s HOST PORT ITERATIONS SEED PID\n", argv[0]);
    return 2;
  }
  g_host = argv[1];
  g_port = argv[2];
  long iterations = atol(argv[3]);
  g_seed = strtoull(argv[4], NULL, 10);
  long pid = atol(argv[5]);
  uint64_t s = g_seed ? g_seed : 1;
  check_alive();
  long rss0 = rss_kib(pid);
  long counts[6] = {0};
  uint64_t t0 = now_ns();
  for (g_iter = 1; g_iter <= iterations; g_iter++) {
    int kind = (int)(rng_next(&s) % 6);
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
         "hpack %ld, split %ld); server alive; resident memory %ld -> %ld KiB\n",
         iterations, (double)(now_ns() - t0) / 1e9, counts[0], counts[1], counts[2], counts[3],
         counts[4], counts[5], rss0, rss1);
  if (rss0 && rss1 - rss0 > 64 * 1024) fail("server memory grew by more than 64 MiB");
  return 0;
}
