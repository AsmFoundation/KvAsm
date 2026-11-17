; =============================================================================
; aeshash.asm — AES-128 as the store's keyed hash, through AES-NI.
;
; Why (ADR 0004, second amendment): the key hash runs on every Get, Put,
; Delete, Upload and Download, and SipHash — an add/rotate/xor chain of
; ~190 instructions — was the largest single item of a request (15% of a
; GET's instructions after it was cut to one 64-bit word). One AES-128
; encryption is ten AESENC instructions that the CPU overlaps with the work
; around them: measured 7.6 core cycles per 16-byte key when hashes overlap
; and 43.8 as a dependency chain, against 50.0 and 66.0 for SipHash-2-4
; (benchmarks/results/2026-11-15-asm-audit).
;
; The construction is CMAC-shaped (NIST SP 800-38B):
;   len <= 16:  h = AES_K(pad0(key) XOR mask[len])
;   len  > 16:  the key is chained in 16-byte blocks, s = AES_K(block XOR s),
;               and the last block (1..16 bytes, zero-padded) is XORed with
;               mask[its length] before the final encryption.
; K is the per-process secret (the store's 16-byte seed, expanded to the 11
; AES round keys); mask[n] = AES_K(n + 1) are 17 secret blocks, one per final
; length — CMAC has two (complete / padded); one per length also separates
; "ab" from "ab\0", which zero padding alone would not.
; What that buys: AES-128 under a secret key is a pseudo-random permutation,
; so nobody outside the process can predict a bucket or a tag (the HashDoS
; defense ADR 0004 asks of the hash), with a primitive that has had far more
; analysis than any hash-table hash. The 128-bit result gives two
; independent words: h1 picks the bucket, h2's top byte is the tag.
;
; AES-NI is not baseline x86-64, so nothing here may run unless
; kv_aes_available() said yes: store.asm builds a table's key block and
; selects this hash only then, and SipHash-2-4 stays the fallback
; (KVASM_HASH, config.asm). Like the SSE2 tag scan it uses XMM registers in
; legacy (non-VEX) encoding: no AVX state, no frequency license, no
; VZEROUPPER concerns.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

section .text

global kv_aes_available
global kv_aeshash_setup
global kv_aeshash128

; AES-128 key expansion step (FIPS-197 5.2): xmm1 = previous round key in,
; next round key out; %1 = the round constant. Clobbers xmm2, xmm3.
%macro KEYEXP 1
    aeskeygenassist xmm2, xmm1, %1
    pshufd  xmm2, xmm2, 0xFF
    movdqa  xmm3, xmm1
    pslldq  xmm3, 4
    pxor    xmm1, xmm3
    pslldq  xmm3, 4
    pxor    xmm1, xmm3
    pslldq  xmm3, 4
    pxor    xmm1, xmm3
    pxor    xmm1, xmm2
%endmacro

; xmm0 = AES-128 encrypt(xmm0) under the 11 round keys at [rdx] (16-byte
; aligned: the round keys are memory operands of legacy-SSE instructions).
%macro AES10 0
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

; =============================================================================
; kv_aes_available — can this CPU run the AES hash?
;
; @returns eax = 1 when CPUID reports AES-NI and SSSE3 (PSHUFB, used for the
;          tail of keys longer than 16 bytes), else 0.
; @remarks The gate for everything else in this file: calling
;          kv_aeshash_setup or kv_aeshash128 on a CPU where this returns 0
;          raises #UD. Every CPU with AES-NI has SSSE3; both bits are checked
;          so a hypervisor that masks one of them cannot produce a fault.
;          Preserves rbx (CPUID clobbers it, and it is callee-saved).
; =============================================================================
kv_aes_available:
    push    rbx
    mov     eax, 1
    cpuid
    xor     eax, eax
    bt      ecx, 25                     ; AES-NI
    jnc     .out
    bt      ecx, 9                      ; SSSE3
    jnc     .out
    mov     eax, 1
.out:
    pop     rbx
    ret

; =============================================================================
; kv_aeshash_setup — derive a table's AES key block from its 16-byte seed.
;
; @param  rdi  const uint8_t seed[16] (secret, random: getrandom)
; @param  rsi  uint8_t block[AESKEY_BYTES], 16-byte aligned: receives the 11
;              round keys, then the 17 length masks
; @returns nothing.
; @remarks The round keys are the standard AES-128 key schedule of the seed;
;          mask[n] = AES_K(the block whose first dword is n + 1), n = 0..16 —
;          distinct blocks under the secret key, as CMAC derives its subkeys
;          from AES_K(0).
;          Requires kv_aes_available(). The block is as secret as the seed:
;          it must never leave the process.
;          Leaf; clobbers rax, rcx, rdx, xmm0-xmm3.
; =============================================================================
kv_aeshash_setup:
    mov     rdx, rsi
    movdqu  xmm1, [rdi]
    movdqa  [rdx], xmm1
    KEYEXP  0x01
    movdqa  [rdx + 16], xmm1
    KEYEXP  0x02
    movdqa  [rdx + 32], xmm1
    KEYEXP  0x04
    movdqa  [rdx + 48], xmm1
    KEYEXP  0x08
    movdqa  [rdx + 64], xmm1
    KEYEXP  0x10
    movdqa  [rdx + 80], xmm1
    KEYEXP  0x20
    movdqa  [rdx + 96], xmm1
    KEYEXP  0x40
    movdqa  [rdx + 112], xmm1
    KEYEXP  0x80
    movdqa  [rdx + 128], xmm1
    KEYEXP  0x1B
    movdqa  [rdx + 144], xmm1
    KEYEXP  0x36
    movdqa  [rdx + 160], xmm1
    xor     ecx, ecx                    ; n
.mask:
    lea     eax, [rcx + 1]
    movd    xmm0, eax                   ; the block n + 1 (upper 96 bits zero)
    AES10
    mov     eax, ecx
    shl     eax, 4
    movdqa  [rdx + AESKEY_RK_BYTES + rax], xmm0
    inc     ecx
    cmp     ecx, AESKEY_MASKS
    jb      .mask
    ret

; =============================================================================
; kv_aeshash128 — the AES keyed hash of a byte string.
;
; @param  rdi  const uint8_t *msg (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  const uint8_t *block: the key block kv_aeshash_setup filled
; @param  rcx  uint64_t out[2] — receives (h1, h2); may be NULL
; @returns rax = h1 (the low 64 bits of the result), rdx = h2 (the high 64
;          bits); also stored to out when non-NULL. The same registers, and
;          the same preserved ones (rsi, rcx), as kv_siphash128 and
;          kv_siphash64, so kv_store_hash's callers cannot tell which hash
;          ran.
; @remarks Requires kv_aes_available(). Never reads outside [msg, msg+len):
;          a key of 8..15 bytes is two overlapping 8-byte loads, a shorter
;          one 4/2/1-byte loads; after full blocks, the last block is one
;          16-byte load that ends exactly at msg+len, moved down by a PSHUFB
;          mask. Constant time in the key bytes (AES-NI has no table
;          lookups; the only data-dependent branches are on the length).
;          Footgun: clobbers xmm0-xmm2, which the SipHash functions never
;          touch. The System V ABI lets a callee do that, but a hand-written
;          caller that kept an XMM register live across kv_store_hash would
;          break when the AES hash is selected (none does: grpc.asm,
;          snapshot.asm and store.asm use no XMM registers).
;          Also clobbers rdi and r8-r10.
; =============================================================================
kv_aeshash128:
    mov     r10, rcx                    ; the out pointer: cl is the shift count below
    cmp     rsi, 16
    ja      .long
    je      .full
    ; ---- 0..15 bytes: gather into r8 (low) / r9 (high) without overreading ----
    xor     r8d, r8d
    xor     r9d, r9d
    cmp     rsi, 8
    jb      .lt8
    mov     r8, [rdi]                   ; the first 8 bytes
    lea     ecx, [rsi - 8]              ; 0..7 bytes beyond them
    test    ecx, ecx
    jz      .gathered
    mov     r9, [rdi + rsi - 8]         ; the last 8 bytes: they overlap the first word
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
    pxor    xmm0, [rdx + AESKEY_RK_BYTES + rax] ; the secret mask of this length
    AES10
.result:
    movq    rax, xmm0
    movhlps xmm1, xmm0                  ; the high word, with SSE2 only (no PEXTRQ)
    movq    rdx, xmm1
    mov     rcx, r10
    test    rcx, rcx
    jz      .done
    mov     [rcx], rax
    mov     [rcx + 8], rdx
.done:
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
    ; The last block: the 1..16 bytes that end at msg+len (= rdi + rcx).
    movdqu  xmm0, [rdi + rcx - 16]      ; in bounds: at least 16 bytes precede the end
    lea     r8, [shift_tab]
    mov     eax, 16
    sub     rax, rcx                    ; bytes to shift out (0..15)
    movdqu  xmm1, [r8 + rax]
    pshufb  xmm0, xmm1                  ; the last rcx bytes moved to the bottom, zero above
    shl     rcx, 4
    pxor    xmm0, [rdx + AESKEY_RK_BYTES + rcx] ; the length mask ends the chain (CMAC)
    pxor    xmm0, xmm2
    AES10
    jmp     .result

section .rodata
align 16
; PSHUFB control for "shift right by n bytes" (n = 0..15), read at
; shift_tab + n: indices n..15 select the kept bytes, 0x80 zeroes the rest.
shift_tab:
    db 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
    times 16 db 0x80

section .note.GNU-stack noalloc noexec nowrite progbits
