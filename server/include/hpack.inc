; =============================================================================
; hpack.inc — HPACK decoder state and field classification codes (RFC 7541),
; shared by hpack.asm (the decoder) and h2n.asm (the native HTTP/2 engine).
;
; A decoded header field reaches the engine as (name, value, meaning). The
; meaning is computed once per distinct field: for static entries at startup,
; for dynamic entries when they are inserted, for literals as they arrive.
; So an indexed field — the common case once a client's dynamic table is warm
; — costs a table lookup, never a string comparison.
;
;   meaning = code | class << 8 | flags
;     code   (bits 0-7)   what the field NAME is (HF_*)
;     class  (bits 8-15)  what its VALUE says, per code (VC_* / route id)
;     flags  (bits 16+)   HF_VALUE_BAD, HF_OVERSIZE
; =============================================================================
%ifndef HPACK_INC
%define HPACK_INC

; ---- field codes (the name) ------------------------------------------------------
; 1..6 are pseudo-header fields: "seen" bits in a request use 1 << code.
%define HF_OTHER            0           ; any regular field KvAsm does not interpret
%define HF_METHOD           1           ; :method
%define HF_SCHEME           2           ; :scheme
%define HF_AUTHORITY        3           ; :authority
%define HF_PATH             4           ; :path
%define HF_STATUS           5           ; :status (response-only: malformed in a request)
%define HF_PSEUDO_UNKNOWN   6           ; any other ':'-name (malformed)
%define HF_PSEUDO_LAST      6
%define HF_BAD_NAME         7           ; empty, uppercase or non-token name (malformed)
%define HF_CONTENT_TYPE     8
%define HF_TE               9
%define HF_GRPC_ENCODING    10
%define HF_CONTENT_LENGTH   11
%define HF_CONN_SPECIFIC    12          ; connection, keep-alive, proxy-connection,
                                        ; transfer-encoding, upgrade (malformed, RFC 9113 §8.2.2)

; ---- value classes (bits 8-15), meaning depends on the code ----------------------
%define VC_EMPTY            0x80        ; the value is empty (with any code)
%define VC_POST             1           ; :method POST
%define VC_HEAD             2           ; :method HEAD (a 405 without a body)
%define VC_OK               1           ; content-type application/grpc[+...|;...],
                                        ; te: trailers, grpc-encoding: identity
; :path classes are the route ids of server.inc: M_UNKNOWN 0, M_GET 1,
; M_PUT 2, M_DELETE 3, M_HEALTH 4, M_UPLOAD 5, M_DOWNLOAD 6.

; ---- flags (bits 16+) ------------------------------------------------------------
%define HF_VALUE_BAD        0x10000     ; value has NUL/CR/LF or edge whitespace
%define HF_OVERSIZE         0x20000     ; a string exceeded the decoder's scratch
                                        ; buffer: its content was not kept

; ---- limits ----------------------------------------------------------------------
%define HPACK_STATIC_COUNT  61          ; RFC 7541 Appendix A
%define HPACK_ENTRY_OVERHEAD 32         ; accounted per entry (RFC 7541 §4.1)
%define HPACK_INT_MAX_SHIFT 28          ; integers above ~2^28 are rejected

; ---- decoder return codes ------------------------------------------------------
%define HPACK_ERR_COMPRESSION   (-1)    ; malformed block: connection error
%define HPACK_ERR_NOMEM         (-2)    ; could not store a table entry

; ---- HpEnt: one dynamic-table entry (name bytes then value bytes in `ptr`) ------
struc HpEnt
    .ptr:       resq 1                  ; pool block holding name then value
    .nlen:      resd 1
    .vlen:      resd 1
    .meaning:   resd 1
    .pad:       resd 1
endstruc

; ---- HpackDec: one connection's decoder ----------------------------------------
struc HpackDec
    .ents:      resq 1                  ; HpEnt[ring_mask + 1] ring
    .ring_mask: resq 1
    .head:      resq 1                  ; slot of the newest entry
    .count:     resq 1                  ; entries in the table
    .size:      resq 1                  ; accounted size (32 + name + value each)
    .max:       resq 1                  ; current maximum (size updates, <= limit)
    .limit:     resq 1                  ; our SETTINGS_HEADER_TABLE_SIZE
    .pool:      resq 1                  ; Pool* the entry blocks come from
    ; Table generation: +1 whenever the dynamic table may change (insertion,
    ; including the oversized one that empties it, and every size update).
    ; Equal generations = identical tables, so a byte-identical block
    ; decodes to identical fields: callers may cache a block's result
    ; keyed by (bytes, generation) — h2n.asm does, per connection.
    .gen:       resq 1
endstruc

%endif
