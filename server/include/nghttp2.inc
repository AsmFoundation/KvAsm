; =============================================================================
; nghttp2.inc — the parts of the nghttp2 C ABI the server uses.
;
; NASM cannot include C headers, so constants and struct offsets are
; hand-mirrored here. Every value is checked against the real headers of the
; linked nghttp2 by server/tests/test_nghttp2_abi.c (via kv_nghttp2_abi in
; src/abi.asm); a mismatch fails `make test` instead of misreading frames.
; Verified identical for nghttp2 1.64 (Debian trixie) and 1.70 (vendored).
; =============================================================================
%ifndef NGHTTP2_INC
%define NGHTTP2_INC

; frame types / flags / categories
%define NGHTTP2_DATA                            0
%define NGHTTP2_HEADERS                         1
%define NGHTTP2_FLAG_NONE                       0
%define NGHTTP2_FLAG_END_STREAM                 1
%define NGHTTP2_HCAT_REQUEST                    0

; data provider flags
%define NGHTTP2_DATA_FLAG_EOF                   1
%define NGHTTP2_DATA_FLAG_NO_END_STREAM         2

; nv flags: the strings outlive the call, so nghttp2 may reference them
%define NGHTTP2_NV_FLAG_NO_COPY_NAME            2
%define NGHTTP2_NV_FLAG_NO_COPY_VALUE           4

; SETTINGS identifiers
%define NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS 3
%define NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE    4
%define NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE   6

; HTTP/2 error codes
%define NGHTTP2_NO_ERROR                        0
%define NGHTTP2_INTERNAL_ERROR                  2
%define NGHTTP2_ENHANCE_YOUR_CALM               11

; library error codes returned by callbacks
%define NGHTTP2_ERR_CALLBACK_FAILURE            (-902)
%define NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE   (-521)

; struct offsets (x86-64 System V)
%define FRAME_HD_STREAM_ID      8       ; nghttp2_frame_hd.stream_id  (int32)
%define FRAME_HD_TYPE           12      ; nghttp2_frame_hd.type       (uint8)
%define FRAME_HD_FLAGS          13      ; nghttp2_frame_hd.flags      (uint8)
%define FRAME_HEADERS_CAT       56      ; nghttp2_frame.headers.cat   (int)
%define NV_SIZE                 40      ; sizeof(nghttp2_nv)

%endif
