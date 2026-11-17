; =============================================================================
; main.asm — KvAsm process entry point.
;
; Startup (fail fast, in this order, before any client can connect):
;   1. ignore SIGPIPE (sockets also use MSG_NOSIGNAL; belt and braces)
;   2. load and validate configuration from the environment      (exit 2)
;   3. refuse a port another process holds (kv_port_probe) — before the
;      possibly long snapshot load, so a misconfigured restart fails in
;      milliseconds                                               (exit 1)
;   4. draw a 16-byte SipHash key from getrandom                 (exit 1)
;   5. create the store; load the snapshot if configured         (exit 1)
;   6. block SIGTERM/SIGINT in this thread (workers inherit the mask, so the
;      signals are only ever consumed by sigwaitinfo below)
;   7. create every worker (binding all listeners), then start them
; Shutdown: sigwaitinfo returns on SIGTERM/SIGINT (works as PID 1 in a
; container, where default signal actions are ignored); every worker drains
; its connections within KVASM_SHUTDOWN_GRACE_SEC, then the process exits 0.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"
%include "log.inc"                      ; LOGBUF_BYTES sizes main's log slot

%define SIGINT          2
%define SIGPIPE         13
%define SIGTERM         15
%define SIG_BLOCK       0
%define SIG_IGN         1
%define CLOCK_MONOTONIC 1
; The release version, logged in the "starting" line. It must equal the git
; tag being released (vX.Y.Z) and KvAsm.Client's <Version>:
; .github/workflows/release.yml refuses to publish when they differ.
%define VERSION_STR     "0.3.0"

extern signal
extern sigemptyset
extern sigaddset
extern pthread_sigmask
extern sigwaitinfo
extern calloc
extern free
extern clock_gettime
extern kv_config_load
extern kv_set_scan_mode
extern kv_crc32c_set_mode
extern kv_crc32c_best_mode
extern kv_set_hash_mode
extern kv_hash_best_mode
extern kv_hash_mode_get
extern kv_store_enable_inval
extern kv_store_create
extern kv_store_destroy
extern kv_store_reclaim
extern kv_store_stats
extern kv_snapshot_load
extern kv_hpack_global_init
extern kv_port_probe
extern kv_worker_create
extern kv_worker_start
extern kv_worker_stop
extern kv_worker_join
extern kv_logb_init
extern kv_logb_str
extern kv_logb_cstr
extern kv_logb_u64
extern kv_logb_emit
extern kv_logbuf_size

; main's frame
%define F_SET       0               ; sigset_t (128 bytes)
%define F_INFO      128             ; siginfo_t (128 bytes)
%define F_SEED      256             ; 16-byte SipHash key
%define F_STATS     272             ; per-worker stats (3 x u64)
%define F_TOTALS    296             ; summed stats (3 x u64)
%define F_TS        320             ; struct timespec
%define F_T0        336
%define F_LOG       352             ; LogBuf lives here, LOGBUF_BYTES long
%define F_FRAME     (F_LOG + LOGBUF_BYTES)

; The prologue does 6 pushes and `sub rsp, F_FRAME + 8`; calls are only
; 16-byte aligned if F_FRAME is a multiple of 16 (entry rsp is 8 mod 16,
; 6 pushes keep it there, the extra 8 fixes it). Checked here so a LOG_CAP
; or frame-field change cannot misalign every call main makes.
%if (F_FRAME % 16) != 0 || (F_LOG % 16) != 0
    %error "main frame: F_LOG and F_FRAME must be multiples of 16"
%endif

section .rodata
lvl_info:   db "INFO", 0
lvl_error:  db "ERROR", 0
CSTR_starting:  db ' msg="starting" version=', VERSION_STR
L_starting      equ $ - CSTR_starting
k_port:     db " port="
k_workers:  db " workers="
k_capacity: db " capacity="
k_maxkey:   db " max_key_bytes="
k_maxval:   db " max_value_bytes="
k_maxmem:   db " max_memory_bytes="
k_streams:  db " max_streams="
k_snapshot: db " snapshot="
k_none:     db "none", 0
k_scan:     db " scan="
k_l1:       db " cache_l1_bits="
k_l2:       db " cache_l2_bits="
v_swar:     db "swar", 0
v_sse2:     db "sse2", 0
k_maxup:    db " max_upload_bytes="
k_crc:      db " crc32c="
v_sse42:    db "sse42", 0
v_table:    db "table", 0
v_pclmul:   db "pclmul", 0
v_vpclmul:  db "vpclmul", 0
k_pinned:   db " pinned="
k_deferred: db " deferred="
m_f_crc:    db ' msg="invalid setting" name=KVASM_CRC32C value='
L_f_crc     equ $ - m_f_crc
m_f_crc2:   db ' reason="this CPU lacks the instructions it needs"'
L_f_crc2    equ $ - m_f_crc2
k_hash:     db " hash="
v_siphash:  db "siphash", 0
v_aes:      db "aes", 0
m_f_hash:   db ' msg="invalid setting" name=KVASM_HASH value=aes reason="this CPU lacks AES-NI"'
L_f_hash    equ $ - m_f_hash
m_listen:   db ' msg="listening" transport=h2c'
L_listen    equ $ - m_listen
m_loaded:   db ' msg="snapshot loaded" records='
L_loaded    equ $ - m_loaded
k_ms:       db " ms="
k_keys:     db " keys="
m_shutdown: db ' msg="shutdown requested" signal='
L_shutdown  equ $ - m_shutdown
m_stopped:  db ' msg="stopped" connections='
L_stopped   equ $ - m_stopped
k_requests: db " requests="
k_errors:   db " errors="
m_f_seed:   db ' msg="getrandom failed"'
L_f_seed    equ $ - m_f_seed
m_f_store:  db ' msg="cannot create store"'
L_f_store   equ $ - m_f_store
m_f_snap:   db ' msg="snapshot load failed, refusing to start"'
L_f_snap    equ $ - m_f_snap
m_f_worker: db ' msg="worker startup failed"'
L_f_worker  equ $ - m_f_worker
m_f_port:   db ' msg="port already in use, refusing to share it"'
L_f_port    equ $ - m_f_port
k_errno:    db " errno="

; Section-level alignment instead of `alignb`: NASM 2.16's alignb macro emits
; [warning push]/[warning pop], which -w+all -w+error rejects as unknown.
; Config is written once before any thread starts, then only read, so it
; needs no cache-line isolation; 64 just keeps it off a split line.
section .bss align=64
g_cfg:      resb Config_size

section .text

global main

; -----------------------------------------------------------------------------
; LOGLIT lit, len — append a literal to the LogBuf at [rsp + F_LOG].
; -----------------------------------------------------------------------------
%macro LOGLIT 2
    lea     rdi, [rsp + F_LOG]
    lea     rsi, [%1]
    mov     edx, %2
    call    kv_logb_str wrt ..plt
%endmacro

; LOGNUM reg_or_mem — append an unsigned number.
%macro LOGNUM 1
    lea     rdi, [rsp + F_LOG]
    mov     rsi, %1
    call    kv_logb_u64 wrt ..plt
%endmacro

; LOGBEGIN level_label / LOGEND
%macro LOGBEGIN 1
    lea     rdi, [rsp + F_LOG]
    lea     rsi, [%1]
    call    kv_logb_init wrt ..plt
%endmacro
%macro LOGEND 0
    lea     rdi, [rsp + F_LOG]
    call    kv_logb_emit wrt ..plt
%endmacro

; =============================================================================
; main — see the file header.
; @param  edi  argc (unused: configuration comes from the environment)
; @param  rsi  argv (unused)
; @returns eax = 0 after a clean shutdown, 1 on a runtime/startup failure,
;          2 on invalid configuration.
; =============================================================================
main:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, F_FRAME + 8            ; 6 pushes + frame + 8 => 16-aligned

    mov     edi, SIGPIPE
    mov     esi, SIG_IGN
    call    signal wrt ..plt

    lea     rdi, [g_cfg]
    call    kv_config_load wrt ..plt
    test    rax, rax
    jz      .config_ok
    mov     eax, 2
    jmp     .return
.config_ok:
    lea     r15, [g_cfg]

    ; CRC-32C implementation (Upload/Download checksums), before any thread:
    ; auto takes the fastest path CPUID reports (kv_crc32c_best_mode); a
    ; forced mode whose instructions the CPU lacks is a configuration error
    ; (exit 2) — kv_crc32c_set_mode's refusal — instead of a #UD crash in a
    ; worker at the first upload. Config words map to crc32c.asm's modes
    ; 0 table, 1 sse42, 2 pclmul, 3 vpclmul. r14 keeps the mode in force for
    ; the "starting" line.
    mov     rax, [r15 + Config.crc_mode]
    xor     edi, edi                    ; table
    cmp     rax, CRC_CFG_TABLE
    je      .crc_set
    mov     edi, 1                      ; sse42
    cmp     rax, CRC_CFG_SSE42
    je      .crc_set
    mov     edi, 2                      ; pclmul
    cmp     rax, CRC_CFG_PCLMUL
    je      .crc_set
    mov     edi, 3                      ; vpclmul
    cmp     rax, CRC_CFG_VPCLMUL
    je      .crc_set
    call    kv_crc32c_best_mode wrt ..plt   ; auto
    mov     edi, eax
.crc_set:
    mov     r14d, edi                   ; the mode requested = the mode in force on success
    call    kv_crc32c_set_mode wrt ..plt
    test    eax, eax
    jns     .crc_ok
    LOGBEGIN lvl_error
    LOGLIT  m_f_crc, L_f_crc
    mov     edi, r14d
    call    crc_mode_name
    lea     rdi, [rsp + F_LOG]
    mov     rsi, rax
    call    kv_logb_cstr wrt ..plt
    LOGLIT  m_f_crc2, L_f_crc2
    LOGEND
    mov     eax, 2
    jmp     .return
.crc_ok:

    ; The key hash, before any key is hashed (the snapshot load below hashes
    ; every key it inserts): auto takes AES-128 where the CPU has AES-NI and
    ; SipHash-2-4 elsewhere; a forced aes the CPU cannot run is a
    ; configuration error (exit 2), not a #UD at the first request.
    mov     rax, [r15 + Config.hash_mode]
    xor     edi, edi                    ; KV_HASH_SIPHASH
    cmp     rax, HASH_CFG_SIPHASH
    je      .hash_set
    mov     edi, 1                      ; KV_HASH_AES
    cmp     rax, HASH_CFG_AES
    je      .hash_set
    call    kv_hash_best_mode wrt ..plt ; auto
    mov     edi, eax
.hash_set:
    call    kv_set_hash_mode wrt ..plt
    test    eax, eax
    jns     .hash_ok
    LOGBEGIN lvl_error
    LOGLIT  m_f_hash, L_f_hash
    LOGEND
    mov     eax, 2
    jmp     .return
.hash_ok:

    LOGBEGIN lvl_info
    LOGLIT  CSTR_starting, L_starting
    LOGLIT  k_port, 6
    LOGNUM  [r15 + Config.port]
    LOGLIT  k_workers, 9
    LOGNUM  [r15 + Config.workers]
    LOGLIT  k_capacity, 10
    LOGNUM  [r15 + Config.capacity]
    LOGLIT  k_maxkey, 15
    LOGNUM  [r15 + Config.max_key]
    LOGLIT  k_maxval, 17
    LOGNUM  [r15 + Config.max_value]
    LOGLIT  k_maxmem, 18
    LOGNUM  [r15 + Config.max_memory]
    LOGLIT  k_streams, 13
    LOGNUM  [r15 + Config.max_streams]
    LOGLIT  k_scan, 6
    lea     rdi, [rsp + F_LOG]
    lea     rsi, [v_swar]
    lea     rax, [v_sse2]
    cmp     qword [r15 + Config.scan_mode], 0
    cmovne  rsi, rax
    call    kv_logb_cstr wrt ..plt
    LOGLIT  k_l1, 15
    LOGNUM  [r15 + Config.cache_l1_bits]
    LOGLIT  k_l2, 15
    LOGNUM  [r15 + Config.cache_l2_bits]
    LOGLIT  k_maxup, 18
    LOGNUM  [r15 + Config.max_upload]
    LOGLIT  k_crc, 8
    mov     edi, r14d
    call    crc_mode_name
    lea     rdi, [rsp + F_LOG]
    mov     rsi, rax
    call    kv_logb_cstr wrt ..plt
    LOGLIT  k_snapshot, 10
    lea     rdi, [rsp + F_LOG]
    mov     rsi, [r15 + Config.snapshot]
    test    rsi, rsi
    jnz     .snap_name
    lea     rsi, [k_none]
.snap_name:
    call    kv_logb_cstr wrt ..plt
    ; Last on the line: the end-to-end suite matches "crc32c=<mode> snapshot=".
    LOGLIT  k_hash, 6
    call    kv_hash_mode_get wrt ..plt
    lea     rsi, [v_siphash]
    lea     rdx, [v_aes]
    cmp     eax, 1                      ; KV_HASH_AES
    cmove   rsi, rdx
    lea     rdi, [rsp + F_LOG]
    call    kv_logb_cstr wrt ..plt
    LOGEND

    ; Refuse to join another process's SO_REUSEPORT group (kv_port_probe
    ; explains the split-brain this prevents).
    mov     rdi, [r15 + Config.port]
    call    kv_port_probe wrt ..plt
    test    rax, rax
    jz      .port_ok
    neg     rax
    mov     r12, rax                    ; errno (98 = EADDRINUSE)
    LOGBEGIN lvl_error
    LOGLIT  m_f_port, L_f_port
    LOGLIT  k_port, 6
    LOGNUM  [r15 + Config.port]
    LOGLIT  k_errno, 7
    LOGNUM  r12
    LOGEND
    mov     eax, 1
    jmp     .return
.port_ok:

    ; The hash key (SipHash's key, and the seed of the AES round keys): must
    ; be unpredictable, or bucket placement becomes predictable to anyone
    ; who can choose keys.
    lea     rdi, [rsp + F_SEED]
    mov     esi, 16
    xor     edx, edx
    mov     eax, SYS_getrandom
    syscall
    cmp     rax, 16
    je      .seed_ok
    LOGBEGIN lvl_error
    LOGLIT  m_f_seed, L_f_seed
    LOGEND
    mov     eax, 1
    jmp     .return
.seed_ok:
    mov     rdi, [r15 + Config.capacity]
    mov     rsi, [r15 + Config.workers]
    lea     rdx, [rsp + F_SEED]
    mov     rcx, [r15 + Config.max_memory]
    call    kv_store_create wrt ..plt
    mov     qword [rsp + F_SEED], 0     ; the store keeps its own copy
    mov     qword [rsp + F_SEED + 8], 0
    test    rax, rax
    jnz     .store_ok
    LOGBEGIN lvl_error
    LOGLIT  m_f_store, L_f_store
    LOGEND
    mov     eax, 1
    jmp     .return
.store_ok:
    mov     rbx, rax                    ; table
    ; Readers' tag scan: both implementations return identical results; the
    ; default comes from bench_store (ADR 0006). Set before any reader runs.
    mov     rdi, [r15 + Config.scan_mode]
    call    kv_set_scan_mode wrt ..plt
    ; Private record caches (per worker, created with the workers) validate
    ; entries against the table's invalidation generations: allocate them
    ; now, while no writer runs (the snapshot load below writes).
    mov     rax, [r15 + Config.cache_l1_bits]
    or      rax, [r15 + Config.cache_l2_bits]
    jz      .cache_ready
    mov     rdi, rbx
    mov     esi, INVAL_BITS_DEFAULT
    call    kv_store_enable_inval wrt ..plt
    test    rax, rax
    jz      .cache_ready
    LOGBEGIN lvl_error
    LOGLIT  m_f_store, L_f_store
    LOGEND
    mov     rdi, rbx
    call    kv_store_destroy wrt ..plt
    mov     eax, 1
    jmp     .return
.cache_ready:

    mov     rsi, [r15 + Config.snapshot]
    test    rsi, rsi
    jz      .signals
    call    now_ms
    mov     [rsp + F_T0], rax
    mov     rdi, rbx
    mov     rsi, [r15 + Config.snapshot]
    mov     rdx, r15
    call    kv_snapshot_load wrt ..plt
    test    rax, rax
    jns     .snap_ok
    LOGBEGIN lvl_error
    LOGLIT  m_f_snap, L_f_snap
    LOGEND
    mov     rdi, rbx
    call    kv_store_destroy wrt ..plt
    mov     eax, 1
    jmp     .return
.snap_ok:
    mov     r12, rax
    mov     rdi, rbx
    call    kv_store_reclaim wrt ..plt  ; duplicates in the file: free replaced records now
    call    now_ms
    sub     rax, [rsp + F_T0]
    mov     r13, rax
    LOGBEGIN lvl_info
    LOGLIT  m_loaded, L_loaded
    LOGNUM  r12
    LOGLIT  k_ms, 4
    LOGNUM  r13
    LOGEND

.signals:
    lea     rdi, [rsp + F_SET]
    call    sigemptyset wrt ..plt
    lea     rdi, [rsp + F_SET]
    mov     esi, SIGTERM
    call    sigaddset wrt ..plt
    lea     rdi, [rsp + F_SET]
    mov     esi, SIGINT
    call    sigaddset wrt ..plt
    mov     edi, SIG_BLOCK
    lea     rsi, [rsp + F_SET]
    xor     edx, edx
    call    pthread_sigmask wrt ..plt

    ; The HPACK decoder (hpack.asm) classifies the 61 static entries once,
    ; before any worker could decode a header block (read-only after).
    call    kv_hpack_global_init wrt ..plt

    mov     rdi, [r15 + Config.workers]
    mov     esi, 8
    call    calloc wrt ..plt
    test    rax, rax
    jz      .worker_failed_early
    mov     r14, rax                    ; Worker*[]
    xor     r12d, r12d
.create:
    cmp     r12, [r15 + Config.workers]
    jae     .start
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, r15
    call    kv_worker_create wrt ..plt
    test    rax, rax
    jz      .worker_failed
    mov     [r14 + r12*8], rax
    inc     r12
    jmp     .create
.start:
    xor     r12d, r12d
.start_next:
    cmp     r12, [r15 + Config.workers]
    jae     .running
    mov     rdi, [r14 + r12*8]
    call    kv_worker_start wrt ..plt
    test    eax, eax
    jnz     .worker_failed              ; (pthread_create failure: fatal)
    inc     r12
    jmp     .start_next

.running:
    LOGBEGIN lvl_info
    LOGLIT  m_listen, L_listen
    LOGLIT  k_port, 6
    LOGNUM  [r15 + Config.port]
    LOGLIT  k_workers, 9
    LOGNUM  [r15 + Config.workers]
    LOGEND

.wait_signal:
    lea     rdi, [rsp + F_SET]
    lea     rsi, [rsp + F_INFO]
    call    sigwaitinfo wrt ..plt
    test    eax, eax
    js      .wait_signal                ; EINTR: keep waiting
    mov     r13d, eax
    LOGBEGIN lvl_info
    LOGLIT  m_shutdown, L_shutdown
    LOGNUM  r13
    LOGEND

    xor     r12d, r12d
.stop_next:
    cmp     r12, [r15 + Config.workers]
    jae     .join
    mov     rdi, [r14 + r12*8]
    call    kv_worker_stop wrt ..plt
    inc     r12
    jmp     .stop_next
.join:
    xor     eax, eax
    mov     [rsp + F_TOTALS], rax
    mov     [rsp + F_TOTALS + 8], rax
    mov     [rsp + F_TOTALS + 16], rax
    xor     r12d, r12d
.join_next:
    cmp     r12, [r15 + Config.workers]
    jae     .joined
    mov     rdi, [r14 + r12*8]
    lea     rsi, [rsp + F_STATS]
    call    kv_worker_join wrt ..plt
    mov     rax, [rsp + F_STATS]
    add     [rsp + F_TOTALS], rax
    mov     rax, [rsp + F_STATS + 8]
    add     [rsp + F_TOTALS + 8], rax
    mov     rax, [rsp + F_STATS + 16]
    add     [rsp + F_TOTALS + 16], rax
    inc     r12
    jmp     .join_next
.joined:
    ; KvStats is 16 x u64: KvStats_size (not a literal) so an appended
    ; counter can never make kv_store_stats write past this slot again.
    sub     rsp, (KvStats_size + 15) & ~15  ; alignment kept
    mov     rdi, rbx
    mov     rsi, rsp
    call    kv_store_stats wrt ..plt
    mov     r13, [rsp]                  ; live keys
    ; Pins left after every worker (and so every Download) is gone must be
    ; zero, and so must records waiting for a last unpin: logged so that the
    ; end-to-end suite can require "pinned=0 deferred=0" after each phase.
    mov     r12, [rsp + KvStats.pinned]
    mov     rbp, [rsp + KvStats.deferred]
    add     rsp, (KvStats_size + 15) & ~15
    LOGBEGIN lvl_info
    LOGLIT  m_stopped, L_stopped
    LOGNUM  [rsp + F_TOTALS]
    LOGLIT  k_requests, 10
    LOGNUM  [rsp + F_TOTALS + 8]
    LOGLIT  k_errors, 8
    LOGNUM  [rsp + F_TOTALS + 16]
    LOGLIT  k_keys, 6
    LOGNUM  r13
    LOGLIT  k_pinned, 8
    LOGNUM  r12
    LOGLIT  k_deferred, 10
    LOGNUM  rbp
    LOGEND
    mov     rdi, r14
    call    free wrt ..plt
    mov     rdi, rbx
    call    kv_store_destroy wrt ..plt
    xor     eax, eax
    jmp     .return

.worker_failed:
    ; Workers created so far never started (or some did): stop and join
    ; the started ones would need bookkeeping; a startup failure is fatal and
    ; the process exits immediately, letting the kernel reclaim resources.
.worker_failed_early:
    LOGBEGIN lvl_error
    LOGLIT  m_f_worker, L_f_worker
    LOGEND
    mov     eax, 1
.return:
    add     rsp, F_FRAME + 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; now_ms — monotonic milliseconds. Internal.
; @returns rax = CLOCK_MONOTONIC in ms.
; =============================================================================
now_ms:
    sub     rsp, 24
    mov     edi, CLOCK_MONOTONIC
    mov     rsi, rsp
    call    clock_gettime wrt ..plt
    mov     rax, [rsp]
    imul    rax, rax, 1000
    mov     rcx, [rsp + 8]
    push    rax
    mov     rax, rcx
    xor     edx, edx
    mov     ecx, 1000000
    div     rcx
    pop     rcx
    add     rax, rcx
    add     rsp, 24
    ret

; =============================================================================
; crc_mode_name — the KVASM_CRC32C word naming a crc32c.asm mode. Internal.
; @param  edi  mode: 0 table, 1 sse42, 2 pclmul, 3 vpclmul
; @returns rax = its NUL-terminated name ("table" for anything else, which
;          kv_crc32c_set_mode never leaves in force).
; @remarks Used by the "starting" line and the refusal of a forced mode, so
;          both always name the mode exactly as the setting spells it.
; =============================================================================
crc_mode_name:
    lea     rax, [v_table]
    lea     rdx, [v_sse42]
    cmp     edi, 1
    cmove   rax, rdx
    lea     rdx, [v_pclmul]
    cmp     edi, 2
    cmove   rax, rdx
    lea     rdx, [v_vpclmul]
    cmp     edi, 3
    cmove   rax, rdx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
