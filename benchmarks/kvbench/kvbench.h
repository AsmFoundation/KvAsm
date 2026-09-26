/*
 * kvbench.h — shared declarations of kvbench, the single load generator used
 * for every system in benchmarks/.
 *
 * Why one tool: comparing KvNode measured with h2load against Redis measured
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
 * memcached meta no-op, gRPC health check — the per-request floor. */
enum { OP_GET, OP_SET, OP_PING, OP_COUNT };

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

enum { WL_MIXED, WL_PING };

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
  /** Moves pending protocol output into c->out (session-based protocols). @returns 0 or -1. */
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
