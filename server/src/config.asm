; =============================================================================
; config.asm — server settings from environment variables.
;
; Twelve-factor style: every setting is an environment variable with a safe
; default. Values are parsed strictly (decimal digits only, no sign, no
; whitespace, no overflow) and range-checked; any invalid value is logged
; with its name and the server refuses to start — a typo must never silently
; fall back to a default in production.
;
;   KVNODE_PORT                 1..65535          default 50051
;   KVNODE_WORKERS              0..256            default 0 = CPUs in affinity mask
;   KVNODE_CAPACITY             1..2^36 keys      default 1000000
;   KVNODE_MAX_KEY_BYTES        1..65536          default 1024
;   KVNODE_MAX_VALUE_BYTES      0..67108864       default 1048576
;   KVNODE_MAX_MEMORY_MB        0..16777216       default 0 = unlimited
;   KVNODE_MAX_STREAMS          1..1000           default 100 (per connection)
;   KVNODE_MAX_CONNECTIONS      1..1000000        default 10000 (per worker)
;   KVNODE_IDLE_TIMEOUT_SEC     0..86400          default 300 (0 = never)
;   KVNODE_SHUTDOWN_GRACE_SEC   0..3600           default 10
;   KVNODE_CONN_BUFFER_MB       1..4096           default 64 (per connection)
;   KVNODE_SNAPSHOT             path              optional snapshot to load
; =============================================================================

default rel
bits 64

%include "kvnode.inc"
%include "server.inc"

extern getenv
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

section .rodata
n_port:     db "KVNODE_PORT", 0
n_workers:  db "KVNODE_WORKERS", 0
n_capacity: db "KVNODE_CAPACITY", 0
n_maxkey:   db "KVNODE_MAX_KEY_BYTES", 0
n_maxval:   db "KVNODE_MAX_VALUE_BYTES", 0
n_maxmem:   db "KVNODE_MAX_MEMORY_MB", 0
n_streams:  db "KVNODE_MAX_STREAMS", 0
n_conns:    db "KVNODE_MAX_CONNECTIONS", 0
n_idle:     db "KVNODE_IDLE_TIMEOUT_SEC", 0
n_grace:    db "KVNODE_SHUTDOWN_GRACE_SEC", 0
n_connbuf:  db "KVNODE_CONN_BUFFER_MB", 0
n_snapshot: db "KVNODE_SNAPSHOT", 0
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
settings_end:

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
    jae     .derived
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
;          (--cpus); with quotas, set KVNODE_WORKERS explicitly.
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
