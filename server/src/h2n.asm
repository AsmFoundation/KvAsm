; =============================================================================
; h2n.asm — KvAsm's native HTTP/2 server engine (RFC 9113), built for gRPC.
;
; Why it exists (ADR 0007): KvAsm first used nghttp2, and profiling it at
; one core showed the library spending ~672 of the 1,427 ns of server time
; per request — generic frame queues, priority trees, per-header
; allocations, stream-id hash lookups, data-provider callbacks. This engine
; does only what a unary gRPC server needs, directly on the connection's
; buffers; since ADR 0008 it is the server's only HTTP/2 implementation
; (nghttp2 remains an independent client and test oracle only).
;
;   input   frames are parsed in place from the worker's read buffer; a
;           partial frame is carried to the next read; header blocks are
;           decoded by hpack.asm, whose per-field "meanings" replace string
;           comparisons; request bodies go through grpc.asm's buffering and
;           budgets (kv_grpc_stream_append).
;   output  responses are framed straight into Conn.out_buf. The response
;           header blocks are precomputed: after a connection's first
;           response, HEADERS carry 2 bytes (:status 200 + content-type) and
;           the trailers 1 byte (grpc-status 0). A GET hit is copied from the
;           store record into its DATA frame with no intermediate buffer.
;   streams Upload DATA is handed to grpc.asm as it arrives (never buffered
;           here); a Download body is generated frame by frame by grpc.asm
;           (ST_GEN) as windows and output room allow, so a value of any
;           size costs a fixed amount of connection memory. A response that
;           completes while the peer is still sending (an upload refused
;           early) is followed by RST_STREAM(NO_ERROR), RFC 9113 §8.1.
;
; Protocol coverage: every RFC 9113 rule a server must enforce — connection
; preface, frame sizes, stream states (idle / open / half-closed / closed),
; stream-id ordering, MAX_CONCURRENT_STREAMS, flow control on both sides
; (including SETTINGS_INITIAL_WINDOW_SIZE changes and window overflow),
; CONTINUATION sequencing, padding, PRIORITY validation, malformed-request
; rules of §8, GOAWAY in both directions — plus hardening: rapid-reset
; budget (CVE-2023-44487), bounded header blocks and CONTINUATION counts
; (CVE-2024-27316 class), bounded output (OUT_HIGH_WATER, half-duplex
; back-pressure in worker.asm). h2spec is part of `make itest`.
;
; Errors: a connection error queues GOAWAY and sets H2F_CLOSING (input is
; then ignored; worker.asm closes the connection once the GOAWAY is sent).
; A stream error queues RST_STREAM and closes that stream only. Out of
; memory returns -1 to the worker, which destroys the connection.
;
; Threading: one worker thread owns a connection and everything here; no
; locks. The worker is QSBR-online while this code runs (store records stay
; valid while a GET's value is copied).
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"
%include "hpack.inc"

extern malloc
extern realloc
extern memcpy
extern memcmp
extern kv_keyeq
extern kv_pool_malloc
extern kv_pool_free
extern kv_pool_calloc
extern kv_pool_realloc
extern kv_hpack_init
extern kv_hpack_free
extern kv_hpack_decode
extern kv_grpc_dispatch
extern kv_grpc_run_batch
extern kv_grpc_stream_append
extern kv_grpc_stream_release
extern kv_grpc_upload_data
extern kv_grpc_gen_fill

; ---- frame types, flags, error codes, settings (RFC 9113 §6, §7) -----------------
%define FT_DATA             0
%define FT_HEADERS          1
%define FT_PRIORITY         2
%define FT_RST_STREAM       3
%define FT_SETTINGS         4
%define FT_PUSH_PROMISE     5
%define FT_PING             6
%define FT_GOAWAY           7
%define FT_WINDOW_UPDATE    8
%define FT_CONTINUATION     9

%define FF_END_STREAM       0x01
%define FF_ACK              0x01
%define FF_END_HEADERS      0x04
%define FF_PADDED           0x08
%define FF_PRIORITY         0x20

%define E_NO_ERROR          0x0
%define E_PROTOCOL          0x1
%define E_INTERNAL          0x2
%define E_FLOW_CONTROL      0x3
%define E_STREAM_CLOSED     0x5
%define E_FRAME_SIZE        0x6
%define E_REFUSED_STREAM    0x7
%define E_COMPRESSION       0x9
%define E_ENHANCE_YOUR_CALM 0xb

%define S_HEADER_TABLE_SIZE 1
%define S_ENABLE_PUSH       2
%define S_INITIAL_WINDOW    4
%define S_MAX_FRAME_SIZE    5
%define S_ENABLE_CONNECT    8           ; RFC 8441: boolean as well

%define FRAME_HDR           9
%define DEFAULT_WINDOW      65535       ; initial windows until SETTINGS say otherwise
%define FRAME_MIN           16384       ; our SETTINGS_MAX_FRAME_SIZE (never raised)
%define FRAME_MAX           16777215
%define HEADER_LIST_ADVICE  16384       ; SETTINGS_MAX_HEADER_LIST_SIZE we advertise
%define HPACK_TABLE         4096        ; SETTINGS_HEADER_TABLE_SIZE (the default, kept)

; ---- engine state ------------------------------------------------------------
%define H2_PREFACE          0           ; waiting for the 24-byte client preface
%define H2_SETTINGS         1           ; preface seen, first frame must be SETTINGS
%define H2_OPEN             2

%define H2F_CLOSING         1           ; connection error: GOAWAY queued, input ignored
%define H2F_GOAWAY_SENT     2           ; no new streams; close when the last one ends
%define H2F_GOAWAY_RECV     4           ; the peer is leaving: same
%define H2F_TSU             8           ; next header block starts with a size update to 0
; An early drain (out_try_drain) found the socket full during this read: no
; further attempt until the next read, so a slow reader costs one refused
; sendto per read instead of one per response. Cleared by kv_h2n_read.
%define H2F_OUT_FULL        16

%define MSG_NOSIGNAL        0x4000      ; sendto: a vanished peer must not SIGPIPE us

; Response encoder (our HPACK dynamic table as the peer's decoder sees it).
%define H2E_FRESH           0           ; empty
%define H2E_CT              1           ; [62] content-type: application/grpc
%define H2E_BOTH            2           ; [62] grpc-status: 0, [63] content-type
%define H2E_LITERAL         3           ; peer shrank its table: literals only

; Stream.h2flags
%define SN_REMOTE_CLOSED    1           ; END_STREAM received (half-closed remote)
%define SN_HEADERS_DONE     2           ; request header block accepted
%define SN_RESP_HEADERS     4           ; our response HEADERS are out
%define SN_PENDING          8           ; in the H2.pend list
%define SN_HEAD             16          ; :method HEAD (answer without a body)

%define CONT_SELF_DEP       0x100       ; block flag: HEADERS depended on its own stream

%define H2_SCRATCH          65536       ; per-worker Huffman output (>= table limit)
%define H2_MAX_BLOCK        65536       ; largest header block assembled from CONTINUATION
%define H2_MAX_CONTINUATION 64          ; CONTINUATION frames per header block
%define H2_NV_MAX           512         ; room for an encoded grpc.asm nv array
%define RESET_BURST         1000        ; rapid reset: burst, then 33 per second
%define RESET_NS_PER_TOKEN  30303030

; Header-decoding context handed to field_handler by decode_fields.
%define HC_TRAILERS         1           ; the block is trailers (no pseudo-headers)
%define HC_REGULAR          2           ; a regular field was seen
%define HC_BAD              4           ; the request is malformed (§8.1.1)
struc HCtx
    .stream:            resq 1          ; Stream*, or 0: decode for HPACK sync only
    .seen:              resq 1          ; 1 << HF code of each pseudo-header seen
    .state:             resq 1          ; HC_*
    .pad:               resq 2
endstruc

section .rodata
h2_magic:       db "PRI * HTTP/2.0", 13, 10, 13, 10, "SM", 13, 10, 13, 10
; Precomputed response header blocks (RFC 7541 Appendix C shows the forms).
; :status 200 (static 8) + content-type: application/grpc, first time: a
; literal with incremental indexing on name 31, which becomes entry 62.
blk_ok_fresh:   db 0x88, 0x5F, 0x10, "application/grpc"
blk_ok_fresh_len equ $ - blk_ok_fresh
; Same fields when the peer's table is off limits: literal without indexing.
blk_ok_lit:     db 0x88, 0x0F, 0x10, 0x10, "application/grpc"
blk_ok_lit_len  equ $ - blk_ok_lit
; grpc-status: 0 with incremental indexing: becomes 62, content-type moves to 63.
blk_tr_insert:  db 0x40, 0x0B, "grpc-status", 0x01, "0"
blk_tr_len      equ $ - blk_tr_insert
blk_tr_lit:     db 0x00, 0x0B, "grpc-status", 0x01, "0"
s_status:       db ":status"
s_ctype:        db "content-type"
v_200:          db "200"
v_grpc:         db "application/grpc"

section .text

global kv_h2n_open
global kv_h2n_close
global kv_h2n_read
global kv_h2n_pump
global kv_h2n_finished
global kv_h2n_goaway
global kv_h2n_reply_parts
global kv_h2n_reply_buf
global kv_h2n_reply_static
global kv_h2n_reply_headers
global kv_h2n_reply_stream

; =============================================================================
; kv_h2n_open — set up HTTP/2 on an accepted connection.
;
; @param  rdi  Conn* (zeroed by calloc; fd, worker, out_buf already set)
; @returns eax = 0, or -1 when out of memory (the caller closes the socket
;          after kv_h2n_close, which handles a half-initialized state).
; @remarks Queues the server connection preface: SETTINGS
;          (MAX_CONCURRENT_STREAMS from Config, a 1 MiB INITIAL_WINDOW_SIZE,
;          an advisory MAX_HEADER_LIST_SIZE), then a WINDOW_UPDATE raising
;          the connection window to the connection's buffering budget.
;          Allocates the worker's Huffman scratch on first use (freed with
;          the worker).
; =============================================================================
kv_h2n_open:
    push    rbx
    push    r12
    push    r13
    sub     rsp, 32                     ; [0..18) SETTINGS payload; 3 pushes + 32: aligned
    mov     rbx, rdi
    mov     r12, [rbx + Conn.worker]
    mov     r13, [r12 + Worker.cfg]
    cmp     qword [r12 + Worker.h2_scratch], 0
    jne     .have_scratch
    mov     edi, H2_SCRATCH
    call    malloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [r12 + Worker.h2_scratch], rax
.have_scratch:
    mov     qword [rbx + Conn.h2 + H2.state], H2_PREFACE
    mov     qword [rbx + Conn.h2 + H2.send_window], DEFAULT_WINDOW
    mov     qword [rbx + Conn.h2 + H2.peer_window], DEFAULT_WINDOW
    mov     qword [rbx + Conn.h2 + H2.peer_frame], FRAME_MIN
    mov     qword [rbx + Conn.h2 + H2.reset_tokens], RESET_BURST
    mov     rax, [r12 + Worker.now]
    mov     [rbx + Conn.h2 + H2.reset_stamp], rax
    ; Connection receive window = the per-connection buffering budget, capped
    ; at the protocol maximum: the peer may have that much in flight, which
    ; is exactly what kv_grpc_stream_append lets the connection buffer.
    mov     rax, [r13 + Config.conn_budget]
    mov     ecx, MAX_WINDOW
    cmp     rax, rcx
    cmova   rax, rcx
    mov     [rbx + Conn.h2 + H2.conn_window], rax
    mov     [rbx + Conn.h2 + H2.recv_window], rax
    lea     rdi, [rbx + Conn.h2 + H2.hpack]
    lea     rsi, [r12 + Worker.pool]
    mov     edx, HPACK_TABLE
    call    kv_hpack_init wrt ..plt
    test    rax, rax
    jnz     .fail
    ; Stream map: 16 slots to start; map_insert doubles it with concurrency.
    mov     edi, 16
    mov     esi, 8
    lea     rdx, [r12 + Worker.pool]
    call    kv_pool_calloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [rbx + Conn.h2 + H2.map], rax
    mov     qword [rbx + Conn.h2 + H2.map_mask], 15
    ; SETTINGS: {3: max_streams} {4: STREAM_WINDOW} {6: HEADER_LIST_ADVICE},
    ; identifiers and values big-endian.
    mov     word [rsp], 0x0300
    mov     eax, [r13 + Config.max_streams]
    bswap   eax
    mov     [rsp + 2], eax
    mov     word [rsp + 6], 0x0400
    mov     eax, STREAM_WINDOW
    bswap   eax
    mov     [rsp + 8], eax
    mov     word [rsp + 12], 0x0600
    mov     eax, HEADER_LIST_ADVICE
    bswap   eax
    mov     [rsp + 14], eax
    mov     rdi, rbx
    mov     esi, FT_SETTINGS
    xor     edx, edx
    xor     ecx, ecx
    mov     r8, rsp
    mov     r9d, 18
    call    write_frame
    test    eax, eax
    jnz     .fail
    mov     rdx, [rbx + Conn.h2 + H2.conn_window]
    sub     rdx, DEFAULT_WINDOW
    jbe     .ok                         ; budget <= default: nothing to announce
    mov     rdi, rbx
    xor     esi, esi
    call    send_window_update
    test    eax, eax
    jnz     .fail
.ok:
    xor     eax, eax
    jmp     .out
.fail:
    mov     eax, -1
.out:
    add     rsp, 32
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_h2n_close — release the engine's per-connection memory. Idempotent.
;
; @param  rdi  Conn*
; @returns nothing.
; @remarks Streams are NOT freed here: they sit on Conn.streams and the
;          worker frees them with kv_grpc_free_streams right after.
;          Called from linger and destroy paths, possibly both, so every
;          pointer is cleared after its release.
; =============================================================================
kv_h2n_close:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, [rbx + Conn.worker]
    lea     rdi, [rbx + Conn.h2 + H2.hpack]
    call    kv_hpack_free wrt ..plt     ; no-op when never initialized
    mov     rdi, [rbx + Conn.h2 + H2.map]
    lea     rsi, [r12 + Worker.pool]
    call    kv_pool_free wrt ..plt      ; NULL is ignored
    mov     rdi, [rbx + Conn.h2 + H2.hb_buf]
    lea     rsi, [r12 + Worker.pool]
    call    kv_pool_free wrt ..plt
    mov     rdi, [rbx + Conn.h2 + H2.carry]
    lea     rsi, [r12 + Worker.pool]
    call    kv_pool_free wrt ..plt
    xor     eax, eax
    mov     [rbx + Conn.h2 + H2.map], rax
    mov     [rbx + Conn.h2 + H2.hb_buf], rax
    mov     [rbx + Conn.h2 + H2.hb_cap], rax
    mov     [rbx + Conn.h2 + H2.carry], rax
    mov     [rbx + Conn.h2 + H2.carry_cap], rax
    mov     [rbx + Conn.h2 + H2.carry_len], rax
    mov     [rbx + Conn.h2 + H2.pend_head], rax
    mov     [rbx + Conn.h2 + H2.pend_tail], rax
    mov     [rbx + Conn.h2 + H2.nstreams], rax
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_h2n_finished — may the worker close this connection gracefully now?
;
; @param  rdi  Conn*
; @returns eax = 1 after a connection error, or once GOAWAY was sent or
;          received and no stream remains; else 0.
; @remarks The worker asks only when all output has been sent, so "1" means
;          the final GOAWAY (if any) is already on the wire: the session
;          wants neither to read nor to write any more.
; =============================================================================
kv_h2n_finished:
    mov     rax, [rdi + Conn.h2 + H2.flags]
    test    eax, H2F_CLOSING
    jnz     .yes
    test    eax, H2F_GOAWAY_SENT | H2F_GOAWAY_RECV
    jz      .no
    cmp     qword [rdi + Conn.h2 + H2.nstreams], 0
    jne     .no
.yes:
    mov     eax, 1
    ret
.no:
    xor     eax, eax
    ret

; =============================================================================
; kv_h2n_goaway — queue GOAWAY(last stream id, code) (drain, idle timeout).
;
; @param  rdi  Conn*   @param esi  error code   @param edx  terminate: 1 =
;              close as soon as it is sent (idle timeout), 0 = let the
;              streams already accepted finish (graceful drain)
; @returns eax = 0, or -1 when out of memory.
; @remarks After a graceful GOAWAY new streams are ignored (RFC 9113 §6.8);
;          the peer retries them elsewhere. Nothing is sent after a
;          connection error (already closing).
; =============================================================================
kv_h2n_goaway:
    test    qword [rdi + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .done
    mov     eax, H2F_GOAWAY_SENT
    mov     ecx, H2F_GOAWAY_SENT | H2F_CLOSING
    test    edx, edx
    cmovnz  eax, ecx
    or      [rdi + Conn.h2 + H2.flags], rax
    jmp     send_goaway                 ; tail call: esi = code
.done:
    xor     eax, eax
    ret

; =============================================================================
; kv_h2n_read — read one chunk from a connection and process it.
;
; @param  rdi  Worker*   @param rsi  Conn*
; @returns rax = 0 (processed or nothing to read: flush next), -1 (destroy
;          the connection now: the peer closed, a socket error, or out of
;          memory).
; @remarks A partial frame from the previous read is copied in front of the
;          new bytes (at most one frame, <= 16,393 bytes, so READ_CHUNK still
;          leaves room for ~240 KiB of fresh input). After the frames: the GETs
;          they queued are answered as one prefetched batch (grpc.asm), and
;          the connection receive window is topped up with one WINDOW_UPDATE.
;          After a connection error, input is read and discarded until the
;          worker closes (the final GOAWAY is flushed first).
; =============================================================================
kv_h2n_read:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    sub     rsp, 16                     ; 5 pushes + 16: aligned for calls
    mov     r14, rdi                    ; Worker*
    mov     rbx, rsi                    ; Conn*
    ; A new read: the previous pass's flush has run since the last refused
    ; early drain, so the socket may take output again (out_try_drain).
    and     qword [rbx + Conn.h2 + H2.flags], ~H2F_OUT_FULL
    mov     r12, [r14 + Worker.read_buf]
    mov     r13, [rbx + Conn.h2 + H2.carry_len]
    test    r13, r13
    jz      .read
    mov     rdi, r12
    mov     rsi, [rbx + Conn.h2 + H2.carry]
    mov     rdx, r13
    call    memcpy wrt ..plt
.read:
    mov     rdi, [rbx + Conn.fd]
    lea     rsi, [r12 + r13]
    mov     edx, READ_CHUNK
    sub     rdx, r13
    xor     eax, eax                    ; SYS_read
    syscall
    ; EAGAIN/EINTR: nothing lost — the carry is still in H2.carry.
    cmp     rax, -EAGAIN
    je      .ok
    cmp     rax, -EINTR
    je      .ok
    test    rax, rax
    jle     .fatal                      ; 0 = peer closed, < 0 = reset/error
    mov     rcx, [r14 + Worker.now]
    mov     [rbx + Conn.last_active], rcx
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .ok
    lea     rdx, [r13 + rax]            ; carried + new bytes
    mov     qword [rbx + Conn.h2 + H2.carry_len], 0
    mov     rdi, rbx
    mov     rsi, r12
    call    h2_process
    test    eax, eax
    js      .fatal
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .ok                         ; failing connection: its batch is dropped with it
    mov     rdi, rbx
    call    kv_grpc_run_batch wrt ..plt
    test    eax, eax
    js      .fatal
    mov     rdi, rbx
    call    conn_window_refill
    test    eax, eax
    js      .fatal
.ok:
    xor     eax, eax
    jmp     .out
.fatal:
    mov     rax, -1
.out:
    add     rsp, 16
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; conn_window_refill — top up the connection receive window. Internal.
; @param  rdi  Conn*
; @returns eax = 0, or -1 when out of memory.
; @remarks Once more than half of the window is used, one WINDOW_UPDATE
;          restores it in full (the usual HTTP/2 receiver strategy: few
;          updates, and the sender never stalls on a half-empty window). Request
;          memory is bounded by the budgets in kv_grpc_stream_append, not by
;          withholding window.
; =============================================================================
conn_window_refill:
    mov     rax, [rdi + Conn.h2 + H2.recv_window]
    mov     rdx, [rdi + Conn.h2 + H2.conn_window]
    mov     rcx, rdx
    shr     rcx, 1
    cmp     rax, rcx
    jge     .none
    sub     rdx, rax                    ; increment back to the full window
    mov     rax, [rdi + Conn.h2 + H2.conn_window]
    mov     [rdi + Conn.h2 + H2.recv_window], rax
    xor     esi, esi
    jmp     send_window_update          ; tail call
.none:
    xor     eax, eax
    ret

; =============================================================================
; h2_process — run every complete frame in a buffer. Internal.
;
; @param  rdi  Conn*   @param rsi  bytes   @param rdx  length (> 0)
; @returns eax = 0 (protocol errors are handled: RST_STREAM or GOAWAY
;          queued), or -1 when out of memory.
; @remarks Registers across the frame loop: rbx Conn*, r12 frame start,
;          r13 end of input, r14 payload, r15 payload length, rbp the
;          frame's Stream* inside handlers. Frame header fields live in the
;          stack slots below. Whatever remains of an incomplete frame is
;          saved in H2.carry.
;          Fused request: a unary call arrives as HEADERS then DATA, and on
;          a warm connection both are in the same read. When a HEADERS frame
;          has just opened a stream (header_block returns it) and the next
;          frame in the buffer is that stream's DATA frame — whole, not
;          padded, flags exactly END_STREAM, a non-empty body small enough to
;          borrow, no declared content-length, not an Upload — it is handled
;          right there (.f_headers) instead of going round the frame loop:
;          no second header parse, no sequencing checks, no map lookup, no
;          request_end. The steps taken are the DATA handler's own, in its
;          order (connection window, stream window, data_len, borrow,
;          SN_REMOTE_CLOSED, dispatch), so the result is the same state; any
;          frame that fails one condition is left untouched for the loop,
;          which then does everything it always did — errors included.
;          Measured with one request per read (callgrind): the frame loop
;          went from 184 to 138 instructions per request, and the DATA
;          frame's map lookup (11) and request_end (5) are gone; in process
;          a GET is 9.9% shorter and a health check 15.2%
;          (benchmarks/results/2026-11-15-asm-audit).
;          It applies only when the DATA frame is next in the buffer. A
;          client that sends the HEADERS frames of several requests before
;          any of their DATA frames (nghttp2 does, when the requests are
;          submitted before one send) is served by the loop as before, at
;          the price of the failed look-ahead: 8 instructions per request.
; =============================================================================
%define L_FLAGS     0
%define L_SID       8
%define L_TYPE      16
%define L_T0        24
%define L_T1        32
h2_process:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 40                     ; 6 pushes + 40: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    lea     r13, [rsi + rdx]
    cmp     qword [rbx + Conn.h2 + H2.state], H2_PREFACE
    jne     .frame
    ; ---- client connection preface (§3.4): compare what has arrived ----
    mov     rdx, r13
    sub     rdx, r12
    mov     eax, 24
    cmp     rdx, rax
    cmova   rdx, rax
    mov     rdi, r12
    lea     rsi, [h2_magic]
    call    memcmp wrt ..plt
    test    eax, eax
    jnz     .protocol_error             ; not HTTP/2 (e.g. an HTTP/1.1 request)
    mov     rax, r13
    sub     rax, r12
    cmp     rax, 24
    jb      .carry                      ; correct so far: wait for the rest
    add     r12, 24
    mov     qword [rbx + Conn.h2 + H2.state], H2_SETTINGS

.frame:
    mov     rax, r13
    sub     rax, r12
    cmp     rax, FRAME_HDR
    jb      .carry
    mov     eax, [r12]
    bswap   eax                         ; length(24) | type(8)
    movzx   edx, al
    shr     eax, 8
    mov     r15, rax                    ; payload length
    mov     [rsp + L_TYPE], rdx
    movzx   ecx, byte [r12 + 4]
    mov     [rsp + L_FLAGS], rcx
    mov     ecx, [r12 + 5]
    bswap   ecx
    and     ecx, 0x7FFFFFFF             ; the reserved bit is ignored (§4.1)
    mov     [rsp + L_SID], rcx
    ; Larger than our SETTINGS_MAX_FRAME_SIZE: connection error (§4.2); the
    ; check comes before waiting for the payload, so nothing oversized is
    ; ever buffered.
    cmp     r15, FRAME_MIN
    ja      .frame_size_error
    mov     rax, r13
    sub     rax, r12
    lea     rcx, [r15 + FRAME_HDR]
    cmp     rax, rcx
    jb      .carry                      ; payload not complete yet
    lea     r14, [r12 + FRAME_HDR]
    ; The first frame after the preface must be SETTINGS without ACK (§3.4).
    cmp     qword [rbx + Conn.h2 + H2.state], H2_SETTINGS
    jne     .sequencing
    cmp     edx, FT_SETTINGS
    jne     .protocol_error
    test    byte [rsp + L_FLAGS], FF_ACK
    jnz     .protocol_error
.sequencing:
    ; Inside a header block only its own CONTINUATION frames may follow
    ; (§6.10) — checked before the type, so unknown types are caught too.
    mov     rax, [rbx + Conn.h2 + H2.cont_id]
    test    rax, rax
    jz      .no_block
    cmp     edx, FT_CONTINUATION
    jne     .protocol_error
    cmp     rax, [rsp + L_SID]
    jne     .protocol_error
    jmp     .f_continuation
.no_block:
    ; Dispatch, most frequent types first.
    cmp     edx, FT_HEADERS
    je      .f_headers
    cmp     edx, FT_DATA
    je      .f_data
    cmp     edx, FT_WINDOW_UPDATE
    je      .f_window
    cmp     edx, FT_SETTINGS
    je      .f_settings
    cmp     edx, FT_PING
    je      .f_ping
    cmp     edx, FT_RST_STREAM
    je      .f_rst
    cmp     edx, FT_PRIORITY
    je      .f_priority
    cmp     edx, FT_GOAWAY
    je      .f_goaway
    cmp     edx, FT_CONTINUATION
    je      .protocol_error             ; no header block is open
    cmp     edx, FT_PUSH_PROMISE
    je      .protocol_error             ; clients cannot push (§8.4)
    ; Unknown frame types are ignored (§4.1, §5.5).

.next:
    lea     r12, [r14 + r15]
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jz      .frame
    jmp     .done                       ; after a connection error nothing else runs

.carry:
    ; Keep the incomplete frame (or preface) for the next read.
    mov     r15, r13
    sub     r15, r12
    jz      .done
    cmp     r15, [rbx + Conn.h2 + H2.carry_cap]
    jbe     .carry_copy
    mov     rdi, [rbx + Conn.h2 + H2.carry]
    mov     rsi, r15
    mov     rdx, [rbx + Conn.worker]
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_realloc wrt ..plt
    test    rax, rax
    jz      .fatal
    mov     [rbx + Conn.h2 + H2.carry], rax
    mov     [rbx + Conn.h2 + H2.carry_cap], r15
.carry_copy:
    mov     rdi, [rbx + Conn.h2 + H2.carry]
    mov     rsi, r12
    mov     rdx, r15
    call    memcpy wrt ..plt
    mov     [rbx + Conn.h2 + H2.carry_len], r15
.done:
    xor     eax, eax
    jmp     .out

.protocol_error:
    mov     esi, E_PROTOCOL
    jmp     .conn_error
.frame_size_error:
    mov     esi, E_FRAME_SIZE
    jmp     .conn_error
.flow_error:
    mov     esi, E_FLOW_CONTROL
    jmp     .conn_error
.calm_error:
    mov     esi, E_ENHANCE_YOUR_CALM
.conn_error:
    mov     rdi, rbx
    call    conn_error
    test    eax, eax
    jns     .done
.fatal:
    mov     eax, -1
.out:
    add     rsp, 40
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

    ; ---------------------------------------------------------------- HEADERS
.f_headers:
    mov     ecx, [rsp + L_SID]
    test    ecx, ecx
    jz      .protocol_error             ; HEADERS on stream 0 (§6.2)
    mov     rsi, r14                    ; fragment
    mov     rdx, r15                    ; fragment length
    xor     r8d, r8d                    ; pad length
    test    byte [rsp + L_FLAGS], FF_PADDED
    jz      .h_unpadded
    test    rdx, rdx
    jz      .frame_size_error           ; no room for the pad length (§4.2)
    movzx   r8d, byte [rsi]
    inc     rsi
    dec     rdx
.h_unpadded:
    xor     r9d, r9d                    ; block flags being built
    test    byte [rsp + L_FLAGS], FF_PRIORITY
    jz      .h_no_priority
    cmp     rdx, 5
    jb      .frame_size_error
    mov     eax, [rsi]
    bswap   eax
    and     eax, 0x7FFFFFFF             ; stream dependency
    cmp     eax, ecx
    jne     .h_priority_ok
    ; Depending on itself is a stream error (§5.3.1), raised after the block
    ; is decoded: the HPACK state must follow every block regardless.
    mov     r9d, CONT_SELF_DEP
.h_priority_ok:
    add     rsi, 5
    sub     rdx, 5
.h_no_priority:
    cmp     r8, rdx
    ja      .protocol_error             ; padding beyond the payload (§6.2)
    sub     rdx, r8
    movzx   eax, byte [rsp + L_FLAGS]
    and     eax, FF_END_STREAM
    or      r9d, eax
    test    byte [rsp + L_FLAGS], FF_END_HEADERS
    jz      .h_open_block
    mov     rdi, rbx
    mov     r8d, r9d
    mov     rcx, rdx
    mov     rdx, rsi
    mov     esi, [rsp + L_SID]
    call    header_block
    test    eax, eax
    js      .fatal
    ; ---- fused request: this stream's DATA frame, when it is right behind ----
    ; rdx = the stream the block opened and left open, or 0 (see @remarks).
    ; Every "jmp/jcc .next" below leaves the next frame untouched: the loop
    ; parses it from scratch, so nothing here needs to be undone.
    test    rdx, rdx
    jz      .next
    lea     rax, [r14 + r15]            ; where the next frame starts
    ; Its header must have arrived whole before any of it is read: the bytes
    ; past r13 are whatever an earlier read left in the buffer, or lie beyond
    ; the buffer's end. The payload check further down would refuse the very
    ; same frames, so no black-box test can tell that this check exists
    ; (benchmarks/results/2026-11-15-asm-audit, mutate.sh) — it is here for
    ; the reads, not for the decision, and must stay.
    lea     rcx, [rax + FRAME_HDR]
    cmp     rcx, r13
    ja      .next                       ; its header is not whole: the loop carries it over
    cmp     word [rax + 3], FT_DATA | (FF_END_STREAM << 8)
    jne     .next                       ; another type, or flags other than exactly END_STREAM
    mov     ecx, [rax + 5]
    cmp     ecx, [r12 + 5]
    jne     .next                       ; another stream (ids as sent, the reserved bit included)
    mov     ecx, [rax]
    bswap   ecx
    shr     ecx, 8                      ; payload length
    jz      .next                       ; no body: nothing to borrow
    cmp     ecx, FRAME_MIN
    ja      .next                       ; oversized: the loop raises FRAME_SIZE_ERROR
    lea     rsi, [rax + FRAME_HDR]      ; its payload
    lea     rax, [rsi + rcx]
    cmp     rax, r13
    ja      .next                       ; the payload is not whole yet
    cmp     qword [rdx + Stream.method], M_UPLOAD
    je      .next                       ; client streaming consumes DATA as it arrives
    cmp     qword [rdx + Stream.clen], 0
    jge     .next                       ; a declared content-length: the loop checks it
    ; Three of the loop's checks cannot fail here and are not repeated: the
    ; connection is not closing (kv_h2n_read never enters with H2F_CLOSING
    ; set, and a block that raised a connection error returned no stream);
    ; and this stream has had no DATA yet, so nothing is buffered
    ; (req_len 0) and ST_TOO_LARGE — which only kv_grpc_stream_append sets —
    ; is clear.
    mov     rax, [rbx + Conn.worker]
    mov     rax, [rax + Worker.cfg]
    mov     rax, [rax + Config.max_request]
    add     rax, GRPC_PREFIX
    cmp     rcx, rax
    ja      .next                       ; beyond max_request: the copy path marks it
    ; Committed. From here on this is .f_data for that frame, step for step.
    sub     [rbx + Conn.h2 + H2.recv_window], rcx
    js      .flow_error                 ; (§6.9; the frame counts whatever the stream's state)
    mov     rbp, rdx                    ; the frame's stream
    mov     r14, rsi                    ; its payload and length: .next steps over
    mov     r15, rcx                    ; the DATA frame
    ; A fresh stream's window is STREAM_WINDOW and one frame is at most
    ; FRAME_MIN (asserted below), so it cannot go negative here.
    sub     [rbp + Stream.recv_window], r15
    mov     [rbp + Stream.data_len], r15 ; (it was 0: this is the stream's first DATA)
    mov     [rbp + Stream.req_buf], r14 ; borrowed: the read buffer outlives dispatch
    mov     [rbp + Stream.req_len], r15 ; and the batch (ST_BORROWED)
    or      qword [rbp + Stream.flags], ST_BORROWED
    or      qword [rbp + Stream.h2flags], SN_REMOTE_CLOSED ; request_end's step
    mov     rdi, rbx
    mov     rsi, rbp
    call    kv_grpc_dispatch wrt ..plt  ; may answer and free the stream
    test    eax, eax
    js      .fatal
    jmp     .next
%if FRAME_MIN > STREAM_WINDOW
    %error "the fused request path assumes one DATA frame fits a fresh stream window"
%endif
.h_open_block:
    ; The block continues in CONTINUATION frames: assemble it.
    mov     [rbx + Conn.h2 + H2.cont_flags], r9
    xor     eax, eax
    mov     [rbx + Conn.h2 + H2.hb_len], rax
    mov     [rbx + Conn.h2 + H2.cont_count], rax
    mov     rdi, rbx
    call    hb_append                   ; rsi = fragment, rdx = length
    test    eax, eax
    js      .fatal
    jnz     .calm_error
    mov     rax, [rsp + L_SID]
    mov     [rbx + Conn.h2 + H2.cont_id], rax
    jmp     .next

    ; ----------------------------------------------------------- CONTINUATION
.f_continuation:
    ; (the sequencing check proved: a block is open and this frame is its)
    inc     qword [rbx + Conn.h2 + H2.cont_count]
    cmp     qword [rbx + Conn.h2 + H2.cont_count], H2_MAX_CONTINUATION
    ja      .calm_error
    mov     rdi, rbx
    mov     rsi, r14
    mov     rdx, r15
    call    hb_append
    test    eax, eax
    js      .fatal
    jnz     .calm_error
    test    byte [rsp + L_FLAGS], FF_END_HEADERS
    jz      .next
    mov     rdi, rbx
    mov     esi, [rbx + Conn.h2 + H2.cont_id]
    mov     rdx, [rbx + Conn.h2 + H2.hb_buf]
    mov     rcx, [rbx + Conn.h2 + H2.hb_len]
    mov     r8d, [rbx + Conn.h2 + H2.cont_flags]
    mov     qword [rbx + Conn.h2 + H2.cont_id], 0
    call    header_block
    test    eax, eax
    js      .fatal
    jmp     .next

    ; ------------------------------------------------------------------- DATA
.f_data:
    mov     ecx, [rsp + L_SID]
    test    ecx, ecx
    jz      .protocol_error             ; DATA on stream 0 (§6.1)
    mov     rsi, r14
    mov     rdx, r15
    test    byte [rsp + L_FLAGS], FF_PADDED
    jz      .d_unpadded
    test    r15, r15
    jz      .frame_size_error
    movzx   eax, byte [r14]
    cmp     rax, r15
    jae     .protocol_error             ; padding covers the payload (§6.1)
    inc     rsi
    lea     rdx, [r15 - 1]
    sub     rdx, rax
.d_unpadded:
    mov     [rsp + L_T0], rsi           ; data
    mov     [rsp + L_T1], rdx           ; data length
    ; The whole payload, padding included, counts against the connection
    ; window whatever the stream's state (§6.9).
    sub     [rbx + Conn.h2 + H2.recv_window], r15
    js      .flow_error
    mov     rdi, rbx
    mov     esi, [rsp + L_SID]
    call    map_find
    test    rax, rax
    jz      .d_no_stream
    mov     rbp, rax
    test    qword [rbp + Stream.h2flags], SN_REMOTE_CLOSED
    jnz     .d_closed                   ; half-closed (remote): STREAM_CLOSED (§5.1)
    sub     [rbp + Stream.recv_window], r15
    js      .d_flow
    ; content-length (§8.1.1): more DATA than declared is malformed
    mov     rax, [rsp + L_T1]
    add     rax, [rbp + Stream.data_len]
    mov     [rbp + Stream.data_len], rax
    mov     rcx, [rbp + Stream.clen]
    test    rcx, rcx
    js      .d_append
    cmp     rax, rcx
    ja      .d_malformed
.d_append:
    mov     rcx, [rsp + L_T1]
    ; Upload: grpc.asm consumes the bytes as they arrive — it decodes the
    ; message stream and writes the value straight into its record — so this
    ; stream never buffers or borrows a body.
    cmp     qword [rbp + Stream.method], M_UPLOAD
    je      .d_upload
    test    rcx, rcx
    jz      .d_appended
    ; Zero copy: a whole body in this one frame (nothing buffered before,
    ; END_STREAM here, within max_request) is dispatched in place. The read
    ; buffer outlives everything that uses it: dispatch, and the GET batch
    ; answered before kv_h2n_read returns (ST_BORROWED).
    test    byte [rsp + L_FLAGS], FF_END_STREAM
    jz      .d_copy
    cmp     qword [rbp + Stream.req_len], 0
    jne     .d_copy
    test    qword [rbp + Stream.flags], ST_TOO_LARGE
    jnz     .d_copy
    mov     rax, [rbx + Conn.worker]
    mov     rax, [rax + Worker.cfg]
    mov     rax, [rax + Config.max_request]
    add     rax, GRPC_PREFIX
    cmp     rcx, rax
    ja      .d_copy                     ; too large: the copy path marks it
    mov     rax, [rsp + L_T0]
    mov     [rbp + Stream.req_buf], rax
    mov     [rbp + Stream.req_len], rcx
    or      qword [rbp + Stream.flags], ST_BORROWED
    jmp     .d_appended
.d_copy:
    mov     rdi, rbx
    mov     rsi, rbp
    mov     rdx, [rsp + L_T0]
    call    kv_grpc_stream_append wrt ..plt
    test    eax, eax
    jnz     .d_calm                     ; over the connection's buffering budget
.d_appended:
    test    byte [rsp + L_FLAGS], FF_END_STREAM
    jnz     .d_end
    ; Top the stream window up once half of it is used (large requests).
    mov     rax, [rbp + Stream.recv_window]
    cmp     rax, STREAM_WINDOW / 2
    jge     .next
    mov     edx, STREAM_WINDOW
    sub     rdx, rax
    mov     qword [rbp + Stream.recv_window], STREAM_WINDOW
    mov     rdi, rbx
    mov     esi, [rsp + L_SID]
    call    send_window_update
    test    eax, eax
    js      .fatal
    jmp     .next
.d_end:
    mov     rdi, rbx
    mov     rsi, rbp
    call    request_end                 ; may answer and free the stream
    test    eax, eax
    js      .fatal
    jmp     .next
.d_upload:
    test    rcx, rcx
    jz      .d_appended                 ; an empty DATA frame: nothing to decode
    mov     rdi, rbx
    mov     rsi, rbp
    mov     rdx, [rsp + L_T0]
    call    kv_grpc_upload_data wrt ..plt
    test    eax, eax
    js      .fatal                      ; out of memory: the connection ends
    jnz     .next                       ; refused early: the stream is closed and freed
    jmp     .d_appended                 ; END_STREAM check, stream window top-up
.d_no_stream:
    mov     eax, [rsp + L_SID]
    cmp     rax, [rbx + Conn.h2 + H2.last_id]
    ja      .d_idle
    lea     rdi, [rbx + Conn.h2 + H2.rst_ring]
    mov     esi, [rsp + L_SID]
    call    ring_has
    test    eax, eax
    jnz     .next                       ; we reset it: frames in flight are ignored
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, [rsp + L_SID]
    mov     ecx, E_STREAM_CLOSED        ; closed stream (§5.1)
    call    stream_error
    test    eax, eax
    js      .fatal
    jmp     .next
.d_idle:
    test    qword [rbx + Conn.h2 + H2.flags], H2F_GOAWAY_SENT
    jnz     .next                       ; a stream we ignored after GOAWAY (§6.8)
    jmp     .protocol_error             ; DATA on an idle stream (§5.1)
.d_closed:
    mov     ecx, E_STREAM_CLOSED
    jmp     .d_stream_error
.d_flow:
    mov     ecx, E_FLOW_CONTROL
    jmp     .d_stream_error
.d_malformed:
    mov     ecx, E_PROTOCOL
    jmp     .d_stream_error
.d_calm:
    mov     ecx, E_ENHANCE_YOUR_CALM
.d_stream_error:
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, [rsp + L_SID]
    call    stream_error
    test    eax, eax
    js      .fatal
    jmp     .next

    ; ---------------------------------------------------------- WINDOW_UPDATE
.f_window:
    cmp     r15, 4
    jne     .frame_size_error           ; (§6.9)
    mov     eax, [r14]
    bswap   eax
    and     eax, 0x7FFFFFFF             ; increment
    mov     ecx, [rsp + L_SID]
    test    ecx, ecx
    jnz     .w_stream
    test    eax, eax
    jz      .protocol_error             ; zero increment on the connection
    add     rax, [rbx + Conn.h2 + H2.send_window]
    cmp     rax, MAX_WINDOW
    jg      .flow_error                 ; beyond 2^31 - 1 (§6.9.1)
    mov     [rbx + Conn.h2 + H2.send_window], rax
    jmp     .next
.w_stream:
    cmp     rcx, [rbx + Conn.h2 + H2.last_id]
    ja      .w_idle
    mov     [rsp + L_T0], rax
    mov     rdi, rbx
    mov     esi, ecx
    call    map_find
    test    rax, rax
    jz      .next                       ; closed stream: ignored (§5.1)
    mov     rbp, rax
    mov     rax, [rsp + L_T0]
    mov     ecx, E_PROTOCOL
    test    rax, rax
    jz      .w_stream_error             ; zero increment on a stream
    add     rax, [rbp + Stream.send_window]
    mov     ecx, E_FLOW_CONTROL
    cmp     rax, MAX_WINDOW
    jg      .w_stream_error
    mov     [rbp + Stream.send_window], rax
    jmp     .next                       ; the worker's flush pumps blocked streams
.w_stream_error:
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, [rsp + L_SID]
    call    stream_error
    test    eax, eax
    js      .fatal
    jmp     .next
.w_idle:
    test    qword [rbx + Conn.h2 + H2.flags], H2F_GOAWAY_SENT
    jnz     .next
    jmp     .protocol_error             ; WINDOW_UPDATE on an idle stream (§5.1)

    ; --------------------------------------------------------------- SETTINGS
.f_settings:
    cmp     qword [rsp + L_SID], 0
    jne     .protocol_error             ; (§6.5)
    test    byte [rsp + L_FLAGS], FF_ACK
    jz      .s_new
    test    r15, r15
    jnz     .frame_size_error           ; an ACK carries nothing
    jmp     .next                       ; our settings are acknowledged
.s_new:
    mov     rax, r15
    xor     edx, edx
    mov     ecx, 6
    div     rcx
    test    rdx, rdx
    jnz     .frame_size_error
    mov     rbp, r14                    ; entry cursor
.s_entry:
    lea     rax, [r14 + r15]
    cmp     rbp, rax
    jae     .s_done
    movzx   eax, word [rbp]
    xchg    al, ah                      ; identifier
    mov     ecx, [rbp + 2]
    bswap   ecx                         ; value
    add     rbp, 6
    cmp     eax, S_HEADER_TABLE_SIZE
    je      .s_table
    cmp     eax, S_ENABLE_PUSH
    je      .s_bool
    cmp     eax, S_INITIAL_WINDOW
    je      .s_window
    cmp     eax, S_MAX_FRAME_SIZE
    je      .s_frame
    cmp     eax, S_ENABLE_CONNECT
    je      .s_bool
    jmp     .s_entry                    ; nothing to do / unknown: ignored (§6.5.2)
.s_table:
    ; The peer's decoder shrank below the 4,096 bytes our encoder may use:
    ; announce size 0 in our next header block and use literals from then
    ; on (RFC 7541 §4.2). Rare, so no attempt to keep the two entries.
    cmp     ecx, HPACK_TABLE
    jae     .s_entry
    cmp     qword [rbx + Conn.h2 + H2.enc], H2E_LITERAL
    je      .s_entry
    mov     qword [rbx + Conn.h2 + H2.enc], H2E_LITERAL
    or      qword [rbx + Conn.h2 + H2.flags], H2F_TSU
    jmp     .s_entry
.s_bool:
    cmp     ecx, 1
    ja      .protocol_error             ; ENABLE_PUSH / ENABLE_CONNECT_PROTOCOL: 0 or 1
    jmp     .s_entry
.s_frame:
    cmp     ecx, FRAME_MIN
    jb      .protocol_error
    cmp     ecx, FRAME_MAX
    ja      .protocol_error
    mov     [rbx + Conn.h2 + H2.peer_frame], rcx
    jmp     .s_entry
.s_window:
    cmp     ecx, MAX_WINDOW
    ja      .flow_error                 ; (§6.5.2)
    mov     rdi, rbx
    mov     esi, ecx
    call    apply_initial_window        ; every stream moves by the difference
    test    eax, eax
    jnz     .flow_error                 ; a window would exceed 2^31 - 1 (§6.9.2)
    jmp     .s_entry
.s_done:
    mov     qword [rbx + Conn.h2 + H2.state], H2_OPEN
    mov     rdi, rbx
    mov     esi, FT_SETTINGS
    mov     edx, FF_ACK
    xor     ecx, ecx
    xor     r8d, r8d
    xor     r9d, r9d
    call    write_frame
    test    eax, eax
    js      .fatal
    jmp     .next

    ; ------------------------------------------------------------------- PING
.f_ping:
    cmp     qword [rsp + L_SID], 0
    jne     .protocol_error             ; (§6.7)
    cmp     r15, 8
    jne     .frame_size_error
    test    byte [rsp + L_FLAGS], FF_ACK
    jnz     .next
    mov     rdi, rbx
    mov     esi, FT_PING
    mov     edx, FF_ACK
    xor     ecx, ecx
    mov     r8, r14                     ; echo the opaque data
    mov     r9d, 8
    call    write_frame
    test    eax, eax
    js      .fatal
    jmp     .next

    ; ------------------------------------------------------------- RST_STREAM
.f_rst:
    mov     ecx, [rsp + L_SID]
    test    ecx, ecx
    jz      .protocol_error             ; (§6.4)
    cmp     r15, 4
    jne     .frame_size_error
    cmp     rcx, [rbx + Conn.h2 + H2.last_id]
    ja      .r_idle
    mov     rdi, rbx
    mov     esi, ecx
    call    map_find
    test    rax, rax
    jz      .next                       ; already closed
    mov     rbp, rax
    mov     rdi, rbx
    call    reset_budget
    test    eax, eax
    jnz     .calm_error                 ; rapid reset (CVE-2023-44487)
    mov     rdi, rbx
    mov     rsi, rbp
    call    stream_close                ; joins closed_ring: later frames → STREAM_CLOSED
    jmp     .next
.r_idle:
    test    qword [rbx + Conn.h2 + H2.flags], H2F_GOAWAY_SENT
    jnz     .next
    jmp     .protocol_error             ; RST_STREAM on an idle stream (§6.4)

    ; --------------------------------------------------------------- PRIORITY
.f_priority:
    mov     ecx, [rsp + L_SID]
    test    ecx, ecx
    jz      .protocol_error             ; (§6.3)
    mov     edx, E_FRAME_SIZE
    cmp     r15, 5
    jne     .p_error
    mov     eax, [r14]
    bswap   eax
    and     eax, 0x7FFFFFFF
    cmp     eax, ecx
    jne     .next                       ; valid priorities are advisory: unused (§5.3.2)
    mov     edx, E_PROTOCOL             ; depends on itself (§5.3.1)
.p_error:
    ; A stream error — but RST_STREAM must never name an idle stream (§5.1),
    ; so on an idle stream it becomes a connection error.
    cmp     rcx, [rbx + Conn.h2 + H2.last_id]
    ja      .p_idle
    mov     [rsp + L_T0], rdx
    mov     rdi, rbx
    mov     esi, ecx
    call    map_find
    mov     rdi, rbx
    mov     rsi, rax                    ; Stream* or 0
    mov     edx, [rsp + L_SID]
    mov     rcx, [rsp + L_T0]
    call    stream_error
    test    eax, eax
    js      .fatal
    jmp     .next
.p_idle:
    mov     esi, edx
    jmp     .conn_error

    ; ----------------------------------------------------------------- GOAWAY
.f_goaway:
    cmp     qword [rsp + L_SID], 0
    jne     .protocol_error             ; (§6.8)
    cmp     r15, 8
    jb      .frame_size_error
    ; The peer opens no more streams: finish ours, then close.
    or      qword [rbx + Conn.h2 + H2.flags], H2F_GOAWAY_RECV
    jmp     .next

; =============================================================================
; header_block — act on one complete header block. Internal.
;
; @param  rdi  Conn*   @param esi  stream id   @param rdx  block
; @param  rcx  block length   @param r8d  END_STREAM | CONT_SELF_DEP
; @returns eax = 0 (including handled protocol errors), or -1 when out of
;          memory; rdx = the Stream* this block opened and left open — a new
;          request whose END_STREAM has not arrived — or 0 in every other
;          case (trailers, refused, malformed, ignored, ended by this very
;          block). h2_process uses it to take the request's DATA frame
;          without a second lookup; a caller with no use for it ignores it.
; @remarks Every path decodes the block, even ones that refuse or ignore
;          the stream: the HPACK dynamic table must see every block, or it
;          diverges from the peer's encoder (§4.3). Cases:
;            new odd id above last_id   open a stream (or REFUSED_STREAM
;                                       over MAX_CONCURRENT_STREAMS; after
;                                       our GOAWAY: ignored, §6.8)
;            open stream                trailers: END_STREAM required, no
;                                       pseudo-headers (§8.1)
;            half-closed (remote)       STREAM_CLOSED (§5.1)
;            id <= last_id, no stream   reset by us: ignored; recently
;                                       closed: STREAM_CLOSED; never opened:
;                                       PROTOCOL_ERROR (§5.1.1) — both as
;                                       connection errors (RFC 9113 §5.1)
;            even id                    PROTOCOL_ERROR (§5.1.1)
; =============================================================================
header_block:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12d, esi                   ; stream id (zero-extended)
    mov     r13, rdx
    mov     r14, rcx
    mov     r15d, r8d
    ; An id above every id this connection has opened cannot be in the map:
    ; streams are opened only below, and each raises last_id to its own id.
    ; So the usual block — a new request — skips the lookup.
    xor     ebp, ebp                    ; no stream (what .ok hands back unless one is opened)
    cmp     r12, [rbx + Conn.h2 + H2.last_id]
    ja      .new_id
    mov     rdi, rbx
    mov     esi, r12d
    call    map_find
    mov     rbp, rax
    test    rax, rax
    jnz     .existing
    jmp     .closed_id
.new_id:
    test    r12d, 1
    jz      .even_id
    test    qword [rbx + Conn.h2 + H2.flags], H2F_GOAWAY_SENT
    jnz     .discard
    mov     [rbx + Conn.h2 + H2.last_id], r12
    mov     rax, [rbx + Conn.worker]
    mov     rax, [rax + Worker.cfg]
    mov     rax, [rax + Config.max_streams]
    cmp     [rbx + Conn.h2 + H2.nstreams], rax
    jae     .refuse
    mov     rdi, rbx
    mov     esi, r12d
    call    stream_open
    test    rax, rax
    jz      .no_memory
    mov     rbp, rax
    mov     rdi, rbx
    mov     rsi, rbp
    mov     rdx, r13
    mov     rcx, r14
    xor     r8d, r8d                    ; request block
    call    decode_fields
    test    eax, eax
    js      .hpack_failed
    jnz     .malformed
    test    r15d, CONT_SELF_DEP
    jnz     .malformed
    or      qword [rbp + Stream.h2flags], SN_HEADERS_DONE
    test    r15d, FF_END_STREAM
    jz      .ok
    mov     rdi, rbx
    mov     rsi, rbp
    call    request_end
    jmp     .out

.existing:
    test    qword [rbp + Stream.h2flags], SN_REMOTE_CLOSED
    jnz     .half_closed
    mov     rdi, rbx
    mov     rsi, rbp
    mov     rdx, r13
    mov     rcx, r14
    mov     r8d, HC_TRAILERS
    call    decode_fields
    test    eax, eax
    js      .hpack_failed
    jnz     .malformed
    test    r15d, CONT_SELF_DEP
    jnz     .malformed
    test    r15d, FF_END_STREAM
    jz      .malformed                  ; a second HEADERS must end the stream (§8.1)
    mov     rdi, rbx
    mov     rsi, rbp
    call    request_end
    jmp     .out

.half_closed:
    mov     rdi, rbx
    mov     rsi, r13
    mov     rdx, r14
    call    decode_discard
    test    eax, eax
    js      .hpack_failed
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, r12d
    mov     ecx, E_STREAM_CLOSED
    call    stream_error
    jmp     .out

.closed_id:
    mov     rdi, rbx
    mov     rsi, r13
    mov     rdx, r14
    call    decode_discard
    test    eax, eax
    js      .hpack_failed
    lea     rdi, [rbx + Conn.h2 + H2.rst_ring]
    mov     esi, r12d
    call    ring_has
    test    eax, eax
    jnz     .ok
    lea     rdi, [rbx + Conn.h2 + H2.closed_ring]
    mov     esi, r12d
    call    ring_has
    mov     esi, E_PROTOCOL
    mov     ecx, E_STREAM_CLOSED
    test    eax, eax
    cmovnz  esi, ecx
    mov     rdi, rbx
    call    conn_error
    jmp     .out

.even_id:
    mov     rdi, rbx
    mov     esi, E_PROTOCOL             ; the connection ends: no HPACK sync needed
    call    conn_error
    jmp     .out

.discard:
    mov     rdi, rbx
    mov     rsi, r13
    mov     rdx, r14
    call    decode_discard
    test    eax, eax
    js      .hpack_failed
    jmp     .ok

.refuse:
    mov     ecx, E_REFUSED_STREAM       ; over MAX_CONCURRENT_STREAMS (§5.1.2): retryable
    jmp     .reject_new
.no_memory:
    mov     ecx, E_INTERNAL
.reject_new:
    mov     [rsp], rcx
    mov     rdi, rbx
    mov     rsi, r13
    mov     rdx, r14
    call    decode_discard
    test    eax, eax
    js      .hpack_failed
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, r12d
    mov     rcx, [rsp]
    call    stream_error
    jmp     .out

.malformed:
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, r12d
    mov     ecx, E_PROTOCOL             ; malformed request: stream error (§8.1.1)
    call    stream_error
    jmp     .out

.hpack_failed:
    cmp     eax, HPACK_ERR_NOMEM
    je      .fatal
    mov     rdi, rbx
    mov     esi, E_COMPRESSION          ; the decoder can no longer follow (§4.3)
    call    conn_error
    jmp     .out
.fatal:
    mov     eax, -1
.out:
    xor     edx, edx                    ; no open, unanswered stream to hand back
    jmp     .ret
.ok:
    xor     eax, eax
    mov     rdx, rbp                    ; the stream this block opened (0: none — rbp is
                                        ; only ever a new request's stream on this path)
.ret:
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; decode_discard — decode a header block only to keep HPACK in sync.
; Internal.  @param rdi Conn*  @param rsi block  @param rdx length
; @returns eax = 0, or an HPACK_ERR_* code (negative).
; =============================================================================
decode_discard:
    mov     rcx, rdx
    mov     rdx, rsi
    xor     esi, esi                    ; no stream: fields are dropped
    xor     r8d, r8d
    jmp     decode_fields               ; tail call

; =============================================================================
; decode_fields — decode a header block into a stream's request state.
; Internal.
;
; @param  rdi  Conn*   @param rsi  Stream* or 0 (decode for HPACK sync only)
; @param  rdx  block   @param rcx  length   @param r8d  HC_TRAILERS or 0
; @returns eax = 0 (accepted), 1 (malformed request: the caller answers
;          with a stream error), or an HPACK_ERR_* code (negative:
;          connection error).
; @remarks A request block must carry :method, :scheme and :path (§8.3.1);
;          field-level rules are applied by field_handler as fields arrive.
;          The Huffman scratch is the worker's (65,536 bytes, >= the 4,096
;          table limit that hpack.asm requires).
;          Request blocks go through the connection's header-block cache
;          (H2.hc_*). Why it is exact: a block's decoded fields depend only
;          on its bytes and the dynamic table, and HpackDec.gen changes
;          whenever the table may change. A stream is fresh when its request
;          block is decoded (flags, route, HEAD bit zero; clen -1), so what
;          the block did to it is fully described by the recorded fields,
;          and replaying them equals decoding again. Only blocks whose own
;          decode left the table unchanged are recorded — a block that
;          inserts could never be replayed under the new generation anyway.
;          The cache is consulted before a frame is built: a hit — every
;          request of a warm connection — saves no registers and touches no
;          stack (blocks_equal is a leaf that leaves r9-r11 alone), which is
;          16 instructions less per request than checking after the prologue.
; =============================================================================
decode_fields:
    ; ---- header-block cache: request blocks of a live stream only ----
    test    rsi, rsi
    jz      .framed
    test    r8d, r8d
    jnz     .framed
    test    rcx, rcx
    jz      .framed                     ; hc_len 0 means "empty": never match it
    cmp     rcx, [rdi + Conn.h2 + H2.hc_len]
    jne     .framed
    mov     rax, [rdi + Conn.h2 + H2.hpack + HpackDec.gen]
    cmp     rax, [rdi + Conn.h2 + H2.hc_gen]
    jne     .framed
    mov     r9, rdi                     ; Conn*   } blocks_equal clobbers rax, rdx, r8
    mov     r10, rsi                    ; Stream* } only; the length stays in rcx
    mov     r11, rdx                    ; block   }
    lea     rdi, [r9 + Conn.h2 + H2.hc_bytes]
    mov     rsi, rdx
    call    blocks_equal
    test    eax, eax
    jz      .differs
    mov     rax, [r9 + Conn.h2 + H2.hc_flags]
    or      [r10 + Stream.flags], rax
    mov     rax, [r9 + Conn.h2 + H2.hc_method]
    mov     [r10 + Stream.method], rax
    mov     rax, [r9 + Conn.h2 + H2.hc_h2flags]
    or      [r10 + Stream.h2flags], rax
    mov     rax, [r9 + Conn.h2 + H2.hc_clen]
    mov     [r10 + Stream.clen], rax
    mov     eax, [r9 + Conn.h2 + H2.hc_result]
    ret
.differs:
    mov     rdi, r9                     ; the arguments back where the frame takes them
    mov     rsi, r10
    mov     rdx, r11
    xor     r8d, r8d                    ; a request block (it was 0; blocks_equal used r8)
.framed:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 64                     ; [0,16) scratch, [16,56) HCtx, [56] gen before
    mov     rbx, rdi
    mov     r12, rsi                    ; Stream* or 0
    mov     r13, rdx                    ; block
    mov     r14, rcx                    ; length
    mov     r15d, r8d                   ; HC_TRAILERS or 0

.decode:
    mov     rax, [rbx + Conn.h2 + H2.hpack + HpackDec.gen]
    mov     [rsp + 56], rax
    mov     [rsp + 16 + HCtx.stream], r12
    mov     qword [rsp + 16 + HCtx.seen], 0
    mov     [rsp + 16 + HCtx.state], r15
    mov     rax, [rbx + Conn.worker]
    mov     r9, [rax + Worker.h2_scratch]
    mov     [rsp], r9
    mov     qword [rsp + 8], H2_SCRATCH
    lea     rdi, [rbx + Conn.h2 + H2.hpack]
    mov     rsi, r13
    mov     rdx, r14
    lea     rcx, [rsp + 16]
    lea     r8, [field_handler]
    mov     r9, rsp
    call    kv_hpack_decode wrt ..plt
    test    eax, eax
    js      .out
    test    r12, r12
    jz      .ok
    mov     rax, [rsp + 16 + HCtx.state]
    test    eax, HC_BAD
    jnz     .malformed
    test    eax, HC_TRAILERS
    jnz     .ok
    mov     rax, [rsp + 16 + HCtx.seen]
    and     eax, (1 << HF_METHOD) | (1 << HF_SCHEME) | (1 << HF_PATH)
    cmp     eax, (1 << HF_METHOD) | (1 << HF_SCHEME) | (1 << HF_PATH)
    jne     .malformed
.ok:
    xor     eax, eax
    jmp     .record
.malformed:
    mov     eax, 1
.record:
    ; Remember this request block if its decode left the table as it was.
    test    r12, r12
    jz      .out
    test    r15d, r15d
    jnz     .out
    test    r14, r14
    jz      .out
    cmp     r14, H2_HC_MAX
    ja      .out
    mov     rcx, [rbx + Conn.h2 + H2.hpack + HpackDec.gen]
    cmp     rcx, [rsp + 56]
    jne     .out                        ; it inserted or resized: not replayable
    mov     [rbx + Conn.h2 + H2.hc_result], rax
    mov     [rbx + Conn.h2 + H2.hc_gen], rcx
    mov     [rbx + Conn.h2 + H2.hc_len], r14
    mov     rcx, [r12 + Stream.flags]
    and     ecx, ST_POST | ST_CT_OK | ST_ENCODED
    mov     [rbx + Conn.h2 + H2.hc_flags], rcx
    mov     rcx, [r12 + Stream.method]
    mov     [rbx + Conn.h2 + H2.hc_method], rcx
    mov     rcx, [r12 + Stream.h2flags]
    and     ecx, SN_HEAD
    mov     [rbx + Conn.h2 + H2.hc_h2flags], rcx
    mov     rcx, [r12 + Stream.clen]
    mov     [rbx + Conn.h2 + H2.hc_clen], rcx
    push    rax
    push    rax                         ; keep the result; 2 pushes keep alignment
    lea     rdi, [rbx + Conn.h2 + H2.hc_bytes]
    mov     rsi, r13
    mov     rdx, r14
    call    memcpy wrt ..plt
    pop     rax
    pop     rax
.out:
    add     rsp, 64
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; blocks_equal — are two byte ranges equal? Internal, leaf.
; @param  rdi  a   @param rsi  b   @param rcx  length (>= 1)
; @returns eax = 1 if equal, else 0. Clobbers rdx, r8.
; @remarks 8 bytes per step, the last word overlapping the previous one, so
;          no load ever leaves either range (a header block may end at the
;          very end of the read buffer).
; =============================================================================
blocks_equal:
    cmp     rcx, 8
    jb      .bytes
    lea     r8, [rcx - 8]               ; offset of the last whole word
    xor     eax, eax
.words:
    mov     rdx, [rsi + rax]
    cmp     rdx, [rdi + rax]
    jne     .differ
    add     rax, 8
    cmp     rax, r8
    jb      .words
    mov     rdx, [rsi + r8]
    cmp     rdx, [rdi + r8]
    jne     .differ
    mov     eax, 1
    ret
.bytes:
    xor     eax, eax
.byte:
    movzx   edx, byte [rsi + rax]
    cmp     dl, [rdi + rax]
    jne     .differ
    inc     rax
    cmp     rax, rcx
    jb      .byte
    mov     eax, 1
    ret
.differ:
    xor     eax, eax
    ret

; =============================================================================
; field_handler — hpack.asm's per-field callback: apply one request field.
; Internal.
;
; @param  rdi  HCtx*   @param rsi  name   @param rdx  name length
; @param  rcx  value   @param r8  value length   @param r9  meaning (hpack.inc)
; @returns eax = 0 always: problems are recorded (HC_BAD) so that decoding
;          continues to the end of the block (HPACK sync).
; @remarks Leaf; clobbers rax, rdx, r10, r11. Rules (RFC 9113 §8): no
;          uppercase/invalid names, no connection-specific fields, te only
;          "trailers", field values without NUL/CR/LF or edge whitespace;
;          pseudo-headers only in the request block, before regular fields,
;          each once, only the request ones, :path/:method/:scheme non-empty;
;          content-length strictly decimal and consistent. gRPC-relevant
;          facts go straight into the Stream (ST_POST, ST_CT_OK, ST_ENCODED,
;          the route in Stream.method), which kv_grpc_dispatch validates at
;          END_STREAM.
; =============================================================================
field_handler:
    mov     r10, [rdi + HCtx.stream]
    test    r10, r10
    jz      .ret                        ; HPACK sync only
    test    r9d, HF_VALUE_BAD | HF_OVERSIZE
    jnz     .bad
    movzx   eax, r9b                    ; field code
    lea     r11d, [rax - 1]
    cmp     r11d, HF_PSEUDO_LAST - 1
    jbe     .pseudo                     ; codes 1..6
    or      qword [rdi + HCtx.state], HC_REGULAR
    cmp     eax, HF_OTHER
    je      .ret
    cmp     eax, HF_BAD_NAME
    je      .bad
    cmp     eax, HF_CONN_SPECIFIC
    je      .bad
    mov     r11d, r9d
    shr     r11d, 8
    and     r11d, 0xFF                  ; value class
    cmp     eax, HF_TE
    je      .te
    test    qword [rdi + HCtx.state], HC_TRAILERS
    jnz     .ret                        ; the rest only matters in the request block
    cmp     eax, HF_CONTENT_TYPE
    je      .ctype
    cmp     eax, HF_GRPC_ENCODING
    je      .encoding
    cmp     eax, HF_CONTENT_LENGTH
    je      .clen
    jmp     .ret
.te:
    cmp     r11d, VC_OK                 ; "te" may only say "trailers" (§8.2.2)
    jne     .bad
    jmp     .ret
.ctype:
    cmp     r11d, VC_OK
    jne     .ret
    or      qword [r10 + Stream.flags], ST_CT_OK
    jmp     .ret
.encoding:
    cmp     r11d, VC_OK
    je      .ret                        ; identity
    or      qword [r10 + Stream.flags], ST_ENCODED
    jmp     .ret
.clen:
    test    r8, r8
    jz      .bad
    cmp     r8, 18
    ja      .bad                        ; > 10^18: no body can be that large
    xor     eax, eax
    xor     edx, edx
.digit:
    movzx   r11d, byte [rcx + rdx]
    sub     r11d, '0'
    cmp     r11d, 9
    ja      .bad                        ; sign, space, anything but a digit
    imul    rax, rax, 10
    add     rax, r11
    inc     rdx
    cmp     rdx, r8
    jb      .digit
    mov     r11, [r10 + Stream.clen]
    test    r11, r11
    js      .clen_set
    cmp     r11, rax
    jne     .bad                        ; two different lengths
.clen_set:
    mov     [r10 + Stream.clen], rax
    jmp     .ret
.pseudo:
    test    qword [rdi + HCtx.state], HC_TRAILERS | HC_REGULAR
    jnz     .bad                        ; in trailers, or after a regular field
    cmp     eax, HF_STATUS
    jae     .bad                        ; :status or an unknown pseudo-header
    mov     r11, [rdi + HCtx.seen]
    bts     r11, rax
    jc      .bad                        ; duplicate
    mov     [rdi + HCtx.seen], r11
    mov     r11d, r9d
    shr     r11d, 8
    and     r11d, 0xFF                  ; value class
    cmp     eax, HF_AUTHORITY
    je      .ret                        ; :authority may be anything, even empty
    test    r11d, VC_EMPTY
    jnz     .bad                        ; empty :method, :scheme or :path
    cmp     eax, HF_PATH
    je      .path
    cmp     eax, HF_METHOD
    jne     .ret                        ; :scheme
    cmp     r11d, VC_POST
    jne     .not_post
    or      qword [r10 + Stream.flags], ST_POST
    jmp     .ret
.not_post:
    cmp     r11d, VC_HEAD
    jne     .ret
    or      qword [r10 + Stream.h2flags], SN_HEAD
    jmp     .ret
.path:
    mov     [r10 + Stream.method], r11  ; class of :path = route id (M_*)
    jmp     .ret
.bad:
    or      qword [rdi + HCtx.state], HC_BAD
.ret:
    xor     eax, eax
    ret

; =============================================================================
; request_end — the peer ended the stream: check it, then dispatch it.
; Internal.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns eax = 0, or -1 (out of memory).
; @remarks content-length, when declared, must equal the DATA received
;          (§8.1.1), else the request is malformed (stream error). The
;          stream may be answered and freed inside kv_grpc_dispatch: the
;          caller must not touch it afterwards.
; =============================================================================
request_end:
    or      qword [rsi + Stream.h2flags], SN_REMOTE_CLOSED
    mov     rax, [rsi + Stream.clen]
    test    rax, rax
    js      .dispatch
    cmp     rax, [rsi + Stream.data_len]
    jne     .malformed
.dispatch:
    jmp     kv_grpc_dispatch wrt ..plt  ; tail call (Conn*, Stream*)
.malformed:
    mov     edx, [rsi + Stream.id]
    mov     ecx, E_PROTOCOL
    jmp     stream_error                ; tail call

; =============================================================================
; hb_append — add a fragment to the header block being assembled. Internal.
; @param  rdi  Conn*   @param rsi  fragment   @param rdx  length
; @returns eax = 0, 1 (block larger than H2_MAX_BLOCK: the caller raises
;          ENHANCE_YOUR_CALM), or -1 (out of memory).
; =============================================================================
hb_append:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; 4 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, [rbx + Conn.h2 + H2.hb_len]
    add     r14, r13                    ; new length
    cmp     r14, H2_MAX_BLOCK
    ja      .too_big
    cmp     r14, [rbx + Conn.h2 + H2.hb_cap]
    jbe     .copy
    mov     rsi, [rbx + Conn.h2 + H2.hb_cap]
    shl     rsi, 1
    cmp     rsi, r14
    cmovb   rsi, r14
    mov     eax, 1024
    cmp     rsi, rax
    cmovb   rsi, rax                    ; max(2 * cap, length, 1024)
    mov     [rsp], rsi
    mov     rdi, [rbx + Conn.h2 + H2.hb_buf]
    mov     rdx, [rbx + Conn.worker]
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_realloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbx + Conn.h2 + H2.hb_buf], rax
    mov     rcx, [rsp]
    mov     [rbx + Conn.h2 + H2.hb_cap], rcx
.copy:
    test    r13, r13
    jz      .ok
    mov     rdi, [rbx + Conn.h2 + H2.hb_buf]
    add     rdi, [rbx + Conn.h2 + H2.hb_len]
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
.ok:
    mov     [rbx + Conn.h2 + H2.hb_len], r14
    xor     eax, eax
    jmp     .out
.too_big:
    mov     eax, 1
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; apply_initial_window — the peer changed SETTINGS_INITIAL_WINDOW_SIZE.
; Internal.
; @param  rdi  Conn*   @param esi  new value (<= 2^31 - 1)
; @returns eax = 0, or 1 when a stream's window would exceed 2^31 - 1
;          (FLOW_CONTROL_ERROR, §6.9.2).
; @remarks Every live stream's send window moves by the difference and may
;          become negative (§6.9.2); blocked streams resume in the worker's
;          next flush. Leaf.
; =============================================================================
apply_initial_window:
    mov     eax, esi
    mov     rcx, rax
    sub     rcx, [rdi + Conn.h2 + H2.peer_window]     ; signed difference
    mov     [rdi + Conn.h2 + H2.peer_window], rax
    mov     rdx, [rdi + Conn.streams]
.next:
    test    rdx, rdx
    jz      .ok
    mov     rax, [rdx + Stream.send_window]
    add     rax, rcx
    cmp     rax, MAX_WINDOW
    jg      .overflow
    mov     [rdx + Stream.send_window], rax
    mov     rdx, [rdx + Stream.next]
    jmp     .next
.ok:
    xor     eax, eax
    ret
.overflow:
    mov     eax, 1
    ret

; =============================================================================
; reset_budget — spend one token of the rapid-reset budget. Internal, leaf.
; @param  rdi  Conn*
; @returns eax = 0, or 1 when the budget is exhausted (ENHANCE_YOUR_CALM).
; @remarks Token bucket: RESET_BURST tokens, refilled at 33 per second from
;          Worker.now — the budget nghttp2 and other servers adopted for
;          CVE-2023-44487 (HTTP/2 rapid reset).
;          Only resets of streams that are still open count.
; =============================================================================
reset_budget:
    mov     rax, [rdi + Conn.worker]
    mov     r8, [rax + Worker.now]
    mov     rax, r8
    sub     rax, [rdi + Conn.h2 + H2.reset_stamp]
    cmp     rax, RESET_NS_PER_TOKEN
    jb      .spend
    xor     edx, edx
    mov     ecx, RESET_NS_PER_TOKEN
    div     rcx                         ; whole tokens earned
    add     rax, [rdi + Conn.h2 + H2.reset_tokens]
    mov     ecx, RESET_BURST
    cmp     rax, rcx
    cmova   rax, rcx
    mov     [rdi + Conn.h2 + H2.reset_tokens], rax
    mov     [rdi + Conn.h2 + H2.reset_stamp], r8
.spend:
    cmp     qword [rdi + Conn.h2 + H2.reset_tokens], 0
    je      .exhausted
    dec     qword [rdi + Conn.h2 + H2.reset_tokens]
    xor     eax, eax
    ret
.exhausted:
    mov     eax, 1
    ret

; =============================================================================
; ring_has — is a stream id in a ring of H2_RING ids? Internal, leaf.
; @param  rdi  ring (dword[H2_RING])   @param esi  stream id (non-zero)
; @returns eax = 1 if present, else 0. Clobbers ecx.
; @remarks Zeroed slots never match: stream 0 is the connection.
; =============================================================================
ring_has:
    xor     eax, eax
    mov     ecx, H2_RING
.loop:
    cmp     [rdi + rcx*4 - 4], esi
    je      .yes
    dec     ecx
    jnz     .loop
    ret
.yes:
    mov     eax, 1
    ret

; =============================================================================
; map_find — the Stream* of a stream id. Internal, leaf.
; @param  rdi  Conn*   @param esi  stream id
; @returns rax = Stream*, or 0. Clobbers rcx, rdx, r8.
; @remarks Open addressing, linear probing, home slot (id >> 1) & mask:
;          client ids are odd and increasing, so concurrent streams occupy
;          consecutive slots and a lookup is usually one load.
; =============================================================================
map_find:
    mov     esi, esi                    ; zero-extend: ids are stored as qwords
    mov     r8, [rdi + Conn.h2 + H2.map]
    mov     rdx, [rdi + Conn.h2 + H2.map_mask]
    mov     rcx, rsi
    shr     rcx, 1
    and     rcx, rdx
.probe:
    mov     rax, [r8 + rcx*8]
    test    rax, rax
    jz      .out
    cmp     [rax + Stream.id], rsi
    je      .out
    inc     rcx
    and     rcx, rdx
    jmp     .probe
.out:
    ret

; =============================================================================
; map_insert — add a stream (its id must not be present). Internal.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns eax = 0, or -1 when the table had to grow and could not.
; @remarks Keeps the load factor <= 1/2 by doubling (map_grow), so probe
;          sequences stay short; H2.nstreams is the caller's to update.
; =============================================================================
map_insert:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     rax, [rbx + Conn.h2 + H2.nstreams]
    lea     rax, [rax * 2 + 2]          ; 2 * (n + 1)
    mov     rcx, [rbx + Conn.h2 + H2.map_mask]
    inc     rcx
    cmp     rax, rcx
    jbe     .place
    mov     rdi, rbx
    call    map_grow
    test    eax, eax
    jnz     .out
.place:
    mov     r8, [rbx + Conn.h2 + H2.map]
    mov     rdx, [rbx + Conn.h2 + H2.map_mask]
    mov     rcx, [r12 + Stream.id]
    shr     rcx, 1
    and     rcx, rdx
    mov     r9, rcx                     ; home slot
.probe:
    cmp     qword [r8 + rcx*8], 0
    je      .slot
    inc     rcx
    and     rcx, rdx
    jmp     .probe
.slot:
    mov     [r8 + rcx*8], r12
    cmp     rcx, r9
    je      .home
    inc     qword [rbx + Conn.h2 + H2.map_displaced]
.home:
    xor     eax, eax
.out:
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; map_grow — double the stream map and rehash. Internal.
; @param  rdi  Conn*
; @returns eax = 0, or -1 when out of memory (the old table is kept).
; =============================================================================
map_grow:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, [rbx + Conn.h2 + H2.map]
    mov     r13, [rbx + Conn.h2 + H2.map_mask]
    lea     r14, [r13 + 1]
    shl     r14, 1                      ; new slot count
    mov     rdi, r14
    mov     esi, 8
    mov     rdx, [rbx + Conn.worker]
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_calloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     rbp, rax
    lea     r15, [r14 - 1]              ; new mask
    xor     ecx, ecx
    xor     r9d, r9d                    ; displaced entries in the new table
.next:
    cmp     rcx, r13
    ja      .rehashed
    mov     rax, [r12 + rcx*8]
    test    rax, rax
    jz      .skip
    mov     rdx, [rax + Stream.id]
    shr     rdx, 1
    and     rdx, r15
    mov     r8, rdx                     ; home slot
.probe:
    cmp     qword [rbp + rdx*8], 0
    je      .put
    inc     rdx
    and     rdx, r15
    jmp     .probe
.put:
    mov     [rbp + rdx*8], rax
    cmp     rdx, r8
    je      .skip
    inc     r9
.skip:
    inc     rcx
    jmp     .next
.rehashed:
    mov     [rbx + Conn.h2 + H2.map], rbp
    mov     [rbx + Conn.h2 + H2.map_mask], r15
    mov     [rbx + Conn.h2 + H2.map_displaced], r9
    mov     rdi, r12
    mov     rsi, [rbx + Conn.worker]
    lea     rsi, [rsi + Worker.pool]
    call    kv_pool_free wrt ..plt
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
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
; map_remove — delete a stream id from the map. Internal, leaf.
; @param  rdi  Conn*   @param esi  stream id (absent ids are ignored)
; @returns nothing. Clobbers rax, rcx, rdx, rsi, r8-r11.
; @remarks Backward-shift deletion instead of tombstones: each entry after
;          the hole moves back into it when the hole lies on that entry's
;          probe path (dist(hole -> j) <= dist(home -> j)), so the table
;          never degrades however many streams come and go.
;          Fast path: an entry in its home slot has a one-slot probe path
;          that no hole can be on, so while H2.map_displaced is 0 nothing
;          can ever move and the scan is skipped — the normal case, since
;          client ids are consecutive odd numbers and land in consecutive
;          home slots of a table kept at most half full.
; =============================================================================
map_remove:
    mov     esi, esi
    mov     r8, [rdi + Conn.h2 + H2.map]
    mov     rdx, [rdi + Conn.h2 + H2.map_mask]
    mov     rcx, rsi
    shr     rcx, 1
    and     rcx, rdx                    ; i: probe for the id
.find:
    mov     rax, [r8 + rcx*8]
    test    rax, rax
    jz      .out
    cmp     [rax + Stream.id], rsi
    je      .found
    inc     rcx
    and     rcx, rdx
    jmp     .find
.found:
    cmp     qword [rdi + Conn.h2 + H2.map_displaced], 0
    je      .clear                      ; nobody displaced: nothing moves back
    mov     r10, rsi
    shr     r10, 1
    and     r10, rdx
    cmp     r10, rcx
    je      .scan
    dec     qword [rdi + Conn.h2 + H2.map_displaced] ; the removed one was displaced
.scan:
    mov     r9, rcx                     ; j scans forward from the hole i
.shift:
    inc     r9
    and     r9, rdx
    mov     rax, [r8 + r9*8]
    test    rax, rax
    jz      .clear
    mov     r10, [rax + Stream.id]
    shr     r10, 1
    and     r10, rdx                    ; home of the entry at j
    mov     r11, r9
    sub     r11, r10
    and     r11, rdx                    ; dist(home -> j)
    mov     rsi, r9
    sub     rsi, rcx
    and     rsi, rdx                    ; dist(hole -> j)
    cmp     rsi, r11
    ja      .shift                      ; the hole is not on its path: stays
    mov     [r8 + rcx*8], rax
    cmp     rcx, r10
    jne     .moved
    dec     qword [rdi + Conn.h2 + H2.map_displaced] ; moved back home
.moved:
    mov     rcx, r9                     ; its old slot is the new hole
    jmp     .shift
.clear:
    mov     qword [r8 + rcx*8], 0
.out:
    ret

; =============================================================================
; stream_open — create a stream for a new request. Internal.
; @param  rdi  Conn*   @param esi  stream id (odd, > last_id)
; @returns rax = Stream*, or 0 when out of memory.
; @remarks The Stream comes from the worker pool, is linked on Conn.streams
;          (so kv_grpc_free_streams releases it at teardown), and enters the
;          map; its send window starts at the peer's INITIAL_WINDOW_SIZE, its
;          receive window at ours.
;          The common case is inline: the block is popped from the pool's
;          free list of the Stream's class and zeroed here, and the stream
;          goes straight into its home map slot. That is every request of a
;          warm connection (a freed Stream is the next one's block, and
;          client ids land in consecutive free slots); an empty list, a map
;          due to grow or an occupied home slot take kv_pool_calloc and
;          map_insert as before. Why inline, when replacing only the memset
;          once saved nothing (1,424 against 1,428 instructions per GET,
;          2026-09-27): the cost was never the zeroing but the three calls
;          around it — kv_pool_calloc, kv_pool_malloc with its size-class
;          BSR, memset — and map_insert's frame.
;          Clobbers xmm0.
; =============================================================================
; The pool class of a Stream block, at assembly time (kv_pool_malloc finds
; it with a BSR per call; pool.asm POOL_CLASS: blocks are 64 << class bytes,
; header included).
%if Stream_size + POOL_HDR <= 64
    %define STREAM_CLASS 0
%elif Stream_size + POOL_HDR <= 128
    %define STREAM_CLASS 1
%elif Stream_size + POOL_HDR <= 256
    %define STREAM_CLASS 2
%elif Stream_size + POOL_HDR <= 512
    %define STREAM_CLASS 3
%else
    %error "Stream no longer fits a 512-byte pool block: extend STREAM_CLASS"
%endif
%if (Stream_size % 16) != 0
    %error "stream_open zeroes the Stream in 16-byte steps: keep its size a multiple of 16"
%endif

stream_open:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12d, esi
    mov     rdx, [rbx + Conn.worker]
    mov     r13, [rdx + Worker.pool + Pool.free + STREAM_CLASS * 8]
    test    r13, r13
    jz      .carve                      ; no freed block of this class: the allocator carves one
    mov     rax, [r13]                  ; LIFO pop (kv_pool_malloc's own step): the block's
    mov     [rdx + Worker.pool + Pool.free + STREAM_CLASS * 8], rax ; header still holds its class
    pxor    xmm0, xmm0                  ; pool blocks are 16-byte aligned
%assign zoff 0
%rep Stream_size / 16
    movdqa  [r13 + zoff], xmm0
%assign zoff zoff + 16
%endrep
    jmp     .init
.carve:
    mov     edi, 1
    mov     esi, Stream_size
    lea     rdx, [rdx + Worker.pool]
    call    kv_pool_calloc wrt ..plt
    test    rax, rax
    jz      .out
    mov     r13, rax
.init:
    mov     [r13 + Stream.conn], rbx
    mov     [r13 + Stream.id], r12
    mov     rax, [rbx + Conn.h2 + H2.peer_window]
    mov     [r13 + Stream.send_window], rax
    mov     qword [r13 + Stream.recv_window], STREAM_WINDOW
    mov     qword [r13 + Stream.clen], -1
    mov     rcx, [rbx + Conn.streams]
    mov     [r13 + Stream.next], rcx
    test    rcx, rcx
    jz      .linked
    mov     [rcx + Stream.prev], r13
.linked:
    mov     [rbx + Conn.streams], r13
    ; Into the map. Inline when the table stays at most half full with this
    ; stream (map_insert's own growth rule) and the home slot is free;
    ; anything else is map_insert's.
    mov     rax, [rbx + Conn.h2 + H2.nstreams]
    lea     rax, [rax * 2 + 2]          ; 2 * (n + 1)
    mov     rdx, [rbx + Conn.h2 + H2.map_mask]
    lea     rcx, [rdx + 1]
    cmp     rax, rcx
    ja      .map_slow                   ; the table must grow first
    mov     rcx, r12
    shr     rcx, 1
    and     rcx, rdx                    ; the home slot
    mov     r8, [rbx + Conn.h2 + H2.map]
    cmp     qword [r8 + rcx*8], 0
    jne     .map_slow                   ; occupied: probe (and count the displacement)
    mov     [r8 + rcx*8], r13
    jmp     .mapped
.map_slow:
    mov     rdi, rbx
    mov     rsi, r13
    call    map_insert
    test    eax, eax
    jnz     .unlink
.mapped:
    inc     qword [rbx + Conn.h2 + H2.nstreams]
    mov     rax, r13
    jmp     .out
.unlink:
    mov     rdi, rbx
    mov     rsi, r13
    call    kv_grpc_stream_release wrt ..plt
    xor     eax, eax
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; stream_close — a stream is closed (answered, reset by either side).
; Internal.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns nothing.
; @remarks Leaves the map (nstreams--) and the pending list, and its id
;          joins closed_ring so later frames on it are recognized as
;          "closed" rather than "never opened". The Stream is freed — unless
;          it is queued in the current GET batch, which then frees it
;          instead of answering (ST_CLOSED, as grpc.asm's on_stream_close).
; =============================================================================
stream_close:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    ; Out of the map. While no entry is displaced every stream sits in its
    ; home slot and nothing can move into a hole (map_remove's own fast
    ; path), so the slot is cleared here; a displaced entry anywhere, or a
    ; home slot that is not this stream's, is map_remove's.
    cmp     qword [rbx + Conn.h2 + H2.map_displaced], 0
    jne     .remove_slow
    mov     rcx, [r12 + Stream.id]
    shr     rcx, 1
    and     rcx, [rbx + Conn.h2 + H2.map_mask]
    mov     rax, [rbx + Conn.h2 + H2.map]
    cmp     [rax + rcx*8], r12
    jne     .remove_slow
    mov     qword [rax + rcx*8], 0
    jmp     .removed
.remove_slow:
    mov     esi, [r12 + Stream.id]
    call    map_remove                  ; rdi = Conn*
.removed:
    dec     qword [rbx + Conn.h2 + H2.nstreams]
    mov     eax, [rbx + Conn.h2 + H2.closed_pos]
    and     eax, H2_RING - 1
    mov     ecx, [r12 + Stream.id]
    mov     [rbx + Conn.h2 + H2.closed_ring + rax*4], ecx
    inc     qword [rbx + Conn.h2 + H2.closed_pos]
    test    qword [r12 + Stream.h2flags], SN_PENDING
    jz      .unqueued
    mov     rdi, rbx
    mov     rsi, r12
    call    pend_unlink
.unqueued:
    test    qword [r12 + Stream.flags], ST_BATCHED
    jnz     .batched
    mov     rdi, rbx
    mov     rsi, r12
    add     rsp, 8
    pop     r12
    pop     rbx
    jmp     kv_grpc_stream_release wrt ..plt ; tail call: the frame is gone, the caller's alignment kept
.batched:
    or      qword [r12 + Stream.flags], ST_CLOSED
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; stream_error — RST_STREAM(id, code) and close the stream. Internal.
; @param  rdi  Conn*   @param rsi  Stream* or 0 (no live stream)
; @param  edx  stream id   @param ecx  error code
; @returns eax = 0, or -1 when out of memory.
; @remarks The id joins rst_ring first: frames the peer sent before seeing
;          our RST_STREAM must be ignored, not answered with more errors
;          (§5.1 "closed").
; =============================================================================
stream_error:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13d, edx
    mov     eax, [rbx + Conn.h2 + H2.rst_pos]
    and     eax, H2_RING - 1
    mov     [rbx + Conn.h2 + H2.rst_ring + rax*4], r13d
    inc     qword [rbx + Conn.h2 + H2.rst_pos]
    mov     rdi, rbx
    mov     esi, r13d
    mov     edx, ecx
    call    send_rst
    mov     r13d, eax                   ; result (the id is no longer needed)
    test    r12, r12
    jz      .out
    mov     rdi, rbx
    mov     rsi, r12
    call    stream_close
.out:
    mov     eax, r13d
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; response_done — our response is complete: close the stream. Internal.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns eax = 0, or -1 when out of memory (queuing RST_STREAM).
; @remarks The normal case — the peer ended its request before we answered,
;          as every unary call does — is plain stream_close. A response that
;          completes while the peer is still sending (an Upload refused
;          early) must also ask the peer to stop: RFC 9113 §8.1 prescribes
;          RST_STREAM(NO_ERROR) after the complete response. Without it the
;          client would keep transmitting a body nobody reads, and its later
;          DATA would draw STREAM_CLOSED errors. stream_error sends exactly
;          that: the frame, the id into rst_ring (so DATA already in flight
;          is ignored, not answered), and the close.
; =============================================================================
response_done:
    test    qword [rsi + Stream.h2flags], SN_REMOTE_CLOSED
    jz      .early
    sub     rsp, 8                      ; aligned for the call
    call    stream_close
    add     rsp, 8
    xor     eax, eax
    ret
.early:
    mov     edx, [rsi + Stream.id]
    mov     ecx, E_NO_ERROR
    jmp     stream_error                ; tail call: 0 or -1

; =============================================================================
; conn_error — queue GOAWAY(last_id, code) and stop the connection.
; Internal.
; @param  rdi  Conn*   @param esi  error code
; @returns eax = 0, or -1 when out of memory. Only the first call sends.
; =============================================================================
conn_error:
    test    qword [rdi + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .done
    or      qword [rdi + Conn.h2 + H2.flags], H2F_CLOSING | H2F_GOAWAY_SENT
    jmp     send_goaway                 ; tail call
.done:
    xor     eax, eax
    ret

; =============================================================================
; pend_push / pend_unlink — the list of streams whose response bytes wait
; for flow-control window or output-buffer room. Internal, leaf.
; @param  rdi  Conn*   @param rsi  Stream*
; @returns nothing. pend_push is idempotent; pend_unlink scans (the list is
;          short: bounded by MAX_CONCURRENT_STREAMS, usually empty).
; =============================================================================
pend_push:
    test    qword [rsi + Stream.h2flags], SN_PENDING
    jnz     .out
    or      qword [rsi + Stream.h2flags], SN_PENDING
    mov     qword [rsi + Stream.pend_next], 0
    mov     rax, [rdi + Conn.h2 + H2.pend_tail]
    test    rax, rax
    jz      .first
    mov     [rax + Stream.pend_next], rsi
    jmp     .tail
.first:
    mov     [rdi + Conn.h2 + H2.pend_head], rsi
.tail:
    mov     [rdi + Conn.h2 + H2.pend_tail], rsi
.out:
    ret

pend_unlink:
    and     qword [rsi + Stream.h2flags], ~SN_PENDING
    xor     ecx, ecx                    ; previous
    mov     rax, [rdi + Conn.h2 + H2.pend_head]
.scan:
    test    rax, rax
    jz      .out
    cmp     rax, rsi
    je      .found
    mov     rcx, rax
    mov     rax, [rax + Stream.pend_next]
    jmp     .scan
.found:
    mov     rdx, [rsi + Stream.pend_next]
    test    rcx, rcx
    jz      .head
    mov     [rcx + Stream.pend_next], rdx
    jmp     .fix_tail
.head:
    mov     [rdi + Conn.h2 + H2.pend_head], rdx
.fix_tail:
    cmp     [rdi + Conn.h2 + H2.pend_tail], rsi
    jne     .out
    mov     [rdi + Conn.h2 + H2.pend_tail], rcx
.out:
    ret

; =============================================================================
; kv_h2n_pump — frame pending response bytes the windows now allow.
;
; @param  rdi  Conn*
; @returns eax = 0, or -1 when out of memory.
; @remarks Called by the worker's flush once the output buffer is empty
;          (after WINDOW_UPDATE, SETTINGS or drained output). Streams are
;          served in FIFO order until the buffer reaches OUT_HIGH_WATER or
;          the connection window closes; a stream blocked on its own window
;          stays queued without blocking the others.
; =============================================================================
kv_h2n_pump:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .ok
    mov     r12, [rbx + Conn.h2 + H2.pend_head]
.next:
    test    r12, r12
    jz      .ok
    cmp     qword [rbx + Conn.out_len], OUT_HIGH_WATER
    jae     .ok
    cmp     qword [rbx + Conn.h2 + H2.send_window], 0
    jle     .ok                         ; connection window shut: nobody can send
    mov     r13, [r12 + Stream.pend_next] ; the stream may complete and be freed
    mov     rdi, rbx
    mov     rsi, r12
    call    emit_progress
    test    eax, eax
    js      .fail
    mov     r12, r13
    jmp     .next
.ok:
    xor     eax, eax
    jmp     .out
.fail:
    mov     eax, -1
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_h2n_reply_parts — answer a gRPC call whose message is head || body.
;
; @param  rdi  Conn*   @param rsi  Stream*
; @param  rdx  head    @param rcx  head length (the 5-byte gRPC prefix and
;              the protobuf bytes before the body)
; @param  r8   body    @param r9  body length (0: none)
; @returns eax = 0, or -1 when out of memory (the caller ends the connection).
; @remarks The fast path frames HEADERS + DATA + trailers directly into the
;          output buffer when the message fits every window and one frame:
;          a GET's value goes from the store record to the socket buffer in
;          one copy. Otherwise the message is copied into Stream.resp_buf
;          and streamed as windows allow (kv_h2n_pump). The stream is closed
;          once its trailers are framed. On a failing connection the call
;          is simply dropped.
; =============================================================================
kv_h2n_reply_parts:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    mov     r12, rdx
    mov     r13, rcx
    mov     r14, r8
    mov     r15, r9
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .drop
    call    reply_fast                  ; same arguments, still in place
    test    eax, eax
    jle     .out                        ; 0 answered, -1 out of memory
    ; Slow path: own a copy (the body may be a store record, only valid
    ; while this worker is online).
    lea     rdi, [r13 + r15]
    mov     rsi, [rbx + Conn.worker]
    lea     rsi, [rsi + Worker.pool]
    call    kv_pool_malloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbp + Stream.resp_buf], rax
    lea     rcx, [r13 + r15]
    mov     [rbp + Stream.resp_len], rcx
    mov     rdi, rax
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    test    r15, r15
    jz      .copied
    mov     rdi, [rbp + Stream.resp_buf]
    add     rdi, r13
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt
.copied:
    mov     rdi, rbx
    mov     rsi, rbp
    call    reply_slow
    jmp     .out
.drop:
    mov     rdi, rbx
    mov     rsi, rbp
    call    stream_close
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
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
; kv_h2n_reply_buf — answer a gRPC call whose message is in Stream.resp_buf.
;
; @param  rdi  Conn*   @param rsi  Stream* (resp_buf / resp_len set; the
;              buffer is freed with the stream)
; @returns eax = 0, or -1 when out of memory.
; @remarks Same fast and slow paths as kv_h2n_reply_parts, without a second
;          copy: for a message that already lives in a pool buffer the
;          stream owns. No caller today — Put / Delete / Health replies are
;          5-7 bytes, built on the stack and sent through kv_h2n_reply_parts,
;          which needs no buffer at all when the windows allow (grpc.asm
;          .bool_reply). Kept as the entry point for a large, pre-built
;          message.
; =============================================================================
kv_h2n_reply_buf:
    push    rbx
    push    rbp
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .drop
    mov     rdx, [rbp + Stream.resp_buf]
    mov     rcx, [rbp + Stream.resp_len]
    xor     r8d, r8d
    xor     r9d, r9d
    call    reply_fast
    test    eax, eax
    jle     .out
    mov     rdi, rbx
    mov     rsi, rbp
    call    reply_slow
    jmp     .out
.drop:
    mov     rdi, rbx
    mov     rsi, rbp
    call    stream_close
    xor     eax, eax
.out:
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_h2n_reply_static — plain HTTP error with a static body (405 / 415).
;
; @param  rdi  Conn*   @param rsi  Stream* (resp_buf / resp_len = the
;              .rodata body, ST_STATIC_RESP set: never freed)
; @param  rdx  HdrField array   @param ecx  entry count
; @returns eax = 0, or -1 when out of memory.
; @remarks The body ends the stream on its last DATA frame: a plain HTTP
;          error has no gRPC trailers (they would be meaningless). A HEAD
;          request gets the same headers and no body (RFC 9110 §9.3.2).
; =============================================================================
kv_h2n_reply_static:
    push    rbx
    push    rbp
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .drop
    mov     r8d, FF_END_HEADERS
    test    qword [rbp + Stream.h2flags], SN_HEAD
    jz      .headers
    mov     r8d, FF_END_HEADERS | FF_END_STREAM
.headers:
    call    put_nv_headers              ; rdi, rsi, rdx, ecx, r8d as passed
    test    eax, eax
    js      .out
    test    qword [rbp + Stream.h2flags], SN_HEAD
    jnz     .head_done                  ; HEAD: complete with the headers
    or      qword [rbp + Stream.h2flags], SN_RESP_HEADERS
    mov     qword [rbp + Stream.resp_off], 0
    mov     rdi, rbx
    mov     rsi, rbp
    call    emit_progress
    test    eax, eax
    js      .out
    xor     eax, eax
    jmp     .out
.head_done:
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done               ; RST(NO_ERROR) too if the peer still sends
    jmp     .out
.drop:
    mov     rdi, rbx
    mov     rsi, rbp
    call    stream_close
    xor     eax, eax
.out:
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_h2n_reply_headers — Trailers-Only gRPC error: one HEADERS frame that
; ends the stream.
;
; @param  rdi  Conn*   @param rsi  Stream*
; @param  rdx  HdrField array (:status, content-type, grpc-status,
;              grpc-message[, grpc-accept-encoding])   @param ecx  count
; @returns eax = 0, or -1 when out of memory.
; =============================================================================
kv_h2n_reply_headers:
    push    rbx
    push    rbp
    sub     rsp, 8                      ; 2 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    test    qword [rbx + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .close
    mov     r8d, FF_END_HEADERS | FF_END_STREAM
    call    put_nv_headers
    test    eax, eax
    js      .out
    ; Complete: a streaming request refused before its end (Upload) also
    ; gets RST_STREAM(NO_ERROR) so the peer stops sending (response_done).
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done
    jmp     .out
.close:
    mov     rdi, rbx
    mov     rsi, rbp
    call    stream_close
    xor     eax, eax
.out:
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_h2n_reply_stream — answer with HEADERS now and a generated body as the
; windows allow (Download).
;
; @param  rdi  Conn*
; @param  rsi  Stream* — ST_GEN set, resp_len = the body's exact length (all
;              gRPC messages, prefixes included), resp_buf unused; the body
;              comes from kv_grpc_gen_fill
; @returns eax = 0, or -1 when out of memory (the caller ends the connection).
; @remarks The body never exists as a whole: each DATA frame is filled from
;          the source when window and output room allow (emit_progress), so
;          a 2 GiB value costs its pinned record plus the connection's 64 KiB
;          output buffer. The trailers follow the last byte and close the
;          stream (its transfer state — the pin — is released with it). On a
;          failing connection the call is simply dropped the same way.
; =============================================================================
kv_h2n_reply_stream:
    test    qword [rdi + Conn.h2 + H2.flags], H2F_CLOSING
    jnz     .drop
    jmp     reply_slow                  ; tail call: HEADERS, then emit_progress
.drop:
    sub     rsp, 8                      ; aligned for the call
    call    stream_close
    add     rsp, 8
    xor     eax, eax
    ret

; =============================================================================
; reply_fast — frame a complete small response in one go, if allowed.
; Internal.
;
; @param  rdi  Conn*   @param rsi  Stream*   @param rdx  head
; @param  rcx  head length   @param r8  body   @param r9  body length
; @returns eax = 0 (answered, stream closed), 1 (does not fit now: nothing
;          written), or -1 (out of memory).
; @remarks "Fits" = the message fits the connection window and the stream
;          window, and the queued output plus this reply stays within
;          OUT_HIGH_WATER — or the queue can be emptied into the socket right
;          now (out_try_drain). The output reservation is generous (headers
;          <= 30, trailers <= 25 bytes) and trimmed to what was written.
;          A message larger than the peer's frame size (a 16 KiB value plus
;          its 11 bytes of gRPC and protobuf framing already is) is split
;          over several DATA frames, still copied once, straight from the
;          caller's body, whatever its length (only for a small head). The
;          queued output is sent each time it reaches the high-water mark,
;          so the buffer stays bounded; if the socket refuses part-way, only
;          the rest of the body is copied to a buffer and queued (.rest).
;          Footgun: a long reply holds the worker for as long as copying it
;          to the socket takes (~0.2 ms per MiB): values of tens of MiB
;          belong to Download, which yields every FLUSH_BUDGET bytes.
;          Why both: without them every such GET took the buffered path — a
;          malloc of the message, a copy into it, the pend list, a second
;          copy into the output buffer and a free — and so did every reply
;          after the first 64 KiB of a read's output (measured: 16 KiB values
;          cost 2 copies and 0.69 heap growths per GET, callgrind).
; =============================================================================
reply_fast:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 40                     ; [0] message length, [8] start, [16] cursor,
                                        ; [24] first-frame payload; 6 pushes + 40: aligned
    mov     rbx, rdi
    mov     rbp, rsi
    mov     r12, rdx
    mov     r13, rcx
    mov     r14, r8
    mov     r15, r9
    lea     rax, [r13 + r15]
    mov     [rsp], rax
    ; Would this reply take the queued output past the high-water mark? Then
    ; hand what is queued to the socket first, rather than buffer the reply a
    ; second time — so the buffer never holds more than the mark plus one
    ; reply's framing (plus one frame for a reply longer than the mark, which
    ; .multi sends as it goes), as before. An empty buffer has nothing to
    ; send first and takes any reply the checks below allow.
    mov     rcx, [rbx + Conn.out_len]
    test    rcx, rcx
    jz      .room
    add     rcx, rax
    cmp     rcx, OUT_HIGH_WATER
    jbe     .room
    mov     rdi, rbx
    call    out_try_drain               ; a raw syscall: clobbers rax, rcx, r11
    test    eax, eax
    jnz     .later                      ; socket full (or replies already queued)
    mov     rax, [rsp]
.room:
    cmp     rax, [rbx + Conn.h2 + H2.send_window]
    jg      .later
    cmp     rax, [rbp + Stream.send_window]
    jg      .later
    cmp     rax, [rbx + Conn.h2 + H2.peer_frame]
    ja      .multi
    ; ---- steady state: the whole response from one template ----
    ; Once a connection's first response is out, both response fields are in
    ; the peer's dynamic table (H2E_BOTH), so every later response is the same
    ; 30 bytes around the message:
    ;   HEADERS  00 00 02 | 01 | 04 | id | 88 BF      (:status 200, [63] content-type)
    ;   DATA     len      | 00 | 00 | id | message
    ;   HEADERS  00 00 01 | 01 | 05 | id | BE         ([62] grpc-status: 0)
    ; Written here with constant stores instead of through put_response_headers,
    ; put_tsu, put_hdr (three times), put_trailers and out_reserve: the same
    ; bytes for under half the instructions (callgrind, a GET: 187 -> 82 for
    ; the framing, 1,446 -> 1,341 in all; in-process timing: 355 -> 333 core
    ; cycles per GET, 211 -> 178 per health check).
    ; Anything unusual keeps the general path below: another encoder state, a
    ; pending table size update, a head the two overlapping moves cannot copy,
    ; or a buffer that must grow.
    cmp     qword [rbx + Conn.h2 + H2.enc], H2E_BOTH
    jne     .general
    test    qword [rbx + Conn.h2 + H2.flags], H2F_TSU
    jnz     .general
    lea     rcx, [r13 - 4]
    cmp     rcx, 12
    ja      .general                    ; head not 4..16 bytes
    mov     rdi, [rbx + Conn.out_len]
    lea     rdx, [rdi + rax + 2 * FRAME_HDR + 12] ; 11 + 9 + message + 10
    cmp     rdx, [rbx + Conn.out_cap]
    ja      .general                    ; no room: out_reserve grows the buffer
    mov     [rbx + Conn.out_len], rdx
    add     rdi, [rbx + Conn.out_buf]   ; rdi = first byte of the response
    mov     ecx, [rbp + Stream.id]
    bswap   ecx                         ; stream id as the wire has it
    mov     dword [rdi], 0x01020000     ; length 2, type HEADERS
    mov     byte [rdi + 4], FF_END_HEADERS
    mov     [rdi + 5], ecx
    mov     word [rdi + 9], 0xBF88
    mov     edx, eax
    shl     edx, 8
    bswap   edx                         ; message length big-endian, then type DATA (0)
    mov     [rdi + 11], edx
    mov     byte [rdi + 15], 0          ; no flags: the trailers end the stream
    mov     [rdi + 16], ecx
    lea     rdx, [rdi + rax + 2 * FRAME_HDR + 2] ; the trailers follow the message
    mov     dword [rdx], 0x01010000     ; length 1, type HEADERS
    mov     byte [rdx + 4], FF_END_HEADERS | FF_END_STREAM
    mov     [rdx + 5], ecx
    mov     byte [rdx + 9], 0xBE
    sub     [rbx + Conn.h2 + H2.send_window], rax
    sub     [rbp + Stream.send_window], rax
    add     rdi, 2 * FRAME_HDR + 2      ; the message goes here
    ; The head with two overlapping moves (both loads inside [head, head+len),
    ; both stores inside the bytes just reserved), as in the general path.
    cmp     r13, 8
    jb      .t_head_small
    mov     rax, [r12]
    mov     rcx, [r12 + r13 - 8]
    mov     [rdi], rax
    mov     [rdi + r13 - 8], rcx
    jmp     .t_body
.t_head_small:
    mov     eax, [r12]
    mov     ecx, [r12 + r13 - 4]
    mov     [rdi], eax
    mov     [rdi + r13 - 4], ecx
.t_body:
    test    r15, r15
    jz      .t_done
    add     rdi, r13
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt
.t_done:
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done               ; 0, or -1 (out of memory for an RST)
    jmp     .out
.general:
    mov     rax, [rsp]
    lea     rsi, [rax + 3 * FRAME_HDR + 64]
    mov     rdi, rbx
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     [rsp + 8], rax
    mov     rdi, rbx
    mov     rsi, rax
    mov     edx, [rbp + Stream.id]
    call    put_response_headers
    add     rax, [rsp + 8]
    mov     [rsp + 16], rax             ; the DATA frame starts here
    mov     rdi, rax
    mov     esi, [rsp]
    mov     edx, FT_DATA
    xor     ecx, ecx
    mov     r8d, [rbp + Stream.id]
    call    put_hdr
    ; The head is tiny (5-13 bytes: gRPC prefix + protobuf field headers):
    ; copy it inline with two overlapping loads/stores instead of a call.
    ; Both loads stay inside [head, head + len); stores stay inside the
    ; reservation.
    mov     rdi, [rsp + 16]
    add     rdi, FRAME_HDR
    cmp     r13, 16
    ja      .head_call
    cmp     r13, 8
    jb      .head_small
    mov     rax, [r12]
    mov     rcx, [r12 + r13 - 8]
    mov     [rdi], rax
    mov     [rdi + r13 - 8], rcx
    jmp     .head_done
.head_small:
    cmp     r13, 4
    jb      .head_call                  ; 1-3 bytes: never produced; stay general
    mov     eax, [r12]
    mov     ecx, [r12 + r13 - 4]
    mov     [rdi], eax
    mov     [rdi + r13 - 4], ecx
    jmp     .head_done
.head_call:
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
.head_done:
    test    r15, r15
    jz      .trailers
    mov     rdi, [rsp + 16]
    lea     rdi, [rdi + FRAME_HDR + r13]
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt
.trailers:
    mov     rax, [rsp]
    sub     [rbx + Conn.h2 + H2.send_window], rax
    sub     [rbp + Stream.send_window], rax
    mov     rsi, [rsp + 16]
    lea     rsi, [rsi + FRAME_HDR + rax]
    mov     [rsp + 16], rsi
    mov     rdi, rbx
    mov     edx, [rbp + Stream.id]
    call    put_trailers
    add     rax, [rsp + 16]
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax   ; trim the reservation
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done               ; 0, or -1 (out of memory for an RST)
    jmp     .out

    ; ---- the message needs more than one DATA frame (rax = its length) ----
    ; Every frame gets its own reservation: the output buffer may be handed
    ; to the socket between two frames (and may move when it grows), so no
    ; pointer into it is kept across a reservation or a drain. The windows
    ; were checked for the whole message above; they are charged frame by
    ; frame, so a reply that ends in .rest has paid exactly what it framed.
.multi:
    cmp     r13, 64
    ja      .later                      ; the head must fit the first frame beside body bytes
    mov     rdi, rbx
    mov     esi, FRAME_HDR + 32
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     [rsp + 8], rax
    mov     rdi, rbx
    mov     rsi, rax
    mov     edx, [rbp + Stream.id]
    call    put_response_headers
    add     rax, [rsp + 8]
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax   ; trim the reservation
    ; First DATA frame: a full one (the message is longer than a frame), with
    ; the head and the first peer_frame - head bytes of the body.
    mov     rsi, [rbx + Conn.h2 + H2.peer_frame]
    mov     [rsp + 24], rsi
    add     rsi, FRAME_HDR
    mov     rdi, rbx
    call    out_reserve
    test    rax, rax
    jz      .nomem                      ; HEADERS are out: -1 ends the connection
    mov     [rsp + 16], rax
    mov     rdi, rax
    mov     esi, [rsp + 24]
    mov     edx, FT_DATA
    xor     ecx, ecx
    mov     r8d, [rbp + Stream.id]
    call    put_hdr                     ; leaf: clobbers rax, rdx, r8 only
    mov     rdi, [rsp + 16]
    add     rdi, FRAME_HDR
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    mov     rdi, [rsp + 16]
    lea     rdi, [rdi + FRAME_HDR + r13]
    mov     rsi, r14
    mov     rdx, [rsp + 24]
    sub     rdx, r13                    ; body bytes that complete the first frame
    add     r14, rdx                    ; body cursor and bytes left, for the next frames
    sub     r15, rdx                    ; > 0: the message exceeds one frame
    call    memcpy wrt ..plt
    mov     rax, [rsp + 24]
    sub     [rbx + Conn.h2 + H2.send_window], rax
    sub     [rbp + Stream.send_window], rax
.multi_next:
    ; r14 = body cursor, r15 = body bytes left (> 0). At the high-water mark
    ; the queue goes to the socket before more is framed: the buffer never
    ; holds more than the mark plus one frame, as on the buffered path.
    cmp     qword [rbx + Conn.out_len], OUT_HIGH_WATER
    jb      .multi_frame
    mov     rdi, rbx
    call    out_try_drain
    test    eax, eax
    jnz     .rest                       ; the socket is full: buffer what is left
.multi_frame:
    mov     r12, [rbx + Conn.h2 + H2.peer_frame]
    cmp     r12, r15
    cmova   r12, r15                    ; this frame's payload: a full frame, or the rest
    lea     rsi, [r12 + FRAME_HDR]
    mov     rdi, rbx
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     [rsp + 16], rax
    mov     rdi, rax
    mov     esi, r12d
    mov     edx, FT_DATA
    xor     ecx, ecx
    mov     r8d, [rbp + Stream.id]
    call    put_hdr
    mov     rdi, [rsp + 16]
    add     rdi, FRAME_HDR
    mov     rsi, r14
    mov     rdx, r12
    call    memcpy wrt ..plt
    sub     [rbx + Conn.h2 + H2.send_window], r12
    sub     [rbp + Stream.send_window], r12
    add     r14, r12
    sub     r15, r12
    jnz     .multi_next
    ; The whole message is framed: the trailers end the stream.
    mov     rdi, rbx
    mov     esi, FRAME_HDR + 32
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     [rsp + 16], rax
    mov     rdi, rbx
    mov     rsi, rax
    mov     edx, [rbp + Stream.id]
    call    put_trailers
    add     rax, [rsp + 16]
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax   ; trim the reservation
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done               ; 0, or -1 (out of memory for an RST)
    jmp     .out
.rest:
    ; The socket refused the queue part-way through the message. What is
    ; left of the body (r14, r15 bytes) must outlive this call — the record
    ; is only valid while the worker is online — so the stream gets its own
    ; copy and continues as a buffered response whose HEADERS are out:
    ; emit_progress frames it as room and windows allow and ends it with the
    ; trailers.
    mov     rdi, r15
    mov     rsi, [rbx + Conn.worker]
    lea     rsi, [rsi + Worker.pool]
    call    kv_pool_malloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbp + Stream.resp_buf], rax
    mov     [rbp + Stream.resp_len], r15
    mov     qword [rbp + Stream.resp_off], 0
    or      qword [rbp + Stream.h2flags], SN_RESP_HEADERS
    mov     rdi, rax
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt
    mov     rdi, rbx
    mov     rsi, rbp
    call    emit_progress               ; 0 complete, 1 queued, -1 out of memory
    test    eax, eax
    js      .out
    xor     eax, eax                    ; answered or queued: the reply is under way
    jmp     .out
.later:
    mov     eax, 1
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    add     rsp, 40
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; reply_slow — HEADERS now, the message from resp_buf as windows allow.
; Internal.
; @param  rdi  Conn*   @param rsi  Stream* (resp_buf / resp_len set)
; @returns eax = 0, or -1 when out of memory.
; @remarks HEADERS are not flow-controlled, so they go out immediately;
;          emit_progress frames what the windows allow and queues the rest.
; =============================================================================
reply_slow:
    push    rbx
    push    rbp
    sub     rsp, 8                      ; [0] reservation start; aligned
    mov     rbx, rdi
    mov     rbp, rsi
    mov     esi, FRAME_HDR + 32
    call    out_reserve                 ; rdi = Conn*
    test    rax, rax
    jz      .nomem
    mov     [rsp], rax
    mov     rdi, rbx
    mov     rsi, rax
    mov     edx, [rbp + Stream.id]
    call    put_response_headers
    add     rax, [rsp]
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax
    or      qword [rbp + Stream.h2flags], SN_RESP_HEADERS
    mov     qword [rbp + Stream.resp_off], 0
    mov     rdi, rbx
    mov     rsi, rbp
    call    emit_progress
    test    eax, eax
    js      .out
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; emit_progress — frame as much of a stream's pending body as allowed.
; Internal.
; @param  rdi  Conn*   @param rsi  Stream* (response HEADERS already out)
; @returns eax = 0 (complete: trailers framed, stream closed), 1 (blocked:
;          queued on H2.pend), or -1 (out of memory).
; @remarks Each DATA frame is bounded by the connection window, the stream
;          window (both signed: SETTINGS may drive them negative), the
;          peer's max frame size and the remaining bytes. A gRPC body ends
;          with the trailers; a static HTTP body ends the stream on its last
;          DATA frame. A generated body (ST_GEN, Download) is filled frame by
;          frame by kv_grpc_gen_fill instead of copied from resp_buf, and each
;          of its frames is also held to the output room below
;          OUT_HIGH_WATER — a peer that allows 16 MiB frames must not grow
;          one connection's buffer to 16 MiB.
; =============================================================================
emit_progress:
    push    rbx
    push    rbp
    push    r12
    push    r13
    sub     rsp, 8                      ; 4 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
.chunk:
    mov     r12, [rbp + Stream.resp_len]
    sub     r12, [rbp + Stream.resp_off]
    jz      .finish
    mov     r13, r12                    ; remaining
    mov     rax, [rbx + Conn.h2 + H2.send_window]
    cmp     r12, rax
    cmovg   r12, rax
    mov     rax, [rbp + Stream.send_window]
    cmp     r12, rax
    cmovg   r12, rax
    mov     rax, [rbx + Conn.h2 + H2.peer_frame]
    cmp     r12, rax
    cmovg   r12, rax
    test    r12, r12
    jle     .blocked                    ; a window is shut (or negative)
    cmp     qword [rbx + Conn.out_len], OUT_HIGH_WATER
    jae     .blocked
    test    qword [rbp + Stream.flags], ST_GEN
    jnz     .gen
    xor     edx, edx
    cmp     r12, r13
    jne     .flags
    test    qword [rbp + Stream.flags], ST_STATIC_RESP
    jz      .flags
    mov     edx, FF_END_STREAM          ; last chunk of a plain HTTP body
.flags:
    mov     rdi, rbx
    mov     esi, FT_DATA
    mov     ecx, [rbp + Stream.id]
    mov     r8, [rbp + Stream.resp_buf]
    add     r8, [rbp + Stream.resp_off]
    mov     r9, r12
    call    write_frame
    test    eax, eax
    js      .nomem
    add     [rbp + Stream.resp_off], r12
    sub     [rbx + Conn.h2 + H2.send_window], r12
    sub     [rbp + Stream.send_window], r12
    jmp     .chunk
.gen:
    ; Generated body: at most the room left below OUT_HIGH_WATER (> 0, checked
    ; above), filled in place by the gRPC layer.
    mov     eax, OUT_HIGH_WATER
    sub     rax, [rbx + Conn.out_len]
    cmp     r12, rax
    cmova   r12, rax
    mov     rdi, rbx
    mov     rsi, rbp
    mov     rdx, r12
    call    write_frame_gen
    test    eax, eax
    js      .nomem
    add     [rbp + Stream.resp_off], r12
    sub     [rbx + Conn.h2 + H2.send_window], r12
    sub     [rbp + Stream.send_window], r12
    jmp     .chunk
.finish:
    test    qword [rbp + Stream.flags], ST_STATIC_RESP
    jnz     .closed
    mov     rdi, rbx
    mov     esi, FRAME_HDR + 32
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     r13, rax
    mov     rdi, rbx
    mov     rsi, rax
    mov     edx, [rbp + Stream.id]
    call    put_trailers
    add     rax, r13
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax
.closed:
    mov     rdi, rbx
    mov     rsi, rbp
    call    response_done               ; 0 (complete), or -1 (out of memory)
    jmp     .out
.blocked:
    mov     rdi, rbx
    mov     rsi, rbp
    call    pend_push
    mov     eax, 1
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    add     rsp, 8
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; put_response_headers — HEADERS(END_HEADERS): :status 200 + content-type
; application/grpc, in the encoder's current form. Internal.
; @param  rdi  Conn*   @param rsi  destination (>= 9 + 21 bytes)
; @param  edx  stream id
; @returns rax = bytes written (frame header included).
; @remarks FRESH inserts content-type (-> CT); CT and BOTH emit two index
;          bytes; LITERAL never touches the peer's table. A pending table
;          size update (H2F_TSU) opens the block.
; =============================================================================
put_response_headers:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13d, edx
    lea     rdi, [r12 + FRAME_HDR]      ; block cursor
    call    put_tsu
    mov     rax, [rbx + Conn.h2 + H2.enc]
    cmp     rax, H2E_BOTH
    je      .both
    cmp     rax, H2E_CT
    je      .ct
    cmp     rax, H2E_FRESH
    je      .fresh
    ; literal (the peer's table is off limits)
    movdqu  xmm0, [blk_ok_lit]
    movdqu  xmm1, [blk_ok_lit + blk_ok_lit_len - 16]
    movdqu  [rdi], xmm0
    movdqu  [rdi + blk_ok_lit_len - 16], xmm1
    add     rdi, blk_ok_lit_len
    jmp     .frame
.fresh:
    movdqu  xmm0, [blk_ok_fresh]
    movdqu  xmm1, [blk_ok_fresh + blk_ok_fresh_len - 16]
    movdqu  [rdi], xmm0
    movdqu  [rdi + blk_ok_fresh_len - 16], xmm1
    add     rdi, blk_ok_fresh_len
    mov     qword [rbx + Conn.h2 + H2.enc], H2E_CT
    jmp     .frame
.ct:
    mov     word [rdi], 0xBE88          ; :status 200, [62] content-type
    add     rdi, 2
    jmp     .frame
.both:
    mov     word [rdi], 0xBF88          ; :status 200, [63] content-type
    add     rdi, 2
.frame:
    mov     rsi, rdi
    sub     rsi, r12
    sub     rsi, FRAME_HDR              ; block length
    push    rsi
    push    rsi                         ; keeps alignment (put_hdr is a leaf anyway)
    mov     rdi, r12
    mov     edx, FT_HEADERS
    mov     ecx, FF_END_HEADERS
    mov     r8d, r13d
    call    put_hdr
    pop     rax
    pop     rax
    add     rax, FRAME_HDR
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; put_trailers — HEADERS(END_STREAM | END_HEADERS): grpc-status 0. Internal.
; @param  rdi  Conn*   @param rsi  destination (>= 9 + 16 bytes)
; @param  edx  stream id
; @returns rax = bytes written (frame header included).
; @remarks CT inserts grpc-status (-> BOTH, one index byte from then on);
;          FRESH cannot precede trailers normally (response HEADERS move it
;          to CT) and, like LITERAL, gets a literal without indexing.
; =============================================================================
put_trailers:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13d, edx
    lea     rdi, [r12 + FRAME_HDR]
    call    put_tsu
    mov     rax, [rbx + Conn.h2 + H2.enc]
    cmp     rax, H2E_BOTH
    je      .both
    lea     rsi, [blk_tr_lit]
    cmp     rax, H2E_CT
    jne     .copy
    lea     rsi, [blk_tr_insert]
    mov     qword [rbx + Conn.h2 + H2.enc], H2E_BOTH
.copy:
    movdqu  xmm0, [rsi]                 ; 15-byte block, read as 16 (both are
    movdqu  [rdi], xmm0                 ; followed by more .rodata; the extra
    add     rdi, blk_tr_len             ; byte lands in the reserved slack)
    jmp     .frame
.both:
    mov     byte [rdi], 0xBE            ; [62] grpc-status: 0
    inc     rdi
.frame:
    mov     rsi, rdi
    sub     rsi, r12
    sub     rsi, FRAME_HDR
    push    rsi
    push    rsi
    mov     rdi, r12
    mov     edx, FT_HEADERS
    mov     ecx, FF_END_HEADERS | FF_END_STREAM
    mov     r8d, r13d
    call    put_hdr
    pop     rax
    pop     rax
    add     rax, FRAME_HDR
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; put_tsu — open a header block with a pending table size update. Internal.
; @param  rbx  Conn*   @param rdi  block cursor (advanced by 0 or 1)
; @returns nothing. Leaf; clobbers nothing else.
; @remarks Size 0 (0x20): the peer shrank SETTINGS_HEADER_TABLE_SIZE and our
;          encoder switched to literals (RFC 7541 §4.2, §6.3).
; =============================================================================
put_tsu:
    test    qword [rbx + Conn.h2 + H2.flags], H2F_TSU
    jz      .out
    and     qword [rbx + Conn.h2 + H2.flags], ~H2F_TSU
    mov     byte [rdi], 0x20
    inc     rdi
.out:
    ret

; =============================================================================
; put_nv_headers — one HEADERS frame from a HdrField array. Internal.
; @param  rdi  Conn*   @param rsi  Stream*   @param rdx  HdrField array
; @param  ecx  count   @param r8d  frame flags (END_HEADERS [| END_STREAM])
; @returns eax = 0, or -1 when out of memory.
; =============================================================================
put_nv_headers:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    sub     rsp, 16                     ; 5 pushes + 16: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    mov     r12, rdx
    mov     r13d, ecx
    mov     r14d, r8d
    mov     esi, FRAME_HDR + H2_NV_MAX
    call    out_reserve                 ; rdi = Conn*
    test    rax, rax
    jz      .nomem
    mov     [rsp], rax
    mov     rdi, rbx
    mov     rsi, r12
    mov     edx, r13d
    lea     rcx, [rax + FRAME_HDR]
    call    encode_nv
    mov     [rsp + 8], rax              ; block length
    mov     rdi, [rsp]
    mov     esi, eax
    mov     edx, FT_HEADERS
    mov     ecx, r14d
    mov     r8d, [rbp + Stream.id]
    call    put_hdr
    mov     rax, [rsp]
    add     rax, FRAME_HDR
    add     rax, [rsp + 8]
    sub     rax, [rbx + Conn.out_buf]
    mov     [rbx + Conn.out_len], rax   ; trim the reservation
    xor     eax, eax
    jmp     .out
.nomem:
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
; encode_nv — HPACK-encode a HdrField array (server.inc). Internal.
; @param  rdi  Conn*   @param rsi  HdrField array   @param edx  count
; @param  rcx  destination (H2_NV_MAX bytes suffice for grpc.asm's arrays:
;              at most 5 fields of short constant strings)
; @returns rax = bytes written.
; @remarks Error paths only, so clarity over speed: ":status: 200" is static
;          entry 8, other statuses name it; "content-type: application/grpc"
;          reuses our dynamic entry when present; everything else is a
;          literal without indexing, so our table state never changes here.
; =============================================================================
encode_nv:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13d, edx
    mov     r14, rcx                    ; start
    mov     rdi, rcx
    call    put_tsu                     ; rbx = Conn*
    mov     r15, rdi                    ; cursor
.field:
    test    r13d, r13d
    jz      .done
    cmp     qword [r12 + HdrField.namelen], 7
    jne     .not_status
    mov     rdi, [r12 + HdrField.name]
    lea     rsi, [s_status]
    mov     edx, 7
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .not_status
    cmp     qword [r12 + HdrField.valuelen], 3
    jne     .status_literal
    mov     rdi, [r12 + HdrField.value]
    lea     rsi, [v_200]
    mov     edx, 3
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .status_literal
    mov     byte [r15], 0x88            ; static entry 8
    inc     r15
    jmp     .advance
.status_literal:
    mov     byte [r15], 0x08            ; literal without indexing, name = entry 8
    inc     r15
    jmp     .value
.not_status:
    cmp     qword [r12 + HdrField.namelen], 12
    jne     .new_name
    mov     rdi, [r12 + HdrField.name]
    lea     rsi, [s_ctype]
    mov     edx, 12
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .new_name
    cmp     qword [r12 + HdrField.valuelen], 16
    jne     .ctype_literal
    mov     rdi, [r12 + HdrField.value]
    lea     rsi, [v_grpc]
    mov     edx, 16
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .ctype_literal
    mov     rax, [rbx + Conn.h2 + H2.enc]
    mov     cl, 0xBE                    ; CT: content-type is entry 62
    cmp     rax, H2E_CT
    je      .ctype_indexed
    mov     cl, 0xBF                    ; BOTH: entry 63
    cmp     rax, H2E_BOTH
    jne     .ctype_literal
.ctype_indexed:
    mov     [r15], cl
    inc     r15
    jmp     .advance
.ctype_literal:
    mov     word [r15], 0x100F          ; literal without indexing, name = entry 31
    add     r15, 2
    jmp     .value
.new_name:
    mov     byte [r15], 0x00            ; literal without indexing, new name
    lea     rdi, [r15 + 1]
    mov     rsi, [r12 + HdrField.name]
    mov     rdx, [r12 + HdrField.namelen]
    call    put_string
    mov     r15, rax
.value:
    mov     rdi, r15
    mov     rsi, [r12 + HdrField.value]
    mov     rdx, [r12 + HdrField.valuelen]
    call    put_string
    mov     r15, rax
.advance:
    add     r12, HdrField_size
    dec     r13d
    jmp     .field
.done:
    mov     rax, r15
    sub     rax, r14
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; put_string — raw HPACK string literal (H = 0). Internal.
; @param  rdi  destination   @param rsi  bytes   @param rdx  length
; @returns rax = the byte after the literal.
; @remarks The length is an HPACK integer with a 7-bit prefix (§5.1).
; =============================================================================
put_string:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     rax, rdx
    cmp     rax, 127
    jae     .long
    mov     [rbx], al
    inc     rbx
    jmp     .copy
.long:
    mov     byte [rbx], 0x7F
    inc     rbx
    sub     rax, 127
.more:
    cmp     rax, 128
    jb      .last
    mov     ecx, eax
    or      cl, 0x80                    ; 7 value bits + continuation
    mov     [rbx], cl
    inc     rbx
    shr     rax, 7
    jmp     .more
.last:
    mov     [rbx], al
    inc     rbx
.copy:
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    lea     rax, [rbx + r13]
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; out_reserve — n bytes at the end of the output buffer. Internal.
; @param  rdi  Conn*   @param rsi  n
; @returns rax = pointer to the n reserved bytes (Conn.out_len already
;          advanced past them), or 0 when the buffer could not grow.
; @remarks Callers that reserve generously set Conn.out_len back to their
;          real end before anything else writes. Grows geometrically with
;          realloc: out_buf is malloc'd by worker.asm and freed with free.
; =============================================================================
out_reserve:
    mov     rax, [rdi + Conn.out_len]
    lea     rdx, [rax + rsi]
    cmp     rdx, [rdi + Conn.out_cap]
    ja      .grow
    mov     [rdi + Conn.out_len], rdx
    add     rax, [rdi + Conn.out_buf]
    ret
.grow:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for the call
    mov     rbx, rdi
    mov     r12, rdx                    ; needed length
    mov     r13, [rbx + Conn.out_cap]
    shl     r13, 1
    cmp     r13, r12
    cmovb   r13, r12
    mov     rdi, [rbx + Conn.out_buf]
    mov     rsi, r13
    call    realloc wrt ..plt
    test    rax, rax
    jz      .out                        ; the old buffer is still valid and owned
    mov     [rbx + Conn.out_buf], rax
    mov     [rbx + Conn.out_cap], r13
    mov     rcx, [rbx + Conn.out_len]
    mov     [rbx + Conn.out_len], r12
    add     rax, rcx
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; out_try_drain — send the queued output now, before the worker's flush.
; Internal.
; @param  rdi  Conn*
; @returns eax = 0 when the output buffer is empty afterwards (out_len and
;          out_off reset: the caller may frame into it from the start), or 1
;          when bytes remain — the socket is full, a send failed, or replies
;          are queued on the pend list.
; @remarks When to use: only when the next reply would take the buffer past
;          OUT_HIGH_WATER (called by reply_fast). Sending early gives up the
;          one-burst-per-pass property of the two-phase loop (ADR 0009) for
;          this connection, which is the right trade there: the peer gets
;          tens of KiB to read, so it is awake either way, and the
;          alternative is copying every further reply of the read into a
;          private buffer.
;          Nothing else of the worker's flush is done here — no pump, no epoll
;          interest change, no close: phase 2 still runs conn_flush for this
;          connection and finds either an empty buffer or the unsent rest
;          (out_off advanced), exactly the states it already handles. A send
;          error is left for that flush to meet again and act on.
;          Footgun: one refused send sets H2F_OUT_FULL, and every later call
;          returns 1 without a syscall until kv_h2n_read clears it — callers
;          must not expect a retry within the same read.
;          Replies waiting on the pend list keep their turn: with any queued,
;          nothing is sent here and the caller buffers as before.
;          Raw syscall: clobbers rax, rcx, rdx, rsi, rdi, r8-r11.
; =============================================================================
out_try_drain:
    cmp     qword [rdi + Conn.h2 + H2.pend_head], 0
    jne     .busy
    test    qword [rdi + Conn.h2 + H2.flags], H2F_OUT_FULL
    jnz     .busy
    push    rbx                         ; no calls below, only syscalls: alignment is moot
    mov     rbx, rdi
.send:
    mov     rdx, [rbx + Conn.out_len]
    mov     rax, [rbx + Conn.out_off]
    sub     rdx, rax
    jz      .empty
    mov     rdi, [rbx + Conn.fd]
    mov     rsi, [rbx + Conn.out_buf]
    add     rsi, rax
    mov     r10d, MSG_NOSIGNAL
    xor     r8d, r8d
    xor     r9d, r9d
    mov     eax, SYS_sendto
    syscall
    cmp     rax, -EINTR
    je      .send
    test    rax, rax
    jle     .full                       ; EAGAIN, or an error the worker's flush will see
    add     [rbx + Conn.out_off], rax
    ; Output the peer accepts is activity (as in conn_flush): the idle timer
    ; must not cut a connection that is only receiving.
    mov     rcx, [rbx + Conn.worker]
    mov     rcx, [rcx + Worker.now]
    mov     [rbx + Conn.last_active], rcx
    jmp     .send                       ; a short send: retry until done or EAGAIN
.empty:
    xor     eax, eax
    mov     [rbx + Conn.out_len], rax
    mov     [rbx + Conn.out_off], rax
    pop     rbx
    ret
.full:
    or      qword [rbx + Conn.h2 + H2.flags], H2F_OUT_FULL
    pop     rbx
.busy:
    mov     eax, 1
    ret

; =============================================================================
; put_hdr — write a 9-byte frame header. Internal, leaf.
; @param  rdi  destination   @param esi  payload length (< 2^24)
; @param  edx  type   @param ecx  flags   @param r8d  stream id
; @returns nothing. Clobbers rax, rdx, r8.
; =============================================================================
put_hdr:
    mov     eax, esi
    shl     eax, 8
    bswap   eax                         ; length bytes big-endian, low byte free
    shl     edx, 24
    or      eax, edx                    ; ... followed by the type
    mov     [rdi], eax
    mov     [rdi + 4], cl
    bswap   r8d
    mov     [rdi + 5], r8d
    ret

; =============================================================================
; write_frame — append one complete frame to the output buffer. Internal.
; @param  rdi  Conn*   @param esi  type   @param edx  flags
; @param  ecx  stream id   @param r8  payload   @param r9  payload length
; @returns eax = 0, or -1 when out of memory.
; =============================================================================
write_frame:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [0] frame start; aligned for calls
    mov     rbx, rdi
    mov     r12d, esi
    mov     r13d, edx
    mov     r14d, ecx
    mov     rbp, r8
    mov     r15, r9
    lea     rsi, [r9 + FRAME_HDR]
    call    out_reserve
    test    rax, rax
    jz      .nomem
    mov     [rsp], rax
    mov     rdi, rax
    mov     esi, r15d
    mov     edx, r12d
    mov     ecx, r13d
    mov     r8d, r14d
    call    put_hdr
    test    r15, r15
    jz      .ok
    mov     rdi, [rsp]
    add     rdi, FRAME_HDR
    mov     rsi, rbp
    mov     rdx, r15
    call    memcpy wrt ..plt
.ok:
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
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
; write_frame_gen — append one DATA frame whose payload the gRPC layer
; generates in place (ST_GEN streams). Internal.
; @param  rdi  Conn*   @param rsi  Stream*   @param rdx  payload length n
;              (1 .. FRAME_MAX; the caller already bounded it by the windows)
; @returns eax = 0, or -1 when out of memory.
; @remarks kv_grpc_gen_fill writes exactly n bytes of the body — the next n
;          after Stream.resp_off — straight into the output buffer: for a
;          Download that is one copy from the pinned record, checksummed on
;          the way (the caller advances resp_off and the windows afterwards).
; =============================================================================
write_frame_gen:
    push    rbx
    push    rbp
    push    r12
    push    r13
    sub     rsp, 8                      ; 4 pushes + 8: aligned for calls
    mov     rbx, rdi
    mov     rbp, rsi
    mov     r12, rdx
    lea     rsi, [r12 + FRAME_HDR]
    call    out_reserve                 ; rdi = Conn*
    test    rax, rax
    jz      .nomem
    mov     r13, rax
    mov     rdi, rax
    mov     esi, r12d
    mov     edx, FT_DATA
    xor     ecx, ecx
    mov     r8d, [rbp + Stream.id]
    call    put_hdr
    mov     rdi, rbp
    lea     rsi, [r13 + FRAME_HDR]
    mov     rdx, r12
    call    kv_grpc_gen_fill wrt ..plt
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    add     rsp, 8
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; send_rst / send_goaway / send_window_update — small control frames.
; Internal.
; send_rst:           @param rdi Conn*  @param esi stream id  @param edx code
; send_goaway:        @param rdi Conn*  @param esi code (last stream id =
;                     H2.last_id: every stream up to it may still be served)
; send_window_update: @param rdi Conn*  @param esi stream id (0 = connection)
;                     @param edx increment (1 .. 2^31 - 1)
; @returns eax = 0, or -1 when out of memory.
; =============================================================================
send_rst:
    sub     rsp, 24                     ; [0..4) payload; aligned for the call
    bswap   edx
    mov     [rsp], edx
    mov     ecx, esi
    mov     esi, FT_RST_STREAM
    xor     edx, edx
    mov     r8, rsp
    mov     r9d, 4
    call    write_frame
    add     rsp, 24
    ret

send_goaway:
    sub     rsp, 24                     ; [0..8) payload; aligned for the call
    mov     eax, [rdi + Conn.h2 + H2.last_id]
    bswap   eax
    mov     [rsp], eax
    bswap   esi
    mov     [rsp + 4], esi
    mov     esi, FT_GOAWAY
    xor     edx, edx
    xor     ecx, ecx
    mov     r8, rsp
    mov     r9d, 8
    call    write_frame
    add     rsp, 24
    ret

send_window_update:
    sub     rsp, 24                     ; [0..4) payload; aligned for the call
    bswap   edx
    mov     [rsp], edx
    mov     ecx, esi
    mov     esi, FT_WINDOW_UPDATE
    xor     edx, edx
    mov     r8, rsp
    mov     r9d, 4
    call    write_frame
    add     rsp, 24
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
