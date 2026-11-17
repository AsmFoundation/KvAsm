; =============================================================================
; config.asm — server settings from environment variables.
;
; Twelve-factor style: every setting is an environment variable with a safe
; default. Values are parsed strictly (decimal digits only, no sign, no
; whitespace, no overflow) and range-checked; any invalid value is logged
; with its name and the server refuses to start — a typo must never silently
; fall back to a default in production.
;
;   KVASM_PORT                  1..65535          default 50051
;   KVASM_WORKERS               0..256            default 0 = CPUs in affinity mask
;   KVASM_CAPACITY              1..2^36 keys      default 1000000
;   KVASM_MAX_KEY_BYTES         1..65536          default 1024
;   KVASM_MAX_VALUE_BYTES       0..67108864       default 1048576
;   KVASM_MAX_MEMORY_MB         0..16777216       default 0 = unlimited
;   KVASM_MAX_STREAMS           1..1000           default 100 (per connection)
;   KVASM_MAX_CONNECTIONS       1..1000000        default 10000 (per worker)
;   KVASM_IDLE_TIMEOUT_SEC      0..86400          default 300 (0 = never)
;   KVASM_SHUTDOWN_GRACE_SEC    0..3600           default 10
;   KVASM_CONN_BUFFER_MB        1..4096           default 64 (per connection)
;   KVASM_CACHE_L1_BITS         0..24             default 0 = no private L1 cache
;   KVASM_CACHE_L2_BITS         0..24             default 0 = no private L2 cache
;   KVASM_MAX_UPLOAD_BYTES      0..2147483647     default 67108864 (64 MiB): the
;                               largest value Upload stores; must not exceed
;                               KVASM_CONN_BUFFER_MB (checked: a larger upload
;                               could never fit one connection's budget)
;   KVASM_SNAPSHOT              path              optional snapshot to load
; Named choices (exact lowercase words; anything else refuses to start):
;   KVASM_SCAN                  swar | sse2       default sse2 (docs/adr/0006)
;   KVASM_CRC32C                auto | vpclmul | pclmul | sse42 | table
;                               default auto: the fastest the CPU has
;                               (256-bit folding, 128-bit folding, the
;                               crc32 instruction, tables — crc32c.asm);
;                               main.asm refuses a forced mode the CPU lacks
;   KVASM_HASH                  auto | siphash | aes
;                               default auto: the keyed hash that places
;                               keys — AES-128 through AES-NI where the CPU
;                               has it, SipHash-2-4 otherwise (ADR 0004);
;                               main.asm refuses aes on a CPU without AES-NI
; (KVASM_H2_ENGINE is gone: HTTP/2 is always KvAsm's own engine, ADR 0008;
; like any unknown variable it is ignored.)
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"

extern getenv
extern strcmp
extern sched_getaffinity
extern __sched_cpucount
extern kv_logb_init
extern kv_logb_str
extern kv_logb_cstr
extern kv_logb_emit
extern kv_logbuf_size

%define MAX_WORKERS     256
%define CPUSET_BYTES    128             ; glibc cpu_set_t (1024 CPUs)

; One row of the settings table.
struc Setting
    .name:      resq 1                  ; const char * (NUL-terminated)
    .offset:    resq 1                  ; field offset in Config
    .min:       resq 1
    .max:       resq 1
    .def:       resq 1
    .scale:     resq 1                  ; stored value = parsed value * scale
endstruc

%macro SETTING 6
    dq %1, %2, %3, %4, %5, %6
%endmacro

; One row of the named-choice table: the stored value is the index of the
; matching word in the NULL-terminated `words` array.
struc Choice
    .name:      resq 1                  ; const char * (NUL-terminated)
    .offset:    resq 1                  ; field offset in Config
    .def:       resq 1                  ; default index
    .words:     resq 1                  ; const char *const words[] (NULL-terminated)
    .allowed:   resq 1                  ; "a|b", printed when the value is invalid
endstruc

section .rodata
n_port:     db "KVASM_PORT", 0
n_workers:  db "KVASM_WORKERS", 0
n_capacity: db "KVASM_CAPACITY", 0
n_maxkey:   db "KVASM_MAX_KEY_BYTES", 0
n_maxval:   db "KVASM_MAX_VALUE_BYTES", 0
n_maxmem:   db "KVASM_MAX_MEMORY_MB", 0
n_streams:  db "KVASM_MAX_STREAMS", 0
n_conns:    db "KVASM_MAX_CONNECTIONS", 0
n_idle:     db "KVASM_IDLE_TIMEOUT_SEC", 0
n_grace:    db "KVASM_SHUTDOWN_GRACE_SEC", 0
n_connbuf:  db "KVASM_CONN_BUFFER_MB", 0
n_cachel1:  db "KVASM_CACHE_L1_BITS", 0
n_cachel2:  db "KVASM_CACHE_L2_BITS", 0
n_snapshot: db "KVASM_SNAPSHOT", 0
n_scan:     db "KVASM_SCAN", 0
w_swar:     db "swar", 0
w_sse2:     db "sse2", 0
a_scan:     db "swar|sse2", 0
n_maxup:    db "KVASM_MAX_UPLOAD_BYTES", 0
n_crc:      db "KVASM_CRC32C", 0
w_auto:     db "auto", 0
w_sse42:    db "sse42", 0
w_table:    db "table", 0
w_pclmul:   db "pclmul", 0
w_vpclmul:  db "vpclmul", 0
a_crc:      db "auto|vpclmul|pclmul|sse42|table", 0
n_hash:     db "KVASM_HASH", 0
w_siphash:  db "siphash", 0
w_aes:      db "aes", 0
a_hash:     db "auto|siphash|aes", 0
m_upbudget: db ' reason="must not exceed KVASM_CONN_BUFFER_MB: a larger upload could never fit one connection', 39, 's budget"'
m_upbudget_len equ $ - m_upbudget
lvl_error:  db "ERROR", 0
m_invalid:  db " msg=", 34, "invalid setting", 34, " name="
m_invalid_len equ $ - m_invalid
m_value:    db " value="
m_range:    db " allowed="
m_dots:     db ".."

; The table holds absolute pointers (Setting.name), so in a PIE each one
; needs a load-time R_X86_64_RELATIVE fix-up. .data.rel.ro is where the
; linker expects such tables: writable while the loader relocates, then
; mprotect'ed read-only by RELRO (-z relro -z now), so a stray write can
; never retarget a name pointer. In .rodata the same table would need a
; text relocation, which the link rejects (-z text in the Makefile).
section .data.rel.ro progbits alloc noexec write align=8
settings:
    SETTING n_port,     Config.port,            1, 65535,               50051,   1
    SETTING n_workers,  Config.workers,         0, MAX_WORKERS,         0,       1
    SETTING n_capacity, Config.capacity,        1, 0x1000000000,        1000000, 1
    SETTING n_maxkey,   Config.max_key,         1, 65536,               1024,    1
    SETTING n_maxval,   Config.max_value,       0, 67108864,            1048576, 1
    SETTING n_maxmem,   Config.max_memory,      0, 16777216,            0,       1048576
    SETTING n_streams,  Config.max_streams,     1, 1000,                100,     1
    SETTING n_conns,    Config.max_conns,       1, 1000000,             10000,   1
    SETTING n_idle,     Config.idle_timeout_ns, 0, 86400,               300,     1000000000
    SETTING n_grace,    Config.grace_ns,        0, 3600,                10,      1000000000
    SETTING n_connbuf,  Config.conn_budget,     1, 4096,                64,      1048576
    SETTING n_cachel1,  Config.cache_l1_bits,   0, HCACHE_BITS_MAX,     0,       1
    SETTING n_cachel2,  Config.cache_l2_bits,   0, HCACHE_BITS_MAX,     0,       1
    ; 2^31 - 1: a record's lengths are 32-bit, and the store refuses more.
    SETTING n_maxup,    Config.max_upload,      0, 2147483647,          67108864, 1
settings_end:

; Word lists, in the order of the values they stand for.
words_scan:     dq w_swar, w_sse2, 0            ; = KV_SCAN_SWAR, KV_SCAN_SSE2
; = CRC_CFG_AUTO, _SSE42, _TABLE, _PCLMUL, _VPCLMUL (in that order)
words_crc:      dq w_auto, w_sse42, w_table, w_pclmul, w_vpclmul, 0
; = HASH_CFG_AUTO, _SIPHASH, _AES (in that order)
words_hash:     dq w_auto, w_siphash, w_aes, 0
choices:
    ; SSE2 by default: faster in the batched GET path at every working set
    ; from L2 to DRAM (bench_store, ADR 0006); SWAR stays selectable.
    dq n_scan,   Config.scan_mode, 1,          words_scan,   a_scan
    ; auto: the fastest path CPUID reports (tests/test_crc32c.c on a 16 KiB
    ; chunk in cache: 256-bit folding ~6x the crc32 instruction, 128-bit
    ; ~3x, the instruction ~4.6x the tables); every path stays selectable,
    ; and the tables are the fallback that runs everywhere.
    dq n_crc,    Config.crc_mode,  CRC_CFG_AUTO, words_crc,  a_crc
    ; auto: AES-128 where the CPU has AES-NI (7.6 core cycles per 16-byte
    ; key when hashes overlap, against 50.0 for SipHash-2-4), SipHash-2-4
    ; everywhere else; both are keyed by the per-process secret (ADR 0004).
    dq n_hash,   Config.hash_mode, HASH_CFG_AUTO, words_hash, a_hash
choices_end:

section .text

global kv_config_load
extern kv_logb_u64

; =============================================================================
; kv_config_load — fill a Config from the environment.
;
; @param  rdi  Config* (fully written on success)
; @returns rax = 0 on success, -EINVAL if any variable is malformed or out of
;          range (the offending name, value and allowed range are logged).
; @remarks Must run before any thread is created (getenv is not thread-safe
;          against setenv). The snapshot path pointer refers to the process
;          environment and stays valid for the process lifetime.
; =============================================================================
kv_config_load:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     rbx, rdi                    ; Config*
    lea     r12, [settings]
.next_setting:
    lea     rax, [settings_end]
    cmp     r12, rax
    jae     .choices
    mov     rdi, [r12 + Setting.name]
    call    getenv wrt ..plt
    mov     r13, rax                    ; raw string or NULL
    mov     r14, [r12 + Setting.def]
    test    r13, r13
    jz      .store
    mov     rdi, r13
    call    parse_u64
    jc      .invalid
    mov     r14, rax
    cmp     r14, [r12 + Setting.min]
    jb      .invalid
    cmp     r14, [r12 + Setting.max]
    ja      .invalid
.store:
    mov     rax, r14
    mul     qword [r12 + Setting.scale] ; ranges keep this far below 2^64
    mov     rcx, [r12 + Setting.offset]
    mov     [rbx + rcx], rax
    add     r12, Setting_size
    jmp     .next_setting

    ; Named choices: exact match against the row's word list (the value is
    ; the word's index). Unset means the default; set but empty or unknown
    ; refuses to start, like a malformed number.
.choices:
    lea     r12, [choices]
.next_choice:
    lea     rax, [choices_end]
    cmp     r12, rax
    jae     .derived
    mov     rdi, [r12 + Choice.name]
    call    getenv wrt ..plt
    mov     r13, rax
    mov     r14, [r12 + Choice.def]
    test    r13, r13
    jz      .choice_store
    mov     rbp, [r12 + Choice.words]
    xor     r14d, r14d
.next_word:
    mov     rsi, [rbp + r14*8]
    test    rsi, rsi
    jz      .choice_invalid             ; end of the list: no word matched
    mov     rdi, r13
    call    strcmp wrt ..plt
    test    eax, eax
    jz      .choice_store
    inc     r14
    jmp     .next_word
.choice_store:
    mov     rcx, [r12 + Choice.offset]
    mov     [rbx + rcx], r14
    add     r12, Choice_size
    jmp     .next_choice
.choice_invalid:
    mov     rdi, r12
    mov     rsi, r13
    call    log_invalid_choice
    mov     rax, -EINVAL
    jmp     .out

.derived:
    cmp     qword [rbx + Config.workers], 0
    jne     .workers_set
    call    affinity_cpus
    cmp     rax, MAX_WORKERS
    jbe     .workers_ok
    mov     eax, MAX_WORKERS
.workers_ok:
    mov     [rbx + Config.workers], rax
.workers_set:
    lea     rdi, [n_snapshot]
    call    getenv wrt ..plt
    test    rax, rax
    jz      .no_snapshot
    cmp     byte [rax], 0
    jne     .have_snapshot
.no_snapshot:
    xor     eax, eax                    ; unset or empty: no snapshot
.have_snapshot:
    mov     [rbx + Config.snapshot], rax
    ; Largest acceptable gRPC message: key + value + protobuf framing (two
    ; tags and two 10-byte length varints fit easily in 64 bytes).
    mov     rax, [rbx + Config.max_key]
    add     rax, [rbx + Config.max_value]
    add     rax, 64
    mov     [rbx + Config.max_request], rax
    ; An upload holds its declared size in the connection's budget until it
    ; completes (grpc.asm ensure_record), so a limit above the budget could
    ; never be reached: refuse the combination now rather than refuse every
    ; such upload later with a puzzling RESOURCE_EXHAUSTED.
    mov     rax, [rbx + Config.max_upload]
    cmp     rax, [rbx + Config.conn_budget]
    jbe     .consistent
    mov     rdi, rax
    mov     rsi, [rbx + Config.conn_budget]
    call    log_upload_budget
    mov     rax, -EINVAL
    jmp     .out
.consistent:
    xor     eax, eax
    jmp     .out

.invalid:
    ; ts=... level=ERROR msg="invalid setting" name=X value=Y allowed=MIN..MAX
    call    kv_logbuf_size
    sub     rsp, rax
    mov     r15, rax
    mov     rdi, rsp
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rsp
    lea     rsi, [m_invalid]
    mov     edx, m_invalid_len
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, [r12 + Setting.name]
    call    kv_logb_cstr
    mov     rdi, rsp
    lea     rsi, [m_value]
    mov     edx, 7
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, r13
    call    kv_logb_cstr
    mov     rdi, rsp
    lea     rsi, [m_range]
    mov     edx, 9
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, [r12 + Setting.min]
    call    kv_logb_u64
    mov     rdi, rsp
    lea     rsi, [m_dots]
    mov     edx, 2
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, [r12 + Setting.max]
    call    kv_logb_u64
    mov     rdi, rsp
    call    kv_logb_emit
    add     rsp, r15
    mov     rax, -EINVAL
.out:
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; log_invalid_choice — "invalid setting name=X value=Y allowed=a|b". Internal.
; @param  rdi  Choice row   @param rsi  the rejected value (NUL-terminated)
; @returns nothing.
; =============================================================================
log_invalid_choice:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for calls
    mov     r12, rdi
    mov     r13, rsi
    call    kv_logbuf_size
    sub     rsp, rax                    ; LogBuf on the stack (size keeps alignment)
    mov     rbx, rax
    mov     rdi, rsp
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rsp
    lea     rsi, [m_invalid]
    mov     edx, m_invalid_len
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, [r12 + Choice.name]
    call    kv_logb_cstr
    mov     rdi, rsp
    lea     rsi, [m_value]
    mov     edx, 7
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, r13
    call    kv_logb_cstr
    mov     rdi, rsp
    lea     rsi, [m_range]
    mov     edx, 9
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, [r12 + Choice.allowed]
    call    kv_logb_cstr
    mov     rdi, rsp
    call    kv_logb_emit
    add     rsp, rbx
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; log_upload_budget — "invalid setting name=KVASM_MAX_UPLOAD_BYTES value=V
; allowed=0..B reason=..." for an upload limit above the connection budget.
; Internal.
; @param  rdi  KVASM_MAX_UPLOAD_BYTES (bytes)    @param rsi  the budget (bytes)
; @returns nothing.
; @remarks Same shape as the range errors above, with the budget as the
;          upper bound, so operators read every configuration error alike.
; =============================================================================
log_upload_budget:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for calls
    mov     r12, rdi
    mov     r13, rsi
    call    kv_logbuf_size
    sub     rsp, rax                    ; LogBuf on the stack (size keeps alignment)
    mov     rbx, rax
    mov     rdi, rsp
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rsp
    lea     rsi, [m_invalid]
    mov     edx, m_invalid_len
    call    kv_logb_str
    mov     rdi, rsp
    lea     rsi, [n_maxup]
    call    kv_logb_cstr
    mov     rdi, rsp
    lea     rsi, [m_value]
    mov     edx, 7
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, r12
    call    kv_logb_u64
    mov     rdi, rsp
    lea     rsi, [m_range]
    mov     edx, 9
    call    kv_logb_str
    mov     rdi, rsp
    xor     esi, esi
    call    kv_logb_u64
    mov     rdi, rsp
    lea     rsi, [m_dots]
    mov     edx, 2
    call    kv_logb_str
    mov     rdi, rsp
    mov     rsi, r13
    call    kv_logb_u64
    mov     rdi, rsp
    lea     rsi, [m_upbudget]
    mov     edx, m_upbudget_len
    call    kv_logb_str
    mov     rdi, rsp
    call    kv_logb_emit
    add     rsp, rbx
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; parse_u64 — strict unsigned decimal. Internal.
; @param  rdi  const char * (NUL-terminated)
; @returns rax = value with CF=0; CF=1 if empty, contains a non-digit, or
;          overflows 64 bits. Clobbers rcx, rdx, rdi.
; =============================================================================
parse_u64:
    xor     eax, eax
    movzx   ecx, byte [rdi]
    test    ecx, ecx
    jz      .bad                        ; empty string
.digit:
    movzx   ecx, byte [rdi]
    test    ecx, ecx
    jz      .ok
    sub     ecx, '0'
    cmp     ecx, 9
    ja      .bad                        ; not a digit (also catches '-', ' ', '+')
    mov     edx, 10
    mul     rdx                         ; rdx:rax = value * 10
    jc      .bad                        ; overflow
    add     rax, rcx
    jc      .bad
    inc     rdi
    jmp     .digit
.ok:
    clc
    ret
.bad:
    stc
    ret

; =============================================================================
; affinity_cpus — number of CPUs this process may run on. Internal.
; @returns rax = CPU count from sched_getaffinity (at least 1).
; @remarks Honors cpusets (docker --cpuset-cpus) but not CFS quotas
;          (--cpus); with quotas, set KVASM_WORKERS explicitly.
; =============================================================================
affinity_cpus:
    sub     rsp, CPUSET_BYTES + 8
    xor     edi, edi                    ; this process
    mov     esi, CPUSET_BYTES
    mov     rdx, rsp
    call    sched_getaffinity wrt ..plt
    test    eax, eax
    jnz     .one
    mov     edi, CPUSET_BYTES
    mov     rsi, rsp
    call    __sched_cpucount wrt ..plt  ; what the CPU_COUNT() macro calls
    test    eax, eax
    jnz     .done
.one:
    mov     eax, 1
.done:
    add     rsp, CPUSET_BYTES + 8
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
