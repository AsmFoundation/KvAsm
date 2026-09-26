/*
 * kvnode_abi.h — C view of the KvNode assembly ABI (include/kvnode.inc).
 *
 * Used by the native tests and benchmarks to call the assembly modules. Every
 * struct here must match its NASM `struc` byte for byte; test_abi.c compares
 * sizes and offsets against values exported by the assembly
 * (kv_abi_layout), so a drift fails `make test` instead of corrupting memory.
 */
#ifndef KVNODE_ABI_H
#define KVNODE_ABI_H

#include <errno.h> /* store results are -errno values (-ENOSPC, -ENOMEM, -E2BIG) */
#include <stddef.h>
#include <stdint.h>

/** Stored record (immutable once published). Value follows the padded key. */
typedef struct KvRec {
  struct KvRec *next;  /* writer-only link */
  uint64_t aux;        /* writer-only (retire epoch / bake-off hash) */
  uint32_t key_len;
  uint32_t val_len;
  uint8_t data[];      /* key, zero-padded to 8, then value */
} KvRec;

/** @return pointer to the record's value bytes. */
static inline const uint8_t *kv_rec_value(const KvRec *r) {
  return r->data + (((size_t)r->key_len + 7) & ~(size_t)7);
}

/** Counter snapshot filled by kv_store_stats. */
typedef struct KvStats {
  uint64_t count, nbuckets, max_records, mem_used, mem_limit, limbo_count, epoch;
  uint64_t inserts, replaces, deletes, bfs, moves, full, freed;
} KvStats;

typedef struct KvTable KvTable; /* opaque */

/* siphash.asm / util.asm */
uint64_t kv_siphash128(const void *msg, size_t len, const uint64_t key[2], uint64_t out[2]);
int kv_keyeq(const void *a, const void *b, size_t len);
uint64_t kv_tag_match(uint64_t tags, uint64_t tag);

/* store.asm */
KvTable *kv_store_create(uint64_t capacity, uint64_t n_readers, const uint8_t seed[16],
                         uint64_t mem_limit);
void kv_store_destroy(KvTable *t);
uint64_t kv_store_hash(const KvTable *t, const void *key, size_t len, uint64_t out[2]);
const KvRec *kv_store_get(KvTable *t, const void *key, size_t len, uint64_t h1, uint64_t h2);
int64_t kv_store_put(KvTable *t, const void *key, size_t len, const void *val, size_t vlen,
                     uint64_t h1, uint64_t h2);
int64_t kv_store_delete(KvTable *t, const void *key, size_t len, uint64_t h1, uint64_t h2);
uint64_t kv_store_reclaim(KvTable *t);
void kv_store_stats(const KvTable *t, KvStats *out);
void kv_qsbr_online(KvTable *t, uint32_t rid);
void kv_qsbr_offline(KvTable *t, uint32_t rid);
void kv_qsbr_quiescent(KvTable *t, uint32_t rid);

/* cuckoo.asm — intrusive index API (used by the bake-off; caller serializes writers) */
const KvRec *kv_cuckoo_get(KvTable *t, const void *key, size_t len, uint64_t h1, uint64_t h2);
int64_t kv_cuckoo_upsert(KvTable *t, KvRec *rec, uint64_t h1, uint64_t h2); /* old ptr / 0 / -ENOSPC */
KvRec *kv_cuckoo_remove(KvTable *t, const void *key, size_t len, uint64_t h1, uint64_t h2);

/* cuckoo.asm — readers' tag-scan implementation (set before readers start) */
enum { KV_SCAN_SWAR = 0, KV_SCAN_SSE2 = 1 };
int kv_set_scan_mode(int mode); /* previous mode, or -EINVAL */

/* store.asm — prefetch stages of the batched GET path and cache invalidation */
void kv_store_prefetch(KvTable *t, uint64_t h1, uint64_t h2);
void kv_store_prefetch_records(KvTable *t, uint64_t h1, uint64_t h2);
int64_t kv_store_enable_inval(KvTable *t, uint32_t bits); /* 0, -EINVAL, -ENOMEM */

/* hcache.asm — one reader thread's private L1/L2 record cache */
typedef struct KvHCache KvHCache; /* opaque */
KvHCache *kv_hcache_create(KvTable *t, uint32_t l1_bits, uint32_t l2_bits);
void kv_hcache_destroy(KvHCache *c);
const KvRec *kv_hcache_get(KvHCache *c, const void *key, size_t len, uint64_t h1, uint64_t h2);
void kv_hcache_stats(const KvHCache *c, uint64_t out[4]); /* l1 hits, l2 hits, table, stale */

/* abi.asm — layout facts exported by the assembler for test_abi.c */
extern const uint64_t kv_abi_layout[];

#endif
