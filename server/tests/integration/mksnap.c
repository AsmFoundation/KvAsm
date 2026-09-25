/*
 * mksnap.c — writes the deterministic snapshot used by the integration run.
 *
 * Usage: mksnap PATH COUNT [corrupt]
 *
 * Writes COUNT records from snapgen.h in the KVNSNAP1 format — byte for byte
 * what the C# SnapshotWriter produces — with the checksum computed by the
 * server's own kv_siphash128. With "corrupt", one byte in the middle of the
 * record area is flipped AFTER the checksum was computed: the server must
 * then refuse to start with reason="checksum mismatch" (the loader verifies
 * integrity before parsing any record, so the flipped byte can never be
 * reported as a different, misleading error).
 */
#include "kvnode_abi.h"
#include "snapgen.h"
#include "test_common.h"

/**
 * Appends bytes to the image.
 * @param buf  image buffer (pre-sized by the caller)
 * @param n    current length, advanced by len
 * @param src  bytes to append
 * @param len  number of bytes
 */
static void append(uint8_t *buf, size_t *n, const void *src, size_t len) {
  memcpy(buf + *n, src, len);
  *n += len;
}

/**
 * Entry point.
 * @param argc  argument count (3 or 4)
 * @param argv  PATH COUNT [corrupt]
 * @returns 0 on success, 2 on usage or I/O errors
 */
int main(int argc, char **argv) {
  if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "corrupt") != 0)) {
    fprintf(stderr, "usage: %s PATH COUNT [corrupt]\n", argv[0]);
    return 2;
  }
  uint64_t count = strtoull(argv[2], NULL, 10);
  int corrupt = argc == 4;
  size_t cap = 32 + (size_t)count * (8 + SNAP_KEY_CAP + SNAP_VALUE_MAX) + 16;
  uint8_t *buf = malloc(cap);
  if (!buf) { perror("malloc"); return 2; }

  size_t n = 0;
  uint32_t version = 1, flags = 0;
  uint64_t reserved = 0;
  append(buf, &n, "KVNSNAP1", 8);
  append(buf, &n, &version, 4);
  append(buf, &n, &flags, 4);
  append(buf, &n, &count, 8);
  append(buf, &n, &reserved, 8);
  char key[SNAP_KEY_CAP];
  uint8_t val[SNAP_VALUE_MAX];
  for (uint64_t i = 0; i < count; i++) {
    uint32_t kl = (uint32_t)snap_key(key, i), vl = (uint32_t)snap_value(val, i);
    append(buf, &n, &kl, 4);
    append(buf, &n, &vl, 4);
    append(buf, &n, key, kl);
    append(buf, &n, val, vl);
  }
  static const uint64_t zero_key[2] = {0, 0};
  uint64_t h[2];
  kv_siphash128(buf, n, zero_key, h);        /* over every byte before the trailer */
  append(buf, &n, "KVNSEND1", 8);
  append(buf, &n, &h[0], 8);
  if (corrupt) buf[32 + (n - 48) / 2] ^= 0x40;

  FILE *f = fopen(argv[1], "wb");
  if (!f || fwrite(buf, 1, n, f) != n || fclose(f) != 0) {
    perror(argv[1]);
    return 2;
  }
  printf("mksnap: %s: %" PRIu64 " records, %zu bytes%s\n", argv[1], count, n,
         corrupt ? " (corrupted after checksum)" : "");
  free(buf);
  return 0;
}
