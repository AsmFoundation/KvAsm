; =============================================================================
; abi.asm — exports the assembler's view of shared struct layouts.
;
; test_abi.c compares these numbers with offsetof/sizeof of the C mirror in
; server/tests/kvnode_abi.h. The order below is part of that contract: append
; new entries at the end and update test_abi.c in the same commit.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"
%include "nghttp2.inc"
%include "server.inc"

section .rodata
align 8
global kv_abi_layout
kv_abi_layout:
    dq Rec_size, Rec.next, Rec.aux, Rec.key_len, Rec.val_len, Rec.data
    dq KvStats_size, KvStats.count, KvStats.freed
    dq Bucket_size, Bucket.ver, Bucket.tags, Bucket.slot
    dq Table_size, QsbrSlot_size
    dq 0xABCDEF0123456789           ; sentinel: catches a short array

; nghttp2.inc values, checked against <nghttp2/nghttp2.h> by test_nghttp2_abi.c
global kv_nghttp2_abi
kv_nghttp2_abi:
    dq NGHTTP2_DATA, NGHTTP2_HEADERS, NGHTTP2_FLAG_NONE, NGHTTP2_FLAG_END_STREAM
    dq NGHTTP2_HCAT_REQUEST, NGHTTP2_DATA_FLAG_EOF, NGHTTP2_DATA_FLAG_NO_END_STREAM
    dq NGHTTP2_NV_FLAG_NO_COPY_NAME, NGHTTP2_NV_FLAG_NO_COPY_VALUE
    dq NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE
    dq NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE
    dq NGHTTP2_NO_ERROR, NGHTTP2_INTERNAL_ERROR, NGHTTP2_ENHANCE_YOUR_CALM
    dq NGHTTP2_ERR_CALLBACK_FAILURE, NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
    dq FRAME_HD_STREAM_ID, FRAME_HD_TYPE, FRAME_HD_FLAGS, FRAME_HEADERS_CAT, NV_SIZE
    dq 0xABCDEF0123456789

; Config layout for C tests that call kv_snapshot_load directly.
global kv_config_abi
kv_config_abi:
    dq Config_size, Config.max_key, Config.max_value
    dq 0xABCDEF0123456789

section .note.GNU-stack noalloc noexec nowrite progbits
