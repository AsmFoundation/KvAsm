; =============================================================================
; grpc.asm — gRPC request handling: validation, routing, store operations and
; the prefetched GET batch, on top of KvAsm's HTTP/2 engine (h2n.asm).
;
; h2n.asm does all HTTP/2 framing, HPACK, flow control and protocol
; hardening, and resolves the request header fields into Stream state while
; it decodes them (hpack.asm "meanings"): the route from :path, ST_POST from
; :method, ST_CT_OK from content-type, ST_ENCODED from grpc-encoding. This
; module does the rest:
;   kv_grpc_stream_append   buffer a request body within per-stream and
;                           per-connection budgets
;   kv_grpc_dispatch        END_STREAM => validate framing, decode protobuf;
;                           Get, Put and Delete join the connection's batch
;                           (a Put's record is allocated and filled here),
;                           health checks and streaming calls are answered
;                           here
;   kv_grpc_run_batch       execute and answer the Gets, Puts and Deletes one
;                           read queued, as one software-pipelined batch: the
;                           writes under one hold of the writer mutex
;                           (ADR 0011), then every answer in arrival order
;   kv_grpc_stream_release  unlink and free one Stream
;   kv_grpc_free_streams    free every Stream of a connection (teardown)
;
; Routes: /kvasm.v1.KvAsm/{Get,Put,Delete} (proto/kvasm/v1/kvasm.proto)
; and /grpc.health.v1.Health/Check (the standard gRPC health protocol:
; SERVING, NOT_SERVING while draining). Anything else is UNIMPLEMENTED.
;
; Streaming routes (docs/adr/0010):
;   Upload    client streaming. kv_grpc_upload_data receives every DATA
;             payload as it arrives and decodes the message stream with
;             kv_pb_stream_feed; the first message's key and total_size
;             reserve the final record (kv_store_rec_alloc), and every data
;             chunk is copied straight into it, CRC-32C computed on the way.
;             At END_STREAM the size and optional checksum are verified and
;             the record is published atomically (kv_store_publish).
;             Anything wrong is answered at once (Trailers-Only error, then
;             RST_STREAM(NO_ERROR) from h2n) and stores nothing; a cancelled
;             or broken connection frees the unpublished record through
;             stream_free.
;   Download  server streaming. The record is looked up and pinned
;             (kv_store_pin) for the whole call, so one call reads one
;             version however the key changes meanwhile; the response body
;             is generated frame by frame (kv_grpc_gen_fill) straight from
;             the record as flow-control windows open: first message
;             total_size, then data chunks, last message crc32c.
;
; Responses: success = HEADERS(:status 200, content-type) + DATA(message) +
; trailer HEADERS(grpc-status 0), framed by h2n from precomputed header
; blocks; gRPC errors = one "Trailers-Only" HEADERS frame with
; grpc-status/grpc-message (END_STREAM); non-gRPC requests get a plain HTTP
; 405/415 with a short text body. Every request runs to completion inside
; dispatch or the batch (store operations are sub-microsecond), so there is
; no asynchronous state to manage.
;
; Threading: all functions run on the worker thread that owns the
; connection. The worker is QSBR-online meanwhile, so records returned by
; kv_store_get stay valid while their value is copied.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"
%include "pbstream.inc"

%define GRPC_OK                 0
%define GRPC_INVALID_ARGUMENT   3
%define GRPC_RESOURCE_EXHAUSTED 8
%define GRPC_UNIMPLEMENTED      12
%define GRPC_INTERNAL           13
; First request-buffer size: 240 + the pool's 16-byte header fills a 256-byte
; block exactly (a GetRequest is 23 bytes; a Put of a 100-byte value ~130).
%define MIN_REQ_CAP             240
; Most writes one hold of the writer mutex applies (batch_apply_writes). A
; 256 KiB read can carry ~1,500 Puts; applying them all in one hold would make
; every other worker's writes wait for it. 64 warm index updates hold the
; mutex for a few microseconds while still amortizing each acquisition over
; 64 writes. It equals the slot's pending capacity (kvasm.inc), so every
; record a hold replaces is retired outside the mutex (ADR 0011).
%define WRITE_HOLD_MAX          SLOT_PENDING_MAX

; Download chunking: data bytes per response message. 64 KiB keeps every
; message far below gRPC clients' default 4 MiB receive limit and gives the
; receiver progress often; the 1 MiB ceiling bounds one message's latency.
%define DL_CHUNK_DEFAULT        65536
%define DL_CHUNK_MAX            1048576

; Uploads of at least this many bytes are copied into their record with
; non-temporal stores (kv_crc32c_copy_nt): such a record is cold memory that
; nobody reads until a later Get or Download, and the NT copy skips reading
; each destination line first (1.9x on that copy, tests/test_crc32c.c) and
; leaves the cache to data that is used again — the table, other requests.
; Smaller records may be read back while still cached, and their copies
; are a few calls long.
%define UPLOAD_NT_MIN           65536

; UploadRequest tags (proto/kvasm/v1/kvasm.proto): field << 3 | wire type.
%define UP_TAG_KEY              0x0A    ; 1: bytes
%define UP_TAG_SIZE             0x10    ; 2: varint (optional uint64 total_size)
%define UP_TAG_DATA             0x1A    ; 3: bytes
%define UP_TAG_CRC              0x25    ; 4: fixed32 (optional)

; Xfer.kind
%define XF_UPLOAD               1
%define XF_DOWNLOAD             2
; Xfer.flags (Upload)
%define XFF_HAVE_KEY            1       ; the key field is complete (and hashed)
%define XFF_HAVE_SIZE           2       ; total_size was sent
%define XFF_HAVE_CRC            4       ; crc32c was sent (want_crc)
%define XFF_FIRST_DONE          8       ; the first message ended; the record exists
; Xfer.field: what the current length-delimited field of an upload is
%define XFD_NONE                0
%define XFD_KEY                 1
%define XFD_DATA                2
%define XFD_SKIP                3

; Upload refusal codes: negative, returned by upload_event through
; kv_pb_stream_feed (which adds its own PB_ERR_* -1..-3) and by the
; END_STREAM checks; upload_refuse maps each to its gRPC status.
%define UE_FIRST_ONLY           (-10)   ; key/total_size after the first message
%define UE_ORDER                (-11)   ; key/total_size after data began, or data first
%define UE_FIRST_MISSING        (-12)   ; the first message lacked key or total_size
%define UE_KEY                  (-13)   ; key length out of range
%define UE_TOO_LARGE            (-14)   ; total_size above KVASM_MAX_UPLOAD_BYTES
%define UE_BUDGET               (-15)   ; the connection's upload budget is used up
%define UE_MEMORY               (-16)   ; the store's memory limit (or malloc) refused
%define UE_OVERFLOW             (-17)   ; more data than total_size
%define UE_CRC_TWICE            (-18)   ; crc32c sent more than once
%define UE_NOMEM                (-19)   ; out of memory for the key buffer: fatal
%define UE_SHORT                (-20)   ; ended before total_size bytes
%define UE_TRUNCATED            (-21)   ; ended inside a message
%define UE_CRC                  (-22)   ; checksum mismatch
%define UE_FULL                 (-23)   ; the store is at key capacity

; ---- Xfer: a streaming call's state (Stream.xfer), from the worker's pool ------
; One struct for both directions; the fields each uses are commented.
struc Xfer
    .kind:      resq 1                  ; XF_UPLOAD / XF_DOWNLOAD
    .rec:       resq 1                  ; up: unpublished record being filled;
                                        ; down: the pinned record
    .val:       resq 1                  ; the record's value bytes
    .size:      resq 1                  ; up: declared total_size; down: value size
    .off:       resq 1                  ; up: value bytes received; down: next to send
    .crc:       resq 1                  ; CRC-32C of the bytes received / sent so far
    ; ---- Upload ----
    .flags:     resq 1                  ; XFF_*
    .want_crc:  resq 1                  ; the client's crc32c (with XFF_HAVE_CRC)
    .h1:        resq 1                  ; kv_store_hash of the key (h1, h2 adjacent:
    .h2:        resq 1                  ;   kv_store_hash writes both)
    .key:       resq 1                  ; pool block holding the key until the
                                        ; record exists, else 0
    .key_len:   resq 1
    .key_off:   resq 1                  ; key bytes received so far
    .field:     resq 1                  ; XFD_*: the current bytes field
    .msgs:      resq 1                  ; messages begun (1 = inside the first)
    .reserved:  resq 1                  ; bytes this upload counts in Conn.up_bytes
    .ps:        resb PbStream_size      ; the message-stream decoder
    ; ---- Download ----
    .end:       resq 1                  ; one past the last value byte to send
    .chunk:     resq 1                  ; data bytes per message (the last may be less)
    .mrem:      resq 1                  ; data bytes of the current message still to send
    .hlen:      resq 1                  ; head bytes of the current message
    .hoff:      resq 1                  ; head bytes already sent
    .tlen:      resq 1                  ; tail bytes (the crc32c field) of the last message
    .toff:      resq 1                  ; tail bytes already sent
    .last:      resq 1                  ; the current message is the last one
    .first:     resq 1                  ; no message has started yet
    .hbuf:      resb 32                 ; prefix + total_size field + data field header
    .tbuf:      resb 8                  ; 0x1D + crc32c (little-endian)
endstruc

; PbDl as written by kv_pb_decode_download (proto.asm)
struc PbDl
    .key:           resq 1
    .key_len:       resq 1
    .offset:        resq 1
    .length:        resq 1
    .chunk:         resq 1
    .has_key:       resd 1
    .has_length:    resd 1
endstruc

extern kv_pool_free
extern kv_pool_realloc
extern kv_pool_malloc
extern kv_pool_calloc
extern memcpy
extern kv_keyeq
extern kv_store_hash
extern kv_store_get
extern kv_store_write_begin
extern kv_store_write_end
extern kv_store_put_locked
extern kv_store_delete_locked
extern kv_store_prefetch
extern kv_store_prefetch_records
extern kv_store_locate
extern kv_store_prefetch_records_at
extern kv_store_get_at
extern kv_store_rec_alloc
extern kv_store_rec_alloc_local
extern kv_store_rec_free
extern kv_store_publish
extern kv_store_pin
extern kv_store_unpin
extern kv_hcache_get
extern kv_pb_decode_kv
extern kv_pb_decode_download
extern kv_pb_stream_feed
extern kv_pb_stream_idle
extern kv_pb_write_varint
extern kv_pb_varint_len
extern kv_crc32c_copy
extern kv_crc32c_copy_nt
extern kv_h2n_reply_parts
extern kv_h2n_reply_static
extern kv_h2n_reply_headers
extern kv_h2n_reply_stream

; PbKv as written by kv_pb_decode_kv (proto.asm)
struc PbKv
    .key:           resq 1
    .key_len:       resq 1
    .value:         resq 1
    .value_len:     resq 1
    .has_key:       resd 1
    .has_value:     resd 1
endstruc

; ---- static strings -------------------------------------------------------------
%macro CSTR 2+
%1: db %2
%{1}_len equ $ - %1
%endmacro

; One HdrField (server.inc) referring to two CSTR labels. The strings live in
; .rodata for the whole process, so h2n encodes straight from them. Built
; with istruc so a layout change in server.inc cannot desynchronize it.
%macro NV 2
    istruc HdrField
        at HdrField.name,     dq %1
        at HdrField.value,    dq %2
        at HdrField.namelen,  dq %{1}_len
        at HdrField.valuelen, dq %{2}_len
    iend
%endmacro

section .rodata
CSTR s_status,      ":status"
CSTR s_200,         "200"
CSTR s_405,         "405"
CSTR s_415,         "415"
CSTR s_ctype,       "content-type"
CSTR s_ctype_grpc,  "application/grpc"
CSTR s_ctype_text,  "text/plain; charset=utf-8"
CSTR s_allow,       "allow"
CSTR s_accept,      "accept"
; Plain HTTP error bodies. Beyond helping a human with curl, a body keeps the
; stream open while flow control holds its DATA — HTTP/2 conformance suites
; (h2spec) rely on that to exercise stream limits and window handling.
CSTR b_405,         "405 method not allowed: KvAsm speaks gRPC, send POST", 10
CSTR b_415,         "415 unsupported media type: send content-type application/grpc", 10
CSTR s_gstatus,     "grpc-status"
CSTR s_gmessage,    "grpc-message"
CSTR s_c3,          "3"
CSTR s_c5,          "5"
CSTR s_c8,          "8"
CSTR s_c12,         "12"
CSTR s_c13,         "13"
CSTR s_m_method,    "unknown method"
CSTR s_m_compress,  "compressed messages are not supported"
CSTR s_m_badflag,   "compressed flag set without a grpc-encoding"
CSTR s_accept_enc,  "grpc-accept-encoding"
CSTR s_m_framing,   "expected exactly one uncompressed request message"
CSTR s_m_malformed, "malformed request message"
CSTR s_m_key,       "key length out of range"
CSTR s_m_value,     "value too large"
CSTR s_m_toolarge,  "request message too large"
CSTR s_m_full,      "store is at capacity"
CSTR s_m_memory,    "store memory limit reached"
CSTR s_m_noservice, "unknown service"
; Streaming (Upload/Download) statuses and messages.
CSTR s_c11,         "11"                ; OUT_OF_RANGE
CSTR s_c15,         "15"                ; DATA_LOSS
CSTR s_m_notfound,  "key not found"
CSTR s_m_range,     "offset is beyond the end of the value"
CSTR s_m_bigget,    "value exceeds KVASM_MAX_VALUE_BYTES: read it with Download"
CSTR s_m_upfirst,   "key and total_size are only allowed in the first message"
CSTR s_m_uporder,   "key and total_size must come before any data"
CSTR s_m_upmissing, "the first message must carry key and total_size"
CSTR s_m_upbudget,  "uploads in progress exceed this connection's budget (KVASM_CONN_BUFFER_MB)"
CSTR s_m_upover,    "more data than total_size"
CSTR s_m_upshort,   "the upload ended before total_size bytes"
CSTR s_m_uptrunc,   "the request stream ended inside a message"
CSTR s_m_upcrc2,    "crc32c sent more than once"
CSTR s_m_upcrc,     "crc32c mismatch: nothing was stored"
CSTR s_m_upflag,    "invalid gRPC message prefix"

CSTR v_post,        "POST"
CSTR v_identity,    "identity"
; The one service name the health check reports on besides "" (whole server).
CSTR v_service,     "kvasm.v1.KvAsm"

; HdrField arrays carry absolute string pointers: PIE-legal only through
; load-time relocations, hence .data.rel.ro (relocated, then read-only via
; RELRO) rather than .rodata (would need a text relocation; -z text rejects
; it) or .data (would stay writable for the process lifetime).
section .data.rel.ro progbits alloc noexec write align=8
; RFC 9110 §15.5.6: a 405 MUST carry Allow. §15.5.16: a 415 can name the
; accepted media type in Accept. Both carry a text body (static, ST_STATIC_RESP).
nv_405:             NV s_status, s_405
                    NV s_allow, v_post
                    NV s_ctype, s_ctype_text
nv_415:             NV s_status, s_415
                    NV s_accept, s_ctype_grpc
                    NV s_ctype, s_ctype_text
%macro TRAILERS_ONLY 3                  ; label, code, message
%1:                 NV s_status, s_200
                    NV s_ctype, s_ctype_grpc
                    NV s_gstatus, %2
                    NV s_gmessage, %3
%endmacro
TRAILERS_ONLY nv_e_method,    s_c12, s_m_method
; Unsupported codec: gRPC requires UNIMPLEMENTED *and* a grpc-accept-encoding
; header so the client can retry with a codec we accept. The fifth entry
; directly follows the macro's four, so the array is contiguous (count 5).
TRAILERS_ONLY nv_e_compress,  s_c12, s_m_compress
                    NV s_accept_enc, v_identity
; Compressed flag without a declared codec is a sender bug, not a missing
; feature: INTERNAL, as grpc-go and grpc-core answer it.
TRAILERS_ONLY nv_e_badflag,   s_c13, s_m_badflag
TRAILERS_ONLY nv_e_framing,   s_c13, s_m_framing
TRAILERS_ONLY nv_e_malformed, s_c3,  s_m_malformed
TRAILERS_ONLY nv_e_key,       s_c3,  s_m_key
TRAILERS_ONLY nv_e_value,     s_c3,  s_m_value
TRAILERS_ONLY nv_e_toolarge,  s_c8,  s_m_toolarge
TRAILERS_ONLY nv_e_full,      s_c8,  s_m_full
TRAILERS_ONLY nv_e_memory,    s_c8,  s_m_memory
TRAILERS_ONLY nv_e_noservice, s_c5,  s_m_noservice
; Streaming.
TRAILERS_ONLY nv_e_notfound,  s_c5,  s_m_notfound
TRAILERS_ONLY nv_e_range,     s_c11, s_m_range
TRAILERS_ONLY nv_e_bigget,    s_c8,  s_m_bigget
TRAILERS_ONLY nv_e_upfirst,   s_c3,  s_m_upfirst
TRAILERS_ONLY nv_e_uporder,   s_c3,  s_m_uporder
TRAILERS_ONLY nv_e_upmissing, s_c3,  s_m_upmissing
TRAILERS_ONLY nv_e_upbudget,  s_c8,  s_m_upbudget
TRAILERS_ONLY nv_e_upover,    s_c3,  s_m_upover
TRAILERS_ONLY nv_e_upshort,   s_c3,  s_m_upshort
TRAILERS_ONLY nv_e_uptrunc,   s_c13, s_m_uptrunc
TRAILERS_ONLY nv_e_upcrc2,    s_c3,  s_m_upcrc2
TRAILERS_ONLY nv_e_upcrc,     s_c15, s_m_upcrc
TRAILERS_ONLY nv_e_upflag,    s_c13, s_m_upflag

section .text

global kv_grpc_free_streams
global kv_grpc_run_batch
global kv_grpc_dispatch
global kv_grpc_stream_append
global kv_grpc_stream_release
global kv_grpc_upload_data
global kv_grpc_gen_fill

; =============================================================================
; kv_grpc_free_streams — free every Stream still attached to a connection.
; @param  rdi  Conn*
; @returns nothing.
; @remarks Called at connection teardown (linger and destroy), after
;          kv_h2n_close: the engine releases its own state (stream map, pend
;          list, HPACK ring) but never the Streams, which live on
;          Conn.streams and would otherwise leak. Idempotent: the list, the
;          buffered count and the GET batch are cleared, so a lingering
;          connection that comes through conn_destroy later frees nothing
;          twice.
; =============================================================================
kv_grpc_free_streams:
    push    rbx
    push    r12
    sub     rsp, 8
    mov     r12, rdi
    mov     rbx, [r12 + Conn.streams]
.next:
    test    rbx, rbx
    jz      .done
    mov     rdi, rbx
    mov     rbx, [rbx + Stream.next]
    call    stream_free
    jmp     .next
.done:
    xor     eax, eax
    mov     [r12 + Conn.streams], rax
    mov     [r12 + Conn.buffered], rax
    mov     [r12 + Conn.batch_head], rax  ; a batch of freed streams must not survive
    mov     [r12 + Conn.batch_tail], rax
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; stream_free — release a Stream and its buffers to the worker's pool. Internal.
; @param  rdi  Stream* (its conn and conn's worker must still be alive)
; @returns nothing.
; @remarks Buffers the Stream does not own are left alone: a borrowed request
;          body (ST_BORROWED, h2n's zero-copy path) and a static response
;          body (ST_STATIC_RESP, .rodata). Absent buffers are skipped here
;          rather than handed to kv_pool_free (which would ignore NULL):
;          on the fast paths — a unary call whose body arrived in one frame,
;          answered straight into the output buffer — neither buffer exists,
;          so this is two calls saved on every request.
;          A streaming call's transfer state is released first (xfer_release):
;          every way a stream ends — its response completed, a reset from
;          either side, the connection's teardown, drain or idle timeout —
;          comes through here, which is what makes "an interrupted upload
;          stores nothing" and "a download never leaks its pin" hold.
; =============================================================================
stream_free:
    ; The common case first: a unary call answered straight into the output
    ; buffer owns nothing but the Stream itself — no transfer state, no
    ; unpublished record, no response buffer, and a request body that was
    ; borrowed from the read buffer or never existed. One block goes back to
    ; the pool, as a tail call (kv_pool_free needs the entry alignment this
    ; function was entered with, and gets it).
    mov     rax, [rdi + Stream.xfer]
    or      rax, [rdi + Stream.wrec]
    or      rax, [rdi + Stream.resp_buf] ; a static body is non-zero too: the full path skips it
    jnz     .full
    test    qword [rdi + Stream.flags], ST_BORROWED
    jnz     .only_stream
    cmp     qword [rdi + Stream.req_buf], 0
    jne     .full
.only_stream:
    mov     rax, [rdi + Stream.conn]
    mov     rax, [rax + Conn.worker]
    lea     rsi, [rax + Worker.pool]
    jmp     kv_pool_free                ; (Stream*, Pool*)
.full:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rax, [rbx + Stream.conn]
    mov     rax, [rax + Conn.worker]
    lea     r12, [rax + Worker.pool]
    cmp     qword [rbx + Stream.xfer], 0
    je      .no_xfer                    ; unary calls: one compare
    mov     rdi, rbx
    call    xfer_release
.no_xfer:
    ; A batched Put whose record was never published — the peer reset the
    ; stream, the connection died before its batch ran, or the store was full
    ; — returns it here, with its memory reservation. Published records were
    ; handed to the store (wrec = 0) and are retired by it instead.
    mov     rsi, [rbx + Stream.wrec]
    test    rsi, rsi
    jz      .no_wrec                    ; every other call: one compare
    mov     rax, [rbx + Stream.conn]
    mov     rax, [rax + Conn.worker]
    mov     rdi, [rax + Worker.table]
    call    kv_store_rec_free wrt ..plt
    mov     qword [rbx + Stream.wrec], 0
.no_wrec:
    test    qword [rbx + Stream.flags], ST_BORROWED
    jnz     .borrowed                   ; points into the read buffer: not ours
    mov     rdi, [rbx + Stream.req_buf]
    test    rdi, rdi
    jz      .borrowed                   ; no body buffer was ever allocated
    mov     rsi, r12
    call    kv_pool_free
.borrowed:
    test    qword [rbx + Stream.flags], ST_STATIC_RESP
    jnz     .static_resp                ; .rodata text: freeing it would crash
    mov     rdi, [rbx + Stream.resp_buf]
    test    rdi, rdi
    jz      .static_resp                ; answered without a response buffer
    mov     rsi, r12
    call    kv_pool_free
.static_resp:
    mov     rdi, rbx
    mov     rsi, r12
    call    kv_pool_free
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_grpc_stream_append — buffer request body bytes of a stream.
;
; @param  rdi  Conn*   @param rsi  Stream*   @param rdx  data   @param rcx  len
; @returns eax = 0 (appended, or dropped because the stream is TOO_LARGE),
;          or 1 (the connection's buffering budget would be exceeded: the
;          stream is now TOO_LARGE and the caller must reset it with
;          ENHANCE_YOUR_CALM).
; @remarks Two budgets bound memory: a stream whose body exceeds max_request
;          is marked TOO_LARGE (answered RESOURCE_EXHAUSTED at END_STREAM, its
;          bytes dropped); a connection whose buffered bytes would exceed
;          conn_budget gets that stream reset. Buffers grow geometrically,
;          capped at max_request; out of memory is treated as oversize.
;          h2n.asm calls this for every DATA payload it does not borrow
;          (a body that arrives whole in one frame is dispatched in place).
; =============================================================================
kv_grpc_stream_append:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [0] result; 6 pushes + 8: aligned for calls
    mov     qword [rsp], 0
    mov     r14, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     r12, rdx                    ; data
    mov     r13, rcx                    ; len
    test    qword [rbx + Stream.flags], ST_TOO_LARGE
    jnz     .out
    mov     rbp, [rbx + Stream.req_len]
    add     rbp, r13                    ; new length
    mov     rcx, [r14 + Conn.worker]
    mov     rcx, [rcx + Worker.cfg]
    mov     rax, [rcx + Config.max_request]
    add     rax, GRPC_PREFIX
    cmp     rbp, rax
    ja      .too_large
    cmp     rbp, [rbx + Stream.req_cap]
    jbe     .append
    ; grow: max(new_len, 2*cap, MIN_REQ_CAP), capped at max_request + prefix
    mov     rdx, [rbx + Stream.req_cap]
    shl     rdx, 1
    cmp     rdx, rbp
    cmovb   rdx, rbp
    mov     r8d, MIN_REQ_CAP
    cmp     rdx, r8
    cmovb   rdx, r8
    cmp     rdx, rax
    cmova   rdx, rax
    mov     r8, rdx
    sub     r8, [rbx + Stream.req_cap]  ; growth
    mov     r9, [r14 + Conn.buffered]
    add     r9, r8
    cmp     r9, [rcx + Config.conn_budget]
    ja      .over_budget
    push    rdx
    push    r8
    mov     rdi, [rbx + Stream.req_buf]
    mov     rsi, rdx
    mov     rax, [r14 + Conn.worker]
    lea     rdx, [rax + Worker.pool]
    call    kv_pool_realloc
    pop     r8
    pop     rdx
    test    rax, rax
    jz      .too_large                  ; out of memory: treat like oversize
    mov     [rbx + Stream.req_buf], rax
    mov     [rbx + Stream.req_cap], rdx
    add     [r14 + Conn.buffered], r8
.append:
    mov     rdi, [rbx + Stream.req_buf]
    add     rdi, [rbx + Stream.req_len]
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    mov     [rbx + Stream.req_len], rbp
    jmp     .out
.over_budget:
    mov     qword [rsp], 1              ; the caller resets the stream
.too_large:
    or      qword [rbx + Stream.flags], ST_TOO_LARGE
    mov     rax, [rbx + Stream.req_cap]
    sub     [r14 + Conn.buffered], rax
    mov     rdi, [rbx + Stream.req_buf]
    mov     rax, [r14 + Conn.worker]
    lea     rsi, [rax + Worker.pool]
    call    kv_pool_free
    xor     eax, eax
    mov     [rbx + Stream.req_buf], rax
    mov     [rbx + Stream.req_len], rax
    mov     [rbx + Stream.req_cap], rax
.out:
    mov     eax, [rsp]
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; stream_unlink_free (exported as kv_grpc_stream_release) — remove a Stream
; from its connection and free it.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns nothing.
; @remarks Also returns the stream's buffered request bytes to the
;          connection's budget. h2n.asm releases every stream it closes
;          through this function, which keeps Conn.streams and
;          Conn.buffered exact.
; =============================================================================
kv_grpc_stream_release:
stream_unlink_free:
    mov     rcx, [rsi + Stream.prev]
    mov     rdx, [rsi + Stream.next]
    test    rcx, rcx
    jz      .head
    mov     [rcx + Stream.next], rdx
    jmp     .fix
.head:
    mov     [rdi + Conn.streams], rdx
.fix:
    test    rdx, rdx
    jz      .unlinked
    mov     [rdx + Stream.prev], rcx
.unlinked:
    mov     rax, [rsi + Stream.req_cap]
    sub     [rdi + Conn.buffered], rax
    mov     rdi, rsi
    jmp     stream_free                 ; tail call (keeps the caller's alignment)

; =============================================================================
; kv_grpc_dispatch — validate a complete request, run it, answer it.
;
; @param  rdi  Conn*   @param rsi  Stream* (END_STREAM received; its header
;              fields were resolved into Stream.method/flags by h2n.asm)
; @returns eax = 0, or -1 when the answer could not be queued (out of
;          memory in h2n's output buffer: the engine ends the connection).
; @remarks The Stream may be answered and freed before this returns (h2n
;          closes a stream whose response is complete), so the caller must
;          not touch it afterwards; a Get, Put or Delete instead joins the
;          connection's batch and is executed and answered by
;          kv_grpc_run_batch after the read (a Put's record is built here,
;          outside the writer mutex; ADR 0011).
;          Validation order follows the gRPC HTTP/2 spec: method/content-type
;          are HTTP-level (405/415), then routing (UNIMPLEMENTED), then the
;          5-byte message framing, then the protobuf payload and field limits.
;          Compressed-flag handling matches grpc-go/grpc-core: flag 1 with a
;          declared codec (grpc-encoding other than identity) is UNIMPLEMENTED
;          + grpc-accept-encoding: identity; flag 1 with no/identity codec is
;          INTERNAL (the sender contradicts itself); flag 0 is accepted
;          whatever grpc-encoding says, because it means "not compressed".
;          Streaming routes: an Upload is finalized here (its body was
;          consumed as it arrived, see upload_finish); a Download is a unary
;          request whose response is generated (download_start). An Upload
;          already answered early (a 405/415 whose body is still going out)
;          reaches here at END_STREAM and is left alone.
; =============================================================================
kv_grpc_dispatch:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 104                    ; [0..40) PbKv, [40] h1, [48] h2,
                                        ; [56..104) PbDl (Download);
                                        ; 6 pushes + 104: rsp 16-aligned for calls
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    test    qword [rbx + Stream.flags], ST_RESPONDED
    jnz     .answered                   ; an Upload refused early: nothing to add
    mov     rax, [r12 + Conn.worker]
    mov     r13, [rax + Worker.cfg]
    mov     r14, [rax + Worker.table]
    inc     qword [rax + Worker.st_requests]
    or      qword [rbx + Stream.flags], ST_RESPONDED

    mov     rax, [rbx + Stream.flags]
    test    rax, ST_POST
    jz      .http_405
    test    rax, ST_CT_OK
    jz      .http_415
    cmp     qword [rbx + Stream.method], M_UNKNOWN
    je      .e_method
    cmp     qword [rbx + Stream.method], M_UPLOAD
    je      .upload                     ; body already consumed: no framing to check
    test    rax, ST_TOO_LARGE
    jnz     .e_toolarge

    ; gRPC framing: exactly one message, uncompressed.
    mov     rcx, [rbx + Stream.req_len]
    cmp     rcx, GRPC_PREFIX
    jb      .e_framing
    mov     rsi, [rbx + Stream.req_buf]
    movzx   eax, byte [rsi]
    cmp     eax, 1
    je      .compressed
    ja      .e_framing                  ; flag values other than 0/1 are undefined
    mov     eax, [rsi + 1]
    bswap   eax                         ; big-endian length
    add     rax, GRPC_PREFIX
    cmp     rax, rcx
    jne     .e_framing

    cmp     qword [rbx + Stream.method], M_DOWNLOAD
    je      .download
    lea     rdi, [rsi + GRPC_PREFIX]
    lea     rsi, [rax - GRPC_PREFIX]
    lea     rdx, [rsp]
    call    kv_pb_decode_kv wrt ..plt
    test    rax, rax
    jnz     .e_malformed
    ; HealthCheckRequest{string service = 1} decodes into PbKv.key; its
    ; service may legitimately be empty, so it branches off before the key
    ; validation below.
    cmp     qword [rbx + Stream.method], M_HEALTH
    je      .health
    cmp     dword [rsp + PbKv.has_key], 0
    je      .e_key
    mov     rax, [rsp + PbKv.key_len]
    test    rax, rax
    jz      .e_key
    cmp     rax, [r13 + Config.max_key]
    ja      .e_key

    mov     rdi, r14
    mov     rsi, [rsp + PbKv.key]
    mov     rdx, [rsp + PbKv.key_len]
    lea     rcx, [rsp + 40]
    call    kv_store_hash wrt ..plt

    cmp     qword [rbx + Stream.method], M_PUT
    jne     .defer                      ; Get, Delete: nothing to build first

    ; ---- Put: build the record now; the batch publishes it (ADR 0011) ----
    ; Allocation (with its memory reservation) and both copies happen here,
    ; outside the writer mutex, while the value is still hot from parsing.
    ; The record usually comes from this worker's own record cache — records
    ; its reclamation freed — so no glibc arena lock is shared with the
    ; worker that will free it.
    ; Refusals that need no index (value too large, memory limit) are
    ; answered at once; a full store (-ENOSPC) is only known when the batch
    ; publishes. From here until it is published the record belongs to the
    ; Stream: stream_free returns it if the stream or connection dies first.
    mov     rax, [rsp + PbKv.value_len]
    cmp     rax, [r13 + Config.max_value]
    ja      .e_value
    mov     r8, rax                     ; value length
    mov     rdi, r14
    mov     rax, [r12 + Conn.worker]
    mov     esi, [rax + Worker.id]      ; this worker's slot: its record cache
    mov     rdx, [rsp + PbKv.key]
    mov     rcx, [rsp + PbKv.key_len]
    call    kv_store_rec_alloc_local wrt ..plt  ; header + key written
    cmp     rax, -ENOMEM
    je      .e_memory
    test    rax, rax
    js      .e_value                    ; -E2BIG
    mov     [rbx + Stream.wrec], rax
    mov     rcx, [rsp + PbKv.key_len]
    add     rcx, 7
    and     rcx, -8                     ; the value follows the key padded to 8
    lea     rdi, [rax + Rec.data + rcx]
    mov     rsi, [rsp + PbKv.value]
    mov     rdx, [rsp + PbKv.value_len]
    call    memcpy wrt ..plt            ; (an empty value copies nothing)

    ; ---- Get, Put, Delete: deferred into the connection's batch ----
    ; The store operation itself runs in kv_grpc_run_batch once the whole
    ; read is parsed. Here: remember the key (it points into req_buf, which
    ; lives as long as the Stream, or into the read buffer for a borrowed
    ; body, which lives until the batch has run) and its hashes, and start
    ; loading both candidate buckets now, so the cache misses of every call
    ; in this read overlap — and, for writes, land before the writer mutex
    ; is taken instead of while every other writer waits on it.
.defer:
    mov     rax, [rsp + PbKv.key]
    mov     [rbx + Stream.key], rax
    mov     rax, [rsp + PbKv.key_len]
    mov     [rbx + Stream.key_len], rax
    mov     rsi, [rsp + 40]
    mov     [rbx + Stream.h1], rsi
    mov     rdx, [rsp + 48]
    mov     [rbx + Stream.h2], rdx
    mov     rdi, r14
    call    kv_store_locate wrt ..plt   ; both bucket lines requested, their addresses kept
    mov     [rbx + Stream.b1], rax
    mov     [rbx + Stream.b2], rdx
    or      qword [rbx + Stream.flags], ST_BATCHED
    mov     qword [rbx + Stream.batch_next], 0
    mov     rax, [r12 + Conn.batch_tail] ; FIFO: answer in arrival order
    test    rax, rax
    jz      .batch_first
    mov     [rax + Stream.batch_next], rbx
    jmp     .batch_linked
.batch_first:
    mov     [r12 + Conn.batch_head], rbx
.batch_linked:
    mov     [r12 + Conn.batch_tail], rbx
    xor     eax, eax
    jmp     .out

    ; ---- grpc.health.v1.Health/Check ----
    ; service "" (whole server) or "kvasm.v1.KvAsm" => SERVING (1), or
    ; NOT_SERVING (2) once this worker is draining, so probes and load
    ; balancers stop routing here before the connection closes; any other
    ; service => NOT_FOUND, as the health protocol specifies.
.health:
    mov     rax, [rsp + PbKv.key_len]
    test    rax, rax
    jz      .health_status
    cmp     rax, v_service_len
    jne     .e_noservice
    mov     rdi, [rsp + PbKv.key]
    lea     rsi, [v_service]
    mov     edx, v_service_len
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .e_noservice
.health_status:
    mov     rax, [r12 + Conn.worker]
    mov     edi, 1                      ; ServingStatus.SERVING
    cmp     qword [rax + Worker.draining], 0
    je      .bool_reply
    mov     edi, 2                      ; ServingStatus.NOT_SERVING

    ; rdi = value of field 1 as a one-byte varint (0..127): 0 => the empty
    ; message (proto3 default: false / not found / UNKNOWN), else 08 <value>.
    ; Serves bool replies (found/created/deleted) and the health status enum.
    ; The whole message (5 or 7 bytes) is built on the stack, in the PbKv
    ; area, which is dead by now (the store call consumed the key and value,
    ; the health check its service name), and framed by kv_h2n_reply_parts
    ; exactly like a GET miss: no response buffer, so no pool allocation now
    ; and no free when the stream closes. h2n copies the bytes (into the
    ; DATA frame, or into its own buffer when the windows are short), so the
    ; stack copy only needs to live for the call.
.bool_reply:
    mov     r13, rdi                    ; (Config* is not needed any more)
    xor     esi, esi                    ; protobuf payload length
    test    r13, r13
    jz      .bool_head
    mov     byte [rsp + GRPC_PREFIX], 0x08      ; field 1, wire type varint
    mov     [rsp + GRPC_PREFIX + 1], r13b       ; < 128: a single varint byte
    mov     esi, 2
.bool_head:
    lea     r13, [rsi + GRPC_PREFIX]    ; whole message length
    mov     rdi, rsp
    call    write_prefix                ; bytes 0..4; the payload is behind them
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, rsp
    mov     rcx, r13
    xor     r8d, r8d                    ; no body: the head is the message
    xor     r9d, r9d
    call    kv_h2n_reply_parts wrt ..plt
    jmp     .submitted

.http_405:
    lea     rax, [b_405]
    mov     ecx, b_405_len
    lea     rdx, [nv_405]
    jmp     .http_error
.http_415:
    lea     rax, [b_415]
    mov     ecx, b_415_len
    lea     rdx, [nv_415]
.http_error:
    ; Plain HTTP error: 3 headers + a static text body. h2n ends the stream on
    ; the last DATA frame (no gRPC trailers) for ST_STATIC_RESP, and
    ; stream_free leaves the .rodata buffer alone.
    or      qword [rbx + Stream.flags], ST_STATIC_RESP
    mov     [rbx + Stream.resp_buf], rax
    mov     [rbx + Stream.resp_len], rcx
    mov     qword [rbx + Stream.resp_off], 0
    mov     rdi, r12
    mov     rsi, rbx
    mov     ecx, 3                      ; rdx = the 405 or 415 fields (set above)
    call    kv_h2n_reply_static wrt ..plt
    jmp     .submitted
.e_method:
    lea     rdx, [nv_e_method]
    jmp     .trailers_only
.compressed:
    ; ST_ENCODED was set by h2n for a grpc-encoding other than identity.
    test    qword [rbx + Stream.flags], ST_ENCODED
    jz      .e_badflag
    lea     rdx, [nv_e_compress]
    mov     ecx, 5                      ; + grpc-accept-encoding
    jmp     .trailers_n
.e_badflag:
    lea     rdx, [nv_e_badflag]
    jmp     .trailers_only
.e_framing:
    lea     rdx, [nv_e_framing]
    jmp     .trailers_only
.e_malformed:
    lea     rdx, [nv_e_malformed]
    jmp     .trailers_only
.e_key:
    lea     rdx, [nv_e_key]
    jmp     .trailers_only
.e_value:
    lea     rdx, [nv_e_value]
    jmp     .trailers_only
.e_toolarge:
    lea     rdx, [nv_e_toolarge]
    jmp     .trailers_only
.e_full:
    lea     rdx, [nv_e_full]
    jmp     .trailers_only
.e_memory:
    lea     rdx, [nv_e_memory]
    jmp     .trailers_only
.e_noservice:
    lea     rdx, [nv_e_noservice]
.trailers_only:
    mov     ecx, 4                      ; :status, content-type, grpc-status, grpc-message
.trailers_n:
    mov     rax, [r12 + Conn.worker]
    inc     qword [rax + Worker.st_errors]
    ; Trailers-Only: one HEADERS frame with END_STREAM (rdx = fields, ecx =
    ; count still as set above).
    mov     rdi, r12
    mov     rsi, rbx
    call    kv_h2n_reply_headers wrt ..plt
    jmp     .submitted

    ; ---- Upload: every byte was consumed as it arrived; verify and publish ----
.upload:
    mov     rdi, r12
    mov     rsi, rbx
    call    upload_finish               ; answers or refuses: 0 / -1
    jmp     .submitted

    ; ---- Download: a unary request with a generated, streamed response ----
    ; (rsi = req_buf, rax = the message's length with its prefix)
.download:
    lea     rdi, [rsi + GRPC_PREFIX]
    lea     rsi, [rax - GRPC_PREFIX]
    lea     rdx, [rsp + 56]
    call    kv_pb_decode_download wrt ..plt
    test    rax, rax
    jnz     .e_malformed
    cmp     dword [rsp + 56 + PbDl.has_key], 0
    je      .e_key
    mov     rax, [rsp + 56 + PbDl.key_len]
    test    rax, rax
    jz      .e_key
    cmp     rax, [r13 + Config.max_key]
    ja      .e_key
    mov     rdi, r12
    mov     rsi, rbx
    lea     rdx, [rsp + 56]
    call    download_start              ; answers: 0 / -1
    jmp     .submitted

.answered:
    xor     eax, eax
    jmp     .out
.submitted:
    ; h2n's reply functions return 0 or -1 (out of memory); normalize so the
    ; contract above holds whatever negative value they pick.
    test    eax, eax
    jz      .out
    mov     eax, -1
.out:
    add     rsp, 104
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_grpc_run_batch — execute and answer every Get, Put and Delete queued
; while parsing the last read.
;
; @param  rdi  Conn*
; @returns eax = 0, or -1 if a response could not be queued (out of memory
;          in h2n's output buffer: the caller destroys the connection).
; @remarks Called by kv_h2n_read right after the read's frames were parsed,
;          on the connection's thread, while the worker is QSBR-online
;          (records stay valid while their values are copied).
;          Three passes, a software pipeline across requests:
;            1. prefetch the candidate records (the buckets were prefetched at
;               parse time and have arrived by now) — for a write, the old
;               record whose key the index update compares;
;            2. apply every Put and Delete in arrival order under ONE hold of
;               the writer mutex (batch_apply_writes, ADR 0011) — their lines
;               are warm, so the mutex is held for a few dozen ns per write
;               instead of the DRAM misses it used to wait out;
;            3. run each Get and frame every response, in arrival order.
;          Writes take effect before the Gets of the same read, as when Puts
;          ran at dispatch: every call of one read was in flight at once (none
;          was answered before the read ended), so this order is a valid
;          linearization (ADR 0005, ADR 0011).
;          Streams the peer reset meanwhile (ST_CLOSED) are freed, not
;          answered — a reset Put is not applied, and its record is returned
;          by stream_free. The batch is detached before pass 2, so nothing
;          can observe a half-processed list.
; =============================================================================
kv_grpc_run_batch:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: rsp 16-aligned for calls
    mov     r12, rdi
    mov     rbx, [r12 + Conn.batch_head]
    xor     ebp, ebp                    ; result
    test    rbx, rbx
    jz      .out
    mov     rax, [r12 + Conn.worker]
    mov     r13, [rax + Worker.table]
    xor     r15d, r15d                  ; writes in the batch
.prefetch:
    mov     rdi, [rbx + Stream.b1]
    mov     rsi, [rbx + Stream.b2]
    mov     rdx, [rbx + Stream.h2]
    call    kv_store_prefetch_records_at wrt ..plt
    cmp     qword [rbx + Stream.method], M_GET
    je      .prefetch_next
    inc     r15                         ; a Put or a Delete
.prefetch_next:
    mov     rbx, [rbx + Stream.batch_next]
    test    rbx, rbx
    jnz     .prefetch
    mov     rbx, [r12 + Conn.batch_head]
    xor     eax, eax
    mov     [r12 + Conn.batch_head], rax
    mov     [r12 + Conn.batch_tail], rax
    test    r15, r15
    jz      .answer                     ; a read of Gets only: no mutex at all
    mov     rdi, r13
    mov     rax, [r12 + Conn.worker]
    mov     esi, [rax + Worker.id]      ; the worker's QSBR slot takes the old records
    mov     rdx, rbx
    call    batch_apply_writes
.answer:
    mov     r14, [rbx + Stream.batch_next]
    and     qword [rbx + Stream.flags], ~ST_BATCHED
    test    qword [rbx + Stream.flags], ST_CLOSED
    jnz     .closed
    mov     rdi, r12
    mov     rsi, rbx
    cmp     qword [rbx + Stream.method], M_GET
    jne     .write
    call    get_execute
    jmp     .answered
.write:
    call    write_reply
.answered:
    test    eax, eax
    jz      .next
    mov     ebp, -1                     ; keep answering the rest; report at the end
    jmp     .next
.closed:
    mov     rdi, r12
    mov     rsi, rbx
    call    stream_unlink_free
.next:
    mov     rbx, r14
    test    rbx, rbx
    jnz     .answer
.out:
    mov     eax, ebp
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; batch_apply_writes — apply a detached batch's Puts and Deletes, in arrival
; order, under as few holds of the writer mutex as WRITE_HOLD_MAX allows.
; Internal.
;
; @param  rdi  Table*   @param esi  the worker's QSBR slot id (Worker.id)
; @param  rdx  first Stream* of the detached batch list
; @returns nothing. Each write's outcome is left in Stream.wres:
;          Put 1 created / 0 replaced / -ENOSPC (store full), Delete 1 / 0.
; @remarks Gets and streams the peer reset (ST_CLOSED) are skipped: a reset
;          write is not applied (gRPC cancellation is best effort either way,
;          and nobody waits for its answer). A published Put's record now
;          belongs to the store (Stream.wrec = 0); a refused one stays with
;          the Stream, which frees it. Nothing here frames a response or
;          calls into h2n: the mutex covers index updates only, so its
;          hold stays short. The records a hold replaces go to the worker's
;          slot, and kv_store_write_end retires and frees them there after
;          the unlock (ADR 0011). A batch longer than WRITE_HOLD_MAX writes
;          releases and retakes the mutex between chunks, so another worker's
;          writes wait for at most one chunk.
; =============================================================================
batch_apply_writes:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14                         ; 5 pushes: rsp 16-aligned for calls
    mov     r12, rdi                    ; Table*
    mov     r13d, esi                   ; slot id
    mov     rbx, rdx                    ; Stream*
    xor     ebp, ebp                    ; writes applied in the current hold
    mov     rdi, r12
    call    kv_store_write_begin wrt ..plt
.loop:
    test    rbx, rbx
    jz      .end
    mov     rax, [rbx + Stream.method]
    cmp     rax, M_GET
    je      .next
    test    qword [rbx + Stream.flags], ST_CLOSED
    jnz     .next                       ; reset by the peer: not applied
    cmp     rax, M_PUT
    jne     .delete
    mov     rdi, r12
    mov     esi, r13d
    mov     rdx, [rbx + Stream.wrec]
    mov     rcx, [rbx + Stream.h1]
    mov     r8, [rbx + Stream.h2]
    call    kv_store_put_locked wrt ..plt
    mov     [rbx + Stream.wres], rax
    test    rax, rax
    js      .counted                    ; refused: the record stays the Stream's
    mov     qword [rbx + Stream.wrec], 0    ; published: the store's now
    jmp     .counted
.delete:
    mov     rdi, r12
    mov     esi, r13d
    mov     rdx, [rbx + Stream.key]
    mov     rcx, [rbx + Stream.key_len]
    mov     r8, [rbx + Stream.h1]
    mov     r9, [rbx + Stream.h2]
    call    kv_store_delete_locked wrt ..plt
    mov     [rbx + Stream.wres], rax
.counted:
    inc     rbp
    cmp     rbp, WRITE_HOLD_MAX
    jb      .next
    ; Bound the hold: let other writers in (retiring and freeing this chunk's
    ; old records in the slot, outside the mutex), then retake it.
    mov     rdi, r12
    mov     esi, r13d
    call    kv_store_write_end wrt ..plt
    xor     ebp, ebp
    mov     rdi, r12
    call    kv_store_write_begin wrt ..plt
.next:
    mov     rbx, [rbx + Stream.batch_next]
    jmp     .loop
.end:
    mov     rdi, r12
    mov     esi, r13d
    call    kv_store_write_end wrt ..plt
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; write_reply — answer a batched Put or Delete from its Stream.wres. Internal.
;
; @param  rdi  Conn*   @param rsi  Stream* (its write was applied)
; @returns eax = 0, or -1 if the response could not be queued (out of memory
;          in h2n's output buffer).
; @remarks PutResponse{created} / DeleteResponse{deleted}: the bool field as
;          08 01, or the empty message for false — 5 or 7 bytes built on the
;          stack and framed by kv_h2n_reply_parts, exactly like dispatch's
;          immediate replies (no response buffer, no allocation). A Put the
;          full store refused (-ENOSPC) gets RESOURCE_EXHAUSTED; its record
;          is freed with the Stream. The Stream may be closed and freed
;          before this returns.
; =============================================================================
write_reply:
    push    rbx
    push    r12
    push    r13
    sub     rsp, 16                     ; [0..16) message; 3 pushes + 16: aligned
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     rax, [rbx + Stream.wres]
    cmp     rax, -ENOSPC
    je      .full
    xor     esi, esi                    ; protobuf payload length
    test    rax, rax
    jz      .head                       ; false: the empty message
    mov     byte [rsp + GRPC_PREFIX], 0x08      ; field 1, wire type varint
    mov     byte [rsp + GRPC_PREFIX + 1], 1     ; true
    mov     esi, 2
.head:
    lea     r13, [rsi + GRPC_PREFIX]    ; whole message length
    mov     rdi, rsp
    call    write_prefix
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, rsp
    mov     rcx, r13
    xor     r8d, r8d                    ; no body: the head is the message
    xor     r9d, r9d
    call    kv_h2n_reply_parts wrt ..plt
    jmp     .out
.full:
    mov     rax, [r12 + Conn.worker]
    inc     qword [rax + Worker.st_errors]
    mov     rdi, r12
    mov     rsi, rbx
    lea     rdx, [nv_e_full]
    mov     ecx, 4                      ; :status, content-type, grpc-status, grpc-message
    call    kv_h2n_reply_headers wrt ..plt
.out:
    test    eax, eax
    jz      .ret
    mov     eax, -1                     ; normalize, as dispatch does
.ret:
    add     rsp, 16
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; get_execute — look up a batched GET and frame its response. Internal.
; @param  rdi  Conn*   @param rsi  Stream* (key, h1, h2 set)
; @returns eax = 0, or -1 if the response could not be queued (out of
;          memory in h2n's output buffer).
; @remarks Response: GetResponse{found = true, value} as 08 01 12 <len>
;          <value> (the value field is omitted for an empty value); a miss is
;          the empty message (found = false). The message is head || value:
;          the head (gRPC prefix + 08 01 [12 <varint len>], at most 13 bytes)
;          is built on the stack, the value stays in the store record and is
;          copied once by h2n — straight into the output buffer's DATA frame
;          when the windows allow — so a GET allocates nothing. The Stream
;          may be closed and freed before this returns.
; =============================================================================
get_execute:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r15
    sub     rsp, 32                     ; [0..32) message head; 5 pushes + 32: aligned
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     rax, [r12 + Conn.worker]
    mov     rsi, [rbx + Stream.key]
    mov     rdx, [rbx + Stream.key_len]
    mov     rcx, [rbx + Stream.h1]
    mov     r8, [rbx + Stream.h2]
    ; Through the worker's private L1/L2 cache when one is configured (same
    ; contract, table on a miss), else straight to the shared table.
    mov     rdi, [rax + Worker.hcache]
    test    rdi, rdi
    jz      .table
    call    kv_hcache_get wrt ..plt
    jmp     .looked_up
.table:
    ; Through the buckets located at dispatch (the same table: a Stream never
    ; changes workers, and a worker has one table).
    mov     rdi, [rbx + Stream.b1]
    mov     rsi, [rbx + Stream.b2]
    mov     rdx, [rbx + Stream.key]
    mov     rcx, [rbx + Stream.key_len]
    call    kv_store_get_at wrt ..plt   ; r8 = h2, loaded above
.looked_up:
    test    rax, rax
    jz      .missing
    mov     r15, rax                    ; Rec* (valid while this worker is online)
    mov     ebp, [r15 + Rec.val_len]
    ; Values above KVASM_MAX_VALUE_BYTES exist only through Upload (whose
    ; limit, KVASM_MAX_UPLOAD_BYTES, may be larger). A unary answer would be
    ; the whole value as one message — copied into a response buffer when
    ; the windows are short, a memory hazard for a 2 GiB value, and beyond
    ; what clients accept — so it is refused, pointing to Download.
    mov     rcx, [r12 + Conn.worker]
    mov     rcx, [rcx + Worker.cfg]
    cmp     rbp, [rcx + Config.max_value]
    ja      .too_big
    mov     word [rsp + GRPC_PREFIX], 0x0108   ; field 1 (found) = true
    mov     r13d, 2
    test    rbp, rbp
    jz      .head                       ; empty value: the field is omitted
    mov     byte [rsp + GRPC_PREFIX + 2], 0x12 ; field 2 (value), length-delimited
    lea     rdi, [rsp + GRPC_PREFIX + 3]
    mov     rsi, rbp
    call    kv_pb_write_varint wrt ..plt
    lea     r13, [rax + 3]              ; 08 01 12 <varint>
.head:
    mov     rdi, rsp
    lea     rsi, [r13 + rbp]            ; protobuf payload length
    call    write_prefix
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, rsp
    lea     rcx, [r13 + GRPC_PREFIX]
    mov     eax, [r15 + Rec.key_len]
    add     rax, 7
    and     rax, -8
    lea     r8, [r15 + Rec.data + rax]  ; value follows the padded key
    mov     r9, rbp
    call    kv_h2n_reply_parts wrt ..plt
    jmp     .out
.missing:
    mov     rdi, rsp
    xor     esi, esi
    call    write_prefix                ; 00 00 00 00 00: GetResponse{}
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, rsp
    mov     ecx, GRPC_PREFIX
    xor     r8d, r8d
    xor     r9d, r9d
    call    kv_h2n_reply_parts wrt ..plt
    jmp     .out
.too_big:
    mov     rax, [r12 + Conn.worker]
    inc     qword [rax + Worker.st_errors]
    mov     rdi, r12
    mov     rsi, rbx
    lea     rdx, [nv_e_bigget]
    mov     ecx, 4
    call    kv_h2n_reply_headers wrt ..plt
.out:
    add     rsp, 32
    pop     r15
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; write_prefix — gRPC message prefix: flag 0 + big-endian 32-bit length.
; Internal, leaf.
; @param  rdi  buffer (5 bytes writable)   @param rsi  payload length (< 2^32)
; @returns nothing. Clobbers eax.
; =============================================================================
write_prefix:
    mov     byte [rdi], 0
    mov     eax, esi
    bswap   eax
    mov     [rdi + 1], eax
    ret

; =============================================================================
; xfer_release — release a streaming call's transfer state. Internal.
; @param  rdi  Stream* (xfer != 0; its connection and worker still alive)
; @returns nothing.
; @remarks Download: drops the pin (the last unpin of a record replaced or
;          deleted meanwhile frees it). Upload: frees the unpublished record
;          — returning its memory reservation — and the key buffer, and gives
;          the declared size back to the connection's upload budget. A
;          published upload has rec = 0 and reserved = 0 (upload_finish
;          cleared them), so nothing is released twice.
; =============================================================================
xfer_release:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14                         ; 5 pushes: rsp 16-aligned for calls
    mov     rbx, rdi                    ; Stream*
    mov     rbp, [rbx + Stream.xfer]
    mov     r12, [rbx + Stream.conn]
    mov     rax, [r12 + Conn.worker]
    mov     r13, [rax + Worker.table]
    lea     r14, [rax + Worker.pool]
    mov     rsi, [rbp + Xfer.rec]
    cmp     qword [rbp + Xfer.kind], XF_DOWNLOAD
    jne     .upload
    test    rsi, rsi
    jz      .free
    mov     rdi, r13
    call    kv_store_unpin wrt ..plt
    jmp     .free
.upload:
    mov     rdi, r13
    call    kv_store_rec_free wrt ..plt ; NULL (published / never allocated): no-op
    mov     rdi, [rbp + Xfer.key]
    mov     rsi, r14
    call    kv_pool_free                ; NULL is ignored
    mov     rax, [rbp + Xfer.reserved]
    sub     [r12 + Conn.up_bytes], rax
.free:
    mov     rdi, rbp
    mov     rsi, r14
    call    kv_pool_free
    mov     qword [rbx + Stream.xfer], 0
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; refuse — answer a call with a Trailers-Only gRPC error. Internal.
; @param  rdi  Conn*   @param rsi  Stream*   @param rdx  HdrField array
; @param  ecx  field count
; @returns eax = 0, or -1 when out of memory (the caller ends the connection).
; @remarks Counts the error. The stream is closed and freed by the answer; a
;          request the peer is still sending (an Upload) also gets
;          RST_STREAM(NO_ERROR) from h2n, which stops the rest of its body.
; =============================================================================
refuse:
    mov     rax, [rdi + Conn.worker]
    inc     qword [rax + Worker.st_errors]
    jmp     kv_h2n_reply_headers wrt ..plt ; tail call, same arguments

; =============================================================================
; upload_refuse — answer an Upload with the status of a refusal code.
; Internal.
; @param  rdi  Conn*   @param rsi  Stream*   @param edx  PB_ERR_* or UE_* code
; @returns eax = 0, or -1 for out of memory — including UE_NOMEM, which is
;          not answered: running out of memory ends the connection, as
;          everywhere else in the server.
; @remarks The mapping is the documented contract (docs/PROTOCOL.md):
;          malformed protobuf and every size/order/checksum rule are
;          INVALID_ARGUMENT, except a checksum mismatch (DATA_LOSS), a full
;          store, memory or budget (RESOURCE_EXHAUSTED) and message framing
;          (INTERNAL; compression follows the unary rules).
; =============================================================================
upload_refuse:
    mov     ecx, 4
    lea     rax, [nv_e_malformed]
    cmp     edx, PB_ERR_MALFORMED
    je      .send
    cmp     edx, PB_ERR_COMPRESSED
    jne     .not_compressed
    lea     rax, [nv_e_badflag]         ; flag 1 without a codec: the sender contradicts itself
    test    qword [rsi + Stream.flags], ST_ENCODED
    jz      .send
    lea     rax, [nv_e_compress]        ; a codec we do not speak: say which we do
    mov     ecx, 5
    jmp     .send
.not_compressed:
    lea     rax, [nv_e_upflag]
    cmp     edx, PB_ERR_FLAG
    je      .send
    lea     rax, [nv_e_upfirst]
    cmp     edx, UE_FIRST_ONLY
    je      .send
    lea     rax, [nv_e_uporder]
    cmp     edx, UE_ORDER
    je      .send
    lea     rax, [nv_e_upmissing]
    cmp     edx, UE_FIRST_MISSING
    je      .send
    lea     rax, [nv_e_key]
    cmp     edx, UE_KEY
    je      .send
    lea     rax, [nv_e_value]
    cmp     edx, UE_TOO_LARGE
    je      .send
    lea     rax, [nv_e_upbudget]
    cmp     edx, UE_BUDGET
    je      .send
    lea     rax, [nv_e_memory]
    cmp     edx, UE_MEMORY
    je      .send
    lea     rax, [nv_e_upover]
    cmp     edx, UE_OVERFLOW
    je      .send
    lea     rax, [nv_e_upcrc2]
    cmp     edx, UE_CRC_TWICE
    je      .send
    lea     rax, [nv_e_upshort]
    cmp     edx, UE_SHORT
    je      .send
    lea     rax, [nv_e_uptrunc]
    cmp     edx, UE_TRUNCATED
    je      .send
    lea     rax, [nv_e_upcrc]
    cmp     edx, UE_CRC
    je      .send
    lea     rax, [nv_e_full]
    cmp     edx, UE_FULL
    je      .send
    mov     eax, -1                     ; UE_NOMEM (or an unknown code): fatal
    ret
.send:
    mov     rdx, rax
    jmp     refuse                      ; tail call: rdi, rsi kept, ecx = count

; =============================================================================
; kv_grpc_upload_data — take the next DATA payload of an Upload call.
;
; @param  rdi  Conn*   @param rsi  Stream* (route M_UPLOAD)
; @param  rdx  data    @param rcx  length (> 0)
; @returns eax = 0 (consumed; the stream lives on), 1 (answered early — an
;          error: the stream may already be closed and freed, the caller must
;          not touch it), or -1 (out of memory: the connection ends).
; @remarks Called by h2n.asm for every non-empty DATA payload of an upload,
;          as it arrives (after flow-control accounting). The first one
;          applies dispatch's HTTP-level checks (POST, content-type) so a
;          non-gRPC upload is refused before its body travels, and creates
;          the Xfer. Every payload goes through kv_pb_stream_feed, whose
;          events upload_event applies; the bytes of data fields land in the
;          record directly — this is the one copy of the value. A refusal is
;          answered at once: Trailers-Only error plus RST_STREAM(NO_ERROR)
;          (h2n), so the client stops sending; the partial record is freed
;          with the stream. Data after an early answer (a 405/415 body still
;          waiting for window) is dropped.
; =============================================================================
kv_grpc_upload_data:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    sub     rsp, 16                     ; 5 pushes + 16: rsp 16-aligned for calls
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     r13, rdx                    ; data
    mov     r14, rcx                    ; length
    test    qword [rbx + Stream.flags], ST_RESPONDED
    jnz     .ignore                     ; answered already: nobody reads the rest
    mov     rbp, [rbx + Stream.xfer]
    test    rbp, rbp
    jnz     .feed
    ; The call's first DATA: dispatch's HTTP-level checks, now.
    mov     rax, [rbx + Stream.flags]
    test    rax, ST_POST
    jz      .http_405
    test    rax, ST_CT_OK
    jz      .http_415
    mov     edi, 1
    mov     esi, Xfer_size
    mov     rdx, [r12 + Conn.worker]
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_calloc wrt ..plt    ; zeroed: an empty decoder, no flags
    test    rax, rax
    jz      .fatal
    mov     rbp, rax
    mov     [rbx + Stream.xfer], rax
    mov     qword [rbp + Xfer.kind], XF_UPLOAD
.feed:
    lea     rdi, [rbp + Xfer.ps]
    mov     rsi, r13
    mov     rdx, r14
    mov     rcx, rbx                    ; ctx = Stream*
    lea     r8, [upload_event]
    call    kv_pb_stream_feed wrt ..plt
    test    rax, rax
    jnz     .refused
.ignore:
    xor     eax, eax
    jmp     .out
.refused:
    cmp     rax, UE_NOMEM
    je      .fatal
    ; Refused early: count the call here — dispatch never sees it, since the
    ; stream closes now.
    mov     rcx, [r12 + Conn.worker]
    inc     qword [rcx + Worker.st_requests]
    or      qword [rbx + Stream.flags], ST_RESPONDED
    mov     rdi, r12
    mov     rsi, rbx
    mov     edx, eax
    call    upload_refuse
    test    eax, eax
    jnz     .fatal
    mov     eax, 1                      ; closed and freed
    jmp     .out
.http_405:
    lea     rax, [b_405]
    mov     ecx, b_405_len
    lea     rdx, [nv_405]
    jmp     .http_error
.http_415:
    lea     rax, [b_415]
    mov     ecx, b_415_len
    lea     rdx, [nv_415]
.http_error:
    ; As dispatch answers it: 3 headers + a static text body that ends the
    ; stream (ST_STATIC_RESP). The body may wait for window, so the stream can
    ; outlive this call — ST_RESPONDED makes its later DATA be ignored, and h2n
    ; resets the rest of the upload once the body is out.
    mov     r8, [r12 + Conn.worker]
    inc     qword [r8 + Worker.st_requests]
    or      qword [rbx + Stream.flags], ST_RESPONDED | ST_STATIC_RESP
    mov     [rbx + Stream.resp_buf], rax
    mov     [rbx + Stream.resp_len], rcx
    mov     qword [rbx + Stream.resp_off], 0
    mov     rdi, r12
    mov     rsi, rbx
    mov     ecx, 3                      ; rdx = the 405 or 415 fields
    call    kv_h2n_reply_static wrt ..plt
    test    eax, eax
    jnz     .fatal
    mov     eax, 1                      ; possibly closed and freed already
    jmp     .out
.fatal:
    mov     eax, -1
.out:
    add     rsp, 16
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; upload_event — kv_pb_stream_feed's handler for an Upload: applies one
; decoder event to the call's Xfer. Internal.
;
; @param  rdi  ctx = Stream* (Stream.xfer set)
; @param  esi  PBE_* event   @param rdx  tag
; @param  rcx  a1 (length, value or pointer)   @param r8  a2 (piece length)
; @returns eax = 0 to continue, or a UE_* code that stops the decoding.
; @remarks The contract, rule by rule:
;          * key and total_size are allowed in the first message only
;            (UE_FIRST_ONLY) and before any data (UE_ORDER); within the first
;            message a repeated one replaces the earlier (protobuf's last-wins);
;          * the record is reserved when the first data field starts, or at
;            the end of the first message (ensure_record: size, budget and
;            memory limits);
;          * data bytes go straight into the record, CRC-32C computed in the
;            same pass; more than total_size is UE_OVERFLOW, at once;
;          * crc32c may come in any message, once (UE_CRC_TWICE);
;          * unknown fields and known numbers of another wire type are
;            skipped (forward compatibility, as in every KvAsm decoder).
;          Each event is O(1) plus the bytes it carries.
; =============================================================================
upload_event:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14                         ; 5 pushes: rsp 16-aligned for calls
    mov     rbx, rdi                    ; Stream*
    mov     rbp, [rbx + Stream.xfer]
    mov     r12, [rbx + Stream.conn]
    mov     r13, rcx                    ; a1
    mov     r14, r8                     ; a2
    cmp     esi, PBE_BYTES
    je      .bytes                      ; the hot event: pieces of data
    cmp     esi, PBE_MSG_BEGIN
    je      .msg_begin
    cmp     esi, PBE_BYTES_BEGIN
    je      .bytes_begin
    cmp     esi, PBE_BYTES_END
    je      .bytes_end
    cmp     esi, PBE_VARINT
    je      .varint
    cmp     esi, PBE_FIXED32
    je      .fixed32
    cmp     esi, PBE_MSG_END
    je      .msg_end
    jmp     .ok                         ; PBE_FIXED64: nothing defined
.msg_begin:
    inc     qword [rbp + Xfer.msgs]
    jmp     .ok
.varint:
    cmp     rdx, UP_TAG_SIZE
    jne     .ok
    cmp     qword [rbp + Xfer.msgs], 1
    jne     .first_only
    cmp     qword [rbp + Xfer.rec], 0
    jne     .order                      ; the record already has its size
    mov     [rbp + Xfer.size], r13
    or      qword [rbp + Xfer.flags], XFF_HAVE_SIZE
    jmp     .ok
.fixed32:
    cmp     rdx, UP_TAG_CRC
    jne     .ok
    test    qword [rbp + Xfer.flags], XFF_HAVE_CRC
    jnz     .crc_twice
    mov     [rbp + Xfer.want_crc], r13
    or      qword [rbp + Xfer.flags], XFF_HAVE_CRC
    jmp     .ok
.bytes_begin:
    mov     qword [rbp + Xfer.field], XFD_SKIP
    cmp     rdx, UP_TAG_DATA
    je      .data_begin
    cmp     rdx, UP_TAG_KEY
    jne     .ok                         ; an unknown bytes field: skipped
    cmp     qword [rbp + Xfer.msgs], 1
    jne     .first_only
    cmp     qword [rbp + Xfer.rec], 0
    jne     .order
    test    r13, r13
    jz      .bad_key
    mov     rax, [r12 + Conn.worker]
    mov     rax, [rax + Worker.cfg]
    cmp     r13, [rax + Config.max_key]
    ja      .bad_key
    ; Last wins: a repeated key in the first message replaces the earlier one.
    mov     rdi, [rbp + Xfer.key]
    mov     rsi, [r12 + Conn.worker]
    lea     rsi, [rsi + Worker.pool]
    call    kv_pool_free                ; NULL is ignored
    mov     qword [rbp + Xfer.key], 0
    and     qword [rbp + Xfer.flags], ~XFF_HAVE_KEY
    mov     rdi, r13
    mov     rsi, [r12 + Conn.worker]
    lea     rsi, [rsi + Worker.pool]
    call    kv_pool_malloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbp + Xfer.key], rax
    mov     [rbp + Xfer.key_len], r13
    mov     qword [rbp + Xfer.key_off], 0
    mov     qword [rbp + Xfer.field], XFD_KEY
    jmp     .ok
.data_begin:
    cmp     qword [rbp + Xfer.rec], 0
    jne     .data_ready
    mov     rax, [rbp + Xfer.flags]
    and     eax, XFF_HAVE_KEY | XFF_HAVE_SIZE
    cmp     eax, XFF_HAVE_KEY | XFF_HAVE_SIZE
    jne     .order                      ; data before key and total_size
    mov     rdi, rbx
    call    ensure_record
    test    eax, eax
    jnz     .out                        ; UE_TOO_LARGE / UE_BUDGET / UE_MEMORY
.data_ready:
    mov     qword [rbp + Xfer.field], XFD_DATA
    jmp     .ok
.bytes:
    mov     rax, [rbp + Xfer.field]
    cmp     eax, XFD_DATA
    je      .data
    cmp     eax, XFD_KEY
    jne     .ok                         ; a skipped field's bytes
    mov     rdi, [rbp + Xfer.key]
    add     rdi, [rbp + Xfer.key_off]
    mov     rsi, r13
    mov     rdx, r14
    call    memcpy wrt ..plt
    add     [rbp + Xfer.key_off], r14
    jmp     .ok
.data:
    ; The value's bytes: straight into the record, checksummed in the same
    ; pass — with non-temporal stores for a large record (UPLOAD_NT_MIN).
    mov     rax, [rbp + Xfer.off]
    add     rax, r14
    cmp     rax, [rbp + Xfer.size]
    ja      .overflow                   ; refused before a byte lands past the end
    mov     edi, [rbp + Xfer.crc]
    mov     rsi, [rbp + Xfer.val]
    add     rsi, [rbp + Xfer.off]
    mov     rdx, r13
    mov     rcx, r14
    cmp     qword [rbp + Xfer.size], UPLOAD_NT_MIN
    jae     .data_nt
    call    kv_crc32c_copy wrt ..plt
    jmp     .data_done
.data_nt:
    ; Every NT store is drained (sfence) before this returns, so the
    ; publish that follows the last chunk orders after all of them.
    call    kv_crc32c_copy_nt wrt ..plt
.data_done:
    mov     [rbp + Xfer.crc], rax
    add     [rbp + Xfer.off], r14
    jmp     .ok
.bytes_end:
    cmp     qword [rbp + Xfer.field], XFD_KEY
    jne     .field_none
    or      qword [rbp + Xfer.flags], XFF_HAVE_KEY
    ; Hash it now: the key buffer goes away once the record holds the key.
    mov     rdi, [r12 + Conn.worker]
    mov     rdi, [rdi + Worker.table]
    mov     rsi, [rbp + Xfer.key]
    mov     rdx, [rbp + Xfer.key_len]
    lea     rcx, [rbp + Xfer.h1]        ; h1 then h2
    call    kv_store_hash wrt ..plt
.field_none:
    mov     qword [rbp + Xfer.field], XFD_NONE
    jmp     .ok
.msg_end:
    cmp     qword [rbp + Xfer.msgs], 1
    jne     .ok
    ; The first message is complete: it must have carried key and total_size.
    mov     rax, [rbp + Xfer.flags]
    and     eax, XFF_HAVE_KEY | XFF_HAVE_SIZE
    cmp     eax, XFF_HAVE_KEY | XFF_HAVE_SIZE
    jne     .first_missing
    mov     rdi, rbx
    call    ensure_record
    test    eax, eax
    jnz     .out
    or      qword [rbp + Xfer.flags], XFF_FIRST_DONE
.ok:
    xor     eax, eax
    jmp     .out
.first_only:
    mov     eax, UE_FIRST_ONLY
    jmp     .out
.order:
    mov     eax, UE_ORDER
    jmp     .out
.first_missing:
    mov     eax, UE_FIRST_MISSING
    jmp     .out
.bad_key:
    mov     eax, UE_KEY
    jmp     .out
.crc_twice:
    mov     eax, UE_CRC_TWICE
    jmp     .out
.overflow:
    mov     eax, UE_OVERFLOW
    jmp     .out
.nomem:
    mov     eax, UE_NOMEM
.out:
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; ensure_record — reserve an Upload's record once key and total_size are
; known. Internal.
; @param  rdi  Stream* (Xfer with key, key_len, size, h1/h2 set)
; @returns eax = 0 (the record exists), or UE_TOO_LARGE (total_size above
;          KVASM_MAX_UPLOAD_BYTES), UE_BUDGET (the connection's uploads in
;          progress would exceed KVASM_CONN_BUFFER_MB), UE_MEMORY (the
;          store's memory limit, or malloc, refused).
; @remarks Idempotent. The declared size is reserved twice on purpose: in
;          the store's memory accounting (kv_store_rec_alloc), which bounds
;          the whole server, and in Conn.up_bytes, which bounds what one
;          connection can hold by declaring uploads it never completes. The
;          key buffer is freed here: the record holds the key now, and the
;          hash was taken when the key arrived.
; =============================================================================
ensure_record:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14                         ; 5 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rbp, [rbx + Stream.xfer]
    cmp     qword [rbp + Xfer.rec], 0
    jne     .ok
    mov     r12, [rbx + Stream.conn]
    mov     r13, [r12 + Conn.worker]
    mov     rax, [r13 + Worker.cfg]
    mov     r14, [rbp + Xfer.size]
    cmp     r14, [rax + Config.max_upload]
    ja      .too_large
    mov     rcx, [r12 + Conn.up_bytes]
    add     rcx, r14
    cmp     rcx, [rax + Config.conn_budget]
    ja      .budget
    mov     rdi, [r13 + Worker.table]
    mov     rsi, [rbp + Xfer.key]
    mov     rdx, [rbp + Xfer.key_len]
    mov     rcx, r14
    call    kv_store_rec_alloc wrt ..plt
    cmp     rax, -4095
    jae     .alloc_failed               ; -E2BIG / -ENOMEM
    mov     [rbp + Xfer.rec], rax
    mov     ecx, [rax + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    lea     rcx, [rax + Rec.data + rcx] ; the value follows the padded key
    mov     [rbp + Xfer.val], rcx
    add     [r12 + Conn.up_bytes], r14
    mov     [rbp + Xfer.reserved], r14
    mov     rdi, [rbp + Xfer.key]
    lea     rsi, [r13 + Worker.pool]
    call    kv_pool_free
    mov     qword [rbp + Xfer.key], 0
.ok:
    xor     eax, eax
    jmp     .out
.too_large:
    mov     eax, UE_TOO_LARGE
    jmp     .out
.budget:
    mov     eax, UE_BUDGET
    jmp     .out
.alloc_failed:
    mov     ecx, UE_MEMORY
    mov     edx, UE_TOO_LARGE
    cmp     rax, -E2BIG
    cmove   ecx, edx
    mov     eax, ecx
.out:
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; upload_finish — the peer ended an Upload: verify, publish, answer.
; Internal (from kv_grpc_dispatch at END_STREAM).
; @param  rdi  Conn*   @param rsi  Stream*
; @returns eax = 0, or -1 when out of memory.
; @remarks Checks, in order: a first message arrived (UE_FIRST_MISSING —
;          also a call with no DATA at all), the stream ended between
;          messages (UE_TRUNCATED), exactly total_size bytes arrived
;          (UE_SHORT), and the client's crc32c, if sent, matches (UE_CRC).
;          Only then is the record published — one pointer store, so readers
;          see the old or the new value, never a part. A full store
;          (UE_FULL) leaves the record with the stream, which frees it.
;          Answer: UploadResponse{created, crc32c} — the checksum of what was
;          stored, so a client that sent none can still verify.
; =============================================================================
upload_finish:
    push    rbx
    push    rbp
    push    r12
    push    r13
    sub     rsp, 24                     ; [0..16) the reply; 4 pushes + 24: aligned
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     rbp, [rbx + Stream.xfer]
    mov     edx, UE_FIRST_MISSING
    test    rbp, rbp
    jz      .refuse                     ; no DATA at all
    lea     rdi, [rbp + Xfer.ps]
    call    kv_pb_stream_idle wrt ..plt
    mov     edx, UE_TRUNCATED
    test    eax, eax
    jz      .refuse
    mov     edx, UE_FIRST_MISSING
    test    qword [rbp + Xfer.flags], XFF_FIRST_DONE
    jz      .refuse                     ; not one complete message
    mov     edx, UE_SHORT
    mov     rax, [rbp + Xfer.off]
    cmp     rax, [rbp + Xfer.size]
    jne     .refuse
    test    qword [rbp + Xfer.flags], XFF_HAVE_CRC
    jz      .publish
    mov     edx, UE_CRC
    mov     eax, [rbp + Xfer.crc]
    cmp     eax, [rbp + Xfer.want_crc]
    jne     .refuse
.publish:
    mov     rax, [r12 + Conn.worker]
    mov     rdi, [rax + Worker.table]
    mov     rsi, [rbp + Xfer.rec]
    mov     rdx, [rbp + Xfer.h1]
    mov     rcx, [rbp + Xfer.h2]
    call    kv_store_publish wrt ..plt
    mov     edx, UE_FULL
    test    rax, rax
    js      .refuse                     ; -ENOSPC: the record is still ours
    ; Published: the store owns the record, and its reservation became a
    ; record's accounted bytes — nothing left for xfer_release to free.
    mov     r13, rax                    ; 1 created, 0 replaced
    mov     qword [rbp + Xfer.rec], 0
    mov     rax, [rbp + Xfer.reserved]
    sub     [r12 + Conn.up_bytes], rax
    mov     qword [rbp + Xfer.reserved], 0
    ; UploadResponse: created (field 1, omitted when false), crc32c (field 2,
    ; fixed32, omitted when 0) — canonical proto3, built on the stack.
    xor     ecx, ecx
    test    r13, r13
    jz      .crc_field
    mov     word [rsp + GRPC_PREFIX], 0x0108
    mov     ecx, 2
.crc_field:
    mov     eax, [rbp + Xfer.crc]
    test    eax, eax
    jz      .reply
    mov     byte [rsp + GRPC_PREFIX + rcx], 0x15
    mov     [rsp + GRPC_PREFIX + rcx + 1], eax  ; little-endian, as fixed32 is
    add     ecx, 5
.reply:
    mov     r13d, ecx                   ; protobuf length
    mov     rdi, rsp
    mov     esi, ecx
    call    write_prefix
    mov     rdi, r12
    mov     rsi, rbx
    mov     rdx, rsp
    lea     ecx, [r13 + GRPC_PREFIX]
    xor     r8d, r8d
    xor     r9d, r9d
    call    kv_h2n_reply_parts wrt ..plt
    jmp     .out
.refuse:
    mov     rdi, r12
    mov     rsi, rbx
    call    upload_refuse               ; edx = the code
.out:
    add     rsp, 24
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; download_start — look a Download's key up, pin its record, start the
; generated response. Internal (from kv_grpc_dispatch).
; @param  rdi  Conn*   @param rsi  Stream*
; @param  rdx  PbDl* (decoded; key present and within limits)
; @returns eax = 0, or -1 when out of memory (pin table, Xfer, output).
; @remarks The lookup runs while the worker is QSBR-online and the pin is
;          taken before its next quiescent state, which is what makes the
;          record safe to read for the rest of the call (kv_store_pin). The
;          range: offset beyond the value is OUT_OF_RANGE; a length reaching
;          past the end is cut at the end (an empty range is fine: one
;          message with total_size and crc32c). chunk_size 0 means 64 KiB;
;          larger than 1 MiB is reduced to 1 MiB. A missing key is NOT_FOUND.
; =============================================================================
download_start:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24                     ; [0] h1, [8] h2, [16] end; aligned
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
    mov     r15, rdx                    ; PbDl*
    mov     rax, [r12 + Conn.worker]
    mov     r13, [rax + Worker.table]
    mov     rdi, r13
    mov     rsi, [r15 + PbDl.key]
    mov     rdx, [r15 + PbDl.key_len]
    mov     rcx, rsp
    call    kv_store_hash wrt ..plt
    mov     rdi, r13
    mov     rsi, [r15 + PbDl.key]
    mov     rdx, [r15 + PbDl.key_len]
    mov     rcx, [rsp]
    mov     r8, [rsp + 8]
    call    kv_store_get wrt ..plt
    test    rax, rax
    jz      .not_found
    mov     r14, rax                    ; Rec*
    mov     ebp, [r14 + Rec.val_len]    ; the value's size
    mov     rax, [r15 + PbDl.offset]
    cmp     rax, rbp
    ja      .out_of_range
    mov     rcx, rbp                    ; end = size unless a shorter length was sent
    cmp     dword [r15 + PbDl.has_length], 0
    je      .have_end
    mov     rdx, rbp
    sub     rdx, rax                    ; bytes from offset to the end
    cmp     [r15 + PbDl.length], rdx
    jae     .have_end
    mov     rcx, rax
    add     rcx, [r15 + PbDl.length]
.have_end:
    mov     [rsp + 16], rcx
    mov     rdi, r13
    mov     rsi, r14
    call    kv_store_pin wrt ..plt
    test    rax, rax
    jnz     .fatal                      ; the pin table could not grow
    mov     edi, 1
    mov     esi, Xfer_size
    mov     rdx, [r12 + Conn.worker]
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_calloc wrt ..plt
    test    rax, rax
    jz      .unpin_fatal
    mov     [rbx + Stream.xfer], rax    ; from here on stream_free unpins
    mov     qword [rax + Xfer.kind], XF_DOWNLOAD
    mov     [rax + Xfer.rec], r14
    mov     ecx, [r14 + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    lea     rcx, [r14 + Rec.data + rcx] ; the value follows the padded key
    mov     [rax + Xfer.val], rcx
    mov     [rax + Xfer.size], rbp
    mov     rcx, [r15 + PbDl.offset]
    mov     [rax + Xfer.off], rcx
    mov     rcx, [rsp + 16]
    mov     [rax + Xfer.end], rcx
    mov     rcx, [r15 + PbDl.chunk]
    mov     edx, DL_CHUNK_DEFAULT
    test    rcx, rcx
    cmovz   rcx, rdx
    mov     edx, DL_CHUNK_MAX
    cmp     rcx, rdx
    cmova   rcx, rdx
    mov     [rax + Xfer.chunk], rcx
    mov     qword [rax + Xfer.first], 1
    mov     rdi, rax
    call    download_body_len
    mov     [rbx + Stream.resp_len], rax
    mov     qword [rbx + Stream.resp_off], 0
    or      qword [rbx + Stream.flags], ST_GEN
    mov     rdi, r12
    mov     rsi, rbx
    call    kv_h2n_reply_stream wrt ..plt
    jmp     .out
.not_found:
    mov     rdi, r12
    mov     rsi, rbx
    lea     rdx, [nv_e_notfound]
    mov     ecx, 4
    call    refuse
    jmp     .out
.out_of_range:
    mov     rdi, r12
    mov     rsi, rbx
    lea     rdx, [nv_e_range]
    mov     ecx, 4
    call    refuse
    jmp     .out
.unpin_fatal:
    mov     rdi, r13
    mov     rsi, r14
    call    kv_store_unpin wrt ..plt
.fatal:
    mov     eax, -1
.out:
    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; download_body_len — the exact byte length of a Download's response body.
; Internal.
; @param  rdi  Xfer* (size, off = the range start, end, chunk set)
; @returns rax = the sum over every response message of its 5-byte prefix
;          and protobuf bytes.
; @remarks Must equal what kv_grpc_gen_fill produces, byte for byte: h2n
;          frames exactly this many bytes and then sends the trailers. The
;          messages: k = max(1, ceil(len / chunk)); the first carries
;          total_size (1 + varint), each non-empty one a data field (1 +
;          varint(n) + n), the last the crc32c field (5):
;            5k + 1 + vlen(size) + 5 + len
;               + (k - 1)(1 + vlen(chunk)) + 1 + vlen(last)   when len > 0
;          The end-to-end tests parse every message of every download they
;          make, so a disagreement here fails them.
; =============================================================================
download_body_len:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15                         ; 5 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12, [rbx + Xfer.end]
    sub     r12, [rbx + Xfer.off]       ; len: bytes in the range
    mov     r13, [rbx + Xfer.chunk]
    mov     eax, 1
    test    r12, r12
    jz      .have_k
    lea     rax, [r12 + r13 - 1]
    xor     edx, edx
    div     r13                         ; ceil(len / chunk)
.have_k:
    mov     r14, rax                    ; k messages
    lea     r15, [rax + rax * 4]        ; 5k prefixes
    add     r15, 6                      ; total_size tag + the 5-byte crc32c field
    add     r15, r12                    ; the data itself
    mov     rdi, [rbx + Xfer.size]
    call    kv_pb_varint_len wrt ..plt
    add     r15, rax                    ; total_size's varint
    test    r12, r12
    jz      .done                       ; an empty range: no data fields
    mov     rdi, r13
    call    kv_pb_varint_len wrt ..plt
    inc     rax                         ; a full chunk's data field header
    lea     rcx, [r14 - 1]
    imul    rax, rcx
    add     r15, rax
    lea     rcx, [r14 - 1]
    imul    rcx, r13
    mov     rdi, r12
    sub     rdi, rcx                    ; the last message's data (1 .. chunk)
    call    kv_pb_varint_len wrt ..plt
    inc     rax
    add     r15, rax
.done:
    mov     rax, r15
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_grpc_gen_fill — produce the next bytes of a Download's response body.
;
; @param  rdi  Stream* (ST_GEN; Xfer of kind XF_DOWNLOAD)
; @param  rsi  destination (inside the DATA frame h2n is writing)
; @param  rdx  n: bytes to produce (h2n never asks past resp_len)
; @returns nothing; exactly n bytes are written.
; @remarks The body is a sequence of messages, each [head][data][tail]:
;          head = 5-byte prefix + (first message) total_size + (non-empty)
;          the data field's tag and length; data = the value's bytes, copied
;          from the pinned record and checksummed in the same pass
;          (kv_crc32c_copy); tail = (last message) the crc32c field, built
;          only once the last data byte is out, so the checksum covers them
;          all. A frame boundary may fall anywhere, even inside a head or
;          tail: the state lives in the Xfer. Never allocates, never fails.
; =============================================================================
kv_grpc_gen_fill:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rbp, [rbx + Stream.xfer]
    mov     r12, rsi                    ; destination cursor
    mov     r13, rdx                    ; bytes still to produce
.loop:
    test    r13, r13
    jz      .done
    ; 1. the rest of the current message's head
    mov     rcx, [rbp + Xfer.hlen]
    sub     rcx, [rbp + Xfer.hoff]
    jz      .data
    cmp     rcx, r13
    cmova   rcx, r13
    lea     rsi, [rbp + Xfer.hbuf]
    add     rsi, [rbp + Xfer.hoff]
    add     [rbp + Xfer.hoff], rcx
    sub     r13, rcx
.hcopy:                                 ; a head is at most 27 bytes
    movzx   eax, byte [rsi]
    mov     [r12], al
    inc     rsi
    inc     r12
    dec     rcx
    jnz     .hcopy
    jmp     .loop
.data:
    ; 2. value bytes: the record into the frame, checksummed
    mov     r14, [rbp + Xfer.mrem]
    test    r14, r14
    jz      .tail
    cmp     r14, r13
    cmova   r14, r13
    mov     edi, [rbp + Xfer.crc]
    mov     rsi, r12
    mov     rdx, [rbp + Xfer.val]
    add     rdx, [rbp + Xfer.off]
    mov     rcx, r14
    call    kv_crc32c_copy wrt ..plt
    mov     [rbp + Xfer.crc], rax
    add     [rbp + Xfer.off], r14
    sub     [rbp + Xfer.mrem], r14
    add     r12, r14
    sub     r13, r14
    jmp     .loop
.tail:
    ; 3. the last message's crc32c field, built once its data is all out
    cmp     qword [rbp + Xfer.last], 0
    je      .next_msg
    cmp     qword [rbp + Xfer.tlen], 0
    jne     .tail_copy
    mov     byte [rbp + Xfer.tbuf], 0x1D        ; field 3, wire type 5 (fixed32)
    mov     eax, [rbp + Xfer.crc]
    mov     [rbp + Xfer.tbuf + 1], eax          ; little-endian
    mov     qword [rbp + Xfer.tlen], 5
    mov     qword [rbp + Xfer.toff], 0
.tail_copy:
    mov     rcx, [rbp + Xfer.tlen]
    sub     rcx, [rbp + Xfer.toff]
    jz      .overrun                    ; the body is complete, yet bytes are asked for
    cmp     rcx, r13
    cmova   rcx, r13
    lea     rsi, [rbp + Xfer.tbuf]
    add     rsi, [rbp + Xfer.toff]
    add     [rbp + Xfer.toff], rcx
    sub     r13, rcx
.tcopy:
    movzx   eax, byte [rsi]
    mov     [r12], al
    inc     rsi
    inc     r12
    dec     rcx
    jnz     .tcopy
    jmp     .loop
.next_msg:
    ; 4. the next message: its data size, whether it is the last, its head
    mov     r14, [rbp + Xfer.end]
    sub     r14, [rbp + Xfer.off]       ; left in the range
    xor     eax, eax
    cmp     r14, [rbp + Xfer.chunk]
    setbe   al                          ; the rest fits one message: the last one
    mov     [rbp + Xfer.last], rax
    jbe     .n_ok
    mov     r14, [rbp + Xfer.chunk]
.n_ok:
    mov     [rbp + Xfer.mrem], r14
    lea     r15, [rbp + Xfer.hbuf + GRPC_PREFIX]   ; head cursor, after the prefix
    cmp     qword [rbp + Xfer.first], 0
    je      .no_total
    mov     qword [rbp + Xfer.first], 0
    mov     byte [r15], 0x08            ; field 1 (total_size), varint
    lea     rdi, [r15 + 1]
    mov     rsi, [rbp + Xfer.size]
    call    kv_pb_write_varint wrt ..plt
    lea     r15, [r15 + rax + 1]
.no_total:
    test    r14, r14
    jz      .head_done                  ; an empty range has no data field
    mov     byte [r15], 0x12            ; field 2 (data), bytes
    lea     rdi, [r15 + 1]
    mov     rsi, r14
    call    kv_pb_write_varint wrt ..plt
    lea     r15, [r15 + rax + 1]
.head_done:
    ; prefix: payload = head fields + data + (last) the 5-byte crc32c field
    mov     rax, r15
    lea     rcx, [rbp + Xfer.hbuf + GRPC_PREFIX]
    sub     rax, rcx
    add     rax, r14
    cmp     qword [rbp + Xfer.last], 0
    je      .no_crc
    add     rax, 5
.no_crc:
    mov     byte [rbp + Xfer.hbuf], 0   ; uncompressed
    bswap   eax
    mov     [rbp + Xfer.hbuf + 1], eax  ; big-endian length
    lea     rax, [rbp + Xfer.hbuf]
    sub     r15, rax
    mov     [rbp + Xfer.hlen], r15
    mov     qword [rbp + Xfer.hoff], 0
    jmp     .loop
.overrun:
    ; Unreachable while download_body_len agrees with this generator. Zero-
    ; fill to keep the HTTP/2 framing exact; the client's length and
    ; crc32c checks then reject the result instead of the connection dying.
    mov     byte [r12], 0
    inc     r12
    dec     r13
    jnz     .overrun
.done:
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
