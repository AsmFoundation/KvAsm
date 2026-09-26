; =============================================================================
; grpc.asm — gRPC request handling: validation, routing, store operations and
; the prefetched GET batch, on top of KvNode's HTTP/2 engine (h2n.asm).
;
; h2n.asm does all HTTP/2 framing, HPACK, flow control and protocol
; hardening, and resolves the request header fields into Stream state while
; it decodes them (hpack.asm "meanings"): the route from :path, ST_POST from
; :method, ST_CT_OK from content-type, ST_ENCODED from grpc-encoding. This
; module does the rest:
;   kv_grpc_stream_append   buffer a request body within per-stream and
;                           per-connection budgets
;   kv_grpc_dispatch        END_STREAM => validate framing, decode protobuf,
;                           run the store operation and answer it (a GET is
;                           queued in the connection's batch instead)
;   kv_grpc_run_batch       answer the GETs one read queued, as one
;                           software-pipelined batch
;   kv_grpc_stream_release  unlink and free one Stream
;   kv_grpc_free_streams    free every Stream of a connection (teardown)
;
; Routes: /kvnode.v1.KvNode/{Get,Put,Delete} (proto/kvnode/v1/kvnode.proto)
; and /grpc.health.v1.Health/Check (the standard gRPC health protocol:
; SERVING, NOT_SERVING while draining). Anything else is UNIMPLEMENTED.
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

%include "kvnode.inc"
%include "server.inc"

%define GRPC_OK                 0
%define GRPC_INVALID_ARGUMENT   3
%define GRPC_RESOURCE_EXHAUSTED 8
%define GRPC_UNIMPLEMENTED      12
%define GRPC_INTERNAL           13
; First request-buffer size: 240 + the pool's 16-byte header fills a 256-byte
; block exactly (a GetRequest is 23 bytes; a Put of a 100-byte value ~130).
%define MIN_REQ_CAP             240

extern kv_pool_malloc
extern kv_pool_free
extern kv_pool_realloc
extern memcpy
extern kv_keyeq
extern kv_store_hash
extern kv_store_get
extern kv_store_put
extern kv_store_delete
extern kv_store_prefetch
extern kv_store_prefetch_records
extern kv_hcache_get
extern kv_pb_decode_kv
extern kv_pb_write_varint
extern kv_h2n_reply_parts
extern kv_h2n_reply_buf
extern kv_h2n_reply_static
extern kv_h2n_reply_headers

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
CSTR b_405,         "405 method not allowed: KvNode speaks gRPC, send POST", 10
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
CSTR s_m_oom,       "server out of memory"
CSTR s_m_noservice, "unknown service"

CSTR v_post,        "POST"
CSTR v_identity,    "identity"
; The one service name the health check reports on besides "" (whole server).
CSTR v_service,     "kvnode.v1.KvNode"

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
TRAILERS_ONLY nv_e_oom,       s_c13, s_m_oom
TRAILERS_ONLY nv_e_noservice, s_c5,  s_m_noservice

section .text

global kv_grpc_free_streams
global kv_grpc_run_batch
global kv_grpc_dispatch
global kv_grpc_stream_append
global kv_grpc_stream_release

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
;          body (ST_STATIC_RESP, .rodata).
; =============================================================================
stream_free:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rax, [rbx + Stream.conn]
    mov     rax, [rax + Conn.worker]
    lea     r12, [rax + Worker.pool]
    test    qword [rbx + Stream.flags], ST_BORROWED
    jnz     .borrowed                   ; points into the read buffer: not ours
    mov     rdi, [rbx + Stream.req_buf]
    mov     rsi, r12
    call    kv_pool_free
.borrowed:
    test    qword [rbx + Stream.flags], ST_STATIC_RESP
    jnz     .static_resp                ; .rodata text: freeing it would crash
    mov     rdi, [rbx + Stream.resp_buf]
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
;          not touch it afterwards; a GET instead joins the connection's
;          batch and is answered by kv_grpc_run_batch after the read.
;          Validation order follows the gRPC HTTP/2 spec: method/content-type
;          are HTTP-level (405/415), then routing (UNIMPLEMENTED), then the
;          5-byte message framing, then the protobuf payload and field limits.
;          Compressed-flag handling matches grpc-go/grpc-core: flag 1 with a
;          declared codec (grpc-encoding other than identity) is UNIMPLEMENTED
;          + grpc-accept-encoding: identity; flag 1 with no/identity codec is
;          INTERNAL (the sender contradicts itself); flag 0 is accepted
;          whatever grpc-encoding says, because it means "not compressed".
; =============================================================================
kv_grpc_dispatch:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 56                     ; [0..40) PbKv, [40] h1, [48] h2;
                                        ; 6 pushes + 56: rsp 16-aligned for calls
    mov     r12, rdi                    ; Conn*
    mov     rbx, rsi                    ; Stream*
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

    mov     rax, [rbx + Stream.method]
    cmp     rax, M_GET
    je      .get
    cmp     rax, M_PUT
    je      .put

    ; ---- Delete ----
    mov     rdi, r14
    mov     rsi, [rsp + PbKv.key]
    mov     rdx, [rsp + PbKv.key_len]
    mov     rcx, [rsp + 40]
    mov     r8, [rsp + 48]
    call    kv_store_delete wrt ..plt
    mov     rdi, rax                    ; 1 => DeleteResponse{deleted=true}
    jmp     .bool_reply

    ; ---- Put ----
.put:
    mov     rax, [rsp + PbKv.value_len]
    cmp     rax, [r13 + Config.max_value]
    ja      .e_value
    mov     rdi, r14
    mov     rsi, [rsp + PbKv.key]
    mov     rdx, [rsp + PbKv.key_len]
    mov     rcx, [rsp + PbKv.value]
    mov     r8, [rsp + PbKv.value_len]
    mov     r9, [rsp + 40]
    sub     rsp, 16
    mov     rax, [rsp + 16 + 48]
    mov     [rsp], rax                  ; 7th argument: h2
    call    kv_store_put wrt ..plt
    add     rsp, 16
    cmp     rax, -ENOSPC
    je      .e_full
    cmp     rax, -ENOMEM
    je      .e_memory
    test    rax, rax
    js      .e_value                    ; -E2BIG
    mov     rdi, rax                    ; 1 => PutResponse{created=true}
    jmp     .bool_reply

    ; ---- Get: deferred into the connection's batch ----
    ; The lookup itself runs in kv_grpc_run_batch once the whole read is
    ; parsed. Here: remember the key (it points into req_buf, which lives as
    ; long as the Stream, or into the read buffer for a borrowed body, which
    ; lives until the batch has run) and its hashes, and start loading both
    ; candidate buckets now, so the cache misses of every GET in this read
    ; overlap.
.get:
    mov     rax, [rsp + PbKv.key]
    mov     [rbx + Stream.key], rax
    mov     rax, [rsp + PbKv.key_len]
    mov     [rbx + Stream.key_len], rax
    mov     rsi, [rsp + 40]
    mov     [rbx + Stream.h1], rsi
    mov     rdx, [rsp + 48]
    mov     [rbx + Stream.h2], rdx
    mov     rdi, r14
    call    kv_store_prefetch wrt ..plt
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
    ; service "" (whole server) or "kvnode.v1.KvNode" => SERVING (1), or
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
.bool_reply:
    mov     r13, rdi
    mov     edi, GRPC_PREFIX + 2
    mov     rax, [r12 + Conn.worker]
    lea     rsi, [rax + Worker.pool]
    call    kv_pool_malloc
    test    rax, rax
    jz      .e_oom
    mov     [rbx + Stream.resp_buf], rax
    xor     esi, esi
    test    r13, r13
    jz      .bool_len
    mov     esi, 2
    mov     byte [rax + GRPC_PREFIX], 0x08      ; field 1, wire type varint
    mov     [rax + GRPC_PREFIX + 1], r13b       ; < 128: a single varint byte
.bool_len:
    lea     rcx, [rsi + GRPC_PREFIX]
    mov     [rbx + Stream.resp_len], rcx
    mov     rdi, rax
    call    write_prefix

    ; resp_buf / resp_len hold the message: h2n frames HEADERS + DATA +
    ; trailers (all at once when the windows allow, else from its pend list)
    ; and frees the buffer with the stream.
    mov     rdi, r12
    mov     rsi, rbx
    call    kv_h2n_reply_buf wrt ..plt
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
    jmp     .trailers_only
.e_oom:
    lea     rdx, [nv_e_oom]
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
.submitted:
    ; h2n's reply functions return 0 or -1 (out of memory); normalize so the
    ; contract above holds whatever negative value they pick.
    test    eax, eax
    jz      .out
    mov     eax, -1
.out:
    add     rsp, 56
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_grpc_run_batch — answer every GET queued while parsing the last read.
;
; @param  rdi  Conn*
; @returns eax = 0, or -1 if a response could not be queued (out of memory
;          in h2n's output buffer: the caller destroys the connection).
; @remarks Called by kv_h2n_read right after the read's frames were parsed,
;          on the connection's thread, while the worker is QSBR-online
;          (records stay valid while their values are copied).
;          Two passes, a software pipeline across requests:
;            1. prefetch the candidate records (the buckets were prefetched at
;               parse time and have arrived by now);
;            2. run each lookup — bucket and record lines are warm — and
;               frame its response.
;          Streams the peer reset meanwhile (ST_CLOSED) are freed, not
;          answered. The batch is detached before pass 2, so nothing can
;          observe a half-processed list.
; =============================================================================
kv_grpc_run_batch:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14                         ; 5 pushes: rsp 16-aligned for calls
    mov     r12, rdi
    mov     rbx, [r12 + Conn.batch_head]
    xor     ebp, ebp                    ; result
    test    rbx, rbx
    jz      .out
    mov     rax, [r12 + Conn.worker]
    mov     r13, [rax + Worker.table]
.prefetch:
    mov     rdi, r13
    mov     rsi, [rbx + Stream.h1]
    mov     rdx, [rbx + Stream.h2]
    call    kv_store_prefetch_records wrt ..plt
    mov     rbx, [rbx + Stream.batch_next]
    test    rbx, rbx
    jnz     .prefetch
    mov     rbx, [r12 + Conn.batch_head]
    xor     eax, eax
    mov     [r12 + Conn.batch_head], rax
    mov     [r12 + Conn.batch_tail], rax
.answer:
    mov     r14, [rbx + Stream.batch_next]
    and     qword [rbx + Stream.flags], ~ST_BATCHED
    test    qword [rbx + Stream.flags], ST_CLOSED
    jnz     .closed
    mov     rdi, r12
    mov     rsi, rbx
    call    get_execute
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
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
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
    mov     rdi, [rax + Worker.table]
    call    kv_store_get wrt ..plt
.looked_up:
    test    rax, rax
    jz      .missing
    mov     r15, rax                    ; Rec* (valid while this worker is online)
    mov     ebp, [r15 + Rec.val_len]
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

section .note.GNU-stack noalloc noexec nowrite progbits
