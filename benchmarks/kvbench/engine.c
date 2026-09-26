/*
 * engine.c — client threads, connections and the event loop.
 *
 * Every worker thread owns a set of connections and a private epoll loop, so
 * the hot path takes no locks. One loop iteration: (1) issue requests,
 * (2) flush every connection with a single send(), (3) wait for readiness,
 * (4) receive and parse, completing requests. Sends and receives are thereby
 * batched identically for every protocol.
 *
 * Load disciplines:
 *   closed loop  keep exactly `depth` requests in flight per connection
 *                (peak throughput; latency then reflects the queue depth)
 *   open loop    Poisson arrivals at a fixed total rate; latency is measured
 *                from each request's *intended* send time, so a stalled
 *                server cannot hide queueing delay (coordinated omission)
 *   preload      each connection writes its own contiguous key range once
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "engine.h"

#define RBUF_BYTES (256 * 1024)
#define MIN_IN_FREE (64 * 1024)

static const Proto *g_proto;
static Worker *g_workers;
static int g_nworkers;
static pthread_barrier_t g_ready;

/* Server address. Numeric only (inet_pton), deliberately: getaddrinfo in a
 * static binary may need glibc's NSS plugins at run time, which a scratch
 * container does not have — and the benchmark always targets an IP. */
static struct sockaddr_storage g_sa;
static socklen_t g_salen;

/**
 * Verifies a value returned by a GET: exact length, the key id in the first
 * 8 bytes (catches a value belonging to another key), then the template.
 * @param key  requested key id
 * @param v    value bytes
 * @param len  value length
 * @returns 1 if exactly the bytes written for this key, else 0
 */
int value_ok(uint64_t key, const uint8_t *v, size_t len) {
  if (len != g_opt.value_size) return 0;
  uint64_t stored;
  memcpy(&stored, v, 8);
  return stored == key && memcmp(v + 8, g_value_template + 8, len - 8) == 0;
}

/**
 * Makes room for n more output bytes, first reclaiming already-sent bytes.
 * @param c  connection
 * @param n  bytes about to be appended
 * @remarks Exits the process on allocation failure: a benchmark that silently
 *          drops requests would report wrong numbers.
 */
void out_reserve(Conn *c, size_t n) {
  if (c->out_off > 0 && c->out_off == c->out_len) c->out_off = c->out_len = 0;
  if (c->out_len + n <= c->out_cap) return;
  size_t cap = c->out_cap ? c->out_cap * 2 : 65536;
  while (cap < c->out_len + n) cap *= 2;
  uint8_t *p = realloc(c->out, cap);
  if (!p) {
    fprintf(stderr, "kvbench: out of memory\n");
    exit(2);
  }
  c->out = p;
  c->out_cap = cap;
}

/**
 * Records a fatal connection problem; the run is then reported as failed.
 * @param c     connection
 * @param what  description (the first one per worker is kept)
 */
void conn_fail(Conn *c, const char *what) {
  Worker *w = c->w;
  w->errors++;
  if (!w->error_text[0]) snprintf(w->error_text, sizeof w->error_text, "%s", what);
  c->fd_dead = 1;
}

/**
 * Accounts one finished request.
 * @param c       connection
 * @param op      operation
 * @param key     key id (unused here; protocols verified the value already)
 * @param t0      start time (intended time in open loop)
 * @param now     completion time (receive timestamp of the batch)
 * @param status  ST_* outcome
 * @remarks Misses, bad values and errors are counted in every phase, since
 *          any of them invalidates the run; latency and throughput only in
 *          the measured phase.
 */
void complete(Conn *c, int op, uint64_t key, uint64_t t0, uint64_t now, int status) {
  (void)key;
  Worker *w = c->w;
  c->inflight--;
  w->done_all++;
  atomic_store_explicit(&w->progress, w->done_all, memory_order_relaxed);
  if (status == ST_MISS) w->misses++;
  else if (status == ST_BAD_VALUE) w->bad_values++;
  else if (status == ST_ERROR) w->errors++;
  if (atomic_load_explicit(&g_phase, memory_order_relaxed) == PHASE_MEASURE) {
    w->done[op]++;
    hist_record(&w->hist[op], now > t0 ? now - t0 : 0);
  }
}

/**
 * Registers the connection's epoll interest.
 * @param c        connection
 * @param writing  also wait for writability
 */
static void set_interest(Conn *c, int writing) {
  struct epoll_event ev = {.events = EPOLLIN | (writing ? EPOLLOUT : 0), .data = {.ptr = c}};
  epoll_ctl(c->w->epfd, EPOLL_CTL_MOD, c->fd, &ev);
  c->epoll_out = writing;
}

/**
 * Connects one socket (blocking connect, then non-blocking I/O).
 * @param c  connection (w, global_index already set)
 * @returns 0, or -1 with the reason recorded
 */
static int conn_open(Conn *c) {
  int fd = socket(g_sa.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&g_sa, g_salen) != 0) {
    if (fd >= 0) close(fd);
    c->fd = -1;
    conn_fail(c, "connect failed");
    return -1;
  }
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  c->fd = fd;
  uint32_t ring = 2;
  while (ring < (uint32_t)g_opt.depth) ring <<= 1;
  c->fifo = calloc(ring, sizeof *c->fifo);
  c->fifo_mask = ring - 1;
  c->in_cap = 2 * MIN_IN_FREE;
  c->in = malloc(c->in_cap);
  if (!c->fifo || !c->in) {
    conn_fail(c, "out of memory");
    return -1;
  }
  struct epoll_event ev = {.events = EPOLLIN, .data = {.ptr = c}};
  epoll_ctl(c->w->epfd, EPOLL_CTL_ADD, fd, &ev);
  if (g_proto->attach && g_proto->attach(c) != 0) {
    conn_fail(c, "protocol setup failed");
    return -1;
  }
  return 0;
}

/**
 * Sends everything pending (after the protocol serialized its frames).
 * @param c  connection
 * @remarks A short write keeps the rest and waits for EPOLLOUT; nothing is
 *          ever dropped.
 */
static void conn_flush(Conn *c) {
  if (c->fd_dead) return;
  if (g_proto->collect && g_proto->collect(c) != 0) {
    conn_fail(c, "protocol send failed");
    return;
  }
  while (c->out_off < c->out_len) {
    ssize_t n = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_NOSIGNAL);
    if (n > 0) {
      c->out_off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      if (!c->epoll_out) set_interest(c, 1);
      return;
    }
    conn_fail(c, "send failed");
    return;
  }
  c->out_off = c->out_len = 0;
  if (c->epoll_out) set_interest(c, 0);
}

/**
 * Receives until the socket is drained, handing each chunk to the protocol.
 * @param c  connection
 */
static void conn_read(Conn *c) {
  while (!c->fd_dead) {
    uint8_t *buf;
    size_t cap;
    if (g_proto->parses_in_place) {
      if (c->in_cap - c->in_len < MIN_IN_FREE) {
        size_t ncap = c->in_cap * 2;
        uint8_t *p = realloc(c->in, ncap);
        if (!p) {
          conn_fail(c, "out of memory");
          return;
        }
        c->in = p;
        c->in_cap = ncap;
      }
      buf = c->in + c->in_len;
      cap = c->in_cap - c->in_len;
    } else {
      buf = c->w->rbuf;
      cap = RBUF_BYTES;
    }
    ssize_t n = recv(c->fd, buf, cap, 0);
    if (n > 0) {
      if (g_proto->input(c, buf, (size_t)n, now_ns()) != 0) return;
      if ((size_t)n < cap) return; /* drained */
      continue;
    }
    if (n == 0) {
      conn_fail(c, "server closed the connection");
      return;
    }
    if (errno == EINTR) continue;
    if (errno != EAGAIN && errno != EWOULDBLOCK) conn_fail(c, "recv failed");
    return;
  }
}

/**
 * Picks and queues one workload request.
 * @param w   worker (RNG)
 * @param c   connection
 * @param t0  start time to attribute
 */
static void issue_one(Worker *w, Conn *c, uint64_t t0) {
  int op;
  uint64_t key = 0;
  if (g_opt.workload == WL_PING) {
    op = OP_PING;
  } else {
    op = g_opt.read_ratio >= 1.0 || rng_unit(&w->rng) < g_opt.read_ratio ? OP_GET : OP_SET;
    key = g_opt.zipf ? scramble(zipf_next(&g_zipf, &w->rng), g_opt.keys)
                     : rng_next(&w->rng) % g_opt.keys;
  }
  c->inflight++;
  g_proto->issue(c, op, key, t0);
}

/**
 * Exponential inter-arrival gap for one connection's Poisson process.
 * @param w  worker (RNG, per-connection rate)
 * @returns gap in nanoseconds (at least 1)
 */
static uint64_t poisson_gap(Worker *w) {
  double gap = -log(1.0 - rng_unit(&w->rng)) * 1e9 / w->conn_rate;
  return gap < 1.0 ? 1 : (uint64_t)gap;
}

/**
 * Worker thread: connect, wait for the start barrier, run the loop.
 * @param arg  Worker*
 * @returns NULL
 */
static void *worker_main(void *arg) {
  Worker *w = arg;
  for (int i = 0; i < w->nconns; i++) conn_open(&w->conns[i]);
  pthread_barrier_wait(&g_ready);

  const int open_loop = !w->preload && g_opt.rate > 0;
  uint64_t start = now_ns();
  for (int i = 0; open_loop && i < w->nconns; i++) w->conns[i].next_due = start + poisson_gap(w);

  struct epoll_event events[64];
  while (atomic_load_explicit(&g_phase, memory_order_relaxed) != PHASE_STOP && !w->errors) {
    uint64_t now = now_ns();
    uint64_t next_due = UINT64_MAX;
    int active = 0;
    for (int i = 0; i < w->nconns; i++) {
      Conn *c = &w->conns[i];
      if (w->preload) {
        while (c->inflight < (uint32_t)g_opt.depth && c->next_key < c->end_key) {
          c->inflight++;
          g_proto->issue(c, OP_SET, c->next_key, now);
          c->next_key++;
        }
        active |= c->inflight > 0 || c->next_key < c->end_key;
      } else if (!open_loop) {
        while (c->inflight < (uint32_t)g_opt.depth) issue_one(w, c, now);
      } else {
        while (c->next_due <= now) {
          if (c->inflight >= (uint32_t)g_opt.depth) {
            /* Full: the request waits and keeps its intended time, so the
             * delay shows up in its latency; count it once. */
            if (!c->stalled) w->stalls++;
            c->stalled = 1;
            break;
          }
          c->stalled = 0;
          issue_one(w, c, c->next_due);
          c->next_due += poisson_gap(w);
        }
        if (c->next_due < next_due) next_due = c->next_due;
      }
      conn_flush(c);
    }
    if (w->preload && !active) break;

    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100 * 1000000};
    if (open_loop) {
      uint64_t t = now_ns();
      uint64_t wait = next_due > t ? next_due - t : 0;
      if (wait < 100000000ull) timeout.tv_nsec = (long)wait;
    }
    int n = epoll_pwait2(w->epfd, events, 64, &timeout, NULL);
    for (int i = 0; i < n; i++) {
      Conn *c = events[i].data.ptr;
      if (events[i].events & (EPOLLERR | EPOLLHUP)) {
        conn_fail(c, "connection error");
        continue;
      }
      if (events[i].events & EPOLLOUT) conn_flush(c);
      if (events[i].events & EPOLLIN) conn_read(c);
    }
  }
  for (int i = 0; i < w->nconns; i++) {
    Conn *c = &w->conns[i];
    if (g_proto->detach) g_proto->detach(c);
    if (c->fd >= 0) close(c->fd);
    free(c->out);
    free(c->in);
    free(c->fifo);
  }
  return NULL;
}

/**
 * Parses the numeric server address once and selects the protocol.
 * @returns 0, or -1 (message printed) for a non-numeric host or bad port
 */
int engine_resolve(void) {
  char *end;
  unsigned long port = strtoul(g_opt.port, &end, 10);
  if (*g_opt.port == 0 || *end != 0 || port == 0 || port > 65535) {
    fprintf(stderr, "kvbench: bad port %s\n", g_opt.port);
    return -1;
  }
  struct sockaddr_in *v4 = (struct sockaddr_in *)&g_sa;
  struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&g_sa;
  memset(&g_sa, 0, sizeof g_sa);
  if (inet_pton(AF_INET, g_opt.host, &v4->sin_addr) == 1) {
    v4->sin_family = AF_INET;
    v4->sin_port = htons((uint16_t)port);
    g_salen = sizeof *v4;
  } else if (inet_pton(AF_INET6, g_opt.host, &v6->sin6_addr) == 1) {
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons((uint16_t)port);
    g_salen = sizeof *v6;
  } else {
    fprintf(stderr, "kvbench: --host must be a numeric IPv4/IPv6 address: %s\n", g_opt.host);
    return -1;
  }
  g_proto = g_opt.proto == PROTO_RESP ? &proto_resp
            : g_opt.proto == PROTO_MEMCACHE ? &proto_memcache
                                            : &proto_grpc;
  return 0;
}

/**
 * Waits until the server port accepts TCP connections.
 * @param seconds  give up after this long
 * @returns 0 when a connection succeeded, -1 on timeout
 */
int engine_wait_ready(double seconds) {
  uint64_t deadline = now_ns() + (uint64_t)(seconds * 1e9);
  do {
    int fd = socket(g_sa.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&g_sa, g_salen) == 0) {
      close(fd);
      return 0;
    }
    if (fd >= 0) close(fd);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 50 * 1000000};
    nanosleep(&pause, NULL);
  } while (now_ns() < deadline);
  return -1;
}

/**
 * Starts the worker threads; returns once every connection is established.
 * @param preload  1: write each key once instead of running the workload
 * @returns 0, or -1 if any connection failed (details in engine_join totals)
 */
int engine_start(int preload) {
  g_nworkers = g_opt.threads;
  g_workers = calloc((size_t)g_nworkers, sizeof *g_workers);
  if (!g_workers) return -1;
  pthread_barrier_init(&g_ready, NULL, (unsigned)g_nworkers + 1);
  int per = g_opt.connections / g_nworkers, extra = g_opt.connections % g_nworkers, next = 0;
  for (int t = 0; t < g_nworkers; t++) {
    Worker *w = &g_workers[t];
    w->id = t;
    w->preload = preload;
    w->rng = (g_opt.seed + 1) * 0x9E3779B97F4A7C15ull ^ (uint64_t)(t + 1) * 0xD1B54A32D192ED03ull;
    if (!w->rng) w->rng = 1;
    w->epfd = epoll_create1(EPOLL_CLOEXEC);
    w->nconns = per + (t < extra);
    w->conns = calloc((size_t)w->nconns, sizeof *w->conns);
    w->rbuf = malloc(RBUF_BYTES);
    w->conn_rate = g_opt.rate / g_opt.connections;
    for (int i = 0; i < w->nconns; i++) {
      Conn *c = &w->conns[i];
      c->w = w;
      c->fd = -1;
      c->global_index = next++;
      /* Preload: connection k writes keys [k*K/C, (k+1)*K/C). */
      c->next_key = (uint64_t)c->global_index * g_opt.keys / (uint64_t)g_opt.connections;
      c->end_key = (uint64_t)(c->global_index + 1) * g_opt.keys / (uint64_t)g_opt.connections;
    }
    pthread_create(&w->thread, NULL, worker_main, w);
  }
  pthread_barrier_wait(&g_ready);
  for (int t = 0; t < g_nworkers; t++)
    if (g_workers[t].errors) return -1;
  return 0;
}

/**
 * Completions so far across all workers (any phase), for the timeline.
 * @returns sum of the workers' published progress counters
 * @remarks Callable from the main thread while workers run: each counter is
 *          read with a relaxed atomic load; the sum is a consistent-enough
 *          snapshot for one-second throughput samples.
 */
uint64_t engine_progress(void) {
  uint64_t sum = 0;
  for (int i = 0; i < g_nworkers; i++)
    sum += atomic_load_explicit(&g_workers[i].progress, memory_order_relaxed);
  return sum;
}

/**
 * Joins the workers and sums their counters and histograms.
 * @param t  receives the totals (fully overwritten)
 */
void engine_join(Totals *t) {
  memset(t, 0, sizeof *t);
  for (int i = 0; i < g_nworkers; i++) {
    Worker *w = &g_workers[i];
    pthread_join(w->thread, NULL);
    for (int op = 0; op < OP_COUNT; op++) {
      t->done[op] += w->done[op];
      hist_merge(&t->hist[op], &w->hist[op]);
    }
    t->done_all += w->done_all;
    t->misses += w->misses;
    t->bad_values += w->bad_values;
    t->errors += w->errors;
    t->stalls += w->stalls;
    if (!t->error_text[0] && w->error_text[0]) memcpy(t->error_text, w->error_text, sizeof t->error_text);
    close(w->epfd);
    free(w->conns);
    free(w->rbuf);
  }
  pthread_barrier_destroy(&g_ready);
  free(g_workers);
  g_workers = NULL;
}
