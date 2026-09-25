; =============================================================================
; proto.asm — protobuf wire-format codec for the kvnode.v1 messages.
;
; All three request messages share one shape: field 1 = key (bytes) and, for
; PutRequest, field 2 = value (bytes). kv_pb_decode_kv extracts both as
; pointers into the request buffer (zero-copy). Responses are tiny and are
; assembled by the gRPC layer with the varint helpers below.
;
; The decoder treats its input as hostile (it comes straight off the network):
;   * every read is bounds-checked against the end of the message; a varint
;     that runs past the end, exceeds 10 bytes or overflows 64 bits is
;     rejected, and a length-delimited field longer than what remains is
;     rejected before any pointer arithmetic uses it;
;   * field number 0, tags >= 2^32, the deprecated group wire types (3, 4) and
;     the undefined ones (6, 7) are malformed;
;   * unknown fields (and known field numbers with an unexpected wire type)
;     are skipped, as protobuf requires; for repeated occurrences of a
;     singular field the last one wins.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"

; Decoded view of a request (pointers into the caller's buffer).
struc PbKv
    .key:           resq 1
    .key_len:       resq 1
    .value:         resq 1
    .value_len:     resq 1
    .has_key:       resd 1
    .has_value:     resd 1
endstruc

section .text

global kv_pb_decode_kv
global kv_pb_varint_len
global kv_pb_write_varint

; =============================================================================
; kv_pb_decode_kv — decode a GetRequest / PutRequest / DeleteRequest.
;
; @param  rdi  const uint8_t *buf (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  PbKv *out — fully overwritten (absent fields: NULL/0/has=0)
; @returns rax = 0 when the message is well-formed, -EINVAL otherwise (out is
;          then unspecified and must not be used).
; @remarks Zero-copy: key/value point into buf, so buf must outlive their use.
;          O(len), no allocation, no reads outside [buf, buf+len).
;          Clobbers rcx, rdx, rsi, r8-r11.
; =============================================================================
kv_pb_decode_kv:
    xor     eax, eax
    mov     [rdx + PbKv.key], rax
    mov     [rdx + PbKv.key_len], rax
    mov     [rdx + PbKv.value], rax
    mov     [rdx + PbKv.value_len], rax
    mov     [rdx + PbKv.has_key], eax
    mov     [rdx + PbKv.has_value], eax
    mov     r10, rdx                    ; out
    lea     rdx, [rdi + rsi]            ; end
    mov     rsi, rdi                    ; cursor
.field:
    cmp     rsi, rdx
    jae     .ok                         ; clean end of message
    call    pb_read_varint              ; tag
    jc      .bad
    mov     r11, rax
    shr     r11, 3                      ; field number
    jz      .bad                        ; field 0 is invalid
    mov     rcx, 0xFFFFFFFF
    cmp     rax, rcx
    ja      .bad                        ; field numbers are < 2^29
    and     eax, 7                      ; wire type
    cmp     eax, 2
    je      .length_delimited
    test    eax, eax
    je      .skip_varint
    cmp     eax, 1
    je      .skip_fixed64
    cmp     eax, 5
    je      .skip_fixed32
    jmp     .bad                        ; 3/4 (groups), 6/7 (undefined)
.skip_varint:
    call    pb_read_varint
    jc      .bad
    jmp     .field
.skip_fixed64:
    mov     rcx, rdx
    sub     rcx, rsi
    cmp     rcx, 8
    jb      .bad
    add     rsi, 8
    jmp     .field
.skip_fixed32:
    mov     rcx, rdx
    sub     rcx, rsi
    cmp     rcx, 4
    jb      .bad
    add     rsi, 4
    jmp     .field
.length_delimited:
    call    pb_read_varint              ; payload length
    jc      .bad
    mov     rcx, rdx
    sub     rcx, rsi                    ; bytes remaining
    cmp     rax, rcx
    ja      .bad                        ; checked before rsi+rax is ever formed
    cmp     r11, 1
    je      .key
    cmp     r11, 2
    je      .value
    add     rsi, rax                    ; unknown field: skip
    jmp     .field
.key:
    mov     [r10 + PbKv.key], rsi
    mov     [r10 + PbKv.key_len], rax
    mov     dword [r10 + PbKv.has_key], 1
    add     rsi, rax
    jmp     .field
.value:
    mov     [r10 + PbKv.value], rsi
    mov     [r10 + PbKv.value_len], rax
    mov     dword [r10 + PbKv.has_value], 1
    add     rsi, rax
    jmp     .field
.ok:
    xor     eax, eax
    ret
.bad:
    mov     rax, -EINVAL
    ret

; =============================================================================
; pb_read_varint — read one base-128 varint. Internal.
;
; @param  rsi  cursor (advanced past the varint on success)
; @param  rdx  end of the message
; @returns rax = value, CF=0 on success; CF=1 if the varint is truncated,
;          longer than 10 bytes, or does not fit in 64 bits.
; @remarks Clobbers rax, rcx, r8, r9 only (callers keep state in r10/r11).
; =============================================================================
pb_read_varint:
    xor     eax, eax
    xor     ecx, ecx                    ; shift
.byte:
    cmp     rsi, rdx
    jae     .err                        ; truncated
    movzx   r8d, byte [rsi]
    inc     rsi
    mov     r9d, r8d
    and     r9d, 0x7F
    cmp     ecx, 63
    jne     .fits
    cmp     r9d, 1                      ; 10th byte may only carry bit 63
    ja      .err
.fits:
    shl     r9, cl
    or      rax, r9
    test    r8b, 0x80
    jz      .done
    add     ecx, 7
    cmp     ecx, 63
    ja      .err                        ; an 11th byte would be needed
    jmp     .byte
.done:
    clc
    ret
.err:
    stc
    ret

; =============================================================================
; kv_pb_varint_len — encoded size of a varint.
; @param  rdi  value
; @returns rax = 1..10 bytes.
; =============================================================================
kv_pb_varint_len:
    mov     eax, 1
.more:
    cmp     rdi, 0x80
    jb      .done
    shr     rdi, 7
    inc     eax
    jmp     .more
.done:
    ret

; =============================================================================
; kv_pb_write_varint — encode a varint.
; @param  rdi  uint8_t *dst (at least kv_pb_varint_len(value) bytes)
; @param  rsi  value
; @returns rax = bytes written (1..10). Clobbers rcx, rsi.
; =============================================================================
kv_pb_write_varint:
    mov     rax, rdi
.more:
    cmp     rsi, 0x80
    jb      .last
    mov     ecx, esi
    or      cl, 0x80
    mov     [rax], cl
    inc     rax
    shr     rsi, 7
    jmp     .more
.last:
    mov     [rax], sil
    inc     rax
    sub     rax, rdi
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
