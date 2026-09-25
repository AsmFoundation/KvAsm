/*
 * test_common.h — minimal, dependency-free test support for the native tests.
 *
 * Why not a framework: the tests link directly against assembly objects and
 * must run inside the slim toolchain container; a 60-line header keeps them
 * self-contained. Every CHECK failure prints file:line and the failing
 * expression and makes the process exit non-zero, which fails `make test`
 * and therefore the Docker image build.
 */
#ifndef KVNODE_TEST_COMMON_H
#define KVNODE_TEST_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* the Makefile defines it; standalone builds get it here */
#endif
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/** Number of failed CHECKs so far; main() returns non-zero when it is > 0. */
static int g_test_failures;

/** Records a failure (without aborting) so one run reports every broken case. */
#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);          \
      if (++g_test_failures > 50) {                                            \
        fprintf(stderr, "too many failures, aborting\n");                      \
        exit(1);                                                               \
      }                                                                        \
    }                                                                          \
  } while (0)

/** Like CHECK but prints two 64-bit values, for "expected vs actual" cases. */
#define CHECK_EQ_U64(a, b)                                                     \
  do {                                                                         \
    uint64_t va_ = (uint64_t)(a), vb_ = (uint64_t)(b);                         \
    if (va_ != vb_) {                                                          \
      fprintf(stderr, "FAIL %s:%d: %s == %s (0x%016" PRIx64 " != 0x%016" PRIx64 \
              ")\n", __FILE__, __LINE__, #a, #b, va_, vb_);                    \
      if (++g_test_failures > 50) exit(1);                                     \
    }                                                                          \
  } while (0)

/** Prints the summary line and converts the failure count to an exit code. */
static inline int test_finish(const char *name) {
  if (g_test_failures) {
    fprintf(stderr, "%s: %d FAILED check(s)\n", name, g_test_failures);
    return 1;
  }
  printf("%s: all checks passed\n", name);
  return 0;
}

/**
 * xorshift64* PRNG — deterministic, so a failing random case reproduces.
 * @param s state (must be non-zero). @return next 64-bit value.
 */
static inline uint64_t rng_next(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1Dull;
}

/** Fills n bytes with PRNG output. */
static inline void rng_fill(uint64_t *s, void *dst, size_t n) {
  uint8_t *p = dst;
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rng_next(s);
}

/**
 * A buffer of `len` bytes that ends exactly at a PROT_NONE guard page, so any
 * read past the end faults immediately — used to prove "never overreads".
 * @param len payload length (<= one page). @param base_out receives the
 *        mapping base for guard_free().
 * @return pointer to the first payload byte.
 */
static inline uint8_t *guard_alloc(size_t len, void **base_out) {
  long page = sysconf(_SC_PAGESIZE);
  uint8_t *base = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (base == MAP_FAILED) { perror("mmap"); exit(2); }
  if (mprotect(base + page, (size_t)page, PROT_NONE) != 0) { perror("mprotect"); exit(2); }
  *base_out = base;
  return base + page - len;
}

/** Releases a guard_alloc mapping. */
static inline void guard_free(void *base) {
  munmap(base, (size_t)sysconf(_SC_PAGESIZE) * 2);
}

/** @return CLOCK_MONOTONIC in nanoseconds. */
static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
