; =============================================================================
; proto.asm — protobuf wire-format codec for the kvasm.v1 messages.
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
;
; Streaming calls add two decoders with the same hostile-input rules:
;   kv_pb_decode_download  a DownloadRequest (key bytes + three varints)
;   kv_pb_stream_feed      a stream of gRPC-framed messages (Upload) fed in
;                          arbitrary pieces, reported as field events; see
;                          include/pbstream.inc for why it streams.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "pbstream.inc"

; Decoded view of a request (pointers into the caller's buffer).
struc PbKv
    .key:           resq 1
    .key_len:       resq 1
    .value:         resq 1
    .value_len:     resq 1
    .has_key:       resd 1
    .has_value:     resd 1
endstruc

; Decoded view of a DownloadRequest (kv_pb_decode_download). Absent varints
; read as 0; has_length tells an absent length (to the end) from length 0.
struc PbDl
    .key:           resq 1
    .key_len:       resq 1
    .offset:        resq 1
    .length:        resq 1
    .chunk:         resq 1
    .has_key:       resd 1
    .has_length:    resd 1
endstruc

section .text

global kv_pb_decode_kv
global kv_pb_varint_len
global kv_pb_write_varint
global kv_pb_decode_download
global kv_pb_stream_feed
global kv_pb_stream_idle

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
;          A fast path takes the encoding every protobuf writer produces for
;          these messages — field 1 (the key) first with a one-byte length,
;          then nothing, or field 2 (the value) with a length of up to three
;          bytes, ending the message — in ~15 instructions instead of the
;          field loop's ~70 (a Get) to ~130 (a Put). It must agree with the
;          loop on every input it accepts: anything else, including anything
;          malformed or truncated, falls through to the loop, which decides.
; =============================================================================
kv_pb_decode_kv:
    ; ---- fast path: {1: key} or {1: key, 2: value}, canonical order ----
    cmp     rsi, 2
    jb      .general
    cmp     byte [rdi], 0x0A            ; field 1, length-delimited
    jne     .general
    movzx   eax, byte [rdi + 1]         ; the key's length, if it is one byte
    test    al, al
    js      .general                    ; 128 bytes or more: a longer varint
    lea     rcx, [rax + 2]              ; bytes the key field occupies
    cmp     rcx, rsi
    ja      .general                    ; the key runs past the end: the loop reports it
    lea     r8, [rdi + 2]
    mov     [rdx + PbKv.key], r8
    mov     [rdx + PbKv.key_len], rax
    mov     dword [rdx + PbKv.has_key], 1
    je      .fast_no_value              ; (flags of the cmp: the message ends with the key)
    ; A value must follow and end the message: tag 0x12, then a length of
    ; one to three bytes (values below 2 MiB; longer ones take the loop).
    lea     r8, [rdi + rcx]             ; the next field
    mov     r9, rsi
    sub     r9, rcx                     ; bytes left, >= 1
    cmp     byte [r8], 0x12             ; field 2, length-delimited
    jne     .general
    cmp     r9, 2
    jb      .general                    ; the tag alone: truncated
    movzx   eax, byte [r8 + 1]
    mov     ecx, 2                      ; bytes of tag + length so far
    test    al, al
    jns     .fast_value
    cmp     r9, 3
    jb      .general
    and     eax, 0x7F
    movzx   r11d, byte [r8 + 2]
    mov     ecx, 3
    test    r11b, r11b
    js      .fast_len3
    shl     r11d, 7
    or      eax, r11d
    jmp     .fast_value
.fast_len3:
    cmp     r9, 4
    jb      .general
    and     r11d, 0x7F
    shl     r11d, 7
    or      eax, r11d
    movzx   r11d, byte [r8 + 3]
    mov     ecx, 4
    test    r11b, r11b
    js      .general                    ; a length of four bytes or more
    shl     r11d, 14
    or      eax, r11d
.fast_value:
    ; rax = value length, rcx = bytes of its tag and length. The value must
    ; be exactly the rest of the message; more fields, or fewer bytes, are
    ; the loop's business.
    lea     r11, [rax + rcx]
    cmp     r11, r9
    jne     .general
    add     r8, rcx
    mov     [rdx + PbKv.value], r8
    mov     [rdx + PbKv.value_len], rax
    mov     dword [rdx + PbKv.has_value], 1
    xor     eax, eax
    ret
.fast_no_value:
    xor     eax, eax
    mov     [rdx + PbKv.value], rax
    mov     [rdx + PbKv.value_len], rax
    mov     [rdx + PbKv.has_value], eax
    ret
.general:
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

; =============================================================================
; kv_pb_decode_download — decode a DownloadRequest.
;
; @param  rdi  const uint8_t *buf (may be NULL when len == 0)
; @param  rsi  size_t len
; @param  rdx  PbDl *out — fully overwritten (absent fields: NULL/0/has=0)
; @returns rax = 0 when the message is well-formed, -EINVAL otherwise (out is
;          then unspecified and must not be used).
; @remarks Fields: 1 key (bytes: a zero-copy pointer into buf), 2 offset,
;          3 length (its presence is recorded: absent means "to the end",
;          which 0 cannot express), 4 chunk_size — varints. Unknown fields
;          and known numbers with an unexpected wire type are skipped, and
;          the last of repeated singular fields wins: kv_pb_decode_kv's
;          rules, so both request shapes of the service follow one parser
;          discipline. chunk_size is kept as the full 64-bit varint (the
;          caller clamps it; a uint32 field wider than 32 bits is a sender
;          bug that clamping absorbs). O(len), no allocation, no reads
;          outside [buf, buf + len). Clobbers rcx, rdx, rsi, r8-r11.
; =============================================================================
kv_pb_decode_download:
    xor     eax, eax
    mov     [rdx + PbDl.key], rax
    mov     [rdx + PbDl.key_len], rax
    mov     [rdx + PbDl.offset], rax
    mov     [rdx + PbDl.length], rax
    mov     [rdx + PbDl.chunk], rax
    mov     [rdx + PbDl.has_key], eax
    mov     [rdx + PbDl.has_length], eax
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
    je      .varint
    cmp     eax, 1
    je      .skip_fixed64
    cmp     eax, 5
    je      .skip_fixed32
    jmp     .bad                        ; 3/4 (groups), 6/7 (undefined)
.varint:
    call    pb_read_varint              ; value (r10/r11 survive)
    jc      .bad
    cmp     r11, 2
    je      .offset
    cmp     r11, 3
    je      .length
    cmp     r11, 4
    jne     .field                      ; unknown, or the key sent as a varint: skipped
    mov     [r10 + PbDl.chunk], rax
    jmp     .field
.offset:
    mov     [r10 + PbDl.offset], rax
    jmp     .field
.length:
    mov     [r10 + PbDl.length], rax
    mov     dword [r10 + PbDl.has_length], 1
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
    jne     .skip                       ; a varint field sent as bytes, or unknown
    mov     [r10 + PbDl.key], rsi
    mov     [r10 + PbDl.key_len], rax
    mov     dword [r10 + PbDl.has_key], 1
.skip:
    add     rsi, rax
    jmp     .field
.ok:
    xor     eax, eax
    ret
.bad:
    mov     rax, -EINVAL
    ret

; -----------------------------------------------------------------------------
; HANDLER — call the stream's handler with the event already in esi/rdx/rcx/
; r8, and stop the feed if it returns a negative code. Only inside
; kv_pb_stream_feed (r14 = ctx, r15 = handler, .stopped = its exit).
; -----------------------------------------------------------------------------
%macro HANDLER 0
    mov     rdi, r14
    call    r15
    test    eax, eax
    js      .stopped
%endmacro

; =============================================================================
; kv_pb_stream_feed — decode the next piece of a gRPC message stream.
;
; @param  rdi  PbStream* (all zero before the first piece)
; @param  rsi  const uint8_t *p   @param rdx  size_t n (0 is allowed)
; @param  rcx  void *ctx (handed to the handler untouched)
; @param  r8   handler(ctx, event, tag, a1, a2) -> int (see pbstream.inc)
; @returns rax = 0 when the whole piece was consumed; PB_ERR_MALFORMED,
;          PB_ERR_COMPRESSED or PB_ERR_FLAG when the stream is invalid; or
;          the handler's negative code when it stopped the decoding. After
;          any error the state is unspecified: the caller abandons the
;          stream.
; @remarks Resumable at every byte: a prefix, varint or fixed value split
;          across pieces is collected in PbStream.hbuf, and a length-
;          delimited payload is reported as PBE_BYTES events pointing into
;          p — one per piece it spans, never copied here. Every field is
;          checked to end inside its message (PB_ERR_MALFORMED otherwise)
;          BEFORE its payload is reported, so a handler never receives bytes
;          of the next message as field data, and never a field of a message
;          whose length the stream then contradicts. A message whose last
;          byte leaves a tag, varint or length unfinished is malformed at
;          that very byte — not later, when the stream ends (the caller
;          would report a truncated stream instead) or never (a client
;          waiting for the answer to what it sent). Events come in stream
;          order; a message with no fields is PBE_MSG_BEGIN(0) + PBE_MSG_END.
;          The handler runs inside the feed: it may keep state in ctx but
;          must not feed this PbStream recursively. O(n) plus handler time,
;          no allocation, no reads outside [p, p + n).
;          Wire-type rules match kv_pb_decode_kv: field 0, tags >= 2^32,
;          groups (3, 4) and undefined types (6, 7) are malformed; varints
;          longer than 10 bytes or wider than 64 bits are malformed.
; =============================================================================
kv_pb_stream_feed:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: aligned for handler calls
    mov     rbx, rdi                    ; PbStream*
    mov     r12, rsi                    ; cursor
    lea     r13, [rsi + rdx]            ; end of the piece
    mov     r14, rcx                    ; ctx
    mov     r15, r8                     ; handler
.loop:
    cmp     r12, r13
    jae     .ok
    mov     rax, [rbx + PbStream.state]
    cmp     eax, PSS_BYTES
    je      .bytes                      ; bulk payload first: an upload's hot state
    cmp     eax, PSS_PREFIX
    je      .prefix
    cmp     eax, PSS_FIXED
    je      .fixed
    ; ---- PSS_TAG / PSS_VARINT / PSS_LEN: one more varint byte ----
    cmp     qword [rbx + PbStream.mleft], 0
    je      .malformed                  ; the message ended inside this field
    movzx   ecx, byte [r12]
    inc     r12
    dec     qword [rbx + PbStream.mleft]
    mov     rdx, [rbx + PbStream.hlen]
    mov     [rbx + PbStream.hbuf + rdx], cl
    inc     rdx
    mov     [rbx + PbStream.hlen], rdx
    test    cl, 0x80
    jz      .varint_done
    cmp     rdx, 10
    jae     .malformed                  ; an 11th byte would be needed
    ; The message's last byte still wants a continuation: malformed NOW —
    ; waiting for a next byte would misreport it as a stream cut inside a
    ; message at END_STREAM, or hang a client that sent it and waits.
    cmp     qword [rbx + PbStream.mleft], 0
    je      .malformed
    jmp     .loop
.varint_done:
    call    pbs_varint                  ; rax = value; CF = wider than 64 bits
    jc      .malformed
    mov     qword [rbx + PbStream.hlen], 0
    mov     rcx, [rbx + PbStream.state]
    cmp     ecx, PSS_TAG
    je      .tag
    cmp     ecx, PSS_VARINT
    je      .varint_value
    ; ---- PSS_LEN: rax = payload length ----
    cmp     rax, [rbx + PbStream.mleft]
    ja      .malformed                  ; the payload would run past its message
    mov     [rbx + PbStream.fleft], rax
    mov     esi, PBE_BYTES_BEGIN
    mov     rdx, [rbx + PbStream.tag]
    mov     rcx, rax
    HANDLER
    cmp     qword [rbx + PbStream.fleft], 0
    je      .bytes_end                  ; an empty payload: begin + end
    mov     qword [rbx + PbStream.state], PSS_BYTES
    jmp     .loop

.tag:
    mov     rcx, rax
    shr     rcx, 3
    jz      .malformed                  ; field number 0
    mov     rcx, 0xFFFFFFFF
    cmp     rax, rcx
    ja      .malformed                  ; field numbers are < 2^29
    mov     [rbx + PbStream.tag], rax
    and     eax, 7                      ; wire type
    jz      .to_varint
    cmp     eax, 2
    je      .to_len
    mov     ecx, 8
    cmp     eax, 1
    je      .to_fixed                   ; fixed64
    mov     ecx, 4
    cmp     eax, 5
    je      .to_fixed                   ; fixed32
    jmp     .malformed                  ; 3/4 (groups), 6/7 (undefined)
.to_varint:
    mov     qword [rbx + PbStream.state], PSS_VARINT
    jmp     .value_room
.to_len:
    mov     qword [rbx + PbStream.state], PSS_LEN
.value_room:
    ; A tag that ends its message has no room for its value (a varint or a
    ; length takes at least one byte): malformed at once, like .to_fixed.
    cmp     qword [rbx + PbStream.mleft], 0
    je      .malformed
    jmp     .loop
.to_fixed:
    cmp     rcx, [rbx + PbStream.mleft]
    ja      .malformed                  ; the value would run past its message
    mov     [rbx + PbStream.fleft], rcx
    mov     qword [rbx + PbStream.state], PSS_FIXED
    jmp     .loop

.varint_value:
    mov     esi, PBE_VARINT
    mov     rdx, [rbx + PbStream.tag]
    mov     rcx, rax
    HANDLER
    jmp     .field_done

.bytes:
    ; The payload passes through as a pointer into the piece: take what this
    ; piece holds of it, up to the payload's end.
    mov     r8, r13
    sub     r8, r12                     ; available
    mov     rax, [rbx + PbStream.fleft]
    cmp     r8, rax
    cmova   r8, rax                     ; take = min(available, payload left)
    sub     [rbx + PbStream.fleft], r8
    sub     [rbx + PbStream.mleft], r8
    mov     rcx, r12
    add     r12, r8
    mov     esi, PBE_BYTES
    mov     rdx, [rbx + PbStream.tag]
    HANDLER                             ; rcx = pointer, r8 = length
    cmp     qword [rbx + PbStream.fleft], 0
    jne     .loop                       ; the piece ended inside the payload
.bytes_end:
    mov     esi, PBE_BYTES_END
    mov     rdx, [rbx + PbStream.tag]
    HANDLER
    jmp     .field_done

.fixed:
    mov     rcx, r13
    sub     rcx, r12                    ; available (>= 1)
    mov     rax, [rbx + PbStream.fleft]
    cmp     rcx, rax
    cmova   rcx, rax                    ; take (>= 1)
    sub     [rbx + PbStream.fleft], rcx
    sub     [rbx + PbStream.mleft], rcx
    mov     rdx, [rbx + PbStream.hlen]
.fixed_copy:
    movzx   eax, byte [r12]
    mov     [rbx + PbStream.hbuf + rdx], al
    inc     r12
    inc     rdx
    dec     rcx
    jnz     .fixed_copy
    mov     [rbx + PbStream.hlen], rdx
    cmp     qword [rbx + PbStream.fleft], 0
    jne     .loop                       ; more bytes of the value in the next piece
    mov     qword [rbx + PbStream.hlen], 0
    mov     rdx, [rbx + PbStream.tag]
    mov     esi, PBE_FIXED64
    mov     rcx, [rbx + PbStream.hbuf]  ; little-endian, as protobuf stores fixed values
    mov     eax, edx
    and     eax, 7
    cmp     eax, 5
    jne     .fixed_emit
    mov     esi, PBE_FIXED32
    mov     ecx, [rbx + PbStream.hbuf]  ; 4 bytes, zero-extended
.fixed_emit:
    HANDLER
    jmp     .field_done

.prefix:
    mov     rcx, r13
    sub     rcx, r12                    ; available (>= 1)
    mov     eax, 5
    sub     rax, [rbx + PbStream.hlen]  ; still needed (>= 1)
    cmp     rcx, rax
    cmova   rcx, rax
    mov     rdx, [rbx + PbStream.hlen]
.prefix_copy:
    movzx   eax, byte [r12]
    mov     [rbx + PbStream.hbuf + rdx], al
    inc     r12
    inc     rdx
    dec     rcx
    jnz     .prefix_copy
    mov     [rbx + PbStream.hlen], rdx
    cmp     rdx, 5
    jb      .loop                       ; the piece ended inside the prefix
    mov     qword [rbx + PbStream.hlen], 0
    movzx   eax, byte [rbx + PbStream.hbuf]
    cmp     eax, 1
    je      .compressed
    ja      .bad_flag                   ; flag values other than 0/1 are undefined
    mov     eax, [rbx + PbStream.hbuf + 1]
    bswap   eax                         ; big-endian message length
    mov     [rbx + PbStream.mleft], rax
    inc     qword [rbx + PbStream.msgs]
    mov     esi, PBE_MSG_BEGIN
    xor     edx, edx
    mov     rcx, rax
    HANDLER
    cmp     qword [rbx + PbStream.mleft], 0
    je      .msg_end                    ; an empty message: no fields
    mov     qword [rbx + PbStream.state], PSS_TAG
    jmp     .loop

.field_done:
    cmp     qword [rbx + PbStream.mleft], 0
    je      .msg_end
    mov     qword [rbx + PbStream.state], PSS_TAG
    jmp     .loop
.msg_end:
    mov     qword [rbx + PbStream.state], PSS_PREFIX
    mov     esi, PBE_MSG_END
    xor     edx, edx
    xor     ecx, ecx
    HANDLER
    jmp     .loop

.ok:
    xor     eax, eax
    jmp     .out
.malformed:
    mov     rax, PB_ERR_MALFORMED
    jmp     .out
.compressed:
    mov     rax, PB_ERR_COMPRESSED
    jmp     .out
.bad_flag:
    mov     rax, PB_ERR_FLAG
    jmp     .out
.stopped:
    movsxd  rax, eax                    ; the handler's code, sign-extended
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
; pbs_varint — value of the varint collected in PbStream.hbuf. Internal, leaf.
; @param  rbx  PbStream* (hlen = 1..10 bytes, the last one without the
;              continuation bit)
; @returns rax = value with CF=0; CF=1 when a 10th byte carries more than
;          bit 63 (the value would not fit 64 bits).
; @remarks Same overflow rule as pb_read_varint. Clobbers rcx, rdx, r8.
; =============================================================================
pbs_varint:
    xor     eax, eax
    xor     ecx, ecx                    ; shift
    xor     edx, edx                    ; index
.byte:
    movzx   r8d, byte [rbx + PbStream.hbuf + rdx]
    and     r8d, 0x7F
    cmp     ecx, 63
    jne     .fits
    cmp     r8d, 1                      ; 10th byte may only carry bit 63
    ja      .err
.fits:
    shl     r8, cl
    or      rax, r8
    add     ecx, 7
    inc     rdx
    cmp     rdx, [rbx + PbStream.hlen]
    jb      .byte
    clc
    ret
.err:
    stc
    ret

; =============================================================================
; kv_pb_stream_idle — is the stream between messages?
; @param  rdi  PbStream*
; @returns eax = 1 when no message is in progress (state PSS_PREFIX with no
;          prefix byte collected), else 0.
; @remarks The end-of-stream test of a client-streaming call: the peer's
;          END_STREAM is clean only here; anywhere else the stream ended
;          inside a message, which the caller reports as a framing error.
; =============================================================================
kv_pb_stream_idle:
    xor     eax, eax
    cmp     qword [rdi + PbStream.state], PSS_PREFIX
    jne     .out
    cmp     qword [rdi + PbStream.hlen], 0
    sete    al
.out:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
