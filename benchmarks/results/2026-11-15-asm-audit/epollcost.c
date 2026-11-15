/* epollcost.c — what the event loop's pre-poll costs: nanoseconds per
 * epoll_wait(timeout = 0) that finds nothing ready, against the two QSBR
 * transitions the alternative pays (a plain store, then an XCHG).
 *
 * Usage: epollcost [ITERATIONS]   (run pinned: taskset -c N ./epollcost)
 * Prints the minimum over 7 trials of each loop: the minimum is the cost
 * without interference from other work on the host. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char **argv) {
  long n = argc > 1 ? atol(argv[1]) : 1000000;
  int ep = epoll_create1(0);
  /* 64 registered, idle descriptors: the poll's cost must not depend on how
   * many connections exist, only on how many are ready (none). */
  for (int i = 0; i < 64; i++) {
    int fd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
  }
  struct epoll_event evs[256];
  volatile uint64_t slot = 0;
  uint64_t epoch = 1;
  double best_poll = 1e30, best_qsbr = 1e30;
  for (int t = 0; t < 7; t++) {
    double t0 = now_ns();
    for (long i = 0; i < n; i++)
      if (epoll_wait(ep, evs, 256, 0) != 0) return 1;
    double d = (now_ns() - t0) / n;
    if (d < best_poll) best_poll = d;
    t0 = now_ns();
    for (long i = 0; i < n; i++) {
      slot = 0;                                   /* kv_qsbr_offline */
      __atomic_exchange_n((uint64_t *)&slot, epoch, __ATOMIC_SEQ_CST); /* kv_qsbr_online */
    }
    d = (now_ns() - t0) / n;
    if (d < best_qsbr) best_qsbr = d;
  }
  printf("epoll_wait(timeout 0), nothing ready: %.1f ns per call\n", best_poll);
  printf("QSBR offline + online (store + XCHG):  %.1f ns per pair\n", best_qsbr);
  return 0;
}
