; =============================================================================
; snapshot.asm — load a KvAsm snapshot file into the store at startup.
;
; File format (little-endian; written by the C# KvAsm.Client SnapshotWriter):
;   0   "KVASNAP1"                      magic
;   8   u32 version = 1
;   12  u32 flags   = 0
;   16  u64 record_count
;   24  u64 reserved (ignored)
;   32  record_count x { u32 key_len, u32 value_len, key bytes, value bytes }
;   end-16  "KVASEND1"                  trailer magic
;   end-8   u64 checksum = SipHash-2-4-128 (key = 16 zero bytes), low word,
;           over every byte before the trailer
;
; The loader trusts nothing: magic, version, flags, trailer and checksum are
; verified before any record is stored; every record is bounds-checked against
; the end of the data and against the configured key/value limits; the record
; count must consume the data exactly. Any violation stops startup with the
; precise reason (and record index) in the log — serving a silently truncated
; dataset would be worse than not starting. Duplicate keys: the later record
; wins, exactly as a sequence of Puts would.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"

%define SYS_openat      257
%define AT_FDCWD        -100
%define O_RDONLY        0
%define O_CLOEXEC       0x80000
%define MADV_SEQUENTIAL 2
%define STAT_SIZE       48              ; offsetof(struct stat, st_size) on x86-64
%define HEADER_SIZE     32
%define TRAILER_SIZE    16

; stack frame of kv_snapshot_load
%define F_STAT          0               ; struct stat (144 bytes)
%define F_H1            144
%define F_H2            152
%define F_BASE          160
%define F_SIZE          168
%define F_INDEX         176
%define F_KLEN          184
%define F_VLEN          192
%define F_FRAME         200             ; 6 pushes + 200 + return = 256 (aligned)

extern kv_siphash128
extern kv_store_hash
extern kv_store_put
extern kv_logb_init
extern kv_logb_str
extern kv_logb_u64
extern kv_logb_cstr
extern kv_logb_emit
extern kv_logbuf_size

section .rodata
align 8
zero_key:       dq 0, 0
lvl_error:      db "ERROR", 0
m_prefix:       db " msg=", 34, "snapshot rejected", 34, " reason=", 34
m_prefix_len    equ $ - m_prefix
m_record:       db 34, " record="
m_record_len    equ $ - m_record
m_errno:        db 34, " errno="
m_errno_len     equ $ - m_errno
m_quote:        db 34
r_open:         db "cannot open or map file", 0
r_small:        db "file too small", 0
r_magic:        db "bad header magic", 0
r_version:      db "unsupported version or flags", 0
r_trailer:      db "bad trailer magic", 0
r_checksum:     db "checksum mismatch", 0
r_truncated:    db "record truncated", 0
r_key:          db "key length out of range", 0
r_value:        db "value too large", 0
r_store:        db "store rejected record (capacity or memory limit)", 0
r_extra:        db "unexpected bytes after the last record", 0

section .text

global kv_snapshot_load

; =============================================================================
; kv_snapshot_load — verify a snapshot file and Put every record.
;
; @param  rdi  KvTable*
; @param  rsi  const char *path
; @param  rdx  const Config* (key/value limits)
; @returns rax = number of records loaded (>= 0), or a negative value on
;          failure: -errno when the file cannot be opened/mapped, -EINVAL for
;          any format/integrity violation, or the store's -ENOSPC/-ENOMEM.
;          The reason is always logged.
; @remarks Runs before workers start (no concurrent readers). Maps the file
;          read-only and unmaps it before returning; records are copied into
;          the store, so the file may change or disappear afterwards.
; =============================================================================
kv_snapshot_load:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, F_FRAME
    mov     rbx, rdi                    ; table
    mov     r12, rsi                    ; path
    mov     r13, rdx                    ; cfg
    mov     qword [rsp + F_INDEX], -1   ; no record index yet
    mov     qword [rsp + F_BASE], 0

    mov     edi, AT_FDCWD
    mov     rsi, r12
    mov     edx, O_RDONLY | O_CLOEXEC
    xor     r10d, r10d
    mov     eax, SYS_openat
    syscall
    test    rax, rax
    js      .io_failed
    mov     r14, rax                    ; fd
    mov     rdi, r14
    lea     rsi, [rsp + F_STAT]
    mov     eax, SYS_fstat
    syscall
    test    rax, rax
    js      .close_io_failed
    mov     r15, [rsp + F_STAT + STAT_SIZE]
    mov     [rsp + F_SIZE], r15
    cmp     r15, HEADER_SIZE + TRAILER_SIZE
    jb      .close_small
    xor     edi, edi
    mov     rsi, r15
    mov     edx, PROT_READ
    mov     r10d, MAP_PRIVATE
    mov     r8, r14
    xor     r9d, r9d
    mov     eax, SYS_mmap
    syscall
    mov     rbp, rax
    mov     rdi, r14
    mov     eax, SYS_close              ; the mapping outlives the descriptor
    syscall
    mov     rax, rbp
    cmp     rbp, -4095
    jae     .io_failed
    mov     [rsp + F_BASE], rbp
    mov     rdi, rbp
    mov     rsi, r15
    mov     edx, MADV_SEQUENTIAL
    mov     eax, SYS_madvise
    syscall

    ; ---- header, trailer, checksum ----
    mov     rax, 'KVASNAP1'
    lea     rdi, [r_magic]
    cmp     [rbp], rax
    jne     .reject
    lea     rdi, [r_version]
    cmp     dword [rbp + 8], 1
    jne     .reject
    cmp     dword [rbp + 12], 0
    jne     .reject
    mov     rax, 'KVASEND1'
    lea     rdi, [r_trailer]
    cmp     [rbp + r15 - TRAILER_SIZE], rax
    jne     .reject
    mov     rdi, rbp
    lea     rsi, [r15 - TRAILER_SIZE]
    lea     rdx, [zero_key]
    xor     ecx, ecx
    call    kv_siphash128 wrt ..plt
    lea     rdi, [r_checksum]
    cmp     rax, [rbp + r15 - 8]
    jne     .reject

    ; ---- records ----
    mov     r14, [rbp + 16]             ; declared record count
    lea     r12, [rbp + HEADER_SIZE]    ; cursor
    lea     r15, [rbp + r15 - TRAILER_SIZE]   ; end of record data
    mov     qword [rsp + F_INDEX], 0
.record:
    mov     rax, [rsp + F_INDEX]
    cmp     rax, r14
    jae     .all_read
    mov     rcx, r15
    sub     rcx, r12
    lea     rdi, [r_truncated]
    cmp     rcx, 8
    jb      .reject
    mov     eax, [r12]
    mov     [rsp + F_KLEN], rax
    mov     eax, [r12 + 4]
    mov     [rsp + F_VLEN], rax
    add     r12, 8
    mov     rax, [rsp + F_KLEN]
    lea     rdi, [r_key]
    test    rax, rax
    jz      .reject
    cmp     rax, [r13 + Config.max_key]
    ja      .reject
    mov     rdx, [rsp + F_VLEN]
    lea     rdi, [r_value]
    cmp     rdx, [r13 + Config.max_value]
    ja      .reject
    add     rax, rdx                    ; bytes this record needs (both < 2^32)
    mov     rcx, r15
    sub     rcx, r12
    lea     rdi, [r_truncated]
    cmp     rax, rcx
    ja      .reject

    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, [rsp + F_KLEN]
    lea     rcx, [rsp + F_H1]
    call    kv_store_hash wrt ..plt
    sub     rsp, 16                     ; outgoing stack argument, alignment kept
    mov     rax, [rsp + 16 + F_H2]
    mov     [rsp], rax                  ; 7th argument: h2
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, [rsp + 16 + F_KLEN]
    lea     rcx, [r12 + rdx]            ; value follows the key
    mov     r8, [rsp + 16 + F_VLEN]
    mov     r9, [rsp + 16 + F_H1]
    call    kv_store_put wrt ..plt
    add     rsp, 16
    test    rax, rax
    js      .store_failed
    add     r12, [rsp + F_KLEN]
    add     r12, [rsp + F_VLEN]
    inc     qword [rsp + F_INDEX]
    jmp     .record

.all_read:
    lea     rdi, [r_extra]
    cmp     r12, r15
    jne     .reject
    mov     rdi, [rsp + F_BASE]
    mov     rsi, [rsp + F_SIZE]
    mov     eax, SYS_munmap
    syscall
    mov     rax, r14
    jmp     .out

.store_failed:
    mov     r14, rax                    ; the store's -errno
    lea     rdi, [r_store]
    mov     rsi, [rsp + F_INDEX]
    xor     edx, edx
    call    log_rejection
    jmp     .unmap_return
.reject:                                ; rdi = reason
    mov     rsi, [rsp + F_INDEX]
    xor     edx, edx
    call    log_rejection
    mov     r14, -EINVAL
.unmap_return:
    mov     rdi, [rsp + F_BASE]
    mov     rsi, [rsp + F_SIZE]
    mov     eax, SYS_munmap
    syscall
    mov     rax, r14
    jmp     .out
.close_small:
    mov     rdi, r14
    mov     eax, SYS_close
    syscall
    lea     rdi, [r_small]
    mov     rsi, -1
    xor     edx, edx
    call    log_rejection
    mov     rax, -EINVAL
    jmp     .out
.close_io_failed:
    mov     r15, rax
    mov     rdi, r14
    mov     eax, SYS_close
    syscall
    mov     rax, r15
.io_failed:                             ; rax = -errno
    mov     r14, rax
    lea     rdi, [r_open]
    mov     rsi, -1
    mov     rdx, rax
    call    log_rejection
    mov     rax, r14
.out:
    add     rsp, F_FRAME
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; log_rejection — 'msg="snapshot rejected" reason="..." [record=N|errno=E]'.
; Internal.
; @param  rdi  reason (NUL-terminated)
; @param  rsi  record index, or -1 when not record-specific
; @param  rdx  -errno, or 0
; =============================================================================
log_rejection:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    call    kv_logbuf_size
    sub     rsp, rax
    mov     r15, rax
    mov     rbx, rsp
    mov     rdi, rbx
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rbx
    lea     rsi, [m_prefix]
    mov     edx, m_prefix_len
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, r12
    call    kv_logb_cstr
    test    r13, r13
    js      .no_record
    mov     rdi, rbx                    ; '" record=N'
    lea     rsi, [m_record]
    mov     edx, m_record_len
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, r13
    call    kv_logb_u64
    jmp     .emit
.no_record:
    test    r14, r14
    jz      .close_quote
    mov     rdi, rbx                    ; '" errno=E'
    lea     rsi, [m_errno]
    mov     edx, m_errno_len
    call    kv_logb_str
    mov     rsi, r14
    neg     rsi
    mov     rdi, rbx
    call    kv_logb_u64
    jmp     .emit
.close_quote:
    mov     rdi, rbx
    lea     rsi, [m_quote]
    mov     edx, 1
    call    kv_logb_str
.emit:
    mov     rdi, rbx
    call    kv_logb_emit
    add     rsp, r15
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
