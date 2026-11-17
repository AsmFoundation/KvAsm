; =============================================================================
; log.asm — structured, single-write log lines on stderr.
;
; Format:  ts=<unix seconds>.<milliseconds> level=<LEVEL> <fields...>\n
; Each line is assembled in a caller-provided LogBuf and emitted with ONE
; write(2), so lines from different threads never interleave mid-line
; (writes below PIPE_BUF are atomic on pipes, which is how container runtimes
; collect stderr). Appends are truncation-safe: a line longer than the buffer
; is cut, never overflowed.
;
; Usage from assembly:
;     lea rdi, [rsp]          ; LogBuf on the stack
;     lea rsi, [lvl_info]
;     call kv_logb_init
;     LOGS "msg=listening port="   (macro in log.inc-style: kv_logb_str)
;     ...
;     call kv_logb_emit
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "log.inc"                      ; LOG_CAP, LogBuf, LOGBUF_BYTES

%define CLOCK_REALTIME  0

extern clock_gettime

section .rodata
str_ts:     db "ts="
str_level:  db " level="

section .text

global kv_logb_init
global kv_logb_str
global kv_logb_cstr
global kv_logb_u64
global kv_logb_emit
global kv_fmt_u64
global kv_logbuf_size

; =============================================================================
; kv_logbuf_size — bytes a caller must reserve for a LogBuf.
; @returns rax = LOGBUF_BYTES (LogBuf_size rounded to 16 so a caller that does
;          `sub rsp, rax` keeps its stack 16-byte aligned for further calls).
; @remarks For callers whose frame size is computed at run time (config.asm,
;          snapshot.asm); fixed frames include log.inc and use LOGBUF_BYTES.
; =============================================================================
kv_logbuf_size:
    mov     eax, LOGBUF_BYTES
    ret

; =============================================================================
; kv_logb_init — start a line: "ts=<sec>.<ms> level=<level>".
; @param  rdi  LogBuf*
; @param  rsi  const char *level (NUL-terminated, e.g. "INFO")
; @returns nothing.
; =============================================================================
kv_logb_init:
    push    rbx
    push    r12
    sub     rsp, 24                     ; struct timespec + alignment
    mov     rbx, rdi
    mov     r12, rsi
    mov     qword [rbx + LogBuf.len], 0
    mov     edi, CLOCK_REALTIME
    mov     rsi, rsp
    call    clock_gettime wrt ..plt     ; vDSO: no syscall
    mov     rdi, rbx
    lea     rsi, [str_ts]
    mov     edx, 3
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, [rsp]                  ; tv_sec
    call    kv_logb_u64
    ; ".mmm" — always three digits
    mov     rax, [rsp + 8]              ; tv_nsec
    xor     edx, edx
    mov     ecx, 1000000
    div     rcx                         ; rax = milliseconds 0..999
    mov     rcx, [rbx + LogBuf.len]
    cmp     rcx, LOG_CAP - 4
    ja      .no_ms
    mov     byte [rbx + LogBuf.data + rcx], '.'
    mov     r8d, 100
    xor     edx, edx
    div     r8d                         ; eax = hundreds, edx = rest
    add     al, '0'
    mov     [rbx + LogBuf.data + rcx + 1], al
    mov     eax, edx
    mov     r8d, 10
    xor     edx, edx
    div     r8d
    add     al, '0'
    mov     [rbx + LogBuf.data + rcx + 2], al
    add     dl, '0'
    mov     [rbx + LogBuf.data + rcx + 3], dl
    add     qword [rbx + LogBuf.len], 4
.no_ms:
    mov     rdi, rbx
    lea     rsi, [str_level]
    mov     edx, 7
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, r12
    call    kv_logb_cstr
    add     rsp, 24
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_logb_str — append bytes (truncating at the line capacity).
; @param  rdi  LogBuf*   @param rsi  bytes   @param rdx  length
; @returns nothing. Clobbers rax, rcx, rdx, rsi, r8.
; =============================================================================
kv_logb_str:
    mov     rax, [rdi + LogBuf.len]
    mov     rcx, LOG_CAP
    sub     rcx, rax                    ; room left
    cmp     rdx, rcx
    cmova   rdx, rcx
    lea     r8, [rdi + LogBuf.data + rax]
    add     [rdi + LogBuf.len], rdx
    test    rdx, rdx
    jz      .done
.copy:
    mov     al, [rsi]
    mov     [r8], al
    inc     rsi
    inc     r8
    dec     rdx
    jnz     .copy
.done:
    ret

; =============================================================================
; kv_logb_cstr — append a NUL-terminated string.
; @param  rdi  LogBuf*   @param rsi  const char * (NULL appends "(null)")
; @returns nothing.
; =============================================================================
kv_logb_cstr:
    test    rsi, rsi
    jnz     .have
    lea     rsi, [str_null]
.have:
    xor     edx, edx
.len:
    cmp     byte [rsi + rdx], 0
    je      kv_logb_str                 ; tail call with rdx = strlen
    inc     rdx
    cmp     rdx, LOG_CAP
    jb      .len
    jmp     kv_logb_str

; =============================================================================
; kv_logb_u64 — append an unsigned decimal number.
; @param  rdi  LogBuf*   @param rsi  value
; @returns nothing.
; =============================================================================
kv_logb_u64:
    push    rbx
    sub     rsp, 32                     ; digit scratch
    mov     rbx, rdi
    mov     rdi, rsp
    call    kv_fmt_u64
    mov     rdx, rax
    mov     rdi, rbx
    mov     rsi, rsp
    call    kv_logb_str
    add     rsp, 32
    pop     rbx
    ret

; =============================================================================
; kv_logb_emit — terminate the line with '\n' and write it to stderr.
; @param  rdi  LogBuf*
; @returns nothing (a failed write to stderr cannot be reported anywhere).
; =============================================================================
kv_logb_emit:
    mov     rax, [rdi + LogBuf.len]
    mov     byte [rdi + LogBuf.data + rax], 10
    lea     rdx, [rax + 1]
    lea     rsi, [rdi + LogBuf.data]
    mov     edi, 2
    mov     eax, SYS_write
    syscall
    ret

; =============================================================================
; kv_fmt_u64 — decimal digits of an unsigned 64-bit value.
; @param  rdi  char *dst (at least 20 bytes)
; @param  rsi  value
; @returns rax = number of digits written (1..20); no terminator.
; @remarks Divides by the constant 10 via DIV (clear, and logging is not a hot
;          path). Clobbers rcx, rdx, r8-r10.
; =============================================================================
kv_fmt_u64:
    mov     rax, rsi
    mov     r8, rdi
    mov     r9d, 10
    xor     ecx, ecx                    ; digits produced (reversed)
    sub     rsp, 24
.digit:
    xor     edx, edx
    div     r9
    add     dl, '0'
    mov     [rsp + rcx], dl
    inc     ecx
    test    rax, rax
    jnz     .digit
    mov     eax, ecx
    xor     r10d, r10d
.reverse:
    dec     ecx
    mov     dl, [rsp + rcx]
    mov     [r8 + r10], dl
    inc     r10d
    test    ecx, ecx
    jnz     .reverse
    add     rsp, 24
    ret

section .rodata
str_null:   db "(null)", 0

section .note.GNU-stack noalloc noexec nowrite progbits
