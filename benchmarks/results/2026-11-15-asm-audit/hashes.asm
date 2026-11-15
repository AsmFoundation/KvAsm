; =============================================================================
; hashes.asm — candidate key hashes for the store, for hashbench.c.
;
; The store places a key by a keyed hash under a per-process secret (ADR 0004:
; SipHash-2-4-128; h1 picks the bucket, h2's top byte is the tag). This file
; holds the alternatives the audit measures against it:
;
;   sip_c_d_128 / _64   SipHash with c compression and d finalization rounds,
;                       128- or 64-bit output (2-4-128 must equal the server's
;                       kv_siphash128; 1-3 is the reduced variant Rust uses)
;   kv_aeshash128       one AES-128 encryption of the padded key under 11
;                       secret round keys: for keys of up to 16 bytes this is
;                       AES as a pseudo-random function, the strongest claim a
;                       128-bit hash can make, in 10 AESENC instructions
;   calib_add           a 1-cycle-per-iteration dependency chain: turns a
;                       wall-clock time into the core's real frequency
;
; AES-NI is not baseline x86-64: a server using kv_aeshash128 needs CPUID
; dispatch and keeps SipHash as the fallback (as crc32c.asm does for PCLMUL).
; =============================================================================

default rel
bits 64

; SipRound on v0..v3 = r8..r11 (the same macro as siphash.asm).
%macro SIPROUND 0
    add     r8, r9
    rol     r9, 13
    xor     r9, r8
    rol     r8, 32
    add     r10, r11
    rol     r11, 16
    xor     r11, r10
    add     r8, r11
    rol     r11, 21
    xor     r11, r8
    add     r10, r9
    rol     r9, 17
    xor     r9, r10
    rol     r10, 32
%endmacro

; SIPHASH name, c, d, bits — a SipHash-c-d function with a 64- or 128-bit
; result. Same ABI as kv_siphash128: rdi msg, rsi len, rdx key[2] -> rax (h1)
; and, for 128 bits, rdx (h2). Never reads outside [msg, msg+len).
%macro SIPHASH 4
global %1
%1:
    mov     r8, 0x736f6d6570736575
    mov     r9, 0x646f72616e646f6d
    mov     r10, 0x6c7967656e657261
    mov     r11, 0x7465646279746573
    mov     rax, [rdx]
    xor     r8, rax
    xor     r10, rax
    mov     rax, [rdx + 8]
    xor     r9, rax
    xor     r11, rax
%if %4 = 128
    xor     r9, 0xee                    ; 128-bit output domain separation
%endif
    mov     rcx, rsi
    shr     rcx, 3
    jz      %%tail
%%block:
    mov     rax, [rdi]
    xor     r11, rax
%rep %2
    SIPROUND
%endrep
    xor     r8, rax
    add     rdi, 8
    dec     rcx
    jnz     %%block
%%tail:
    mov     rdx, rsi
    and     edx, 7
    xor     eax, eax
    test    edx, edx
    jz      %%final
    cmp     rsi, 8
    jb      %%short
    mov     rax, [rdi + rdx - 8]        ; last 8 bytes end at msg+len
    mov     ecx, 8
    sub     ecx, edx
    shl     ecx, 3
    shr     rax, cl
    jmp     %%final
%%short:
    xor     ecx, ecx
    test    dl, 4
    jz      %%t2
    mov     eax, [rdi]
    mov     ecx, 4
%%t2:
    test    dl, 2
    jz      %%t1
    push    rdx
    movzx   edx, word [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    shr     ecx, 3
    or      rax, rdx
    pop     rdx
    add     ecx, 2
%%t1:
    test    dl, 1
    jz      %%final
    movzx   edx, byte [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    or      rax, rdx
%%final:
    mov     rdx, rsi
    shl     rdx, 56
    or      rax, rdx
    xor     r11, rax
%rep %2
    SIPROUND
%endrep
    xor     r8, rax
%if %4 = 128
    xor     r10, 0xee
%else
    xor     r10, 0xff
%endif
%rep %3
    SIPROUND
%endrep
    mov     rax, r8
    xor     rax, r9
    xor     rax, r10
    xor     rax, r11
%if %4 = 128
    xor     r9, 0xdd
%rep %3
    SIPROUND
%endrep
    mov     rdx, r8
    xor     rdx, r9
    xor     rdx, r10
    xor     rdx, r11
%endif
    ret
%endmacro

section .text

SIPHASH sip_2_4_128, 2, 4, 128
SIPHASH sip_2_4_64,  2, 4, 64
SIPHASH sip_1_3_128, 1, 3, 128
SIPHASH sip_1_3_64,  1, 3, 64

; =============================================================================
; kv_aeshash128 — AES-128 of the key as a keyed 128-bit hash.
;
; @param  rdi  const uint8_t *msg (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  const AesKey *k: 11 round keys (176 bytes, 16-byte aligned),
;              then 17 length masks of 16 bytes each (mask[n] for a final
;              block of n bytes), all secret and random
; @returns rax = low 64 bits (h1), rdx = high 64 bits (h2).
; @remarks len <= 16: the key is zero-padded to one block, XORed with the
;          secret mask of its length (so "ab" and "ab\0" start from unrelated
;          blocks) and encrypted once: ten AESENC. Longer keys are chained
;          CBC-MAC style — every 16-byte block is XORed into the state and
;          the state encrypted — with the length mask applied to the last
;          block, which makes the chain prefix-free.
;          Never reads outside [msg, msg+len): a tail of 8..15 bytes is two
;          overlapping 8-byte loads, shorter ones are 4/2/1-byte loads; a
;          tail after full blocks is one 16-byte load ending at msg+len,
;          shifted down by a PSHUFB mask (SSSE3, present on every AES-NI CPU).
;          Constant time in the key bytes (AES-NI has no table lookups).
;          Clobbers rcx, r8, r9, xmm0-xmm2.
; =============================================================================
%macro AES10 0                          ; xmm0 = AES-128 encrypt(xmm0) under [rdx]
    pxor    xmm0, [rdx]
    aesenc  xmm0, [rdx + 16]
    aesenc  xmm0, [rdx + 32]
    aesenc  xmm0, [rdx + 48]
    aesenc  xmm0, [rdx + 64]
    aesenc  xmm0, [rdx + 80]
    aesenc  xmm0, [rdx + 96]
    aesenc  xmm0, [rdx + 112]
    aesenc  xmm0, [rdx + 128]
    aesenc  xmm0, [rdx + 144]
    aesenclast xmm0, [rdx + 160]
%endmacro

global kv_aeshash128
kv_aeshash128:
    cmp     rsi, 16
    ja      .long
    je      .full
    ; ---- 0..15 bytes: gather into r8 (low) / r9 (high) without overreading ----
    xor     r8d, r8d
    xor     r9d, r9d
    cmp     rsi, 8
    jb      .lt8
    mov     r8, [rdi]                   ; first 8 bytes
    lea     ecx, [rsi - 8]              ; 0..7 bytes beyond them
    test    ecx, ecx
    jz      .gathered
    mov     r9, [rdi + rsi - 8]         ; last 8 bytes: overlap the first word
    neg     ecx
    add     ecx, 8
    shl     ecx, 3                      ; drop the overlapping low bytes
    shr     r9, cl
    jmp     .gathered
.lt8:
    xor     ecx, ecx                    ; byte offset gathered so far
    test    sil, 4
    jz      .lt4
    mov     r8d, [rdi]
    mov     ecx, 4
.lt4:
    test    sil, 2
    jz      .lt2
    movzx   eax, word [rdi + rcx]
    shl     ecx, 3
    shl     rax, cl
    shr     ecx, 3
    or      r8, rax
    add     ecx, 2
.lt2:
    test    sil, 1
    jz      .gathered
    movzx   eax, byte [rdi + rcx]
    shl     ecx, 3
    shl     rax, cl
    or      r8, rax
.gathered:
    movq    xmm0, r8
    movq    xmm1, r9
    punpcklqdq xmm0, xmm1
    jmp     .mask
.full:
    movdqu  xmm0, [rdi]
.mask:
    mov     rax, rsi
    shl     rax, 4
    pxor    xmm0, [rdx + 176 + rax]     ; secret mask of this length
    AES10
    movq    rax, xmm0
    pextrq  rdx, xmm0, 1
    ret
.long:
    ; ---- more than 16 bytes: CBC-MAC over 16-byte blocks ----
    pxor    xmm2, xmm2                  ; chaining state
    mov     rcx, rsi
.block:
    movdqu  xmm0, [rdi]
    pxor    xmm0, xmm2
    AES10
    movdqa  xmm2, xmm0
    add     rdi, 16
    sub     rcx, 16
    cmp     rcx, 16
    ja      .block
    ; last block: 1..16 bytes that end at msg+len (rdi + rcx)
    movdqu  xmm0, [rdi + rcx - 16]      ; in bounds: at least 16 bytes precede the end
    lea     r8, [rel shift_tab]
    mov     rax, 16
    sub     rax, rcx                    ; bytes to shift out (0..15)
    movdqu  xmm1, [r8 + rax]
    pshufb  xmm0, xmm1                  ; keep the top rcx bytes, moved to the bottom
    shl     rcx, 4
    pxor    xmm0, [rdx + 176 + rcx]     ; length mask: ends the chain
    pxor    xmm0, xmm2
    AES10
    movq    rax, xmm0
    pextrq  rdx, xmm0, 1
    ret

; =============================================================================
; calib_add — rdi iterations of a one-cycle dependency chain.
; @param  rdi  iterations (> 0)
; @returns rax = iterations (the chain's value).
; @remarks ADD and the fused DEC/JNZ both have one cycle of latency and the
;          loop carries two independent chains, so the loop runs at exactly
;          one iteration per core cycle: time / iterations = cycle time.
; =============================================================================
global calib_add
calib_add:
    xor     eax, eax
.loop:
    add     rax, 1
    dec     rdi
    jnz     .loop
    ret

section .rodata
align 16
; PSHUFB control for "shift right by n bytes" (n = 0..15) read at shift_tab+n:
; indices n..15 select the kept bytes, 0x80 zeroes the rest.
shift_tab:
    db 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    times 16 db 0x80

section .note.GNU-stack noalloc noexec nowrite progbits
