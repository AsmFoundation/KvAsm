/*
 * test_pool.c — the per-worker allocator (pool.asm) must honour malloc's
 * contract, because the HTTP/2 engine and the gRPC layer use it for every
 * per-connection and per-stream allocation (streams, request bodies, stream
 * maps, HPACK rings):
 *   - every pointer is 16-byte aligned;
 *   - blocks never overlap: a randomized model check fills every live block
 *     with its own pattern through thousands of malloc/calloc/realloc/free
 *     steps (small classes and the large malloc path) and re-verifies all of
 *     them at the end;
 *   - realloc preserves min(old, new) bytes, realloc(NULL, n) allocates,
 *     realloc(p, 0) frees; calloc zeroes and rejects overflowing products;
 *   - freed blocks are reused LIFO within a class (the cache-warmth property
 *     the pool exists for).
 */
#include "test_common.h"

void *kv_pool_malloc(size_t size, void *pool);
void kv_pool_free(void *ptr, void *pool);
void *kv_pool_calloc(size_t n, size_t size, void *pool);
void *kv_pool_realloc(void *ptr, size_t size, void *pool);
void kv_pool_destroy(void *pool);
extern const uint64_t kv_pool_struct_size;

/** One live block of the model. */
typedef struct {
  uint8_t *p;
  size_t size;
  uint8_t seed;
} Live;

/**
 * Fills a block with a pattern derived from its seed.
 * @param b  block
 */
static void fill(const Live *b) {
  for (size_t i = 0; i < b->size; i++) b->p[i] = (uint8_t)(b->seed + i * 7);
}

/**
 * Checks a block's first n bytes against its pattern.
 * @param b  block
 * @param n  bytes to check
 * @returns 1 if intact
 */
static int intact(const Live *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (b->p[i] != (uint8_t)(b->seed + i * 7)) return 0;
  return 1;
}

/**
 * Random allocation size: mostly small (the classes), sometimes large.
 * @param s  RNG state
 * @returns bytes
 */
static size_t rand_size(uint64_t *s) {
  uint64_t r = rng_next(s);
  return r % 10 == 0 ? 4000 + r % 20000 : r % 700;
}

/** Entry point. @returns 0 when every check passed. */
int main(void) {
  void *pool = calloc(1, kv_pool_struct_size);
  enum { SLOTS = 512, STEPS = 60000 };
  static Live live[SLOTS];
  uint64_t s = 42;

  for (int step = 0; step < STEPS; step++) {
    Live *b = &live[rng_next(&s) % SLOTS];
    int action = (int)(rng_next(&s) % 4);
    if (!b->p) {
      b->size = rand_size(&s);
      b->seed = (uint8_t)rng_next(&s);
      if (action == 0) {
        b->p = kv_pool_calloc(1, b->size, pool);
        CHECK(b->p != NULL);
        for (size_t i = 0; i < b->size; i++) CHECK(b->p[i] == 0);
      } else {
        b->p = kv_pool_malloc(b->size, pool);
        CHECK(b->p != NULL);
      }
      CHECK(((uintptr_t)b->p & 15) == 0);
      fill(b);
    } else if (action == 1) {
      size_t n = rand_size(&s);
      if (n == 0) n = 1; /* size 0 frees; covered separately */
      size_t keep = n < b->size ? n : b->size;
      CHECK(intact(b, b->size));
      b->p = kv_pool_realloc(b->p, n, pool);
      CHECK(b->p != NULL && ((uintptr_t)b->p & 15) == 0);
      CHECK(intact(b, keep)); /* preserved prefix */
      b->size = n;
      fill(b);
    } else {
      CHECK(intact(b, b->size));
      kv_pool_free(b->p, pool);
      b->p = NULL;
    }
  }
  for (int i = 0; i < SLOTS; i++) { /* nothing overlapped, nothing was lost */
    if (!live[i].p) continue;
    CHECK(intact(&live[i], live[i].size));
    kv_pool_free(live[i].p, pool);
    live[i].p = NULL;
  }

  /* LIFO reuse within a class, and edge cases */
  void *a = kv_pool_malloc(100, pool);
  kv_pool_free(a, pool);
  CHECK(kv_pool_malloc(90, pool) == a); /* same class: the block just freed */
  kv_pool_free(a, pool);
  CHECK(kv_pool_calloc((size_t)1 << 40, (size_t)1 << 40, pool) == NULL); /* overflow */
  void *r = kv_pool_realloc(NULL, 50, pool);
  CHECK(r != NULL);
  CHECK(kv_pool_realloc(r, 0, pool) == NULL); /* frees */
  kv_pool_free(NULL, pool);                   /* no-op */
  CHECK(kv_pool_malloc((size_t)-8, pool) == NULL); /* size + header would wrap */

  kv_pool_destroy(pool);
  free(pool);
  return test_finish("test_pool");
}
