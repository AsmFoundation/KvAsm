/*
 * snapgen.h — the deterministic dataset shared by mksnap (which writes it as
 * a snapshot file) and itest_grpc --expect-snapshot (which reads it back
 * through gRPC). Keeping the generator in one place guarantees writer and
 * verifier can never drift apart.
 */
#ifndef KVNODE_SNAPGEN_H
#define KVNODE_SNAPGEN_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/** Largest value length generated; values cycle through 0..SNAP_VALUE_MAX. */
#define SNAP_VALUE_MAX 300

/** Buffer size callers must provide to snap_key. */
#define SNAP_KEY_CAP 32

/**
 * Key of record i: "snap-<i>".
 * @param buf  destination of at least SNAP_KEY_CAP bytes
 * @param i    record index
 * @returns key length (no NUL counted)
 */
static inline size_t snap_key(char *buf, uint64_t i) {
  return (size_t)snprintf(buf, SNAP_KEY_CAP, "snap-%" PRIu64, i);
}

/**
 * Value of record i: (i mod 301) bytes, byte j = (uint8_t)(i*131 + j*7).
 * Every 301st record therefore has an empty value, so the snapshot path is
 * also exercised with zero-length values.
 * @param buf  destination of at least SNAP_VALUE_MAX bytes
 * @param i    record index
 * @returns value length
 */
static inline size_t snap_value(uint8_t *buf, uint64_t i) {
  size_t n = (size_t)(i % (SNAP_VALUE_MAX + 1));
  for (size_t j = 0; j < n; j++) buf[j] = (uint8_t)(i * 131 + j * 7);
  return n;
}

#endif
