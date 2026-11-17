; =============================================================================
; abi.asm — exports the assembler's view of shared struct layouts.
;
; test_abi.c compares these numbers with offsetof/sizeof of the C mirror in
; server/tests/kvasm_abi.h. The order below is part of that contract: append
; new entries at the end and update test_abi.c in the same commit.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"

section .rodata
align 8
global kv_abi_layout
kv_abi_layout:
    dq Rec_size, Rec.next, Rec.aux, Rec.key_len, Rec.val_len, Rec.data
    dq KvStats_size, KvStats.count, KvStats.freed
    dq Bucket_size, Bucket.ver, Bucket.tags, Bucket.slot
    dq Table_size, QsbrSlot_size
    dq HEntry_size, HCache_size     ; private cache (hcache.asm): 32 B, 2 lines
    dq KvStats.pinned, KvStats.deferred ; pins (Download), appended to KvStats
    dq 0xABCDEF0123456789           ; sentinel: catches a short array

; Config layout for C tests that call kv_snapshot_load directly.
global kv_config_abi
kv_config_abi:
    dq Config_size, Config.max_key, Config.max_value
    dq 0xABCDEF0123456789

section .note.GNU-stack noalloc noexec nowrite progbits
