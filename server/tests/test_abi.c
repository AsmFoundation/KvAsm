/*
 * test_abi.c — the C mirror (kvnode_abi.h) must match the NASM layouts.
 *
 * A mismatch here would make every other test (and the server's C-facing
 * callers) read the wrong fields, so this test runs first.
 */
#include "kvnode_abi.h"
#include "test_common.h"

/** Entry point. @return 0 when every layout fact matches. */
int main(void) {
  const uint64_t *L = kv_abi_layout;
  CHECK_EQ_U64(L[0], sizeof(KvRec));
  CHECK_EQ_U64(L[1], offsetof(KvRec, next));
  CHECK_EQ_U64(L[2], offsetof(KvRec, aux));
  CHECK_EQ_U64(L[3], offsetof(KvRec, key_len));
  CHECK_EQ_U64(L[4], offsetof(KvRec, val_len));
  CHECK_EQ_U64(L[5], offsetof(KvRec, data));
  CHECK_EQ_U64(L[6], sizeof(KvStats));
  CHECK_EQ_U64(L[7], offsetof(KvStats, count));
  CHECK_EQ_U64(L[8], offsetof(KvStats, freed));
  CHECK_EQ_U64(L[9], 64);  /* Bucket is exactly one cache line */
  CHECK_EQ_U64(L[10], 0);  /* version first */
  CHECK_EQ_U64(L[11], 8);  /* tags word */
  CHECK_EQ_U64(L[12], 16); /* slots */
  CHECK(L[13] % 64 == 0);  /* Table is a whole number of cache lines */
  CHECK_EQ_U64(L[14], 1472); /* observed line + private reclamation and cache lines (ADR 0011) */
  CHECK_EQ_U64(L[15], 32); /* HEntry: two per cache line, index * 32 in hcache.asm */
  CHECK(L[16] % 64 == 0);  /* HCache: whole lines (private, but aligned_alloc'd) */
  CHECK_EQ_U64(L[17], offsetof(KvStats, pinned));
  CHECK_EQ_U64(L[18], offsetof(KvStats, deferred));
  CHECK_EQ_U64(L[19], 0xABCDEF0123456789ull);
  return test_finish("test_abi");
}
