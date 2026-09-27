/*
 * tlb_chase.c — does this VM's memory get TLB-reach from guest huge pages?
 *
 * A dependent pointer chase visits one cache line per 4 KiB page, in random
 * order, over a region of S bytes, twice: once on 4 KiB pages
 * (MADV_NOHUGEPAGE) and once on 2 MiB pages (MADV_HUGEPAGE, prefaulted with
 * writes). One line per page means every step needs a new translation:
 *   - if the hypervisor backs guest memory with 2 MiB pages too, the huge
 *     page run needs ~S/2MiB TLB entries and stays near cache latency until
 *     S is large;
 *   - if it backs it with 4 KiB pages, a TLB entry still covers only 4 KiB
 *     and both runs miss alike (huge pages then only shorten the walk).
 * Usage: tlb_chase [cpu]
 */
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define PAGE 4096ull
#define HUGE (2ull << 20)

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t rng(uint64_t *s) {
  *s ^= *s >> 12; *s ^= *s << 25; *s ^= *s >> 27;
  return *s * 0x2545F4914F6CDD1Dull;
}

/**
 * Maps size bytes, 2 MiB aligned, with or without huge pages, faulted in by
 * writes. *base receives the raw mapping (size + HUGE bytes) for munmap: the
 * aligned pointer must not be unmapped with the full length, or the unmap
 * would run past the mapping's end into whatever follows it.
 */
static unsigned char *region(size_t size, int huge, unsigned char **base) {
  unsigned char *m = mmap(NULL, size + HUGE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (m == MAP_FAILED) { perror("mmap"); exit(2); }
  *base = m;
  unsigned char *r = (unsigned char *)(((uintptr_t)m + HUGE - 1) & ~(HUGE - 1));
  madvise(r, size, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
  for (size_t o = 0; o < size; o += PAGE) r[o] = 1;
  return r;
}

/** Builds a random cyclic chase through one line per page; returns ns per step. */
static double chase(unsigned char *r, size_t size) {
  size_t pages = size / PAGE;
  size_t *perm = malloc(pages * sizeof *perm);
  for (size_t i = 0; i < pages; i++) perm[i] = i;
  uint64_t s = 0x1234567;
  for (size_t i = pages - 1; i > 0; i--) { /* Sattolo: one cycle */
    size_t j = rng(&s) % i, t = perm[i];
    perm[i] = perm[j];
    perm[j] = t;
  }
  /* line offset within each page varies, so the chase is not cache-set aligned */
  for (size_t i = 0; i < pages; i++) {
    size_t from = perm[i], to = perm[(i + 1) % pages];
    uintptr_t *slot = (uintptr_t *)(r + from * PAGE + (from % 63) * 64);
    *slot = (uintptr_t)(r + to * PAGE + (to % 63) * 64);
  }
  free(perm);
  uintptr_t *p = (uintptr_t *)(r + (0 % 63) * 64);
  for (size_t k = 0; k < pages; k++) p = (uintptr_t *)*p; /* warm */
  uint64_t steps = 4u << 20, t0 = now_ns();
  for (uint64_t k = 0; k < steps; k++) p = (uintptr_t *)*p;
  double ns = (double)(now_ns() - t0) / (double)steps;
  if (!p) puts("");
  return ns;
}

int main(int argc, char **argv) {
  int cpu = argc > 1 ? atoi(argv[1]) : 2;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof set, &set);
  static const size_t mib[] = {4, 16, 64, 256, 1024};
  printf("region    4K pages   2M pages   (ns per dependent step, one line per 4 KiB page)\n");
  for (int i = 0; i < 5; i++) {
    size_t size = mib[i] << 20;
    unsigned char *ma, *mb;
    unsigned char *a = region(size, 0, &ma), *b = region(size, 1, &mb);
    double na = chase(a, size), nb = chase(b, size);
    printf("%5zu MiB  %8.1f   %8.1f\n", mib[i], na, nb);
    fflush(stdout);
    munmap(ma, size + HUGE);
    munmap(mb, size + HUGE);
  }
  return 0;
}
