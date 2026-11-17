; =============================================================================
; hpack.asm — HPACK header decoding (RFC 7541) for the native HTTP/2 engine.
;
; What it does: turns one complete header block into a stream of
; (name, value, meaning) calls to a handler, and maintains the connection's
; dynamic table exactly as the peer's encoder expects:
;   * integers with N-bit prefixes (§5.1), bounded to ~2^35 so no overflow;
;   * string literals, raw (zero copy: pointers into the block) or Huffman
;     coded (decoded into a caller-provided scratch buffer, 4 bits per step
;     through the generated table in huffman.inc, §5.2 padding rules);
;   * indexed fields, literals with/without/never indexing, and dynamic
;     table size updates, which are legal only before the first field of a
;     block (§4.2, §6.3);
;   * insertion with eviction (§4.4): the new entry's strings are copied
;     BEFORE old entries are evicted, because a literal may name the very
;     entry that insertion evicts.
; Any malformed input returns HPACK_ERR_COMPRESSION: the engine turns it into
; a connection error (COMPRESSION_ERROR), because the decoder's table would
; otherwise diverge from the peer's encoder.
;
; Meanings (hpack.inc) are computed once per distinct field, so the hot path —
; indexed fields of a warm table — never compares strings.
;
; Memory: entry strings live in blocks from the connection's worker pool
; (pool.asm); the ring of entry descriptors too. Bounded by the table limit.
; Threading: one HpackDec per connection, used only by its worker thread.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"
%include "hpack.inc"

extern kv_pool_malloc
extern kv_pool_free
extern memcpy
extern kv_keyeq

section .rodata
%include "huffman.inc"

; ---- static table (RFC 7541 Appendix A) as offsets into one blob --------------
; Offsets instead of pointers: position independent, no load-time relocations.
%macro HPSTR 3                          ; index, name, value
hpn_%1:     db %2
hpn_%1_len  equ $ - hpn_%1
hpv_%1:     db %3
hpv_%1_len  equ $ - hpv_%1
%endmacro
%macro HPENT 1                          ; one 16-byte entry
    dd hpn_%1 - hp_sblob, hpv_%1 - hp_sblob
    db hpn_%1_len, hpv_%1_len, 0, 0
    dd 0
%endmacro

hp_sblob:
HPSTR 1,  ":authority", ""
HPSTR 2,  ":method", "GET"
HPSTR 3,  ":method", "POST"
HPSTR 4,  ":path", "/"
HPSTR 5,  ":path", "/index.html"
HPSTR 6,  ":scheme", "http"
HPSTR 7,  ":scheme", "https"
HPSTR 8,  ":status", "200"
HPSTR 9,  ":status", "204"
HPSTR 10, ":status", "206"
HPSTR 11, ":status", "304"
HPSTR 12, ":status", "400"
HPSTR 13, ":status", "404"
HPSTR 14, ":status", "500"
HPSTR 15, "accept-charset", ""
HPSTR 16, "accept-encoding", "gzip, deflate"
HPSTR 17, "accept-language", ""
HPSTR 18, "accept-ranges", ""
HPSTR 19, "accept", ""
HPSTR 20, "access-control-allow-origin", ""
HPSTR 21, "age", ""
HPSTR 22, "allow", ""
HPSTR 23, "authorization", ""
HPSTR 24, "cache-control", ""
HPSTR 25, "content-disposition", ""
HPSTR 26, "content-encoding", ""
HPSTR 27, "content-language", ""
HPSTR 28, "content-length", ""
HPSTR 29, "content-location", ""
HPSTR 30, "content-range", ""
HPSTR 31, "content-type", ""
HPSTR 32, "cookie", ""
HPSTR 33, "date", ""
HPSTR 34, "etag", ""
HPSTR 35, "expect", ""
HPSTR 36, "expires", ""
HPSTR 37, "from", ""
HPSTR 38, "host", ""
HPSTR 39, "if-match", ""
HPSTR 40, "if-modified-since", ""
HPSTR 41, "if-none-match", ""
HPSTR 42, "if-range", ""
HPSTR 43, "if-unmodified-since", ""
HPSTR 44, "last-modified", ""
HPSTR 45, "link", ""
HPSTR 46, "location", ""
HPSTR 47, "max-forwards", ""
HPSTR 48, "proxy-authenticate", ""
HPSTR 49, "proxy-authorization", ""
HPSTR 50, "range", ""
HPSTR 51, "referer", ""
HPSTR 52, "refresh", ""
HPSTR 53, "retry-after", ""
HPSTR 54, "server", ""
HPSTR 55, "set-cookie", ""
HPSTR 56, "strict-transport-security", ""
HPSTR 57, "transfer-encoding", ""
HPSTR 58, "user-agent", ""
HPSTR 59, "vary", ""
HPSTR 60, "via", ""
HPSTR 61, "www-authenticate", ""

align 16
hp_static:
%assign i 1
%rep HPACK_STATIC_COUNT
    HPENT %[i]                          ; %[...] pastes the number, not the letter i
%assign i i + 1
%endrep

; Layout facts for test_hpack.c's C mirror of HpackDec (a drift fails the
; test instead of corrupting a decoder): size, offset of .size, sentinel.
align 8
global kv_hpack_abi
kv_hpack_abi:   dq HpackDec_size, HpackDec.size, 0xABCDEF0123456789

; ---- names and values that classification recognizes ---------------------------
%macro CSTR 2+
%1: db %2
%{1}_len equ $ - %1
%endmacro
CSTR k_authority,   ":authority"
CSTR k_method,      ":method"
CSTR k_path,        ":path"
CSTR k_scheme,      ":scheme"
CSTR k_status,      ":status"
CSTR k_te,          "te"
CSTR k_upgrade,     "upgrade"
CSTR k_connection,  "connection"
CSTR k_keepalive,   "keep-alive"
CSTR k_ctype,       "content-type"
CSTR k_encoding,    "grpc-encoding"
CSTR k_clength,     "content-length"
CSTR k_proxyconn,   "proxy-connection"
CSTR k_transfer,    "transfer-encoding"
CSTR v_post,        "POST"
CSTR v_head,        "HEAD"
CSTR v_grpc,        "application/grpc"
CSTR v_trailers,    "trailers"
CSTR v_identity,    "identity"
CSTR v_health,      "/grpc.health.v1.Health/Check"
CSTR v_prefix,      "/kvasm.v1.KvAsm/"
CSTR v_get,         "Get"
CSTR v_put,         "Put"
CSTR v_delete,      "Delete"
CSTR v_upload,      "Upload"            ; client streaming (grpc.asm consumes it as it arrives)
CSTR v_download,    "Download"          ; server streaming (a generated body)

; hp_token[c] = 1 for characters legal in an HTTP/2 field name: RFC 9110
; tchar, lowercase only (RFC 9113 §8.2.1: uppercase names are malformed).
align 64
hp_token:
%assign c 0
%rep 256
  %if c == 0x21 || (c >= 0x23 && c <= 0x27) || c == 0x2A || c == 0x2B || c == 0x2D || c == 0x2E || (c >= 0x30 && c <= 0x39) || (c >= 0x5E && c <= 0x7A) || c == 0x7C || c == 0x7E
    db 1
  %else
    db 0
  %endif
%assign c c + 1
%endrep

section .bss align=64
; Meanings of the static entries, computed by kv_hpack_global_init at startup
; (before any worker runs), read-only afterwards.
hp_static_meaning:  resd HPACK_STATIC_COUNT + 1

section .text

global kv_hpack_global_init
global kv_hpack_init
global kv_hpack_free
global kv_hpack_decode
global kv_huff_decode
global kv_hpack_classify

; =============================================================================
; kv_hpack_global_init — classify the 61 static entries.
; @returns nothing.
; @remarks Call once before any decoder runs (main, tests). Idempotent.
; =============================================================================
kv_hpack_global_init:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     ebx, 1
.next:
    lea     r12, [hp_static]
    mov     rax, rbx
    dec     rax
    shl     rax, 4
    add     r12, rax                    ; entry
    lea     r13, [hp_sblob]
    mov     eax, [r12]
    lea     rsi, [r13 + rax]            ; name
    movzx   edx, byte [r12 + 8]
    call    classify_name
    mov     edi, eax
    mov     eax, [r12 + 4]
    lea     rsi, [r13 + rax]            ; value
    movzx   edx, byte [r12 + 9]
    call    classify_value
    lea     rcx, [hp_static_meaning]
    mov     [rcx + rbx*4], eax
    inc     ebx
    cmp     ebx, HPACK_STATIC_COUNT
    jbe     .next
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_hpack_init — prepare a connection's decoder.
;
; @param  rdi  HpackDec*   @param rsi  Pool* (entry storage)
; @param  rdx  limit: our advertised SETTINGS_HEADER_TABLE_SIZE (>= 32)
; @returns rax = 0, or -ENOMEM (nothing to free).
; @remarks The ring holds limit/32 descriptors rounded up to a power of two:
;          every entry accounts at least 32 bytes, so the table can never
;          hold more entries than that.
; =============================================================================
kv_hpack_init:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     rax, r13
    shr     rax, 5                      ; limit / 32
    mov     ecx, 1
.pow2:
    cmp     rcx, rax
    jae     .have_ring
    shl     rcx, 1
    jmp     .pow2
.have_ring:
    lea     rax, [rcx - 1]
    mov     [rbx + HpackDec.ring_mask], rax
    imul    rdi, rcx, HpEnt_size
    mov     rsi, r12
    call    kv_pool_malloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbx + HpackDec.ents], rax
    xor     eax, eax
    mov     [rbx + HpackDec.head], rax
    mov     [rbx + HpackDec.count], rax
    mov     [rbx + HpackDec.size], rax
    mov     [rbx + HpackDec.max], r13
    mov     [rbx + HpackDec.limit], r13
    mov     [rbx + HpackDec.pool], r12
    mov     qword [rbx + HpackDec.gen], 0
    jmp     .out
.nomem:
    mov     rax, -ENOMEM
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_hpack_free — release every entry and the ring.
; @param  rdi  HpackDec* (its ents may be 0 if init failed)
; @returns nothing.
; =============================================================================
kv_hpack_free:
    push    rbx
    mov     rbx, rdi
    cmp     qword [rbx + HpackDec.ents], 0
    je      .out
    xor     esi, esi
    call    hp_evict_to                 ; target size 0: frees every entry
    mov     rdi, [rbx + HpackDec.ents]
    mov     rsi, [rbx + HpackDec.pool]
    call    kv_pool_free wrt ..plt
    mov     qword [rbx + HpackDec.ents], 0
.out:
    pop     rbx
    ret

; =============================================================================
; kv_hpack_decode — decode one complete header block.
;
; @param  rdi  HpackDec*
; @param  rsi  block        @param rdx  block length
; @param  rcx  ctx          passed to the handler as its first argument
; @param  r8   handler      int handler(ctx, name, nlen, value, vlen, meaning)
;                           returning 0 to continue; anything else stops
;                           decoding and is returned
; @param  r9   const uint64_t scratch[2] = {buffer, capacity} for Huffman
;              strings; strings that do not fit are flagged HF_OVERSIZE
;              (their bytes are not kept). The capacity must be >= the table
;              limit, so every insertable entry fits.
; @returns rax = 0, HPACK_ERR_COMPRESSION (malformed block), HPACK_ERR_NOMEM
;          (the pool could not store an entry, or the scratch buffer is
;          smaller than the table limit and an oversize entry would have been
;          stored), or the handler's non-zero result.
; @remarks The (name, value) pointers handed to the handler stay valid only
;          during that call: they point into the block, the scratch buffer
;          (reused per field) or table entries (evictable by the next field).
;          When the meaning has HF_OVERSIZE, the lengths are the true ones
;          (for header-list accounting) but the bytes must NOT be read: they
;          were not kept, and a length can exceed the scratch buffer.
;          Stream-level problems (a malformed request) must be recorded in
;          ctx with the handler returning 0: every block has to be decoded
;          to its end, or this table diverges from the peer's encoder.
; =============================================================================
%define F_SCR       0                   ; scratch buffer
%define F_CAP       8                   ; scratch capacity
%define F_USED      16                  ; scratch bytes used by this field
%define F_EMITTED   24                  ; 1 once a field was decoded (size updates end)
%define F_INSERT    32                  ; 1: literal with incremental indexing
%define F_NPTR      40
%define F_NLEN      48
%define F_VPTR      56
%define F_VLEN      64
%define F_MEAN      72                  ; name's code, then the full meaning
%define F_FLAGS     80                  ; HF_OVERSIZE accumulated for this field
kv_hpack_decode:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 88                     ; 6 pushes + 88: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12, rsi                    ; cursor
    lea     r13, [rsi + rdx]            ; end
    mov     r14, rcx
    mov     r15, r8
    mov     rax, [r9]
    mov     [rsp + F_SCR], rax
    mov     rax, [r9 + 8]
    mov     [rsp + F_CAP], rax
    mov     qword [rsp + F_EMITTED], 0
.field:
    cmp     r12, r13
    jae     .done
    xor     eax, eax
    mov     [rsp + F_USED], rax
    mov     [rsp + F_FLAGS], rax
    mov     [rsp + F_INSERT], rax       ; only .lit_inc sets it
    movzx   eax, byte [r12]
    test    al, 0x80
    jnz     .indexed
    test    al, 0x40
    jnz     .lit_inc
    test    al, 0x20
    jnz     .size_update
    mov     edi, 4                      ; 0000xxxx without / 0001xxxx never indexed
    call    hp_int
    jc      .corrupt
    jmp     .literal
.lit_inc:
    mov     edi, 6
    call    hp_int
    jc      .corrupt
    mov     qword [rsp + F_INSERT], 1

.literal:                               ; rax = name index, 0 = literal name
    test    rax, rax
    jz      .new_name
    call    hp_get
    jc      .corrupt
    mov     [rsp + F_NPTR], rax
    mov     [rsp + F_NLEN], rdx
    and     r9d, 0xFF                   ; keep the name's code, drop its value class
    mov     [rsp + F_MEAN], r9
    jmp     .value
.new_name:
    mov     rdi, rsp                    ; hp_str addresses this frame through rdi
    call    hp_str
    jc      .corrupt
    mov     [rsp + F_NPTR], rax
    mov     [rsp + F_NLEN], rdx
    mov     qword [rsp + F_MEAN], HF_OTHER
    ; An oversize name's bytes were not kept (and its length runs past the
    ; scratch buffer): classifying it would read out of bounds. The field is
    ; flagged HF_OVERSIZE, which the engine refuses anyway.
    test    qword [rsp + F_FLAGS], HF_OVERSIZE
    jnz     .value
    mov     rsi, rax
    call    classify_name
    mov     [rsp + F_MEAN], rax
.value:
    mov     rdi, rsp                    ; hp_str addresses this frame through rdi
    call    hp_str
    jc      .corrupt
    mov     [rsp + F_VPTR], rax
    mov     [rsp + F_VLEN], rdx
    mov     rax, [rsp + F_MEAN]         ; oversize: the name's code only, no class
    test    qword [rsp + F_FLAGS], HF_OVERSIZE
    jnz     .flagged
    mov     edi, eax
    mov     rsi, [rsp + F_VPTR]
    mov     rdx, [rsp + F_VLEN]
    call    classify_value
.flagged:
    or      rax, [rsp + F_FLAGS]
    mov     [rsp + F_MEAN], rax
    jmp     .emit

.indexed:
    mov     edi, 7
    call    hp_int
    jc      .corrupt
    call    hp_get
    jc      .corrupt
    mov     [rsp + F_NPTR], rax
    mov     [rsp + F_NLEN], rdx
    mov     [rsp + F_VPTR], rcx
    mov     [rsp + F_VLEN], r8
    mov     [rsp + F_MEAN], r9

    ; The handler runs BEFORE the insertion: inserting may evict the very
    ; entry an indexed name points into (§4.4), which would leave the
    ; handler a dangling pointer. The handler does not touch the decoder, so
    ; the strings are still intact for hp_insert to copy afterwards. A
    ; non-zero handler result aborts the block; the table no longer matters
    ; then, because the engine tears the connection down.
.emit:
    mov     qword [rsp + F_EMITTED], 1
    mov     rdi, r14
    mov     rsi, [rsp + F_NPTR]
    mov     rdx, [rsp + F_NLEN]
    mov     rcx, [rsp + F_VPTR]
    mov     r8, [rsp + F_VLEN]
    mov     r9, [rsp + F_MEAN]
    call    r15
    test    eax, eax
    jnz     .stopped
    cmp     qword [rsp + F_INSERT], 0
    je      .field
    ; Oversize strings were not kept. With a scratch buffer at least as large
    ; as the table limit (the documented contract) such an entry can never
    ; fit the table, and hp_insert just empties it. If it WOULD fit, the
    ; caller broke the contract: refuse rather than store bytes never kept.
    test    qword [rsp + F_FLAGS], HF_OVERSIZE
    jz      .insert
    mov     rax, [rsp + F_NLEN]
    add     rax, [rsp + F_VLEN]
    add     rax, HPACK_ENTRY_OVERHEAD
    cmp     rax, [rbx + HpackDec.max]
    jbe     .nomem
.insert:
    mov     rsi, [rsp + F_NPTR]
    mov     rdx, [rsp + F_NLEN]
    mov     rcx, [rsp + F_VPTR]
    mov     r8, [rsp + F_VLEN]
    mov     r9, [rsp + F_MEAN]
    call    hp_insert
    test    rax, rax
    js      .out                        ; HPACK_ERR_NOMEM
    jmp     .field
.stopped:
    movsxd  rax, eax
    jmp     .out
.nomem:
    mov     rax, HPACK_ERR_NOMEM
    jmp     .out

.size_update:
    ; §4.2: only at the start of a block; never above our advertised limit.
    cmp     qword [rsp + F_EMITTED], 0
    jne     .corrupt
    mov     edi, 5
    call    hp_int
    jc      .corrupt
    cmp     rax, [rbx + HpackDec.limit]
    ja      .corrupt
    inc     qword [rbx + HpackDec.gen]  ; the table may shrink: cached results expire
    mov     [rbx + HpackDec.max], rax
    mov     rsi, rax
    call    hp_evict_to
    jmp     .field

.done:
    xor     eax, eax
    jmp     .out
.corrupt:
    mov     rax, HPACK_ERR_COMPRESSION
.out:
    add     rsp, 88
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; hp_int — decode an integer with an N-bit prefix (RFC 7541 §5.1). Internal.
; @param  edi  N (4..7)   @param r12 cursor (advanced)   @param r13 end
; @returns rax = value with CF=0; CF=1 if truncated or wider than ~2^35.
;          Clobbers rcx, rdx, r8.
; =============================================================================
hp_int:
    cmp     r12, r13
    jae     .err
    movzx   eax, byte [r12]
    inc     r12
    mov     ecx, edi
    mov     edx, 1
    shl     edx, cl
    dec     edx                         ; 2^N - 1
    and     eax, edx
    cmp     eax, edx
    jb      .ok                         ; fits in the prefix
    xor     ecx, ecx                    ; shift
.more:
    cmp     r12, r13
    jae     .err
    movzx   edx, byte [r12]
    inc     r12
    mov     r8d, edx
    and     r8d, 0x7F
    shl     r8, cl
    add     rax, r8
    add     ecx, 7
    test    dl, 0x80
    jz      .ok
    cmp     ecx, HPACK_INT_MAX_SHIFT
    jbe     .more
.err:
    stc
    ret
.ok:
    clc
    ret

; =============================================================================
; hp_str — decode a string literal (RFC 7541 §5.2). Internal.
; @param  rdi  the caller's decode frame (kv_hpack_decode's rsp)
; @param  r12  cursor (advanced)   r13 end
; @returns CF=0: rax = pointer, rdx = length. Raw strings point into the
;          block; Huffman strings are decoded into the frame's scratch buffer
;          at F_USED (advanced). If one does not fit, HF_OVERSIZE is added to
;          F_FLAGS and rdx is its true length (content not kept). CF=1 if the
;          literal is truncated or its Huffman code is invalid.
;          Clobbers rcx, rsi, r8-r11.
; =============================================================================
hp_str:
    push    rbx
    push    rbp                         ; 2 pushes + return: kv_huff_decode needs no alignment
    mov     rbp, rdi                    ; frame
    cmp     r12, r13
    jae     .err
    movzx   ebx, byte [r12]             ; H bit
    push    rdi
    mov     edi, 7
    call    hp_int
    pop     rdi
    jc      .err
    mov     rcx, r13
    sub     rcx, r12
    cmp     rax, rcx
    ja      .err                        ; longer than what is left of the block
    test    bl, 0x80
    jnz     .huffman
    mov     rdx, rax
    mov     rax, r12                    ; zero copy
    add     r12, rdx
    clc
    jmp     .out
.huffman:
    mov     rsi, rax                    ; coded length
    mov     rdi, r12
    add     r12, rax
    mov     rdx, [rbp + F_SCR]
    add     rdx, [rbp + F_USED]         ; destination
    mov     rcx, [rbp + F_CAP]
    sub     rcx, [rbp + F_USED]         ; room left
    push    rdx
    push    rcx
    call    kv_huff_decode
    pop     rcx
    pop     rdx
    test    rax, rax
    js      .err
    cmp     rax, rcx
    jbe     .fits
    or      qword [rbp + F_FLAGS], HF_OVERSIZE
    mov     rcx, rax                    ; true length for table accounting
    mov     rax, rdx
    mov     rdx, rcx
    clc
    jmp     .out
.fits:
    add     [rbp + F_USED], rax
    xchg    rax, rdx                    ; rax = pointer, rdx = length
    clc
    jmp     .out
.err:
    stc
.out:
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_huff_decode — decode a Huffman-coded string (RFC 7541 §5.2, Appendix B).
;
; @param  rdi  src   @param rsi  src length
; @param  rdx  dst   @param rcx  dst capacity
; @returns rax = decoded length (may exceed the capacity: bytes past it are
;          not written, so the caller can detect "too long" and still account
;          the length), or -1 if the code is invalid: EOS inside the string,
;          padding longer than 7 bits, or padding that is not all ones.
; @remarks Four input bits per step through huff_dec (16 KiB). Clobbers r8-r11.
; =============================================================================
%macro HUFF_STEP 1                      ; %1 = register holding a nibble (0..15)
    mov     rbx, r8
    shl     rbx, 4
    add     rbx, %1
    mov     ebx, [r10 + rbx*4]
    test    ebx, HUFF_FAIL << 16
    jnz     .bad
    test    ebx, HUFF_SYM << 16
    jz      %%nosym
    cmp     rax, rcx
    jae     %%count                     ; past the capacity: count only
    mov     r9d, ebx
    shr     r9d, 24
    mov     [rdx + rax], r9b
%%count:
    inc     rax
%%nosym:
    movzx   r8d, bx                     ; next state
%endmacro
kv_huff_decode:
    push    rbx
    xor     eax, eax
    test    rsi, rsi
    jz      .ok                         ; the empty string is valid
    xor     r8d, r8d                    ; state: root
    lea     r10, [huff_dec]
    lea     r11, [rdi + rsi]
.byte:
    movzx   esi, byte [rdi]
    inc     rdi
    mov     r9d, esi
    shr     r9d, 4
    HUFF_STEP r9
    and     esi, 15
    HUFF_STEP rsi
    cmp     rdi, r11
    jb      .byte
    test    ebx, HUFF_ACCEPTED << 16    ; the last step's flags decide the padding
    jz      .bad
.ok:
    pop     rbx
    ret
.bad:
    mov     rax, -1
    pop     rbx
    ret

; =============================================================================
; hp_get — resolve a table index. Internal.
; @param  rax  index (1..61 static, 62.. dynamic)   @param rbx HpackDec*
; @returns CF=0: rax = name, rdx = name length, rcx = value, r8 = value
;          length, r9 = meaning; CF=1 for index 0 or beyond the table.
;          Clobbers r10, r11.
; =============================================================================
hp_get:
    test    rax, rax
    jz      .bad
    cmp     rax, HPACK_STATIC_COUNT
    ja      .dynamic
    lea     r11, [hp_static_meaning]
    mov     r9d, [r11 + rax*4]
    lea     r10, [rax - 1]
    shl     r10, 4
    lea     r11, [hp_static]
    add     r10, r11                    ; entry
    lea     r11, [hp_sblob]
    mov     eax, [r10]
    mov     ecx, [r10 + 4]
    movzx   edx, byte [r10 + 8]
    movzx   r8d, byte [r10 + 9]
    add     rax, r11
    add     rcx, r11
    clc
    ret
.dynamic:
    sub     rax, HPACK_STATIC_COUNT     ; 1 = newest
    cmp     rax, [rbx + HpackDec.count]
    ja      .bad
    mov     r10, [rbx + HpackDec.head]
    sub     r10, rax
    inc     r10
    and     r10, [rbx + HpackDec.ring_mask]
    imul    r10, r10, HpEnt_size
    add     r10, [rbx + HpackDec.ents]
    mov     rax, [r10 + HpEnt.ptr]
    mov     edx, [r10 + HpEnt.nlen]
    mov     r8d, [r10 + HpEnt.vlen]
    mov     r9d, [r10 + HpEnt.meaning]
    lea     rcx, [rax + rdx]            ; value follows the name
    clc
    ret
.bad:
    stc
    ret

; =============================================================================
; hp_insert — add an entry as the newest (RFC 7541 §4.4). Internal.
; @param  rbx  HpackDec*
; @param  rsi  name  @param rdx nlen  @param rcx value  @param r8 vlen
; @param  r9   meaning
; @returns rax = 0, or HPACK_ERR_NOMEM.
; @remarks An entry larger than the maximum empties the table and is not
;          stored (not an error). Otherwise the strings are copied first and
;          old entries evicted afterwards: the name may belong to an entry
;          this very insertion evicts. Preserves rbx, rbp, r12-r15.
; =============================================================================
hp_insert:
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 16                     ; [0] = meaning; 5 pushes + 16: aligned
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     r15, r8
    mov     [rsp], r9
    inc     qword [rbx + HpackDec.gen]  ; every insertion changes the table (even
                                        ; an oversized one, which empties it)
    lea     rbp, [r13 + r15 + HPACK_ENTRY_OVERHEAD]   ; accounted size
    cmp     rbp, [rbx + HpackDec.max]
    jbe     .fits
    xor     esi, esi
    call    hp_evict_to                 ; too big: the table ends up empty
    xor     eax, eax
    jmp     .out
.fits:
    lea     rdi, [r13 + r15]
    test    rdi, rdi
    jnz     .alloc
    mov     edi, 1                      ; empty name and value: still one block
.alloc:
    mov     rsi, [rbx + HpackDec.pool]
    call    kv_pool_malloc wrt ..plt
    test    rax, rax
    jz      .nomem
    push    rax
    push    rax
    mov     rdi, rax
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    mov     rdi, [rsp]
    add     rdi, r13
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt
    ; Evict until the new entry fits (names were copied above).
    mov     rsi, [rbx + HpackDec.max]
    sub     rsi, rbp
    call    hp_evict_to
    pop     rax
    pop     rax
    mov     rcx, [rbx + HpackDec.head]
    inc     rcx
    and     rcx, [rbx + HpackDec.ring_mask]
    mov     [rbx + HpackDec.head], rcx
    imul    rcx, rcx, HpEnt_size
    add     rcx, [rbx + HpackDec.ents]
    mov     [rcx + HpEnt.ptr], rax
    mov     [rcx + HpEnt.nlen], r13d
    mov     [rcx + HpEnt.vlen], r15d
    mov     rax, [rsp]
    mov     [rcx + HpEnt.meaning], eax
    inc     qword [rbx + HpackDec.count]
    add     [rbx + HpackDec.size], rbp
    xor     eax, eax
    jmp     .out
.nomem:
    mov     rax, HPACK_ERR_NOMEM
.out:
    add     rsp, 16
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    ret

; =============================================================================
; hp_evict_to — evict oldest entries until the accounted size <= target.
; Internal.
; @param  rbx  HpackDec*   @param rsi  target size
; @returns nothing. Preserves rbx, rbp, r12-r15; clobbers the rest.
; =============================================================================
hp_evict_to:
    push    r12
    push    r13
    push    r14                         ; 3 pushes: aligned for calls
    mov     r12, rsi
.next:
    cmp     [rbx + HpackDec.size], r12
    jbe     .done
    cmp     qword [rbx + HpackDec.count], 0
    je      .done                       ; defensive: size and count move together
    mov     r13, [rbx + HpackDec.head]
    sub     r13, [rbx + HpackDec.count]
    inc     r13
    and     r13, [rbx + HpackDec.ring_mask]
    imul    r13, r13, HpEnt_size
    add     r13, [rbx + HpackDec.ents]  ; oldest entry
    mov     eax, [r13 + HpEnt.nlen]
    mov     ecx, [r13 + HpEnt.vlen]
    lea     r14, [rax + rcx + HPACK_ENTRY_OVERHEAD]
    mov     rdi, [r13 + HpEnt.ptr]
    mov     rsi, [rbx + HpackDec.pool]
    call    kv_pool_free wrt ..plt
    sub     [rbx + HpackDec.size], r14
    dec     qword [rbx + HpackDec.count]
    jmp     .next
.done:
    pop     r14
    pop     r13
    pop     r12
    ret

; =============================================================================
; kv_hpack_classify — the meaning of a field (exported for tests).
; @param  rdi  name  @param rsi  nlen  @param rdx  value  @param rcx  vlen
; @returns eax = meaning (hpack.inc).
; =============================================================================
kv_hpack_classify:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     r12, rdx
    mov     r13, rcx
    mov     rdx, rsi
    mov     rsi, rdi
    call    classify_name
    mov     edi, eax
    mov     rsi, r12
    mov     rdx, r13
    call    classify_value
    pop     r13
    pop     r12
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; NAMEIS label — ZF=1 iff (rsi, rdx) equals the CSTR `label`. Clobbers rax,
; rcx, r8, rdi (kv_keyeq keeps rsi, rdx).
; -----------------------------------------------------------------------------
%macro NAMEIS 1
    cmp     rdx, %1_len
    jne     %%no
    lea     rdi, [%1]
    push    rdx
    call    kv_keyeq wrt ..plt
    pop     rdx
    test    eax, eax
    jz      %%no
    cmp     eax, eax                    ; ZF=1
    jmp     %%done
%%no:
    or      eax, 1                      ; ZF=0
%%done:
%endmacro

; =============================================================================
; classify_name — field code of a name. Internal.
; @param  rsi  name   @param rdx  length
; @returns eax = HF_* code. Preserves rsi, rdx; clobbers rax, rcx, rdi, r8,
;          r9, r10.
; @remarks Pseudo-header names must match exactly (lowercase); any other ':'
;          name is HF_PSEUDO_UNKNOWN. Regular names must consist of lowercase
;          token characters only, else HF_BAD_NAME.
; =============================================================================
classify_name:
    test    rdx, rdx
    jz      .bad
    cmp     byte [rsi], ':'
    jne     .regular
    NAMEIS  k_path
    mov     r9d, HF_PATH
    je      .done
    NAMEIS  k_method
    mov     r9d, HF_METHOD
    je      .done
    NAMEIS  k_scheme
    mov     r9d, HF_SCHEME
    je      .done
    NAMEIS  k_authority
    mov     r9d, HF_AUTHORITY
    je      .done
    NAMEIS  k_status
    mov     r9d, HF_STATUS
    je      .done
    mov     r9d, HF_PSEUDO_UNKNOWN
    jmp     .done
.regular:
    lea     r10, [hp_token]
    xor     ecx, ecx
.char:
    movzx   eax, byte [rsi + rcx]
    cmp     byte [r10 + rax], 0
    je      .bad
    inc     rcx
    cmp     rcx, rdx
    jb      .char
    NAMEIS  k_ctype
    mov     r9d, HF_CONTENT_TYPE
    je      .done
    NAMEIS  k_te
    mov     r9d, HF_TE
    je      .done
    NAMEIS  k_encoding
    mov     r9d, HF_GRPC_ENCODING
    je      .done
    NAMEIS  k_clength
    mov     r9d, HF_CONTENT_LENGTH
    je      .done
    mov     r9d, HF_CONN_SPECIFIC
    NAMEIS  k_connection
    je      .done
    NAMEIS  k_keepalive
    je      .done
    NAMEIS  k_proxyconn
    je      .done
    NAMEIS  k_transfer
    je      .done
    NAMEIS  k_upgrade
    je      .done
    mov     r9d, HF_OTHER
    jmp     .done
.bad:
    mov     r9d, HF_BAD_NAME
.done:
    mov     eax, r9d
    ret

; =============================================================================
; classify_value — full meaning of a field given its name's code. Internal.
; @param  edi  HF_* code   @param rsi  value   @param rdx  length
; @returns eax = code | class << 8 | flags. Clobbers rax, rcx, rdi, r8-r11.
; @remarks HF_VALUE_BAD: NUL, CR or LF anywhere, or SP/HTAB at either end
;          (RFC 9113 §8.2.1). Classes are only computed for the codes whose
;          values KvAsm acts on.
; =============================================================================
%macro VALUEIS 1                        ; ZF=1 iff (rsi, rdx) equals `label`
    NAMEIS  %1
%endmacro
classify_value:
    mov     r11d, edi                   ; result being built
    ; ---- value hygiene ----
    xor     ecx, ecx
.scan:
    cmp     rcx, rdx
    jae     .edges
    movzx   eax, byte [rsi + rcx]
    inc     rcx
    cmp     eax, 13
    ja      .scan
    test    eax, eax
    jz      .badval
    cmp     eax, 10
    je      .badval
    cmp     eax, 13
    je      .badval
    jmp     .scan
.edges:
    test    rdx, rdx
    jz      .empty
    movzx   eax, byte [rsi]
    cmp     eax, ' '
    je      .badval
    cmp     eax, 9
    je      .badval
    movzx   eax, byte [rsi + rdx - 1]
    cmp     eax, ' '
    je      .badval
    cmp     eax, 9
    je      .badval
    jmp     .classes
.badval:
    or      r11d, HF_VALUE_BAD
    jmp     .classes
.empty:
    or      r11d, VC_EMPTY << 8
    jmp     .ret                        ; an empty value has no other class
    ; ---- per-code classes ----
.classes:
    movzx   eax, r11b
    cmp     eax, HF_METHOD
    je      .method
    cmp     eax, HF_PATH
    je      .path
    cmp     eax, HF_CONTENT_TYPE
    je      .ctype
    cmp     eax, HF_TE
    je      .te
    cmp     eax, HF_GRPC_ENCODING
    je      .encoding
    jmp     .ret
.method:
    VALUEIS v_post
    jne     .not_post
    or      r11d, VC_POST << 8
    jmp     .ret
.not_post:
    VALUEIS v_head
    jne     .ret
    or      r11d, VC_HEAD << 8
    jmp     .ret
.path:
    VALUEIS v_health                    ; exact path, before the KvAsm prefix
    jne     .kv_path
    or      r11d, M_HEALTH << 8
    jmp     .ret
.kv_path:
    cmp     rdx, v_prefix_len
    jbe     .ret
    push    rsi
    push    rdx
    mov     rdx, v_prefix_len
    VALUEIS v_prefix
    pop     rdx
    pop     rsi
    jne     .ret
    push    rsi
    push    rdx
    add     rsi, v_prefix_len
    sub     rdx, v_prefix_len
    mov     r10d, M_GET
    VALUEIS v_get
    je      .route
    mov     r10d, M_PUT
    VALUEIS v_put
    je      .route
    mov     r10d, M_DELETE
    VALUEIS v_delete
    je      .route
    mov     r10d, M_UPLOAD
    VALUEIS v_upload
    je      .route
    mov     r10d, M_DOWNLOAD
    VALUEIS v_download
    je      .route
    xor     r10d, r10d                  ; M_UNKNOWN
.route:
    pop     rdx
    pop     rsi
    shl     r10d, 8
    or      r11d, r10d
    jmp     .ret
.ctype:
    ; "application/grpc" exactly, or followed by '+' (subtype) or ';' (params)
    cmp     rdx, v_grpc_len
    jb      .ret
    push    rdx
    mov     rdx, v_grpc_len
    VALUEIS v_grpc
    pop     rdx
    jne     .ret
    cmp     rdx, v_grpc_len
    je      .ok
    movzx   eax, byte [rsi + v_grpc_len]
    cmp     eax, '+'
    je      .ok
    cmp     eax, ';'
    je      .ok
    jmp     .ret
.te:
    VALUEIS v_trailers
    je      .ok
    jmp     .ret
.encoding:
    VALUEIS v_identity
    jne     .ret
.ok:
    or      r11d, VC_OK << 8
.ret:
    mov     eax, r11d
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
