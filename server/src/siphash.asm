; =============================================================================
; siphash.asm — SipHash-2-4 with 128-bit output (Aumasson & Bernstein).
;
; Why SipHash: KvAsm places keys by hash, and clients choose the keys. A
; keyed pseudo-random function with a per-process secret key (from
; getrandom) means nobody outside the process can predict which bucket a key
; lands in, so bucket occupancy stays random no matter what keys arrive. It is
; pure 64-bit add/rotate/xor (ARX) — no SIMD, no multiplication — which keeps
; the "no SIMD requirement" rule.
;
; Why the 128-bit variant: one pass yields two independent 64-bit words:
; h1 selects the primary cuckoo bucket, h2 supplies the one-byte tag (which in
; turn selects the alternate bucket). The extra cost over the 64-bit variant
; is four SipRounds.
;
; Verified against the reference vectors (veorq/SipHash vectors.h,
; vectors_sip128) in server/tests/test_siphash.c.
; =============================================================================

default rel
bits 64

; SipRound on v0..v3 = r8..r11 (reference order, see the SipHash paper).
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

section .text

global kv_siphash128

; =============================================================================
; kv_siphash128 — SipHash-2-4-128 of a byte string.
;
; @param  rdi  const uint8_t *msg (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  const uint64_t key[2] (k0, k1; little-endian key bytes)
; @param  rcx  uint64_t out[2] — receives (h1, h2); may be NULL
; @returns rax = h1 and rdx = h2 (also stored to out when non-NULL), so hot
;          callers can keep both halves in registers.
; @remarks Never reads outside [msg, msg+len): the final partial word is
;          assembled either from an overlapping 8-byte load that ends exactly
;          at msg+len (len >= 8) or from 4/2/1-byte loads (len < 8). A naive
;          8-byte load of the tail could cross into an unmapped page.
;          Clobbers rdi and r8-r11 (plus rax/rdx as results); rsi and rcx
;          are preserved, so callers may keep len/out in them.
; =============================================================================
kv_siphash128:
    mov     r8, 0x736f6d6570736575      ; v0 = k0 ^ "somepseu"
    mov     r9, 0x646f72616e646f6d      ; v1 = k1 ^ "dorandom"
    mov     r10, 0x6c7967656e657261     ; v2 = k0 ^ "lygenera"
    mov     r11, 0x7465646279746573     ; v3 = k1 ^ "tedbytes"
    mov     rax, [rdx]                  ; k0
    xor     r8, rax
    xor     r10, rax
    mov     rax, [rdx + 8]              ; k1
    xor     r9, rax
    xor     r11, rax
    xor     r9, 0xee                    ; 128-bit output domain separation

    push    rcx                         ; out pointer, restored at the end
    mov     rcx, rsi
    shr     rcx, 3                      ; number of full 8-byte words
    jz      .tail
.block:
    mov     rax, [rdi]
    xor     r11, rax
    SIPROUND
    SIPROUND
    xor     r8, rax
    add     rdi, 8
    dec     rcx
    jnz     .block

.tail:
    ; b = (len << 56) | remaining (len % 8) bytes, little-endian.
    mov     rdx, rsi
    and     edx, 7                      ; left = len % 8
    xor     eax, eax
    test    edx, edx
    jz      .final
    cmp     rsi, 8
    jb      .short_msg
    ; len >= 8: the last 8 bytes end at msg+len; the wanted `left` bytes are
    ; the top `left` bytes of that word. rdi already points at the tail.
    mov     rax, [rdi + rdx - 8]
    mov     ecx, 8
    sub     ecx, edx
    shl     ecx, 3                      ; shift = 64 - 8*left (1..7 bytes kept)
    shr     rax, cl
    jmp     .final
.short_msg:
    ; len < 8 (so left == len): gather 4/2/1-byte pieces without overreading.
    xor     ecx, ecx                    ; byte offset
    test    dl, 4
    jz      .t2
    mov     eax, [rdi]
    mov     ecx, 4
.t2:
    test    dl, 2
    jz      .t1
    push    rdx
    movzx   edx, word [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    shr     ecx, 3
    or      rax, rdx
    pop     rdx
    add     ecx, 2
.t1:
    test    dl, 1
    jz      .final
    movzx   edx, byte [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    or      rax, rdx
.final:
    mov     rdx, rsi
    shl     rdx, 56
    or      rax, rdx                    ; b
    xor     r11, rax
    SIPROUND
    SIPROUND
    xor     r8, rax

    xor     r10, 0xee                   ; finalization (128-bit variant)
    SIPROUND
    SIPROUND
    SIPROUND
    SIPROUND
    mov     rax, r8
    xor     rax, r9
    xor     rax, r10
    xor     rax, r11                    ; h1

    xor     r9, 0xdd
    SIPROUND
    SIPROUND
    SIPROUND
    SIPROUND
    mov     rdx, r8
    xor     rdx, r9
    xor     rdx, r10
    xor     rdx, r11                    ; h2

    pop     rcx
    test    rcx, rcx
    jz      .done
    mov     [rcx], rax
    mov     [rcx + 8], rdx
.done:
    ret

global kv_siphash64

; =============================================================================
; kv_siphash64 — SipHash-2-4 with the standard 64-bit output, returned twice.
;
; @param  rdi  const uint8_t *msg (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  const uint64_t key[2] (k0, k1; little-endian key bytes)
; @param  rcx  uint64_t out[2] — receives (h, h); may be NULL
; @returns rax = rdx = h, the 64-bit SipHash-2-4 of the message (also stored
;          to both words of out when non-NULL).
; @remarks When to use: as the store's placement hash in place of
;          kv_siphash128. The table needs two things from a key's hash — a
;          bucket index (low bits) and a one-byte tag (top byte) — and disjoint
;          bits of one 64-bit pseudo-random word serve both, so the second
;          64-bit word of the 128-bit variant is never needed: this saves its
;          four extra finalization rounds (56 instructions; 24 core cycles of
;          72-88 per 16-byte key, measured). Same function, same key, same
;          HashDoS argument as SipHash-2-4-128.
;          Tradeoff: callers that want two INDEPENDENT 64-bit words (the
;          private caches index by h2, hcache.asm) get the same word twice —
;          still correct there, because every candidate is confirmed by an
;          exact key compare, but their two indices then come from one word.
;          It returns the word in both result registers so every caller of
;          kv_store_hash keeps working unchanged with (h1, h2) = (h, h).
;          Never reads outside [msg, msg+len) (same tail handling as
;          kv_siphash128). Clobbers rdi and r8-r11; rsi and rcx are preserved.
; =============================================================================
kv_siphash64:
    mov     r8, 0x736f6d6570736575      ; v0 = k0 ^ "somepseu"
    mov     r9, 0x646f72616e646f6d      ; v1 = k1 ^ "dorandom"
    mov     r10, 0x6c7967656e657261     ; v2 = k0 ^ "lygenera"
    mov     r11, 0x7465646279746573     ; v3 = k1 ^ "tedbytes"
    mov     rax, [rdx]                  ; k0
    xor     r8, rax
    xor     r10, rax
    mov     rax, [rdx + 8]              ; k1
    xor     r9, rax
    xor     r11, rax                    ; (no 0xee: that is the 128-bit variant's separator)

    push    rcx                         ; out pointer, restored at the end
    mov     rcx, rsi
    shr     rcx, 3                      ; number of full 8-byte words
    jz      .tail
.block:
    mov     rax, [rdi]
    xor     r11, rax
    SIPROUND
    SIPROUND
    xor     r8, rax
    add     rdi, 8
    dec     rcx
    jnz     .block

.tail:
    ; b = (len << 56) | remaining (len % 8) bytes, little-endian.
    mov     rdx, rsi
    and     edx, 7                      ; left = len % 8
    xor     eax, eax
    test    edx, edx
    jz      .final
    cmp     rsi, 8
    jb      .short_msg
    ; len >= 8: the last 8 bytes end at msg+len; the wanted `left` bytes are
    ; the top `left` bytes of that word. rdi already points at the tail.
    mov     rax, [rdi + rdx - 8]
    mov     ecx, 8
    sub     ecx, edx
    shl     ecx, 3                      ; shift = 64 - 8*left (1..7 bytes kept)
    shr     rax, cl
    jmp     .final
.short_msg:
    ; len < 8 (so left == len): gather 4/2/1-byte pieces without overreading.
    xor     ecx, ecx                    ; byte offset
    test    dl, 4
    jz      .t2
    mov     eax, [rdi]
    mov     ecx, 4
.t2:
    test    dl, 2
    jz      .t1
    push    rdx
    movzx   edx, word [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    shr     ecx, 3
    or      rax, rdx
    pop     rdx
    add     ecx, 2
.t1:
    test    dl, 1
    jz      .final
    movzx   edx, byte [rdi + rcx]
    shl     ecx, 3
    shl     rdx, cl
    or      rax, rdx
.final:
    mov     rdx, rsi
    shl     rdx, 56
    or      rax, rdx                    ; b
    xor     r11, rax
    SIPROUND
    SIPROUND
    xor     r8, rax

    xor     r10, 0xff                   ; finalization (64-bit variant)
    SIPROUND
    SIPROUND
    SIPROUND
    SIPROUND
    mov     rax, r8
    xor     rax, r9
    xor     rax, r10
    xor     rax, r11                    ; h
    mov     rdx, rax                    ; the same word as "h2": the tag is its top byte

    pop     rcx
    test    rcx, rcx
    jz      .done
    mov     [rcx], rax
    mov     [rcx + 8], rdx
.done:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
