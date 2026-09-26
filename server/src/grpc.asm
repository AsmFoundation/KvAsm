; =============================================================================
; grpc.asm — gRPC (HTTP/2) request handling on top of nghttp2 callbacks.
;
; nghttp2 does all HTTP/2 framing, HPACK, flow control and protocol hardening;
; this module turns its callbacks into KvNode calls:
;   on_begin_headers   allocate a Stream for a request
;   on_header          route (:path), check :method / content-type / encoding
;   on_data_chunk_recv buffer the request body within per-stream and
;                      per-connection budgets
;   on_frame_recv      END_STREAM => validate framing, decode protobuf, run
;                      the store operation, submit the response
;   on_stream_close    free the Stream
;
; Routes: /kvnode.v1.KvNode/{Get,Put,Delete} (proto/kvnode/v1/kvnode.proto)
; and /grpc.health.v1.Health/Check (the standard gRPC health protocol:
; SERVING, NOT_SERVING while draining). Anything else is UNIMPLEMENTED.
;
; Responses: success = HEADERS(:status 200, content-type) + DATA(message) +
; trailer HEADERS(grpc-status 0); gRPC errors = one "Trailers-Only" HEADERS
; frame with grpc-status/grpc-message (END_STREAM); non-gRPC requests get a
; plain HTTP 405/415 with a short text body. Every request runs to
; completion inside the callback (store operations are sub-microsecond), so
; there is no asynchronous state to manage.
;
; Threading: all functions run on the worker thread that owns the connection
; (user_data = Conn*). The worker is QSBR-online while nghttp2 callbacks run,
; so records returned by kv_store_get stay valid while their value is copied.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"
%include "server.inc"
%include "nghttp2.inc"

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
extern kv_pool_calloc
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
extern kv_pb_varint_len
extern kv_pb_write_varint
extern nghttp2_session_callbacks_new
extern nghttp2_session_callbacks_set_on_begin_headers_callback
extern nghttp2_session_callbacks_set_on_header_callback
extern nghttp2_session_callbacks_set_on_data_chunk_recv_callback
extern nghttp2_session_callbacks_set_on_frame_recv_callback
extern nghttp2_session_callbacks_set_on_stream_close_callback
extern nghttp2_session_get_stream_user_data
extern nghttp2_session_set_stream_user_data
extern nghttp2_submit_response2
extern nghttp2_submit_trailer
extern nghttp2_submit_rst_stream

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

; One nghttp2_nv (40 bytes) referring to two CSTR labels; NO_COPY because the
; strings live in .rodata for the whole process.
%macro NV 2
    dq %1, %2, %{1}_len, %{2}_len
    db NGHTTP2_NV_FLAG_NO_COPY_NAME | NGHTTP2_NV_FLAG_NO_COPY_VALUE
    times 7 db 0
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
CSTR s_c0,          "0"
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

CSTR h_method,      ":method"
CSTR h_path,        ":path"
CSTR h_encoding,    "grpc-encoding"
CSTR v_post,        "POST"
CSTR v_identity,    "identity"
CSTR v_prefix,      "/kvnode.v1.KvNode/"
CSTR v_get,         "Get"
CSTR v_put,         "Put"
CSTR v_delete,      "Delete"
; Standard gRPC health checking protocol (grpc/health/v1/health.proto), as
; called by Kubernetes gRPC probes and gRPC-aware load balancers.
CSTR v_health,      "/grpc.health.v1.Health/Check"
CSTR v_service,     "kvnode.v1.KvNode"

; nghttp2_nv arrays carry absolute string pointers: PIE-legal only through
; load-time relocations, hence .data.rel.ro (relocated, then read-only via
; RELRO) rather than .rodata (would need a text relocation; -z text rejects
; it) or .data (would stay writable for the process lifetime).
section .data.rel.ro progbits alloc noexec write align=8
nv_ok:              NV s_status, s_200
                    NV s_ctype, s_ctype_grpc
nv_trailer_ok:      NV s_gstatus, s_c0
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

global kv_grpc_callbacks_new
global kv_grpc_free_streams
global kv_grpc_run_batch

; =============================================================================
; kv_grpc_callbacks_new — the callback table shared by every session.
; @returns rax = nghttp2_session_callbacks*, or 0 on allocation failure.
; @remarks Built once by main before workers start; nghttp2 copies it into
;          each session, so sharing it read-only across threads is safe.
; =============================================================================
kv_grpc_callbacks_new:
    push    rbx
    sub     rsp, 16
    lea     rdi, [rsp]
    call    nghttp2_session_callbacks_new wrt ..plt
    test    eax, eax
    jnz     .fail
    mov     rbx, [rsp]
    mov     rdi, rbx
    lea     rsi, [on_begin_headers]
    call    nghttp2_session_callbacks_set_on_begin_headers_callback wrt ..plt
    mov     rdi, rbx
    lea     rsi, [on_header]
    call    nghttp2_session_callbacks_set_on_header_callback wrt ..plt
    mov     rdi, rbx
    lea     rsi, [on_data_chunk_recv]
    call    nghttp2_session_callbacks_set_on_data_chunk_recv_callback wrt ..plt
    mov     rdi, rbx
    lea     rsi, [on_frame_recv]
    call    nghttp2_session_callbacks_set_on_frame_recv_callback wrt ..plt
    mov     rdi, rbx
    lea     rsi, [on_stream_close]
    call    nghttp2_session_callbacks_set_on_stream_close_callback wrt ..plt
    mov     rax, rbx
    jmp     .out
.fail:
    xor     eax, eax
.out:
    add     rsp, 16
    pop     rbx
    ret

; =============================================================================
; kv_grpc_free_streams — free every Stream still attached to a connection.
; @param  rdi  Conn*
; @returns nothing.
; @remarks Call after nghttp2_session_del: nghttp2 frees its own stream
;          objects without invoking on_stream_close, so streams alive at that
;          moment would otherwise leak.
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
    mov     [r12 + Conn.cur_stream], rax  ; the cache must never outlive its stream
    mov     [r12 + Conn.batch_head], rax  ; nor may a batch of freed streams
    mov     [r12 + Conn.batch_tail], rax
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; stream_lookup — Stream* of a stream id, answered from the connection's
; one-entry cache when possible. Internal.
; @param  rdi  session   @param esi  stream id   @param rdx  Conn*
; @returns rax = Stream*, or 0 when nghttp2 knows no user data for the id.
;          Clobbers caller-saved registers.
; @remarks Why: every header, DATA chunk and frame callback names its stream
;          by id, and nghttp2 resolves ids through a hash map. A request's
;          callbacks arrive back to back, so the last stream answers almost
;          all of them. Safe because stream ids are never reused within a
;          connection and the entry is cleared whenever its Stream is freed.
; =============================================================================
stream_lookup:
    movsxd  rax, esi                    ; ids are stored sign-extended (all positive)
    cmp     rax, [rdx + Conn.cur_id]
    jne     .miss
    mov     rax, [rdx + Conn.cur_stream]
    test    rax, rax
    jz      .miss
    ret
.miss:
    push    rbx                         ; 1 push: rsp 16-aligned for the call
    mov     rbx, rdx
    call    nghttp2_session_get_stream_user_data wrt ..plt  ; rdi/esi still as passed
    test    rax, rax
    jz      .out
    mov     [rbx + Conn.cur_stream], rax
    mov     rcx, [rax + Stream.id]
    mov     [rbx + Conn.cur_id], rcx
.out:
    pop     rbx
    ret

; =============================================================================
; stream_free — release a Stream and its buffers to the worker's pool. Internal.
; @param  rdi  Stream* (its conn and conn's worker must still be alive)
; @returns nothing.
; =============================================================================
stream_free:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rax, [rbx + Stream.conn]
    mov     rax, [rax + Conn.worker]
    lea     r12, [rax + Worker.pool]
    mov     rdi, [rbx + Stream.req_buf]
    mov     rsi, r12
    call    kv_pool_free
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
; on_begin_headers — nghttp2_on_begin_headers_callback.
; @param  rdi  session  @param rsi  const nghttp2_frame*  @param rdx  Conn*
; @returns eax = 0, or NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE when out of
;          memory (resets only this stream, the connection survives).
; @remarks Only request HEADERS start a stream; trailers are ignored.
; =============================================================================
on_begin_headers:
    cmp     byte [rsi + FRAME_HD_TYPE], NGHTTP2_HEADERS
    jne     .ignore
    cmp     dword [rsi + FRAME_HEADERS_CAT], NGHTTP2_HCAT_REQUEST
    jne     .ignore
    push    rbx
    push    r12
    push    r13
    mov     rbx, rdi
    mov     r12, rdx
    movsxd  r13, dword [rsi + FRAME_HD_STREAM_ID]
    mov     edi, 1
    mov     esi, Stream_size
    mov     rax, [r12 + Conn.worker]
    lea     rdx, [rax + Worker.pool]
    call    kv_pool_calloc
    test    rax, rax
    jz      .nomem
    mov     [rax + Stream.conn], r12
    mov     [rax + Stream.id], r13
    mov     rcx, [r12 + Conn.streams]   ; push front of the connection's list
    mov     [rax + Stream.next], rcx
    test    rcx, rcx
    jz      .linked
    mov     [rcx + Stream.prev], rax
.linked:
    mov     [r12 + Conn.streams], rax
    mov     [r12 + Conn.cur_stream], rax  ; its headers follow immediately
    mov     [r12 + Conn.cur_id], r13
    mov     rdi, rbx
    mov     esi, r13d
    mov     rdx, rax
    call    nghttp2_session_set_stream_user_data wrt ..plt
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret
.ignore:
    xor     eax, eax
    ret

; -----------------------------------------------------------------------------
; MATCH label — ZF=1 iff (rdi=value/name, rsi=len) equals the CSTR `label`.
; Uses kv_keyeq (clobbers rax, rcx, r8). Expects: r14 = ptr, r15 = len.
; -----------------------------------------------------------------------------
%macro MATCH 1
    cmp     r15, %1_len
    jne     %%no
    mov     rdi, r14
    lea     rsi, [%1]
    mov     edx, %1_len
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      %%no
    cmp     eax, eax                    ; ZF=1: equal
    jmp     %%done
%%no:
    or      eax, 1                      ; ZF=0
%%done:
%endmacro

; =============================================================================
; on_header — nghttp2_on_header_callback: route and validate one header.
; @param  rdi  session  @param rsi  frame  @param rdx  name  @param rcx  namelen
; @param  r8   value    @param r9   valuelen  [rsp+8] flags  [rsp+16] Conn*
; @returns eax = 0 always (problems are answered at END_STREAM, not by
;          resetting the stream, so the client gets a proper gRPC status).
; @remarks nghttp2 has already validated header syntax (lowercase names,
;          pseudo-header rules, no connection-specific headers, te: trailers).
; =============================================================================
on_header:
    cmp     byte [rsi + FRAME_HD_TYPE], NGHTTP2_HEADERS
    jne     .ret0
    cmp     dword [rsi + FRAME_HEADERS_CAT], NGHTTP2_HCAT_REQUEST
    jne     .ret0
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     r12, rdx                    ; name
    mov     r13, rcx                    ; namelen
    mov     r14, r8                     ; value
    mov     r15, r9                     ; valuelen
    mov     esi, [rsi + FRAME_HD_STREAM_ID]
    mov     rdx, [rsp + 56]             ; Conn*: 8th argument, above 5 pushes + return
    call    stream_lookup
    test    rax, rax
    jz      .out
    mov     rbx, rax                    ; Stream*

    ; Dispatch on the header name (swap in name/len for MATCH).
    xchg    r12, r14
    xchg    r13, r15                    ; r14/r15 = name, r12/r13 = value
    MATCH   h_path
    je      .path
    MATCH   h_method
    je      .method
    MATCH   s_ctype
    je      .ctype
    MATCH   h_encoding
    je      .encoding
    jmp     .out

.path:
    mov     r14, r12                    ; value
    mov     r15, r13
    MATCH   v_health                    ; exact path, checked before the KvNode prefix
    jne     .kv_path
    mov     qword [rbx + Stream.method], M_HEALTH
    jmp     .out
.kv_path:
    cmp     r15, v_prefix_len
    jbe     .out
    mov     rdi, r14
    lea     rsi, [v_prefix]
    mov     edx, v_prefix_len
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .out
    add     r14, v_prefix_len           ; method name after the service prefix
    sub     r15, v_prefix_len
    MATCH   v_get
    jne     .try_put
    mov     qword [rbx + Stream.method], M_GET
    jmp     .out
.try_put:
    MATCH   v_put
    jne     .try_delete
    mov     qword [rbx + Stream.method], M_PUT
    jmp     .out
.try_delete:
    MATCH   v_delete
    jne     .out
    mov     qword [rbx + Stream.method], M_DELETE
    jmp     .out

.method:
    mov     r14, r12
    mov     r15, r13
    MATCH   v_post
    jne     .out
    or      qword [rbx + Stream.flags], ST_POST
    jmp     .out

.ctype:
    ; "application/grpc" exactly, or followed by '+' (subtype) or ';' (params)
    cmp     r13, s_ctype_grpc_len
    jb      .out
    mov     rdi, r12
    lea     rsi, [s_ctype_grpc]
    mov     edx, s_ctype_grpc_len
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .out
    cmp     r13, s_ctype_grpc_len
    je      .ctype_ok
    mov     al, [r12 + s_ctype_grpc_len]
    cmp     al, '+'
    je      .ctype_ok
    cmp     al, ';'
    jne     .out
.ctype_ok:
    or      qword [rbx + Stream.flags], ST_CT_OK
    jmp     .out

.encoding:
    mov     r14, r12
    mov     r15, r13
    MATCH   v_identity
    je      .out
    or      qword [rbx + Stream.flags], ST_ENCODED
.out:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
.ret0:
    xor     eax, eax
    ret

; =============================================================================
; on_data_chunk_recv — nghttp2_on_data_chunk_recv_callback: buffer the body.
; @param  rdi  session  @param sil  flags  @param edx  stream id
; @param  rcx  data     @param r8   len    @param r9   Conn*
; @returns eax = 0 (always: returning an error would kill the connection).
; @remarks Two budgets bound memory: a stream whose body exceeds max_request
;          is marked TOO_LARGE (answered RESOURCE_EXHAUSTED at END_STREAM, its
;          bytes dropped); a connection whose buffered bytes would exceed
;          conn_budget gets that stream reset with ENHANCE_YOUR_CALM.
;          Buffers grow geometrically, capped at max_request.
; =============================================================================
on_data_chunk_recv:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     [rsp], rdi                  ; session
    mov     r12, rcx                    ; data
    mov     r13, r8                     ; len
    mov     r14, r9                     ; Conn*
    mov     r15d, edx                   ; stream id
    mov     esi, edx
    mov     rdx, r14
    call    stream_lookup
    test    rax, rax
    jz      .out
    mov     rbx, rax
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
    mov     rdi, [rsp]
    mov     esi, NGHTTP2_FLAG_NONE
    mov     edx, r15d
    mov     ecx, NGHTTP2_ENHANCE_YOUR_CALM
    call    nghttp2_submit_rst_stream wrt ..plt
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
    xor     eax, eax
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; on_stream_close — nghttp2_on_stream_close_callback.
; @param  rdi  session  @param esi  stream id  @param edx  error code
; @param  rcx  Conn*
; @returns eax = 0.
; =============================================================================
on_stream_close:
    push    rbx
    push    r12
    sub     rsp, 8
    mov     r12, rcx
    mov     rdx, rcx
    call    stream_lookup
    test    rax, rax
    jz      .out
    mov     rbx, rax
    cmp     [r12 + Conn.cur_stream], rbx
    jne     .uncached
    mov     qword [r12 + Conn.cur_stream], 0  ; about to be freed: drop from the cache
.uncached:
    ; A GET queued in this read's batch was reset by the peer: it is still
    ; referenced by Conn.batch, so only mark it; kv_grpc_run_batch frees it
    ; instead of answering (answering a closed stream would be a bug).
    test    qword [rbx + Stream.flags], ST_BATCHED
    jz      .free_now
    or      qword [rbx + Stream.flags], ST_CLOSED
    jmp     .out
.free_now:
    mov     rdi, r12
    mov     rsi, rbx
    call    stream_unlink_free
.out:
    xor     eax, eax
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; stream_unlink_free — remove a Stream from its connection and free it.
; Internal.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns nothing.
; @remarks Also returns the stream's buffered request bytes to the
;          connection's budget.
; =============================================================================
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
; on_frame_recv — nghttp2_on_frame_recv_callback: dispatch complete requests.
; @param  rdi  session  @param rsi  frame  @param rdx  Conn*
; @returns eax = 0, or NGHTTP2_ERR_CALLBACK_FAILURE if a response could not
;          be submitted (allocation failure inside nghttp2).
; =============================================================================
on_frame_recv:
    movzx   eax, byte [rsi + FRAME_HD_TYPE]
    cmp     eax, NGHTTP2_DATA
    je      .check
    cmp     eax, NGHTTP2_HEADERS
    jne     .ret0
.check:
    test    byte [rsi + FRAME_HD_FLAGS], NGHTTP2_FLAG_END_STREAM
    jz      .ret0
    push    rbx
    push    r12
    push    r13
    mov     rbx, rdi
    mov     r12, rdx
    mov     r13d, [rsi + FRAME_HD_STREAM_ID]
    mov     esi, r13d
    mov     rdx, r12
    call    stream_lookup
    test    rax, rax
    jz      .done
    test    qword [rax + Stream.flags], ST_RESPONDED
    jnz     .done
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, rax
    call    dispatch
    jmp     .out
.done:
    xor     eax, eax
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret
.ret0:
    xor     eax, eax
    ret

; =============================================================================
; dispatch — validate a complete request, run it, submit the response.
; Internal.
; @param  rdi  session  @param rsi  Conn*  @param rdx  Stream*
; @returns eax = 0, or NGHTTP2_ERR_CALLBACK_FAILURE on submission failure.
; @remarks Validation order follows the gRPC HTTP/2 spec: method/content-type
;          are HTTP-level (405/415), then routing (UNIMPLEMENTED), then the
;          5-byte message framing, then the protobuf payload and field limits.
;          Compressed-flag handling matches grpc-go/grpc-core: flag 1 with a
;          declared codec (grpc-encoding other than identity) is UNIMPLEMENTED
;          + grpc-accept-encoding: identity; flag 1 with no/identity codec is
;          INTERNAL (the sender contradicts itself); flag 0 is accepted
;          whatever grpc-encoding says, because it means "not compressed".
; =============================================================================
dispatch:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 88                     ; [0..40) PbKv, [40] h1, [48] h2, [56] session,
                                        ; [64..80) nghttp2_data_provider2, [80] pad
    mov     [rsp + 56], rdi
    mov     r12, rsi                    ; Conn*
    mov     rbx, rdx                    ; Stream*
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
    ; long as the Stream) and its hashes, and start loading both candidate
    ; buckets now, so the cache misses of every GET in this read overlap.
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

.submit_ok:
    ; nghttp2 copies the provider struct, so a stack-resident one is fine;
    ; the Stream it points to lives until on_stream_close.
    mov     qword [rbx + Stream.resp_off], 0
    mov     [rsp + 64], rbx             ; provider.source.ptr = Stream*
    lea     rax, [resp_read]
    mov     [rsp + 72], rax             ; provider.read_callback
    mov     rdi, [rsp + 56]
    mov     esi, [rbx + Stream.id]
    lea     rdx, [nv_ok]
    mov     ecx, 2
    lea     r8, [rsp + 64]
    call    nghttp2_submit_response2 wrt ..plt
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
    ; Plain HTTP error: 3 headers + a static text body. resp_read ends the
    ; stream on the last DATA frame (no gRPC trailers) for ST_STATIC_RESP, and
    ; stream_free leaves the .rodata buffer alone.
    or      qword [rbx + Stream.flags], ST_STATIC_RESP
    mov     [rbx + Stream.resp_buf], rax
    mov     [rbx + Stream.resp_len], rcx
    mov     qword [rbx + Stream.resp_off], 0
    mov     [rsp + 64], rbx             ; provider.source.ptr = Stream*
    lea     rax, [resp_read]
    mov     [rsp + 72], rax             ; provider.read_callback
    mov     rdi, [rsp + 56]
    mov     esi, [rbx + Stream.id]
    mov     ecx, 3                      ; rdx = nva (set above)
    lea     r8, [rsp + 64]
    call    nghttp2_submit_response2 wrt ..plt
    jmp     .submitted
.e_method:
    lea     rdx, [nv_e_method]
    jmp     .trailers_only
.compressed:
    ; ST_ENCODED was set by on_header for a grpc-encoding other than identity.
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
.headers_only:
    ; No data provider => nghttp2 ends the stream on this HEADERS frame.
    mov     rdi, [rsp + 56]
    mov     esi, [rbx + Stream.id]
    xor     r8d, r8d
    call    nghttp2_submit_response2 wrt ..plt
.submitted:
    test    eax, eax
    jz      .ok
    mov     eax, NGHTTP2_ERR_CALLBACK_FAILURE
    jmp     .out
.ok:
    xor     eax, eax
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
; kv_grpc_run_batch — answer every GET queued while parsing the last read.
;
; @param  rdi  Conn*
; @returns eax = 0, or -1 if nghttp2 could not accept a response (out of
;          memory inside the session: the caller destroys the connection).
; @remarks Called by the worker right after nghttp2_session_mem_recv2, on the
;          connection's thread, while the worker is QSBR-online (records stay
;          valid while their values are copied).
;          Two passes, a software pipeline across requests:
;            1. prefetch the candidate records (the buckets were prefetched at
;               parse time and have arrived by now);
;            2. run each lookup — bucket and record lines are warm — and
;               submit its response.
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
    mov     rdi, [r12 + Conn.session]
    mov     rsi, r12
    mov     rdx, rbx
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
; get_execute — look up a batched GET and submit its response. Internal.
; @param  rdi  session   @param rsi  Conn*   @param rdx  Stream* (key, h1, h2 set)
; @returns eax = 0, or nghttp2's error code if the response was not accepted.
; @remarks Response: GetResponse{found = true, value} as 08 01 12 <len> <value>
;          (the value field is omitted for an empty value); a miss is the
;          empty message (found = false); out of memory is Trailers-Only
;          INTERNAL.
; =============================================================================
get_execute:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: rsp 16-aligned for calls
    mov     r14, rdi                    ; session
    mov     r12, rsi                    ; Conn*
    mov     rbx, rdx                    ; Stream*
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
    mov     r13d, 2                     ; 08 01
    test    rbp, rbp
    jz      .alloc
    mov     rdi, rbp
    call    kv_pb_varint_len wrt ..plt
    lea     r13, [r13 + rax + 1]        ; + 12 <varint len>
    add     r13, rbp                    ; + value
.alloc:
    lea     rdi, [r13 + GRPC_PREFIX]
    mov     rax, [r12 + Conn.worker]
    lea     rsi, [rax + Worker.pool]
    call    kv_pool_malloc
    test    rax, rax
    jz      .oom
    mov     [rbx + Stream.resp_buf], rax
    lea     rcx, [r13 + GRPC_PREFIX]
    mov     [rbx + Stream.resp_len], rcx
    mov     rdi, rax
    mov     rsi, r13
    call    write_prefix
    mov     rdi, [rbx + Stream.resp_buf]
    mov     word [rdi + GRPC_PREFIX], 0x0108   ; field 1 (found) = true
    test    rbp, rbp
    jz      .submit
    mov     byte [rdi + GRPC_PREFIX + 2], 0x12 ; field 2 (value), length-delimited
    lea     rdi, [rdi + GRPC_PREFIX + 3]
    mov     rsi, rbp
    call    kv_pb_write_varint wrt ..plt
    mov     rdi, [rbx + Stream.resp_buf]
    lea     rdi, [rdi + GRPC_PREFIX + 3 + rax]
    mov     ecx, [r15 + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    lea     rsi, [r15 + Rec.data + rcx]  ; value follows the padded key
    mov     rdx, rbp
    call    memcpy wrt ..plt
.submit:
    mov     rdi, r14
    mov     rsi, rbx
    call    respond_message
    jmp     .out
.missing:
    mov     rdi, r14
    mov     rsi, r12
    mov     rdx, rbx
    xor     ecx, ecx                    ; GetResponse{} (found = false)
    call    respond_varint
    jmp     .out
.oom:
    mov     rdi, r14
    mov     rsi, r12
    mov     rdx, rbx
    lea     rcx, [nv_e_oom]
    mov     r8d, 4
    call    respond_error
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
; respond_message — submit HEADERS + the stream's resp_buf as DATA + trailers.
; Internal.
; @param  rdi  session   @param rsi  Stream* (resp_buf/resp_len set)
; @returns eax = nghttp2_submit_response2's result (0 on success).
; @remarks nghttp2 copies the data-provider struct, so a stack-resident one
;          is fine; resp_read streams resp_buf and adds the grpc-status
;          trailers on the last chunk.
; =============================================================================
respond_message:
    sub     rsp, 24                     ; [0..16) nghttp2_data_provider2; rsp aligned
    mov     qword [rsi + Stream.resp_off], 0
    mov     [rsp], rsi                  ; provider.source.ptr = Stream*
    lea     rax, [resp_read]
    mov     [rsp + 8], rax              ; provider.read_callback
    mov     esi, [rsi + Stream.id]      ; (the id is read before rsi is replaced)
    lea     rdx, [nv_ok]
    mov     ecx, 2
    lea     r8, [rsp]
    call    nghttp2_submit_response2 wrt ..plt
    add     rsp, 24
    ret

; =============================================================================
; respond_error — Trailers-Only gRPC error (single HEADERS frame). Internal.
; @param  rdi  session   @param rsi  Conn*   @param rdx  Stream*
; @param  rcx  nghttp2_nv array   @param r8  entry count
; @returns eax = nghttp2_submit_response2's result.
; =============================================================================
respond_error:
    mov     rax, [rsi + Conn.worker]
    inc     qword [rax + Worker.st_errors]
    mov     esi, [rdx + Stream.id]
    mov     rdx, rcx
    mov     rcx, r8
    xor     r8d, r8d                    ; no data provider: END_STREAM on HEADERS
    jmp     nghttp2_submit_response2 wrt ..plt  ; tail call

; =============================================================================
; respond_varint — reply with a message whose only field is 1 = value.
; Internal.
; @param  rdi  session   @param rsi  Conn*   @param rdx  Stream*
; @param  rcx  value (0..127); 0 sends the empty message (proto3 default)
; @returns eax = nghttp2's result (0 on success).
; =============================================================================
respond_varint:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; 4 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     edi, GRPC_PREFIX + 2
    mov     rax, [r12 + Conn.worker]
    lea     rsi, [rax + Worker.pool]
    call    kv_pool_malloc
    test    rax, rax
    jz      .oom
    mov     [r13 + Stream.resp_buf], rax
    xor     esi, esi
    test    r14, r14
    jz      .len
    mov     esi, 2
    mov     byte [rax + GRPC_PREFIX], 0x08     ; field 1, wire type varint
    mov     [rax + GRPC_PREFIX + 1], r14b      ; < 128: a single varint byte
.len:
    lea     rcx, [rsi + GRPC_PREFIX]
    mov     [r13 + Stream.resp_len], rcx
    mov     rdi, rax
    call    write_prefix
    mov     rdi, rbx
    mov     rsi, r13
    call    respond_message
    jmp     .out
.oom:
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, r13
    lea     rcx, [nv_e_oom]
    mov     r8d, 4
    call    respond_error
.out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; write_prefix — gRPC message prefix: flag 0 + big-endian 32-bit length.
; Internal.  @param rdi  buffer  @param rsi  payload length
; =============================================================================
write_prefix:
    mov     byte [rdi], 0
    mov     eax, esi
    bswap   eax
    mov     [rdi + 1], eax
    ret

; =============================================================================
; resp_read — nghttp2_data_source_read_callback2 for response messages.
; @param  rdi  session  @param esi  stream id  @param rdx  buf  @param rcx  len
; @param  r8   uint32_t *data_flags  @param r9  nghttp2_data_source* (Stream*)
; @returns rax = bytes copied, or NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE if the
;          trailers cannot be queued (resets only this stream).
; @remarks On the last chunk of a gRPC response sets EOF | NO_END_STREAM and
;          submits the grpc-status trailers: gRPC requires the stream to end
;          on trailing HEADERS, never on the last DATA frame. A plain HTTP
;          error body (ST_STATIC_RESP) instead ends the stream on its last
;          DATA frame (EOF alone) — trailers there would be meaningless.
;          nghttp2 may call this repeatedly with small `len` (flow-control
;          window, frame size), hence the resp_off cursor.
; =============================================================================
resp_read:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     rbx, [r9]                   ; Stream*
    mov     r12, rdi
    mov     r13d, esi
    mov     r14, r8
    mov     rax, [rbx + Stream.resp_len]
    sub     rax, [rbx + Stream.resp_off]
    cmp     rax, rcx
    cmova   rax, rcx
    mov     r15, rax                    ; n
    mov     rdi, rdx
    mov     rsi, [rbx + Stream.resp_buf]
    add     rsi, [rbx + Stream.resp_off]
    mov     rdx, r15
    call    memcpy wrt ..plt
    add     [rbx + Stream.resp_off], r15
    mov     rax, [rbx + Stream.resp_off]
    cmp     rax, [rbx + Stream.resp_len]
    jne     .ret_n
    test    qword [rbx + Stream.flags], ST_STATIC_RESP
    jz      .grpc_eof
    or      dword [r14], NGHTTP2_DATA_FLAG_EOF          ; END_STREAM on this DATA
    jmp     .ret_n
.grpc_eof:
    or      dword [r14], NGHTTP2_DATA_FLAG_EOF | NGHTTP2_DATA_FLAG_NO_END_STREAM
    mov     rdi, r12
    mov     esi, r13d
    lea     rdx, [nv_trailer_ok]
    mov     ecx, 1
    call    nghttp2_submit_trailer wrt ..plt
    test    eax, eax
    jz      .ret_n
    mov     r15, NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE
.ret_n:
    mov     rax, r15
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
