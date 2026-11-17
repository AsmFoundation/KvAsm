; =============================================================================
; util.asm — small leaf primitives shared by the store and the bake-off
; structures: exact key comparison and SWAR tag matching.
;
; Both are written to clobber as few registers as possible (rax, rcx, r8),
; which lets hot loops in cuckoo.asm keep their state in the remaining
; caller-saved registers across calls instead of spilling to the stack.
; =============================================================================

default rel
bits 64

section .text

global kv_keyeq
global kv_tag_match

; =============================================================================
; kv_keyeq — are two byte strings of the same length equal?
;
; @param  rdi  const uint8_t *a
; @param  rsi  const uint8_t *b
; @param  rdx  size_t len (0 compares equal)
; @returns eax = 1 if the first len bytes are identical, else 0.
; @remarks Exactness is the point: KvAsm identifies keys by their bytes, not
;          by hashes, so no hash collision can ever return another key's
;          value. Reads never go outside [a, a+len) or [b, b+len): lengths
;          >= 8 compare 8-byte words and finish with an overlapping word that
;          ends exactly at len; shorter lengths use 4/2/1-byte loads.
;          No SIMD. Clobbers only rax, rcx, r8 (rdi/rsi/rdx preserved) —
;          cuckoo.asm relies on this.
; =============================================================================
kv_keyeq:
    cmp     rdx, 8
    jb      .small
    lea     rcx, [rdx - 8]              ; offset of the final (overlapping) word
    xor     r8d, r8d
.words:
    cmp     r8, rcx
    jae     .last
    mov     rax, [rdi + r8]
    cmp     rax, [rsi + r8]
    jne     .ne
    add     r8, 8
    jmp     .words
.last:
    mov     rax, [rdi + rcx]
    cmp     rax, [rsi + rcx]
    jne     .ne
    mov     eax, 1
    ret
.small:
    xor     r8d, r8d                    ; byte offset
    test    dl, 4
    jz      .s2
    mov     eax, [rdi]
    cmp     eax, [rsi]
    jne     .ne
    mov     r8d, 4
.s2:
    test    dl, 2
    jz      .s1
    movzx   eax, word [rdi + r8]
    movzx   ecx, word [rsi + r8]
    cmp     eax, ecx
    jne     .ne
    add     r8d, 2
.s1:
    test    dl, 1
    jz      .eq
    movzx   eax, byte [rdi + r8]
    cmp     al, [rsi + r8]
    jne     .ne
.eq:
    mov     eax, 1
    ret
.ne:
    xor     eax, eax
    ret

; =============================================================================
; kv_tag_match — which of 8 packed one-byte tags equal `tag`?
;
; @param  rdi  uint64_t tags (byte i = tag of slot i)
; @param  rsi  tag to find (low 8 bits used; 0 finds empty slots)
; @returns rax = 0x80 in every byte whose tag equals `tag`, 0 elsewhere.
; @remarks Exact per byte (no false positives), 64-bit GPR arithmetic only:
;            x = tags ^ broadcast(tag)          ; equal bytes become 0x00
;            y = (x & 0x7F..7F) + 0x7F..7F      ; bit7 set iff low 7 bits != 0
;            r = ~(y | x | 0x7F..7F)            ; bit7 set iff byte == 0
;          The add cannot carry across bytes (max 0x7F + 0x7F = 0xFE), unlike
;          the popular (x - 0x01..) & ~x & 0x80.. form, which can also flag
;          the byte above a real match. cuckoo.asm inlines the same sequence;
;          this exported copy exists for the unit test.
; =============================================================================
kv_tag_match:
    movzx   esi, sil
    mov     rax, 0x0101010101010101
    imul    rsi, rax
    xor     rdi, rsi
    mov     rax, 0x7F7F7F7F7F7F7F7F
    mov     rcx, rdi
    and     rcx, rax
    add     rcx, rax
    or      rcx, rdi
    or      rcx, rax
    not     rcx
    mov     rax, rcx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
