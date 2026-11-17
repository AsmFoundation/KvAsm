/*
 * kvbench.h — shared declarations of kvbench, the single load generator used
 * for every system in benchmarks/.
 *
 * Why one tool: comparing KvAsm measured with h2load against Redis measured
 * with redis-benchmark would fold the tools' own efficiency into the result.
 * kvbench drives every system with the same threads, event loop, workload
 * generator, pipelining rule, latency histogram and response validation; only
 * the wire encoding differs (RESP, memcached meta, gRPC over HTTP/2), because
 * that difference is part of each system. The standard tools are still run
 * against kvbench in the "validation" experiment to show it handicaps no one.
 */
#ifndef KVBENCH_H
#define KVBENCH_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* Keys are "key:" + 12 zero-padded digits: exactly the format redis-benchmark
 * uses for __rand_int__ (-r), so both tools address the same preloaded data. */
#define KEY_BYTES 16
#define KEY_PREFIX "key:"

/* Protocols (one per system family). */
enum { PROTO_RESP, PROTO_MEMCACHE, PROTO_GRPC };

/* Operations. PING is each protocol's cheapest round trip: RESP PING,
 * memcached meta no-op, gRPC health check — the per-request floor. UPLOAD
 * and DOWNLOAD are KvAsm's streaming calls (gRPC only): a whole value moved
 * as a stream of chunks, one transfer per request. */
enum { OP_GET, OP_SET, OP_PING, OP_UPLOAD, OP_DOWNLOAD, OP_COUNT };

/* Outcome of one request, as judged by the client. */
enum { ST_OK, ST_MISS, ST_BAD_VALUE, ST_ERROR };

/* Measurement phases, driven by the main thread. */
enum { PHASE_WARMUP, PHASE_MEASURE, PHASE_STOP };

/* ------------------------------------------------------------------------- */
/* Latency histogram                                                          */
/* ------------------------------------------------------------------------- */

/* Log-linear buckets: exact below 64 ns, then 64 sub-buckets per power of two
 * (relative error <= 1/64 = 1.6%), up to 2^63 ns. Fixed size, no allocation,
 * mergeable by addition — per-thread histograms are merged at the end. */
#define HIST_SUB 64
#define HIST_BUCKETS ((63 - 6 + 1) * HIST_SUB + HIST_SUB)

/** Latency histogram in nanoseconds. */
typedef struct {
  uint64_t count[HIST_BUCKETS];
  uint64_t total;   /**< number of samples */
  uint64_t max;     /**< exact maximum */
  double sum;       /**< for the mean */
} Hist;

void hist_record(Hist *h, uint64_t ns);
void hist_merge(Hist *dst, const Hist *src);
uint64_t hist_percentile(const Hist *h, double pct);
size_t hist_bucket(uint64_t ns);
uint64_t hist_bucket_value(size_t index);

/* ------------------------------------------------------------------------- */
/* Randomness and key choice                                                  */
/* ------------------------------------------------------------------------- */

/**
 * xorshift64* — fast, per-thread, deterministic from the seed.
 * @param s  state (never 0)
 * @returns the next 64-bit value
 */
static inline uint64_t rng_next(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1Dull;
}

/**
 * Uniform double in [0, 1) with 53 random bits.
 * @param s  RNG state
 * @returns the sample
 */
static inline double rng_unit(uint64_t *s) { return (double)(rng_next(s) >> 11) * 0x1.0p-53; }

/** YCSB-style Zipfian generator (Gray et al.), ranks in [0, n). */
typedef struct {
  uint64_t n;
  double theta, alpha, zetan, eta, half_pow_theta;
} Zipf;

void zipf_init(Zipf *z, uint64_t n, double theta);
uint64_t zipf_next(const Zipf *z, uint64_t *rng);
uint64_t scramble(uint64_t rank, uint64_t n);

/**
 * Writes the 16-byte key for an id: "key:" + 12 zero-padded decimal digits.
 * @param dst  16 writable bytes (no terminator written)
 * @param id   key id, < 10^12
 */
void key_format(char *dst, uint64_t id);

/* ------------------------------------------------------------------------- */
/* Options (immutable after parsing) and shared state                         */
/* ------------------------------------------------------------------------- */

/* Workloads: GET/SET mix, protocol no-op, or one streaming call repeated
 * (every request moves one whole value of --value-size bytes). */
enum { WL_MIXED, WL_PING, WL_UPLOAD, WL_DOWNLOAD };

/** Command-line options; see usage() in main.c for meanings and defaults. */
typedef struct {
  int proto;
  const char *host;
  const char *port;
  int threads;
  int connections;
  int depth;
  uint64_t keys;
  uint32_t value_size;
  double read_ratio;
  int workload;
  int zipf;
  double zipf_theta;
  double rate;
  double warmup;
  double duration;
  int load;          /**< 0 = no preload, 1 = preload then measure, 2 = preload only */
  double settle;
  const char *cg_cpu;
  const char *cg_mem;
  uint64_t seed;
  const char *label;
  double wait_ready;
  double interval;   /**< timeline sampling period in seconds */
  /**
   * 1 = client threads never sleep: epoll_pwait2 with a zero timeout, so a
   * server's send finds no sleeping waiter and pays no cross-CPU wakeup.
   * Diagnostic only (it burns every client CPU): on one machine over
   * loopback the server's sendto runs the client's TCP receive and wakes the
   * client thread from the server's own CPU — measured at about half of a
   * KvAsm core under WSL2, where that wakeup is a VM-exiting IPI. A remote
   * client is woken by its own NIC instead, so busy-polling approximates the
   * server's cost in production. Never mix busy-poll and blocking runs in
   * one comparison.
   */
  int busy_poll;
  /**
   * Streaming workloads: value bytes per Upload message, and the
   * chunk_size a Download asks for (0 = 64 KiB for uploads, the server's
   * default for downloads).
   */
  uint32_t chunk;
  /** Server process whose threads' schedstat gives server_cpu_s (0 = unused). */
  int server_pid;
} Options;

extern Options g_opt;
extern _Atomic int g_phase;
extern uint8_t *g_value_template;
extern Zipf g_zipf;

/**
 * Monotonic clock in nanoseconds (vDSO, ~20 ns per call).
 * @returns CLOCK_MONOTONIC as ns
 */
static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------------- */
/* Connections, workers, protocols                                            */
/* ------------------------------------------------------------------------- */

/** A request awaiting its response (FIFO protocols keep these in order). */
typedef struct {
  uint64_t key;
  uint64_t t0;      /**< start time: send time (closed loop) or intended time (open loop) */
  uint8_t op;
} Pending;

typedef struct Worker Worker;

/** One client connection, owned by exactly one worker thread. */
typedef struct Conn {
  int fd;
  Worker *w;
  int global_index;         /**< across all workers; selects the preload key range */
  uint8_t *out;             /**< bytes to send: [out_off, out_len) */
  size_t out_off, out_len, out_cap;
  uint8_t *in;              /**< received bytes not yet parsed (RESP/memcache) */
  size_t in_len, in_cap;
  Pending *fifo;            /**< ring of in-flight requests (RESP/memcache) */
  uint32_t fifo_mask, head, tail;
  uint32_t inflight;
  int epoll_out;            /**< EPOLLOUT currently registered */
  int fd_dead;              /**< a fatal error happened; no more I/O */
  void *proto_state;        /**< protocol-private (gRPC session) */
  uint64_t next_due;        /**< open loop: intended time of the next request */
  int stalled;              /**< open loop: the due request is waiting for room */
  uint64_t next_key, end_key; /**< preload: keys still to write on this connection */
} Conn;

/** A client thread: private event loop, connections, counters, histograms. */
struct Worker {
  int id;
  pthread_t thread;
  int epfd;
  Conn *conns;
  int nconns;
  uint64_t rng;
  int preload;              /**< this run writes the key range instead of the workload */
  double conn_rate;         /**< open loop: requests per second per connection */
  Hist hist[OP_COUNT];      /**< measured phase only */
  uint64_t done[OP_COUNT];  /**< completions in the measured phase */
  uint64_t done_all;        /**< completions in any phase (preload progress) */
  /** done_all published for the main thread's per-interval timeline; a
   *  relaxed atomic store is a plain mov on x86, so the hot path pays nothing. */
  _Atomic uint64_t progress;
  uint64_t misses, bad_values, errors;
  uint64_t stalls;          /**< open loop: a due request found the connection full */
  char error_text[200];
  uint8_t *rbuf;            /**< receive buffer for protocols that parse elsewhere */
};

/** Protocol module: request encoding and incremental response parsing. */
typedef struct {
  const char *name;
  /** Sets up protocol state after connect. @returns 0 or -1. */
  int (*attach)(Conn *c);
  /** Frees protocol state. */
  void (*detach)(Conn *c);
  /** Queues one request (output goes to c->out, or into the protocol session). */
  void (*issue)(Conn *c, int op, uint64_t key, uint64_t t0);
  /** Consumes received bytes, completing requests. @returns 0 or -1 (fatal protocol error). */
  int (*input)(Conn *c, const uint8_t *data, size_t len, uint64_t now);
  /**
   * Moves pending protocol output into c->out (session-based protocols).
   * @returns 0 when all of it moved, 1 when it stopped early to let the
   *          engine send first (call again after sending), -1 on error.
   */
  int (*collect)(Conn *c);
  /** True when the protocol receives into c->in itself (zero-copy parsing). */
  int parses_in_place;
} Proto;

extern const Proto proto_resp, proto_memcache, proto_grpc;

void complete(Conn *c, int op, uint64_t key, uint64_t t0, uint64_t now, int status);
int value_ok(uint64_t key, const uint8_t *v, size_t len);
void conn_fail(Conn *c, const char *what);
void out_reserve(Conn *c, size_t n);

/**
 * Appends bytes to a connection's output buffer.
 * @param c    connection
 * @param src  bytes
 * @param n    count
 */
static inline void out_put(Conn *c, const void *src, size_t n) {
  out_reserve(c, n);
  memcpy(c->out + c->out_len, src, n);
  c->out_len += n;
}

/**
 * Writes a value of g_opt.value_size bytes for a key: the key id (8 bytes,
 * little-endian) followed by the shared template. Every system stores and
 * returns exactly these bytes, so every GET can be verified completely.
 * @param dst  value_size writable bytes
 * @param key  key id
 */
static inline void value_fill(uint8_t *dst, uint64_t key) {
  memcpy(dst, g_value_template, g_opt.value_size);
  memcpy(dst, &key, 8);
}

/**
 * Writes bytes [off, off + n) of the value value_fill(key) would write,
 * without materializing the rest — how an Upload streams a value of any
 * size from the one shared template.
 * @param dst  n writable bytes
 * @param key  key id
 * @param off  offset in the value (off + n <= value_size)
 * @param n    count
 */
static inline void value_copy(uint8_t *dst, uint64_t key, uint64_t off, size_t n) {
  memcpy(dst, g_value_template + off, n);
  if (off < 8) {
    /* The range overlaps the key id: patch its covered bytes. */
    uint8_t id[8];
    memcpy(id, &key, 8);
    size_t end = off + n < 8 ? (size_t)(off + n) : 8;
    memcpy(dst, id + off, end - (size_t)off);
  }
}

/**
 * Compares received bytes with bytes [off, off + n) of the value stored for
 * key — how a Download is verified in place, chunk by chunk, with no copy.
 * @param p    received bytes
 * @param key  key id
 * @param off  their offset in the value (off + n <= value_size)
 * @param n    count
 * @returns 1 when every byte matches, else 0
 */
static inline int value_matches(const uint8_t *p, uint64_t key, uint64_t off, size_t n) {
  size_t head = 0;
  if (off < 8) {
    uint8_t id[8];
    memcpy(id, &key, 8);
    head = off + n < 8 ? n : 8 - (size_t)off;
    if (memcmp(p, id + off, head) != 0) return 0;
  }
  return memcmp(p + head, g_value_template + off + head, n - head) == 0;
}

/* ------------------------------------------------------------------------- */
/* CRC-32C (the streaming calls' checksum)                                    */
/* ------------------------------------------------------------------------- */

/**
 * CRC-32C (Castagnoli, zlib-style: pre/post inversion inside), continued
 * from crc (0 to start). SSE4.2 crc32 when the CPU has it, else a table.
 * @param crc  previous result, 0 to start
 * @param p    bytes
 * @param n    count
 * @returns the continued checksum
 */
uint32_t crc32c_update(uint32_t crc, const uint8_t *p, size_t n);

/**
 * The GF(2) operator that shifts a CRC over len zero bytes: x^(8*len) mod P.
 * @param len  byte count
 * @returns the operator, for crc32c_combine_op
 */
uint32_t crc32c_shift_op(uint64_t len);

/**
 * crc(A || B) from crc(A), crc(B) and the shift operator of |B|
 * (crc32c_shift_op(|B|)) — zlib's crc32_combine_op for Castagnoli. Lets a
 * client derive the CRC of "key id || shared template" per request from two
 * precomputed parts instead of re-reading the value.
 * @param crc_a  crc(A)
 * @param crc_b  crc(B)
 * @param op     crc32c_shift_op(len(B))
 * @returns crc(A || B)
 */
uint32_t crc32c_combine_op(uint32_t crc_a, uint32_t crc_b, uint32_t op);

/**
 * The CRC-32C of the value value_fill(key) writes, in O(1): the key id's 8
 * bytes combined with the precomputed CRC of the shared template's tail.
 * @param key  key id
 * @returns crc32c of the value stored for key
 */
uint32_t value_crc(uint64_t key);

/**
 * Precomputes what value_crc needs. Call on the main thread after
 * g_value_template is filled and before any worker starts.
 */
void value_crc_init(void);

/** Test hook: 1 forces crc32c_update's table path (see crc32c.c). */
extern int crc32c_force_table;

/* ------------------------------------------------------------------------- */
/* Streaming codec (grpc.c; exposed for the unit tests)                       */
/* ------------------------------------------------------------------------- */

/**
 * Generator of one Upload request stream, byte-exact and resumable at any
 * byte: gRPC-framed UploadRequest messages of --chunk data bytes each; the
 * first also carries key and total_size, the last the value's crc32c (where
 * the .NET client puts it). Nothing is materialized: data bytes come from
 * the shared template (value_copy), so a 64 MiB upload costs one copy of
 * its bytes into the socket buffer's staging area.
 */
typedef struct {
  uint64_t key;      /**< key id being uploaded */
  uint64_t off;      /**< value bytes emitted so far */
  uint32_t data_left; /**< data bytes left in the current message */
  uint32_t crc;      /**< value_crc(key), for the last message */
  uint8_t piece[48]; /**< pending non-data bytes: a message header or the crc field */
  uint8_t piece_len, piece_off;
  uint8_t stage;     /**< UP_PIECE, UP_DATA, UP_TAIL, UP_END (grpc.c) */
} UpGen;

/**
 * Length of every Upload request stream of this run (it depends only on
 * value_size and the chunk, never on the key).
 * @returns bytes of gRPC framing plus messages
 */
uint64_t upload_stream_len(void);

/**
 * Starts generating the Upload request stream for a key.
 * @param u    generator (fully overwritten)
 * @param key  key id
 */
void up_start(UpGen *u, uint64_t key);

/**
 * Emits the next bytes of the stream.
 * @param u    generator
 * @param dst  n writable bytes
 * @param n    bytes wanted
 * @returns bytes written: n, or fewer only at the end of the stream
 */
size_t up_emit(UpGen *u, uint8_t *dst, size_t n);

/**
 * Incremental parser and verifier of one Download response body (gRPC-framed
 * DownloadResponse messages), fed in whatever pieces DATA frames arrive.
 * Data bytes are compared with the expected value where they lie in the
 * receive buffer (value_matches); nothing is copied or buffered.
 */
typedef struct {
  uint64_t key;       /**< key id being downloaded */
  uint64_t received;  /**< value bytes received (offset of the next one) */
  uint64_t total;     /**< total_size from the first message; UINT64_MAX until seen */
  uint64_t field_left; /**< bytes left in the data field being received */
  uint64_t acc;       /**< varint / fixed32 being assembled */
  uint32_t msg_left;  /**< bytes left in the current message */
  uint32_t crc;       /**< crc32c from the last message */
  uint8_t tok[5];     /**< gRPC message prefix being assembled */
  uint8_t tok_len;    /**< bytes of tok or of acc assembled so far */
  uint8_t state;      /**< DL_* (grpc.c) */
  uint8_t have_crc;   /**< the crc32c field arrived (it must be the last one) */
  uint8_t messages;   /**< messages started, saturating at 255 */
  uint8_t bad;        /**< framing or field-order violation: ST_ERROR */
  uint8_t mismatch;   /**< a data byte, total_size or crc32c was wrong: ST_BAD_VALUE */
} DlParse;

/**
 * Starts parsing a Download response body.
 * @param d    parser (fully overwritten)
 * @param key  key id requested
 */
void dl_start(DlParse *d, uint64_t key);

/**
 * Consumes the next piece of the response body.
 * @param d  parser
 * @param p  bytes
 * @param n  count
 */
void dl_feed(DlParse *d, const uint8_t *p, size_t n);

/**
 * Judges a Download whose stream ended cleanly (HTTP 200, grpc-status 0).
 * @param d  parser after the last dl_feed
 * @returns ST_OK, ST_BAD_VALUE (wrong bytes, size or checksum) or ST_ERROR
 *          (malformed or truncated body)
 */
int dl_judge(const DlParse *d);

/* FIFO helpers for request/response-ordered protocols (RESP, memcache). */

/**
 * Records a sent request. The ring is sized to the maximum in-flight count.
 * @param c   connection
 * @param op  operation
 * @param key key id
 * @param t0  start time
 */
static inline void fifo_push(Conn *c, int op, uint64_t key, uint64_t t0) {
  Pending *p = &c->fifo[c->tail++ & c->fifo_mask];
  p->op = (uint8_t)op;
  p->key = key;
  p->t0 = t0;
}

/**
 * Removes the oldest in-flight request.
 * @param c    connection
 * @param out  receives the request
 * @returns 1 if one was pending, 0 if the server answered more than asked
 */
static inline int fifo_pop(Conn *c, Pending *out) {
  if (c->head == c->tail) return 0;
  *out = c->fifo[c->head++ & c->fifo_mask];
  return 1;
}

#endif
