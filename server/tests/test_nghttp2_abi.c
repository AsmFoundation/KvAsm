/*
 * test_nghttp2_abi.c — every value hand-mirrored in include/nghttp2.inc must
 * equal the real one in <nghttp2/nghttp2.h> of the nghttp2 we link.
 *
 * NASM cannot parse C headers; a wrong offset would not fail to assemble, it
 * would silently read the wrong frame field at run time. This test turns the
 * mirror into a checked fact for the exact library version in use.
 */
#include <stddef.h>
#include <nghttp2/nghttp2.h>

#include "test_common.h"

extern const int64_t kv_nghttp2_abi[];

/** Entry point. @return 0 when every mirrored value matches the header. */
int main(void) {
  const int64_t *v = kv_nghttp2_abi;
  const int64_t expect[] = {
      NGHTTP2_DATA, NGHTTP2_HEADERS, NGHTTP2_FLAG_NONE, NGHTTP2_FLAG_END_STREAM,
      NGHTTP2_HCAT_REQUEST, NGHTTP2_DATA_FLAG_EOF, NGHTTP2_DATA_FLAG_NO_END_STREAM,
      NGHTTP2_NV_FLAG_NO_COPY_NAME, NGHTTP2_NV_FLAG_NO_COPY_VALUE,
      NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,
      NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE,
      NGHTTP2_NO_ERROR, NGHTTP2_INTERNAL_ERROR, NGHTTP2_ENHANCE_YOUR_CALM,
      NGHTTP2_ERR_CALLBACK_FAILURE, NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE,
      offsetof(nghttp2_frame_hd, stream_id), offsetof(nghttp2_frame_hd, type),
      offsetof(nghttp2_frame_hd, flags), offsetof(nghttp2_frame, headers.cat),
      sizeof(nghttp2_nv)};
  for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
    if (v[i] != expect[i]) fprintf(stderr, "  entry %zu: asm %lld, header %lld\n", i,
                                   (long long)v[i], (long long)expect[i]);
    CHECK(v[i] == expect[i]);
  }
  CHECK((uint64_t)v[sizeof expect / sizeof expect[0]] == 0xABCDEF0123456789ull);
  printf("  nghttp2 %s\n", NGHTTP2_VERSION);
  return test_finish("test_nghttp2_abi");
}
