/* =============================================================================
 * bench_h2.c — the server's user-space request path, timed in process.
 *
 * What it measures: nanoseconds (and TSC ticks) per unary request spent in
 * h2n.asm + grpc.asm + the store for a steady stream of pipelined requests on
 * one connection — frame parsing, the header-block cache, stream open/close,
 * dispatch, hashing, lookup, reply framing — with no socket, no syscall and
 * no client: the input is a prebuilt buffer of 16 requests handed straight to
 * h2_process, the replies land in the connection's output buffer, which is
 * dropped after every batch.
 *
 * Why it exists: on a shared host (other benchmarks, VMs) end-to-end
 * throughput moves by +-10% between identical runs, so a 2% saving in the
 * request path cannot be seen there. This loop is cache-resident and
 * single-threaded; the minimum over many short trials is repeatable to well
 * under 1%, which makes small instruction-path changes measurable. It says
 * nothing about kernel time or memory stalls at large key counts (use
 * getcost.sh / the suite for those).
 *
 * Usage: bench_h2 [get|ping] [KEYS] [TRIALS] [BATCHES_PER_TRIAL]
 *   get   Get of an existing 16-byte key with a 100-byte value (default)
 *   ping  grpc.health.v1.Health/Check — the protocol floor, no store work
 *   KEYS  preloaded keys the GETs cycle through (default 1024: L1/L2-resident,
 *         so the figure is the instruction path, not DRAM latency)
 *
 * Build: see build_bench.sh — links the variant's own objects; h2_process and
 * conn_window_refill are internal to h2n.asm and are made global with objcopy
 * (the object code is untouched).
 * ============================================================================= */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#include "kvasm_abi.h"

/* Offsets exported by benchabi.asm (same order). */
enum {
  BA_WORKER_SIZE, BA_W_TABLE, BA_W_CFG, BA_W_READ_BUF, BA_W_NOW, BA_CONN_SIZE, BA_C_FD,
  BA_C_WORKER, BA_C_OUT_BUF, BA_C_OUT_CAP, BA_C_OUT_LEN, BA_C_OUT_OFF, BA_C_SEND_WINDOW,
  BA_C_NSTREAMS, BA_CONFIG_SIZE, BA_W_ID, BA_C_PEND_HEAD, BA_W_REQUESTS, BA_SENTINEL
};
extern const uint64_t bench_abi[];

int64_t kv_config_load(void *cfg);
void kv_hpack_global_init(void);
int kv_h2n_open(void *conn);
int h2_process(void *conn, const uint8_t *buf, size_t len); /* h2n.asm, globalized */
int kv_grpc_run_batch(void *conn);
int conn_window_refill(void *conn);                         /* h2n.asm, globalized */
uint64_t calib_add(uint64_t n);                             /* calib.asm: 1 cycle per iteration */
/* store.asm, only in variants that can choose the key hash: weak, so the
 * harness links against older variants too (the pointers are then NULL). */
int kv_set_hash_mode(int mode) __attribute__((weak));
int kv_hash_best_mode(void) __attribute__((weak));
int kv_hash_mode_get(void) __attribute__((weak));

/* FNV-1a over every output byte produced through feed() (setup and warm-up):
 * two variants fed the same input must print the same digest, or one of them
 * changed the bytes on the wire. */
static uint64_t out_digest = 0xcbf29ce484222325ull;

/* Field `idx` of the hand-built struct at `base`, as the assembler lays it out. */
#define F(base, idx) (*(uint64_t *)((uint8_t *)(base) + bench_abi[idx]))

#define BATCH 16
#define KEY_LEN 16
#define VAL_LEN 100
#define OUT_INITIAL 65536

/* Writes a 9-byte HTTP/2 frame header; returns the byte after it. */
static uint8_t *frame_hdr(uint8_t *p, uint32_t len, uint8_t type, uint8_t flags, uint32_t sid) {
  p[0] = (uint8_t)(len >> 16); p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)len;
  p[3] = type; p[4] = flags;
  p[5] = (uint8_t)(sid >> 24); p[6] = (uint8_t)(sid >> 16); p[7] = (uint8_t)(sid >> 8);
  p[8] = (uint8_t)sid;
  return p + 9;
}

/* Big-endian stream id into a frame header that starts at `hdr`. */
static inline void set_sid(uint8_t *hdr, uint32_t sid) {
  uint32_t be = __builtin_bswap32(sid);
  memcpy(hdr + 5, &be, 4);
}

static void make_key(uint8_t *dst, unsigned i) {
  char tmp[KEY_LEN + 1];
  snprintf(tmp, sizeof tmp, "key:%012u", i);
  memcpy(dst, tmp, KEY_LEN);
}

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/* Runs one input buffer through the engine the way kv_h2n_read does after
 * its read(): frames, then the batch they queued, then the window top-up.
 * Returns the bytes of output produced (and drops them). Exits on any
 * failure: a harness that kept going would time an error path. */
static uint64_t feed(void *conn, const uint8_t *buf, size_t len) {
  if (h2_process(conn, buf, len) < 0) { fprintf(stderr, "h2_process failed\n"); exit(1); }
  if (kv_grpc_run_batch(conn) < 0) { fprintf(stderr, "run_batch failed\n"); exit(1); }
  if (conn_window_refill(conn) < 0) { fprintf(stderr, "window refill failed\n"); exit(1); }
  uint64_t n = F(conn, BA_C_OUT_LEN);
  const uint8_t *o = (const uint8_t *)F(conn, BA_C_OUT_BUF);
  for (uint64_t i = 0; i < n; i++) out_digest = (out_digest ^ o[i]) * 0x100000001b3ull;
  F(conn, BA_C_OUT_LEN) = 0;
  return n;
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "get";
  unsigned nkeys = argc > 2 ? (unsigned)atoi(argv[2]) : 1024;
  int trials = argc > 3 ? atoi(argv[3]) : 300;
  int batches = argc > 4 ? atoi(argv[4]) : 2000;
  int ping = strcmp(mode, "ping") == 0;
  if (bench_abi[BA_SENTINEL] != 0xABCDEF0123456789ull) { fprintf(stderr, "abi mismatch\n"); return 1; }

  /* ---- the pieces main.asm and worker.asm set up for a real connection ---- */
  void *cfg = calloc(1, bench_abi[BA_CONFIG_SIZE] + 64);
  if (kv_config_load(cfg) != 0) { fprintf(stderr, "config\n"); return 1; }
  /* The key hash, as main.asm chooses it: the best the CPU runs, unless
   * BENCH_HASH=siphash|aes forces one (an A/B inside one binary, so both
   * sides have the same code layout). Before the store exists: the mode must
   * be final before the first key is hashed. */
  if (kv_set_hash_mode) {
    const char *want = getenv("BENCH_HASH");
    int mode = kv_hash_best_mode();
    if (want && strcmp(want, "siphash") == 0) mode = 0;
    if (want && strcmp(want, "aes") == 0) mode = 1;
    if (kv_set_hash_mode(mode) < 0) { fprintf(stderr, "hash mode %d refused\n", mode); return 1; }
  }
  uint8_t seed[16];
  for (int i = 0; i < 16; i++) seed[i] = (uint8_t)(0x5a ^ (i * 37));
  KvTable *t = kv_store_create(nkeys * 2 + 1024, 4, seed, 0);
  if (!t) { fprintf(stderr, "store\n"); return 1; }
  kv_set_scan_mode(1);                       /* the server's default: sse2 */
  kv_hpack_global_init();

  uint8_t val[VAL_LEN];
  for (int i = 0; i < VAL_LEN; i++) val[i] = (uint8_t)('a' + i % 26);
  for (unsigned i = 0; i < nkeys; i++) {
    uint8_t k[KEY_LEN];
    uint64_t h[2];
    make_key(k, i);
    kv_store_hash(t, k, KEY_LEN, h);
    if (kv_store_put(t, k, KEY_LEN, val, VAL_LEN, h[0], h[1]) < 0) { fprintf(stderr, "put\n"); return 1; }
  }

  void *w = calloc(1, bench_abi[BA_WORKER_SIZE] + 64);
  F(w, BA_W_TABLE) = (uint64_t)t;
  F(w, BA_W_CFG) = (uint64_t)cfg;
  F(w, BA_W_READ_BUF) = (uint64_t)malloc(262144);
  F(w, BA_W_NOW) = 1000000000ull;
  F(w, BA_W_ID) = 0;
  kv_qsbr_online(t, 0);                      /* the worker is online while h2n runs */

  void *conn = calloc(1, bench_abi[BA_CONN_SIZE] + 64);
  F(conn, BA_C_FD) = (uint64_t)-1;
  F(conn, BA_C_WORKER) = (uint64_t)w;
  F(conn, BA_C_OUT_BUF) = (uint64_t)malloc(OUT_INITIAL);
  F(conn, BA_C_OUT_CAP) = OUT_INITIAL;
  if (kv_h2n_open(conn) != 0) { fprintf(stderr, "h2n open\n"); return 1; }
  F(conn, BA_C_OUT_LEN) = 0;                 /* drop the server preface */

  /* ---- client preface, SETTINGS (16 MiB stream windows), ACK, 1 GiB connection window ---- */
  uint8_t pre[256], *p = pre;
  memcpy(p, "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n", 24); p += 24;
  p = frame_hdr(p, 6, 4, 0, 0);
  *p++ = 0; *p++ = 4; *p++ = 0x01; *p++ = 0x00; *p++ = 0x00; *p++ = 0x00;   /* INITIAL_WINDOW_SIZE */
  p = frame_hdr(p, 0, 4, 1, 0);                                               /* SETTINGS ACK */
  p = frame_hdr(p, 4, 8, 0, 0);
  uint32_t inc = (1u << 30) - 65535;
  *p++ = (uint8_t)(inc >> 24); *p++ = (uint8_t)(inc >> 16); *p++ = (uint8_t)(inc >> 8); *p++ = (uint8_t)inc;
  feed(conn, pre, (size_t)(p - pre));

  /* ---- the request's two frames ---- */
  const char *path = ping ? "/grpc.health.v1.Health/Check" : "/kvasm.v1.KvAsm/Get";
  size_t plen = strlen(path);
  uint8_t msg[64];                           /* gRPC message: prefix + protobuf */
  size_t mlen;
  if (ping) {
    memset(msg, 0, 5); mlen = 5;             /* empty HealthCheckRequest */
  } else {
    msg[0] = 0; msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 2 + KEY_LEN;
    msg[5] = 0x0A; msg[6] = KEY_LEN;         /* field 1 (key), length-delimited */
    make_key(msg + 7, 0);
    mlen = 7 + KEY_LEN;
  }

  /* First request: its header block fills the peer-side dynamic table
   * (:authority, content-type, te with incremental indexing), as a gRPC
   * client's first call does. Later blocks are 3 index bytes + the :path
   * literal, which nghttp2-based clients never index. */
  uint8_t first[512];
  p = first;
  uint8_t blk[256], *b = blk;
  *b++ = 0x83;                               /* :method POST */
  *b++ = 0x86;                               /* :scheme http */
  *b++ = 0x04; *b++ = (uint8_t)plen; memcpy(b, path, plen); b += plen;
  *b++ = 0x41; *b++ = 9; memcpy(b, "localhost", 9); b += 9;
  *b++ = 0x5F; *b++ = 16; memcpy(b, "application/grpc", 16); b += 16;
  *b++ = 0x40; *b++ = 2; memcpy(b, "te", 2); b += 2; *b++ = 8; memcpy(b, "trailers", 8); b += 8;
  p = frame_hdr(p, (uint32_t)(b - blk), 1, 0x4, 1);
  memcpy(p, blk, (size_t)(b - blk)); p += b - blk;
  p = frame_hdr(p, (uint32_t)mlen, 0, 0x1, 1);
  memcpy(p, msg, mlen); p += mlen;
  uint64_t first_out = feed(conn, first, (size_t)(p - first));

  /* Steady-state batches: BATCH requests each, keys differing per slot. A
   * ring of buffers so consecutive batches ask for different keys. */
  enum { RING = 64 };
  size_t blen = 2 + 2 + plen + 3;            /* 83 86 04 len path C0 BF BE */
  size_t req_bytes = 9 + blen + 9 + mlen;
  /* Where request j's two frames sit in a batch. BENCH_LAYOUT=pairs (the
   * default): each request's DATA frame right behind its HEADERS — how gRPC
   * clients that write a call's frames back to back fill a read (and the
   * shape h2n.asm's fused request path takes). BENCH_LAYOUT=burst: all the
   * HEADERS frames of the batch, then all its DATA frames — what nghttp2
   * sends when several requests are submitted before one send (kvbench at
   * depth 16), where no DATA frame is behind its own HEADERS. */
  const char *layout = getenv("BENCH_LAYOUT");
  int burst = layout && strcmp(layout, "burst") == 0;
  #define HDR_OFF(j)  (burst ? (size_t)(j) * (9 + blen) : (size_t)(j) * req_bytes)
  #define DATA_OFF(j) (burst ? (size_t)BATCH * (9 + blen) + (size_t)(j) * (9 + mlen) \
                             : (size_t)(j) * req_bytes + 9 + blen)
  uint8_t *ring = malloc((size_t)RING * BATCH * req_bytes);
  for (int r = 0; r < RING; r++)
    for (int j = 0; j < BATCH; j++) {
      uint8_t *base = ring + (size_t)r * BATCH * req_bytes;
      uint8_t *q = base + HDR_OFF(j);
      q = frame_hdr(q, (uint32_t)blen, 1, 0x4, 0);
      *q++ = 0x83; *q++ = 0x86; *q++ = 0x04; *q++ = (uint8_t)plen;
      memcpy(q, path, plen); q += plen;
      *q++ = 0xC0; *q++ = 0xBF; *q++ = 0xBE;
      q = base + DATA_OFF(j);
      q = frame_hdr(q, (uint32_t)mlen, 0, 0x1, 0);
      memcpy(q, msg, mlen);
      if (!ping) make_key(q + 7, (unsigned)((r * BATCH + j) * 7919u) % nkeys);
    }

  uint32_t sid = 3;
  uint64_t expect = 0, sent = 0;
  /* Warm up, and learn the steady-state output size of one batch. */
  for (int i = 0; i < 2000; i++) {
    uint8_t *buf = ring + (size_t)(i % RING) * BATCH * req_bytes;
    for (int j = 0; j < BATCH; j++, sid += 2) {
      set_sid(buf + HDR_OFF(j), sid);
      set_sid(buf + DATA_OFF(j), sid);
    }
    uint64_t n = feed(conn, buf, BATCH * req_bytes);
    if (i == 1000) expect = n;
    if (i > 1000 && n != expect) { fprintf(stderr, "output size moved: %lu vs %lu\n", (unsigned long)n, (unsigned long)expect); return 1; }
    sent += BATCH;
  }

  /* The sid-patching loop alone, to report it apart from the engine. */
  double patch_ns = 1e30;
  for (int tr = 0; tr < 50; tr++) {
    double t0 = now_ns();
    uint32_t s = 3;
    for (int i = 0; i < batches; i++) {
      uint8_t *buf = ring + (size_t)(i % RING) * BATCH * req_bytes;
      for (int j = 0; j < BATCH; j++, s += 2) {
        set_sid(buf + HDR_OFF(j), s);
        set_sid(buf + DATA_OFF(j), s);
      }
    }
    double d = (now_ns() - t0) / ((double)batches * BATCH);
    if (d < patch_ns) patch_ns = d;
  }

  double *ns = malloc(sizeof(double) * (size_t)trials);
  double best_ticks = 1e30;
  for (int tr = 0; tr < trials; tr++) {
    /* Keep the connection send window open: each reply consumes it, and a
     * real client tops it up with WINDOW_UPDATE now and then. */
    if ((int64_t)F(conn, BA_C_SEND_WINDOW) < (1 << 28)) {
      uint8_t wu[13], *q = frame_hdr(wu, 4, 8, 0, 0);
      uint32_t add = (1u << 30) - (uint32_t)F(conn, BA_C_SEND_WINDOW);
      *q++ = (uint8_t)(add >> 24); *q++ = (uint8_t)(add >> 16); *q++ = (uint8_t)(add >> 8); *q++ = (uint8_t)add;
      feed(conn, wu, 13);
    }
    if (sid > 0x70000000u) { fprintf(stderr, "stream ids exhausted: lower TRIALS x BATCHES\n"); return 1; }
    double t0 = now_ns();
    uint64_t c0 = __rdtsc();
    for (int i = 0; i < batches; i++) {
      uint8_t *buf = ring + (size_t)(i % RING) * BATCH * req_bytes;
      for (int j = 0; j < BATCH; j++, sid += 2) {
        set_sid(buf + HDR_OFF(j), sid);
        set_sid(buf + DATA_OFF(j), sid);
      }
      if (h2_process(conn, buf, BATCH * req_bytes) < 0 || kv_grpc_run_batch(conn) < 0 ||
          conn_window_refill(conn) < 0) { fprintf(stderr, "engine failed\n"); return 1; }
      /* A batch's output is its replies, plus — once per ~32 MiB of request
       * DATA — the 13-byte WINDOW_UPDATE that restores the connection's
       * receive window (conn_window_refill). Anything else is an error path. */
      if (F(conn, BA_C_OUT_LEN) != expect && F(conn, BA_C_OUT_LEN) != expect + 13) {
        fprintf(stderr, "output size moved in trial: %lu vs %lu\n", (unsigned long)F(conn, BA_C_OUT_LEN),
                (unsigned long)expect);
        return 1;
      }
      F(conn, BA_C_OUT_LEN) = 0;
    }
    uint64_t c1 = __rdtsc();
    double d = (now_ns() - t0) / ((double)batches * BATCH);
    ns[tr] = d;
    double ticks = (double)(c1 - c0) / ((double)batches * BATCH);
    if (ticks < best_ticks) best_ticks = ticks;
    sent += (uint64_t)batches * BATCH;
  }
  qsort(ns, (size_t)trials, sizeof(double), cmp_d);
  if (F(conn, BA_C_PEND_HEAD) != 0 || F(conn, BA_C_NSTREAMS) != 0) { fprintf(stderr, "streams left over\n"); return 1; }
  /* The core's real cycle time right now (a shared host moves it): the
   * minimum of several runs of a one-cycle-per-iteration loop. */
  double cyc = 1e30;
  for (int i = 0; i < 9; i++) {
    double t0 = now_ns();
    calib_add(100000000ull);
    double d = (now_ns() - t0) / 100e6;
    if (d < cyc) cyc = d;
  }
  printf("%s: %.2f ns/request min = %.0f cycles (cycle %.4f ns), %.2f median, %.2f p90; sid patch %.2f ns; "
         "%lu bytes out per batch; output digest %016lx; %lu requests, server counted %lu; hash %s\n",
         mode, ns[0], ns[0] / cyc, cyc, ns[trials / 2], ns[trials * 9 / 10], patch_ns,
         (unsigned long)expect, (unsigned long)out_digest, (unsigned long)sent + 1,
         (unsigned long)F(w, BA_W_REQUESTS),
         !kv_hash_mode_get ? "siphash (fixed)" : kv_hash_mode_get() ? "aes" : "siphash");
  printf("layout: %s\n", burst ? "burst (all HEADERS, then all DATA)" : "pairs (DATA behind its HEADERS)");
  (void)best_ticks;
  (void)first_out;
  return 0;
}
