; =============================================================================
; crc32c.asm — CRC-32C (Castagnoli), the integrity check of Upload/Download.
;
; What: the CRC of iSCSI, ext4 and SCTP (RFC 3720 §12.1): reflected
; polynomial 0x82F63B78, register preset to all ones, result inverted. The
; functions here are zlib-style: pass 0 for a fresh checksum and the previous
; result to continue one, so crc(a || b) == crc(crc(a), b) — which is how a
; value that arrives in chunks is checked chunk by chunk.
;
; Why CRC-32C: it is the checksum large-object stores already speak (Google
; Cloud Storage, S3's checksum option, gRPC-adjacent tooling), every client
; language has it, and x86-64 CPUs since 2008 compute it in hardware (the
; SSE4.2 `crc32` instruction), so an end-to-end check costs little next to
; the copy it rides on.
;
; Two implementations with identical results, chosen once at startup
; (kv_crc32c_set_mode, from KVNODE_CRC32C):
;   sse42  `crc32` on 8 bytes per instruction. SSE4.2 is NOT part of the
;          x86-64 baseline (it is x86-64-v2), so it is used only when CPUID
;          reports it.
;   table  slicing-by-8 with plain 64-bit registers and eight 1 KiB tables:
;          runs on every x86-64 CPU. It is also the mode before any
;          kv_crc32c_set_mode call, so an uninitialized process is slow but
;          never wrong.
; Both exist in two shapes: checksum only (kv_crc32c*) and fused copy +
; checksum (kv_crc32c_copy*), which moves bytes and checks them in one pass
; over memory — the shape the transfer paths need (an upload chunk into its
; record, a download chunk into the output buffer).
;
; The tables are computed by the assembler (NASM preprocessor loops below):
; no generated source file, no startup work, and nothing to initialize. The
; unit test (tests/test_crc32c.c) checks them against a bitwise reference and
; the RFC 3720 vectors.
;
; Threading: every function is a pure computation; kv_crc32c_mode is written
; only by kv_crc32c_set_mode before any worker thread starts.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"

%define CRC32C_POLY     0x82F63B78      ; reflected Castagnoli polynomial
%define CRC_MODE_TABLE  0               ; slicing-by-8, baseline x86-64
%define CRC_MODE_SSE42  1               ; SSE4.2 crc32 instruction
%define CRC_MODE_MAX    1
%define ENODEV          19              ; mode not supported by this CPU

section .rodata
; -----------------------------------------------------------------------------
; crc_tab — eight 256-entry tables for slicing-by-8, table k at crc_tab + k*1024.
; Entry i of table k is the raw (unconditioned) reflected CRC-32C register
; after 8*(k+1) bit steps starting from the value i: table 0 is the classic
; one-byte table, and table k folds in k further zero bytes, which is exactly
; what slicing-by-8 needs to consume 8 input bytes with 8 independent loads.
; Computing each entry from scratch (instead of from table k-1) keeps the
; preprocessor free of cross-references; it costs ~74k bit steps at assembly
; time, a fraction of a second.
; -----------------------------------------------------------------------------
align 64
crc_tab:
%assign k 0
%rep 8
  %assign i 0
  %rep 256
    %assign c i
    %rep 8 * (k + 1)
      %if c & 1
        %assign c (c >> 1) ^ CRC32C_POLY
      %else
        %assign c (c >> 1)
      %endif
    %endrep
    dd c
    %assign i i + 1
  %endrep
  %assign k k + 1
%endrep

section .data
align 64
global kv_crc32c_mode
; The mode byte dispatch reads on every call: alone on its cache line, and
; written only before workers start, so it is effectively read-only shared.
kv_crc32c_mode: db CRC_MODE_TABLE
                times 63 db 0

section .text

global kv_crc32c
global kv_crc32c_copy
global kv_crc32c_sw
global kv_crc32c_hw
global kv_crc32c_copy_sw
global kv_crc32c_copy_hw
global kv_crc32c_hw_available
global kv_crc32c_set_mode

; =============================================================================
; kv_crc32c — CRC-32C of a buffer, continuing a previous result.
;
; @param  edi  crc: 0 for a new checksum, or the result of a previous call
;              over the bytes that precede this buffer
; @param  rsi  const void *buf (may be NULL when len is 0)
; @param  rdx  size_t len
; @returns eax = the CRC-32C of everything checksummed so far (upper half of
;          rax zero).
; @remarks Dispatches on kv_crc32c_mode (a perfectly predicted branch); both
;          targets give identical results. Reads exactly [buf, buf + len).
; =============================================================================
kv_crc32c:
    cmp     byte [rel kv_crc32c_mode], CRC_MODE_SSE42
    je      kv_crc32c_hw
    jmp     kv_crc32c_sw

; =============================================================================
; kv_crc32c_copy — copy a buffer and return the continued CRC-32C of it.
;
; @param  edi  crc to continue (0 for a new checksum)
; @param  rsi  void *dst (len writable bytes; must not overlap src)
; @param  rdx  const void *src
; @param  rcx  size_t len
; @returns eax = the continued CRC-32C; dst holds a copy of src.
; @remarks One pass over the data: each 8-byte word is loaded once, stored
;          and folded into the checksum, so checking a chunk that must be
;          copied anyway costs little more than the copy (measured in
;          tests/test_crc32c.c). Overlapping buffers give an unspecified
;          copy (the checksum is still that of src as read).
; =============================================================================
kv_crc32c_copy:
    cmp     byte [rel kv_crc32c_mode], CRC_MODE_SSE42
    je      kv_crc32c_copy_hw
    jmp     kv_crc32c_copy_sw

; =============================================================================
; kv_crc32c_hw — kv_crc32c with the SSE4.2 crc32 instruction.
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Requires SSE4.2 (kv_crc32c_hw_available): on a CPU without it the
;          instruction faults (#UD). Exported for the unit test, which
;          compares it with the table version; the server reaches it only
;          through kv_crc32c after kv_crc32c_set_mode verified the CPU.
;          One dependency chain of `crc32 r64, m64` (3-cycle latency): about
;          2.7 bytes per cycle. A 3-way interleave with a PCLMULQDQ merge
;          could approach 8 bytes per cycle — not done: the transfer paths
;          are bounded by copies and syscalls first (see the measurement in
;          tests/test_crc32c.c before optimizing this).
; =============================================================================
kv_crc32c_hw:
    mov     eax, edi
    not     eax                         ; preset: all ones (zlib-style API)
    cmp     rdx, 8
    jb      .tail
.words:
    crc32   rax, qword [rsi]            ; 8 bytes; the result is zero-extended
    add     rsi, 8
    sub     rdx, 8
    cmp     rdx, 8
    jae     .words
.tail:
    test    rdx, rdx
    jz      .done
.byte:
    crc32   eax, byte [rsi]
    inc     rsi
    dec     rdx
    jnz     .byte
.done:
    not     eax                         ; final inversion
    ret

; =============================================================================
; kv_crc32c_copy_hw — kv_crc32c_copy with the SSE4.2 crc32 instruction.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src.
; @remarks Requires SSE4.2 (see kv_crc32c_hw). Unaligned 8-byte loads and
;          stores: both buffers may start anywhere. Clobbers r8, rcx, rdx,
;          rsi.
; =============================================================================
kv_crc32c_copy_hw:
    mov     eax, edi
    not     eax
    cmp     rcx, 8
    jb      .tail
.words:
    mov     r8, [rdx]
    mov     [rsi], r8
    crc32   rax, r8
    add     rdx, 8
    add     rsi, 8
    sub     rcx, 8
    cmp     rcx, 8
    jae     .words
.tail:
    test    rcx, rcx
    jz      .done
.byte:
    movzx   r8d, byte [rdx]
    mov     [rsi], r8b
    crc32   eax, r8b
    inc     rdx
    inc     rsi
    dec     rcx
    jnz     .byte
.done:
    not     eax
    ret

; -----------------------------------------------------------------------------
; SLICE8 w — fold one 8-byte little-endian word (in register w, a 64-bit GPR
; other than rax, rcx, r10, r11) into the running register value in eax.
; Standard slicing-by-8: the low 4 bytes are XORed with the register, then
; each of the 8 bytes indexes its own table (byte j of the word uses table
; 7 - j) and the eight entries are XORed together. Clobbers w, rcx, r10,
; r11; uses r9 = crc_tab.
; Bytes are extracted with shifts and the low-byte registers only: the legacy
; high-byte registers (ch) cannot be encoded in an instruction that also
; needs a REX prefix, which every use of r10/r11 does.
; -----------------------------------------------------------------------------
%macro SLICE8 1
    mov     rcx, %1
    xor     ecx, eax                    ; first 4 bytes ^ register (clears bits 32-63)
    shr     %1, 32                      ; last 4 bytes
    movzx   r10d, cl
    mov     eax, [r9 + 7 * 1024 + r10 * 4]
    mov     r11d, ecx
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 6 * 1024 + r10 * 4]
    mov     r11d, ecx
    shr     r11d, 16
    movzx   r10d, r11b
    xor     eax, [r9 + 5 * 1024 + r10 * 4]
    shr     ecx, 24
    xor     eax, [r9 + 4 * 1024 + rcx * 4]
    mov     r11, %1
    movzx   r10d, r11b
    xor     eax, [r9 + 3 * 1024 + r10 * 4]
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 2 * 1024 + r10 * 4]
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 1 * 1024 + r10 * 4]
    shr     r11d, 8
    xor     eax, [r9 + r11 * 4]         ; table 0: the byte with no zero bytes after it
%endmacro

; -----------------------------------------------------------------------------
; SLICE1 b — fold one byte (in the low 8 bits of 32-bit register b, not eax,
; ecx or r10) into eax with table 0. Clobbers ecx, r10; uses r9 = crc_tab.
; -----------------------------------------------------------------------------
%macro SLICE1 1
    mov     ecx, eax
    xor     ecx, %1
    movzx   r10d, cl
    shr     eax, 8
    xor     eax, [r9 + r10 * 4]
%endmacro

; =============================================================================
; kv_crc32c_sw — kv_crc32c with slicing-by-8 tables (any x86-64 CPU).
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Plain 64-bit registers only (the "no SIMD requirement" path).
;          Eight table lookups per 8 bytes, all independent of each other,
;          so they overlap in the load ports; the tables (8 KiB) stay in L1
;          while a chunk is processed. Clobbers rcx, rdx, rsi, r8-r11.
; =============================================================================
kv_crc32c_sw:
    mov     eax, edi
    not     eax
    lea     r9, [crc_tab]
    cmp     rdx, 8
    jb      .tail
.words:
    mov     r8, [rsi]
    SLICE8  r8
    add     rsi, 8
    sub     rdx, 8
    cmp     rdx, 8
    jae     .words
.tail:
    test    rdx, rdx
    jz      .done
.byte:
    movzx   r8d, byte [rsi]
    SLICE1  r8d
    inc     rsi
    dec     rdx
    jnz     .byte
.done:
    not     eax
    ret

; =============================================================================
; kv_crc32c_copy_sw — kv_crc32c_copy with slicing-by-8 tables.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src.
; @remarks Each word is stored to dst from the register it was folded from,
;          so src is read once. The length moves to rdi because SLICE8
;          clobbers rcx. Clobbers rcx, rdx, rsi, rdi, r8-r11.
; =============================================================================
kv_crc32c_copy_sw:
    mov     eax, edi
    not     eax
    mov     rdi, rcx                    ; length (rcx is SLICE8 scratch)
    lea     r9, [crc_tab]
    cmp     rdi, 8
    jb      .tail
.words:
    mov     r8, [rdx]
    mov     [rsi], r8                   ; store before SLICE8 consumes r8
    SLICE8  r8
    add     rdx, 8
    add     rsi, 8
    sub     rdi, 8
    cmp     rdi, 8
    jae     .words
.tail:
    test    rdi, rdi
    jz      .done
.byte:
    movzx   r8d, byte [rdx]
    mov     [rsi], r8b
    SLICE1  r8d
    inc     rdx
    inc     rsi
    dec     rdi
    jnz     .byte
.done:
    not     eax
    ret

; =============================================================================
; kv_crc32c_hw_available — does this CPU have the SSE4.2 crc32 instruction?
;
; @returns eax = 1 if CPUID leaf 1 reports SSE4.2 (ECX bit 20), else 0.
; @remarks crc32 works on general-purpose registers only, so unlike AVX it
;          needs no operating-system support check (XGETBV): the CPUID bit is
;          the whole answer. CPUID is slow (~100 cycles, a VM exit under a
;          hypervisor): call it once at startup, never per operation.
; =============================================================================
kv_crc32c_hw_available:
    push    rbx                         ; cpuid writes ebx (callee-saved)
    mov     eax, 1
    xor     ecx, ecx
    cpuid
    xor     eax, eax
    bt      ecx, 20
    setc    al
    pop     rbx
    ret

; =============================================================================
; kv_crc32c_set_mode — choose the implementation behind kv_crc32c*.
;
; @param  edi  CRC_MODE_TABLE (0) or CRC_MODE_SSE42 (1)
; @returns eax = the previous mode, -EINVAL for an unknown mode, or -ENODEV
;          when SSE4.2 is requested on a CPU without it (mode unchanged in
;          both error cases).
; @remarks Not synchronized: call it before any thread computes checksums
;          (main does, from KVNODE_CRC32C). Refusing SSE4.2 on an older CPU
;          is what keeps a forced setting from crashing a worker with #UD
;          later: main turns -ENODEV into a startup error.
; =============================================================================
kv_crc32c_set_mode:
    cmp     edi, CRC_MODE_MAX
    ja      .bad
    cmp     edi, CRC_MODE_SSE42
    jne     .set
    push    rdi                         ; also realigns the stack for the call
    call    kv_crc32c_hw_available
    pop     rdi
    test    eax, eax
    jz      .nodev
.set:
    movzx   eax, byte [rel kv_crc32c_mode]
    mov     [rel kv_crc32c_mode], dil
    ret
.nodev:
    mov     eax, -ENODEV
    ret
.bad:
    mov     eax, -EINVAL
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
