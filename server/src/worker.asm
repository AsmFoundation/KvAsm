; =============================================================================
; worker.asm — one event loop per worker thread (thread-per-core).
;
; Each worker owns: an epoll instance, its own listening socket (SO_REUSEPORT,
; so the kernel spreads new connections across workers — no accept lock and
; no hand-off), an eventfd doorbell for shutdown, and every connection it
; accepted, together with that connection's nghttp2 session. Nothing here is
; shared with other workers except the store, so no locks are taken.
;
; Per connection, I/O is non-blocking and level-triggered with half-duplex
; back-pressure: while output is pending the connection is watched for
; EPOLLOUT only (its input is not read), so a client that sends requests but
; never reads responses cannot make the server buffer unbounded replies.
; Output is coalesced: every frame nghttp2 produces in one pass is copied into
; one buffer and sent with one sendto (measured: 3 syscalls/request -> ~1
; per batch).
;
; QSBR: the worker is an online reader while it processes events, announces a
; quiescent state at the top of every loop pass, and goes offline only while
; blocked in epoll_wait — so an idle worker never delays memory reclamation.
;
; Shutdown: main rings the eventfd; the worker closes its listener, sends
; GOAWAY on every connection (in-flight calls complete), waits for sessions to
; finish, and force-closes whatever remains after the grace period.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"
%include "server.inc"
%include "nghttp2.inc"

%define SYS_close           3
%define SYS_socket          41
%define SYS_accept4         288
%define SYS_bind            49
%define SYS_listen          50
%define SYS_setsockopt      54
%define SYS_epoll_wait      232
%define SYS_epoll_ctl       233
%define SYS_epoll_create1   291
%define SYS_eventfd2        290

%define AF_INET             2
%define SOCK_STREAM         1
%define SOCK_NONBLOCK       0x800
%define SOCK_CLOEXEC        0x80000
%define SOL_SOCKET          1
%define SO_REUSEADDR        2
%define SO_REUSEPORT        15
%define IPPROTO_TCP         6
%define TCP_NODELAY         1
%define MSG_NOSIGNAL        0x4000
%define SHUT_WR             1
%define EPOLLIN             0x001
%define EPOLLOUT            0x004
%define EPOLLERR            0x008
%define EPOLLHUP            0x010
%define EPOLL_CTL_ADD       1
%define EPOLL_CTL_DEL       2
%define EPOLL_CTL_MOD       3
%define EPOLL_CLOEXEC       0x80000
%define EFD_NONBLOCK        0x800
%define EFD_CLOEXEC         0x80000
%define ECONNABORTED        103
%define CLOCK_MONOTONIC     1
%define LISTEN_BACKLOG      4096
%define LISTEN_TAG          1           ; epoll data of the listener
%define EVENT_TAG           2           ; epoll data of the eventfd
%define TICK_NS             1000000000  ; housekeeping period
%define LINGER_NS           2000000000  ; max time to wait for the peer's FIN
%define ACCEPT_BURST        64          ; accepts per readiness event (fairness)
%define EVENT_SIZE          12          ; struct epoll_event is packed on x86-64
%define STREAM_WINDOW       1048576     ; per-stream receive window we grant
%define MAX_WINDOW          0x7FFFFFFF

extern aligned_alloc
extern malloc
extern calloc
extern realloc
extern free
extern memcpy
extern memset
extern clock_gettime
extern pthread_create
extern pthread_join
extern kv_qsbr_online
extern kv_qsbr_offline
extern kv_qsbr_quiescent
extern kv_store_reclaim
extern kv_grpc_free_streams
extern kv_logb_init
extern kv_logb_str
extern kv_logb_u64
extern kv_logb_emit
extern kv_logbuf_size
extern nghttp2_session_server_new
extern nghttp2_session_del
extern nghttp2_session_mem_recv2
extern nghttp2_session_mem_send2
extern nghttp2_session_want_read
extern nghttp2_session_want_write
extern nghttp2_session_set_local_window_size
extern nghttp2_session_terminate_session
extern nghttp2_session_get_last_proc_stream_id
extern nghttp2_submit_settings
extern nghttp2_submit_goaway

section .rodata
lvl_warn:       db "WARN", 0
lvl_error:      db "ERROR", 0
m_create:       db " msg=", 34, "worker setup failed", 34, " step="
m_create_len    equ $ - m_create
m_errno:        db " errno="
m_worker:       db " worker="
m_accept:       db " msg=", 34, "accept paused: out of file descriptors", 34, " errno="
m_accept_len    equ $ - m_accept
m_epoll:        db " msg=", 34, "epoll_wait failed", 34, " errno="
m_epoll_len     equ $ - m_epoll
st_epoll:       db "epoll_create1", 0
st_eventfd:     db "eventfd", 0
st_socket:      db "socket", 0
st_bind:        db "bind", 0
st_listen:      db "listen", 0
st_alloc:       db "alloc", 0
align 4
int_one:        dd 1

section .text

global kv_port_probe
global kv_worker_create
global kv_worker_start
global kv_worker_stop
global kv_worker_join

; =============================================================================
; kv_port_probe — fail fast if another process already holds the TCP port.
;
; @param  rdi  port (1..65535, host byte order)
; @returns rax = 0 if the port is free, or -errno from socket()/bind()
;          (-EADDRINUSE when any other socket is bound or listening on it).
; @remarks Why this exists: the workers bind with SO_REUSEPORT, and Linux
;          lets every process with the same effective UID join a REUSEPORT
;          group. A second kvnode started on the same port (host networking,
;          a dev machine, a botched rolling restart) would therefore NOT fail
;          to bind — it would silently receive a share of new connections and
;          answer them from its own, different store. The probe binds WITHOUT
;          SO_REUSEPORT, which the kernel refuses while any socket holds the
;          port, so an instance that is already serving is always detected.
;          SO_REUSEADDR is still set so connections of a previous run in
;          TIME_WAIT do not block a restart. The probe is closed before the
;          workers bind (a bound, never-listening socket releases the port
;          immediately); two instances started in the same instant could both
;          pass — inherent to check-then-act — but never one started while
;          another is serving.
; =============================================================================
kv_port_probe:
    push    rbx
    push    r12
    sub     rsp, 24                     ; sockaddr_in scratch (16) + alignment
    mov     r12, rdi
    mov     edi, AF_INET
    mov     esi, SOCK_STREAM | SOCK_CLOEXEC
    xor     edx, edx
    mov     eax, SYS_socket
    syscall
    test    rax, rax
    js      .out                        ; -errno straight to the caller
    mov     rbx, rax
    mov     rdi, rbx
    mov     esi, SOL_SOCKET
    mov     edx, SO_REUSEADDR           ; deliberately no SO_REUSEPORT
    lea     r10, [int_one]
    mov     r8d, 4
    mov     eax, SYS_setsockopt
    syscall
    mov     qword [rsp], 0              ; sockaddr_in {AF_INET, htons(port), ANY}
    mov     qword [rsp + 8], 0
    mov     word [rsp], AF_INET
    mov     eax, r12d
    xchg    al, ah                      ; htons
    mov     [rsp + 2], ax
    mov     rdi, rbx
    mov     rsi, rsp
    mov     edx, 16
    mov     eax, SYS_bind
    syscall
    mov     r12, rax                    ; 0 or -errno, kept across close
    mov     rdi, rbx
    mov     eax, SYS_close
    syscall
    mov     rax, r12
.out:
    add     rsp, 24
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_worker_create — allocate a worker and bind its listening socket.
;
; @param  rdi  worker id (also its QSBR reader slot)
; @param  rsi  KvTable*          @param rdx  const Config*
; @param  rcx  nghttp2_session_callbacks* (shared, read-only)
; @returns rax = Worker*, or 0 on failure (the failing step and errno are
;          logged; partially created resources are released).
; @remarks Runs on the main thread before any worker starts, so a port that
;          cannot be bound fails startup synchronously instead of leaving a
;          half-working server.
; =============================================================================
kv_worker_create:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24                     ; [0..16) sockaddr_in / epoll_event scratch
    ; Callers run kv_port_probe first: SO_REUSEPORT below would otherwise let
    ; this listener silently join a group owned by another process.
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    mov     r15, rcx
    mov     edi, 64
    mov     esi, (Worker_size + 63) & ~63
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail_alloc_early
    mov     rbx, rax
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, (Worker_size + 63) & ~63
    call    memset wrt ..plt
    mov     [rbx + Worker.id], r12
    mov     [rbx + Worker.table], r13
    mov     [rbx + Worker.cfg], r14
    mov     [rbx + Worker.callbacks], r15
    mov     qword [rbx + Worker.epfd], -1
    mov     qword [rbx + Worker.listen_fd], -1
    mov     qword [rbx + Worker.event_fd], -1

    mov     edi, READ_CHUNK
    call    malloc wrt ..plt
    mov     [rbx + Worker.read_buf], rax
    test    rax, rax
    jz      .fail_alloc
    mov     edi, MAX_EVENTS * EVENT_SIZE
    call    malloc wrt ..plt
    mov     [rbx + Worker.events], rax
    test    rax, rax
    jz      .fail_alloc

    mov     edi, EPOLL_CLOEXEC
    mov     eax, SYS_epoll_create1
    syscall
    lea     rbp, [st_epoll]
    test    rax, rax
    js      .fail_errno
    mov     [rbx + Worker.epfd], rax

    xor     edi, edi
    mov     esi, EFD_NONBLOCK | EFD_CLOEXEC
    mov     eax, SYS_eventfd2
    syscall
    lea     rbp, [st_eventfd]
    test    rax, rax
    js      .fail_errno
    mov     [rbx + Worker.event_fd], rax

    mov     edi, AF_INET
    mov     esi, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC
    xor     edx, edx
    mov     eax, SYS_socket
    syscall
    lea     rbp, [st_socket]
    test    rax, rax
    js      .fail_errno
    mov     [rbx + Worker.listen_fd], rax
    ; SO_REUSEADDR (fast restarts) and SO_REUSEPORT (one listener per worker,
    ; kernel load-balances new connections). Failures surface at bind.
    mov     rdi, rax
    mov     esi, SOL_SOCKET
    mov     edx, SO_REUSEADDR
    lea     r10, [int_one]
    mov     r8d, 4
    mov     eax, SYS_setsockopt
    syscall
    mov     rdi, [rbx + Worker.listen_fd]
    mov     esi, SOL_SOCKET
    mov     edx, SO_REUSEPORT
    lea     r10, [int_one]
    mov     r8d, 4
    mov     eax, SYS_setsockopt
    syscall

    ; sockaddr_in {AF_INET, htons(port), INADDR_ANY}
    mov     qword [rsp], 0
    mov     qword [rsp + 8], 0
    mov     word [rsp], AF_INET
    mov     rax, [r14 + Config.port]
    xchg    al, ah                      ; htons
    mov     [rsp + 2], ax
    mov     rdi, [rbx + Worker.listen_fd]
    mov     rsi, rsp
    mov     edx, 16
    mov     eax, SYS_bind
    syscall
    lea     rbp, [st_bind]
    test    rax, rax
    js      .fail_errno
    mov     rdi, [rbx + Worker.listen_fd]
    mov     esi, LISTEN_BACKLOG
    mov     eax, SYS_listen
    syscall
    lea     rbp, [st_listen]
    test    rax, rax
    js      .fail_errno

    mov     rdi, rbx
    mov     rsi, [rbx + Worker.listen_fd]
    mov     edx, LISTEN_TAG
    call    epoll_add_in
    lea     rbp, [st_epoll]
    test    rax, rax
    js      .fail_errno
    mov     rdi, rbx
    mov     rsi, [rbx + Worker.event_fd]
    mov     edx, EVENT_TAG
    call    epoll_add_in
    test    rax, rax
    js      .fail_errno
    mov     rax, rbx
    jmp     .out

.fail_alloc:
    lea     rbp, [st_alloc]
    mov     rax, -ENOMEM
.fail_errno:
    mov     rdi, rbp
    mov     rsi, rax
    mov     rdx, r12
    call    log_setup_failure
    mov     rdi, rbx
    call    worker_free
    xor     eax, eax
    jmp     .out
.fail_alloc_early:
    xor     eax, eax
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
; kv_worker_start — run the worker's event loop on a new thread.
; @param  rdi  Worker*
; @returns rax = 0 on success, else the pthread_create error number.
; @remarks The new thread inherits main's blocked signal mask, so termination
;          signals are only ever delivered to main's sigwaitinfo.
; =============================================================================
kv_worker_start:
    push    rbx
    mov     rbx, rdi
    lea     rdi, [rbx + Worker.thread]
    xor     esi, esi
    lea     rdx, [worker_main]
    mov     rcx, rbx
    call    pthread_create wrt ..plt
    pop     rbx
    ret

; =============================================================================
; kv_worker_stop — ask a worker to drain and exit (async-signal-safe).
; @param  rdi  Worker*
; @returns nothing.
; @remarks Rings the eventfd; the worker notices on its next epoll_wait.
; =============================================================================
kv_worker_stop:
    sub     rsp, 8
    mov     qword [rsp], 1
    mov     rdi, [rdi + Worker.event_fd]
    mov     rsi, rsp
    mov     edx, 8
    mov     eax, SYS_write
    syscall
    add     rsp, 8
    ret

; =============================================================================
; kv_worker_join — wait for a stopped worker and free it.
; @param  rdi  Worker*
; @param  rsi  uint64_t stats[3] out: accepted, requests, errors (may be NULL)
; @returns nothing.
; =============================================================================
kv_worker_join:
    push    rbx
    push    r12
    sub     rsp, 8
    mov     rbx, rdi
    mov     r12, rsi
    mov     rdi, [rbx + Worker.thread]
    xor     esi, esi
    call    pthread_join wrt ..plt
    test    r12, r12
    jz      .free
    mov     rax, [rbx + Worker.st_accepted]
    mov     [r12], rax
    mov     rax, [rbx + Worker.st_requests]
    mov     [r12 + 8], rax
    mov     rax, [rbx + Worker.st_errors]
    mov     [r12 + 16], rax
.free:
    mov     rdi, rbx
    call    worker_free
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; worker_free — close a worker's descriptors and free its memory. Internal.
; @param  rdi  Worker* (NULL is a no-op). Connections must already be closed.
; =============================================================================
worker_free:
    test    rdi, rdi
    jz      .none
    push    rbx
    mov     rbx, rdi
    mov     rdi, [rbx + Worker.listen_fd]
    call    close_if_open
    mov     rdi, [rbx + Worker.event_fd]
    call    close_if_open
    mov     rdi, [rbx + Worker.epfd]
    call    close_if_open
    mov     rdi, [rbx + Worker.read_buf]
    call    free wrt ..plt
    mov     rdi, [rbx + Worker.events]
    call    free wrt ..plt
    mov     rdi, rbx
    call    free wrt ..plt
    pop     rbx
.none:
    ret

; close_if_open — close(fd) when fd >= 0. Internal. @param rdi fd
close_if_open:
    test    rdi, rdi
    js      .skip
    mov     eax, SYS_close
    syscall
.skip:
    ret

; =============================================================================
; epoll_add_in — register fd for EPOLLIN with the given epoll data. Internal.
; @param  rdi  Worker*   @param rsi  fd   @param rdx  data (tag or Conn*)
; @returns rax = 0 or -errno.
; =============================================================================
epoll_add_in:
    sub     rsp, 24
    mov     dword [rsp], EPOLLIN
    mov     [rsp + 4], rdx              ; packed struct: data at offset 4
    mov     r10, rsp
    mov     rdx, rsi
    mov     rdi, [rdi + Worker.epfd]
    mov     esi, EPOLL_CTL_ADD
    mov     eax, SYS_epoll_ctl
    syscall
    add     rsp, 24
    ret

; =============================================================================
; log_setup_failure — "worker setup failed step=... errno=... worker=...".
; Internal.  @param rdi step name  @param rsi -errno  @param rdx worker id
; =============================================================================
log_setup_failure:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    call    kv_logbuf_size
    sub     rsp, rax
    mov     r15, rax
    mov     rbx, rsp
    mov     rdi, rbx
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rbx
    lea     rsi, [m_create]
    mov     edx, m_create_len
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, r12
    call    cstr_append
    mov     rdi, rbx
    lea     rsi, [m_errno]
    mov     edx, 7
    call    kv_logb_str
    mov     rsi, r13
    neg     rsi
    mov     rdi, rbx
    call    kv_logb_u64
    mov     rdi, rbx
    lea     rsi, [m_worker]
    mov     edx, 8
    call    kv_logb_str
    mov     rdi, rbx
    mov     rsi, r14
    call    kv_logb_u64
    mov     rdi, rbx
    call    kv_logb_emit
    add     rsp, r15
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; cstr_append — append a NUL-terminated string to a LogBuf. Internal.
; @param rdi LogBuf*  @param rsi string
cstr_append:
    xor     edx, edx
.len:
    cmp     byte [rsi + rdx], 0
    je      .go
    inc     rdx
    jmp     .len
.go:
    jmp     kv_logb_str wrt ..plt

; =============================================================================
; worker_main — the event loop (thread entry point).
; @param  rdi  Worker*
; @returns rax = 0 (thread exit value, unused).
; =============================================================================
worker_main:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     rbx, rdi
    mov     r12, [rbx + Worker.table]
    mov     r13, [rbx + Worker.id]
    mov     rdi, r12
    mov     esi, r13d
    call    kv_qsbr_online wrt ..plt
    mov     rdi, rbx
    call    update_now
    mov     rax, [rbx + Worker.now]
    add     rax, TICK_NS
    mov     [rbx + Worker.next_tick], rax

.loop:
    mov     rdi, r12
    mov     esi, r13d
    call    kv_qsbr_quiescent wrt ..plt ; no record pointers are held here
    mov     rdi, [rbx + Worker.epfd]
    mov     rsi, [rbx + Worker.events]
    mov     edx, MAX_EVENTS
    xor     r10d, r10d                  ; poll: do not block yet
    mov     eax, SYS_epoll_wait
    syscall
    test    rax, rax
    jnz     .have_events
    ; Nothing ready: go offline so reclamation never waits on an idle worker.
    mov     rdi, r12
    mov     esi, r13d
    call    kv_qsbr_offline wrt ..plt
    mov     rdi, [rbx + Worker.epfd]
    mov     rsi, [rbx + Worker.events]
    mov     edx, MAX_EVENTS
    mov     r10d, 1000
    cmp     qword [rbx + Worker.draining], 0
    je      .wait
    mov     r10d, 100                   ; drain checks run more often
.wait:
    mov     eax, SYS_epoll_wait
    syscall
    mov     r14, rax
    mov     rdi, r12
    mov     esi, r13d
    call    kv_qsbr_online wrt ..plt    ; XCHG fence before touching the table
    mov     rax, r14
.have_events:
    mov     r14, rax
    mov     rdi, rbx
    call    update_now
    test    r14, r14
    jns     .dispatch
    cmp     r14, -EINTR
    je      .after_events
    mov     rdi, r14
    call    log_epoll_failure
    jmp     .exit
.dispatch:
    xor     r15d, r15d
.next_event:
    cmp     r15, r14
    jae     .after_events
    lea     rax, [r15 + r15*2]
    shl     rax, 2                      ; *12
    add     rax, [rbx + Worker.events]
    mov     esi, [rax]                  ; events mask
    mov     rdx, [rax + 4]              ; data
    cmp     rdx, LISTEN_TAG
    je      .ev_listen
    cmp     rdx, EVENT_TAG
    je      .ev_doorbell
    mov     rdi, rbx
    call    conn_event
    jmp     .ev_done
.ev_listen:
    mov     rdi, rbx
    call    accept_all
    jmp     .ev_done
.ev_doorbell:
    mov     rdi, [rbx + Worker.event_fd]
    mov     rsi, rsp
    mov     edx, 8
    xor     eax, eax                    ; SYS_read: consume the counter
    syscall
    cmp     qword [rbx + Worker.draining], 0
    jne     .ev_done
    mov     rdi, rbx
    call    begin_drain
.ev_done:
    inc     r15
    jmp     .next_event

.after_events:
    mov     rax, [rbx + Worker.now]
    cmp     rax, [rbx + Worker.next_tick]
    jb      .check_drain
    add     rax, TICK_NS
    mov     [rbx + Worker.next_tick], rax
    mov     rdi, rbx
    call    housekeeping
.check_drain:
    cmp     qword [rbx + Worker.draining], 0
    je      .loop
    cmp     qword [rbx + Worker.nconns], 0
    je      .exit
    mov     rax, [rbx + Worker.now]
    cmp     rax, [rbx + Worker.drain_deadline]
    jb      .loop
    mov     rdi, rbx
    call    close_all_now               ; grace period over
.exit:
    mov     rdi, r12
    mov     esi, r13d
    call    kv_qsbr_offline wrt ..plt
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
; update_now — refresh Worker.now (CLOCK_MONOTONIC, vDSO). Internal.
; @param  rdi  Worker*
; =============================================================================
update_now:
    push    rbx
    sub     rsp, 16
    mov     rbx, rdi
    mov     edi, CLOCK_MONOTONIC
    mov     rsi, rsp
    call    clock_gettime wrt ..plt
    mov     rax, [rsp]
    imul    rax, rax, 1000000000
    add     rax, [rsp + 8]
    mov     [rbx + Worker.now], rax
    add     rsp, 16
    pop     rbx
    ret

; log_epoll_failure — fatal epoll_wait error. Internal. @param rdi -errno
log_epoll_failure:
    push    rbx
    push    r12
    push    r13
    mov     r12, rdi
    call    kv_logbuf_size
    sub     rsp, rax
    mov     r13, rax
    mov     rbx, rsp
    mov     rdi, rbx
    lea     rsi, [lvl_error]
    call    kv_logb_init
    mov     rdi, rbx
    lea     rsi, [m_epoll]
    mov     edx, m_epoll_len
    call    kv_logb_str
    mov     rsi, r12
    neg     rsi
    mov     rdi, rbx
    call    kv_logb_u64
    mov     rdi, rbx
    call    kv_logb_emit
    add     rsp, r13
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; accept_all — accept pending connections (bounded burst). Internal.
; @param  rdi  Worker*
; @remarks On EMFILE/ENFILE/ENOBUFS/ENOMEM the listener is unregistered until
;          the next housekeeping tick: with level-triggered epoll a readable
;          listener that cannot be accepted would otherwise spin the loop.
; =============================================================================
accept_all:
    push    rbx
    push    r12
    push    r13
    mov     rbx, rdi
    mov     r12d, ACCEPT_BURST
.again:
    mov     rdi, [rbx + Worker.listen_fd]
    test    rdi, rdi
    js      .done
    xor     esi, esi
    xor     edx, edx
    mov     r10d, SOCK_NONBLOCK | SOCK_CLOEXEC
    mov     eax, SYS_accept4
    syscall
    test    rax, rax
    js      .error
    mov     rdi, rbx
    mov     rsi, rax
    call    conn_open
    dec     r12d
    jnz     .again
    jmp     .done
.error:
    cmp     rax, -EAGAIN
    je      .done
    cmp     rax, -EINTR
    je      .again
    cmp     rax, -ECONNABORTED
    je      .again                      ; peer gave up before we accepted
    mov     r13, rax
    mov     qword [rbx + Worker.accept_paused], 1
    inc     qword [rbx + Worker.st_rejected]
    sub     rsp, 16
    mov     rdi, [rbx + Worker.epfd]
    mov     esi, EPOLL_CTL_DEL
    mov     rdx, [rbx + Worker.listen_fd]
    mov     r10, rsp
    mov     eax, SYS_epoll_ctl
    syscall
    add     rsp, 16
    mov     rdi, r13
    call    log_accept_paused
.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

; log_accept_paused — warning line. Internal. @param rdi -errno
log_accept_paused:
    push    rbx
    push    r12
    push    r13
    mov     r12, rdi
    call    kv_logbuf_size
    sub     rsp, rax
    mov     r13, rax
    mov     rbx, rsp
    mov     rdi, rbx
    lea     rsi, [lvl_warn]
    call    kv_logb_init
    mov     rdi, rbx
    lea     rsi, [m_accept]
    mov     edx, m_accept_len
    call    kv_logb_str
    mov     rsi, r12
    neg     rsi
    mov     rdi, rbx
    call    kv_logb_u64
    mov     rdi, rbx
    call    kv_logb_emit
    add     rsp, r13
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; conn_open — set up an accepted socket as an HTTP/2 connection. Internal.
; @param  rdi  Worker*   @param rsi  connected fd (non-blocking)
; @remarks Over the per-worker connection limit, or on any allocation/setup
;          failure, the socket is simply closed (the client sees a reset).
;          Our SETTINGS are flushed immediately (server connection preface).
; =============================================================================
conn_open:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    sub     rsp, 32                     ; [0..24) settings entries, [24] pad
    mov     rbx, rdi
    mov     r12, rsi                    ; fd
    mov     r14, [rbx + Worker.cfg]
    mov     rax, [rbx + Worker.nconns]
    cmp     rax, [r14 + Config.max_conns]
    jae     .reject
    ; TCP_NODELAY: gRPC replies are small frame bursts; Nagle would delay them.
    mov     rdi, r12
    mov     esi, IPPROTO_TCP
    mov     edx, TCP_NODELAY
    lea     r10, [int_one]
    mov     r8d, 4
    mov     eax, SYS_setsockopt
    syscall
    mov     edi, 1
    mov     esi, Conn_size
    call    calloc wrt ..plt
    test    rax, rax
    jz      .reject
    mov     rbp, rax                    ; Conn*
    mov     [rbp + Conn.fd], r12
    mov     [rbp + Conn.worker], rbx
    mov     rax, [rbx + Worker.now]
    mov     [rbp + Conn.last_active], rax
    mov     edi, OUT_INITIAL
    call    malloc wrt ..plt
    test    rax, rax
    jz      .free_conn
    mov     [rbp + Conn.out_buf], rax
    mov     qword [rbp + Conn.out_cap], OUT_INITIAL
    lea     rdi, [rbp + Conn.session]
    mov     rsi, [rbx + Worker.callbacks]
    mov     rdx, rbp                    ; user_data = Conn*
    call    nghttp2_session_server_new wrt ..plt
    test    eax, eax
    jnz     .free_conn

    ; SETTINGS: stream concurrency, a 1 MiB stream window (large values need
    ; no WINDOW_UPDATE round trips), and an advisory header-list limit.
    mov     dword [rsp], NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS
    mov     rax, [r14 + Config.max_streams]
    mov     [rsp + 4], eax
    mov     dword [rsp + 8], NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE
    mov     dword [rsp + 12], STREAM_WINDOW
    mov     dword [rsp + 16], NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE
    mov     dword [rsp + 20], 16384
    mov     rdi, [rbp + Conn.session]
    mov     esi, NGHTTP2_FLAG_NONE
    mov     rdx, rsp
    mov     ecx, 3
    call    nghttp2_submit_settings wrt ..plt
    test    eax, eax
    jnz     .free_conn
    ; Connection-level window = the per-connection buffering budget.
    mov     rcx, [r14 + Config.conn_budget]
    mov     eax, MAX_WINDOW
    cmp     rcx, rax
    cmova   rcx, rax
    mov     rdi, [rbp + Conn.session]
    mov     esi, NGHTTP2_FLAG_NONE
    xor     edx, edx
    call    nghttp2_session_set_local_window_size wrt ..plt

    mov     rax, [rbx + Worker.conns]   ; link into the worker's list
    mov     [rbp + Conn.next], rax
    test    rax, rax
    jz      .linked
    mov     [rax + Conn.prev], rbp
.linked:
    mov     [rbx + Worker.conns], rbp
    inc     qword [rbx + Worker.nconns]
    inc     qword [rbx + Worker.st_accepted]
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, rbp
    call    epoll_add_in
    test    rax, rax
    js      .destroy
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_flush
    test    rax, rax
    js      .destroy
    jmp     .out
.destroy:
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy
    jmp     .out
.free_conn:
    mov     rdi, [rbp + Conn.session]
    test    rdi, rdi
    jz      .no_session
    call    nghttp2_session_del wrt ..plt
.no_session:
    mov     rdi, [rbp + Conn.out_buf]
    call    free wrt ..plt
    mov     rdi, rbp
    call    free wrt ..plt
.reject:
    inc     qword [rbx + Worker.st_rejected]
    mov     rdi, r12
    mov     eax, SYS_close
    syscall
.out:
    add     rsp, 32
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; conn_event — handle readiness of one connection. Internal.
; @param  rdi  Worker*   @param esi  epoll events   @param rdx  Conn*
; @remarks Reads at most one chunk per event (level-triggered epoll re-reports
;          remaining data), which keeps one busy connection from starving the
;          others on the same worker.
; =============================================================================
conn_event:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    mov     rbx, rdi
    mov     rbp, rdx                    ; Conn*
    mov     r12d, esi                   ; events
    test    qword [rbp + Conn.flags], CONN_LINGER
    jnz     .linger
    test    r12d, EPOLLIN
    jnz     .readable
    test    r12d, EPOLLERR | EPOLLHUP
    jnz     .destroy
    jmp     .flush
.readable:
    mov     rdi, [rbp + Conn.fd]
    mov     rsi, [rbx + Worker.read_buf]
    mov     edx, READ_CHUNK
    xor     eax, eax                    ; SYS_read
    syscall
    cmp     rax, -EAGAIN
    je      .flush
    cmp     rax, -EINTR
    je      .flush
    test    rax, rax
    jle     .destroy                    ; 0 = peer closed, <0 = reset/error
    mov     r13, rax
    mov     rax, [rbx + Worker.now]
    mov     [rbp + Conn.last_active], rax
    mov     rdi, [rbp + Conn.session]
    mov     rsi, [rbx + Worker.read_buf]
    mov     rdx, r13
    call    nghttp2_session_mem_recv2 wrt ..plt   ; runs the gRPC callbacks
    test    rax, rax
    js      .destroy                    ; fatal protocol/flood/callback error
.flush:
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_flush
    test    rax, rax
    js      .destroy
    jz      .out
    mov     rdi, rbx                    ; session finished gracefully
    mov     rsi, rbp
    call    conn_linger
    jmp     .out
.linger:
    ; Discard whatever the peer still sends until its FIN arrives.
    mov     rdi, [rbp + Conn.fd]
    mov     rsi, [rbx + Worker.read_buf]
    mov     edx, READ_CHUNK
    xor     eax, eax
    syscall
    test    rax, rax
    jg      .out                        ; more may follow; wait for next event
    cmp     rax, -EAGAIN
    je      .out
.destroy:
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy
.out:
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; conn_flush — send pending output and pull more frames from nghttp2.
; Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; @returns rax = 0 (idle or waiting for EPOLLOUT), 1 (the session has
;          nothing more to read or write: close gracefully), -1 (error).
; @remarks mem_send2's pointer is valid only until the next nghttp2 call, so
;          each chunk is copied into out_buf before pulling the next one.
;          Pulling stops at OUT_HIGH_WATER so one connection cannot build an
;          unbounded buffer; the rest stays queued inside nghttp2.
; =============================================================================
conn_flush:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    sub     rsp, 16                     ; [0] mem_send2 data pointer
    mov     rbx, rdi
    mov     rbp, rsi
.send_pending:
    mov     rdx, [rbp + Conn.out_len]
    mov     rax, [rbp + Conn.out_off]
    sub     rdx, rax
    jz      .sent
    mov     rdi, [rbp + Conn.fd]
    mov     rsi, [rbp + Conn.out_buf]
    add     rsi, rax
    mov     r10d, MSG_NOSIGNAL          ; a vanished peer must not SIGPIPE us
    xor     r8d, r8d
    xor     r9d, r9d
    mov     eax, SYS_sendto
    syscall
    cmp     rax, -EINTR
    je      .send_pending
    cmp     rax, -EAGAIN
    je      .would_block
    test    rax, rax
    js      .error
    add     [rbp + Conn.out_off], rax
    jmp     .send_pending
.sent:
    xor     eax, eax
    mov     [rbp + Conn.out_len], rax
    mov     [rbp + Conn.out_off], rax
.fill:
    cmp     qword [rbp + Conn.out_len], OUT_HIGH_WATER
    jae     .send_pending
    mov     rdi, [rbp + Conn.session]
    mov     rsi, rsp
    call    nghttp2_session_mem_send2 wrt ..plt
    test    rax, rax
    js      .error
    jz      .drained
    mov     r12, rax                    ; chunk length
    mov     r13, [rbp + Conn.out_len]
    lea     r14, [r13 + r12]            ; needed capacity
    cmp     r14, [rbp + Conn.out_cap]
    jbe     .copy
    mov     rsi, [rbp + Conn.out_cap]
    shl     rsi, 1
    cmp     rsi, r14
    cmovb   rsi, r14
    push    rsi
    push    rsi
    mov     rdi, [rbp + Conn.out_buf]
    call    realloc wrt ..plt
    pop     rsi
    pop     rsi
    test    rax, rax
    jz      .error
    mov     [rbp + Conn.out_buf], rax
    mov     [rbp + Conn.out_cap], rsi
.copy:
    mov     rdi, [rbp + Conn.out_buf]
    add     rdi, r13
    mov     rsi, [rsp]
    mov     rdx, r12
    call    memcpy wrt ..plt
    mov     [rbp + Conn.out_len], r14
    jmp     .fill
.drained:
    cmp     qword [rbp + Conn.out_len], 0
    jne     .send_pending
    mov     rdi, rbx
    mov     rsi, rbp
    xor     edx, edx
    call    set_interest                ; nothing pending: read again
    mov     rdi, [rbp + Conn.session]
    call    nghttp2_session_want_read wrt ..plt
    test    eax, eax
    jnz     .idle
    mov     rdi, [rbp + Conn.session]
    call    nghttp2_session_want_write wrt ..plt
    test    eax, eax
    jnz     .idle
    mov     eax, 1                      ; GOAWAY exchanged, no active streams
    jmp     .out
.would_block:
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, 1
    call    set_interest                ; wait for EPOLLOUT, stop reading
.idle:
    xor     eax, eax
    jmp     .out
.error:
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
; set_interest — switch a connection between EPOLLIN and EPOLLOUT. Internal.
; @param  rdi  Worker*   @param rsi  Conn*   @param edx  1 = output pending
; @remarks Half-duplex on purpose: see the file header (back-pressure).
; =============================================================================
set_interest:
    mov     rax, [rsi + Conn.flags]
    and     eax, CONN_WANT_OUT
    cmp     eax, edx
    je      .same
    xor     qword [rsi + Conn.flags], CONN_WANT_OUT
    sub     rsp, 24
    mov     dword [rsp], EPOLLIN
    test    edx, edx
    jz      .mask_ok
    mov     dword [rsp], EPOLLOUT
.mask_ok:
    mov     [rsp + 4], rsi
    mov     rdx, [rsi + Conn.fd]
    mov     rdi, [rdi + Worker.epfd]
    mov     esi, EPOLL_CTL_MOD
    mov     r10, rsp
    mov     eax, SYS_epoll_ctl
    syscall
    add     rsp, 24
.same:
    ret

; =============================================================================
; conn_linger — graceful close after the HTTP/2 session finished. Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; @remarks Frees the session now, sends FIN (shutdown SHUT_WR) and keeps
;          reading until the peer's FIN or LINGER_NS: closing a socket with
;          unread input makes the kernel send RST, which can destroy our final
;          GOAWAY before the peer reads it.
; =============================================================================
conn_linger:
    push    rbx
    push    rbp
    sub     rsp, 8
    mov     rbx, rdi
    mov     rbp, rsi
    mov     rdi, [rbp + Conn.session]
    call    nghttp2_session_del wrt ..plt
    mov     qword [rbp + Conn.session], 0
    mov     rdi, rbp
    call    kv_grpc_free_streams wrt ..plt
    mov     rdi, [rbp + Conn.fd]
    mov     esi, SHUT_WR
    mov     eax, SYS_shutdown
    syscall
    or      qword [rbp + Conn.flags], CONN_LINGER
    mov     rax, [rbx + Worker.now]
    mov     [rbp + Conn.last_active], rax
    mov     rdi, rbx
    mov     rsi, rbp
    xor     edx, edx
    call    set_interest
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; conn_destroy — close a connection immediately and free everything. Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; @remarks close() also removes the fd from epoll (no other reference to it
;          exists). Streams alive in the session are freed explicitly because
;          nghttp2_session_del does not report them.
; =============================================================================
conn_destroy:
    push    rbx
    push    rbp
    sub     rsp, 8
    mov     rbx, rdi
    mov     rbp, rsi
    mov     rdi, [rbp + Conn.session]
    test    rdi, rdi
    jz      .no_session
    call    nghttp2_session_del wrt ..plt
.no_session:
    mov     rdi, rbp
    call    kv_grpc_free_streams wrt ..plt
    mov     rdi, [rbp + Conn.fd]
    mov     eax, SYS_close
    syscall
    mov     rcx, [rbp + Conn.prev]      ; unlink from the worker's list
    mov     rdx, [rbp + Conn.next]
    test    rcx, rcx
    jz      .head
    mov     [rcx + Conn.next], rdx
    jmp     .fix
.head:
    mov     [rbx + Worker.conns], rdx
.fix:
    test    rdx, rdx
    jz      .unlinked
    mov     [rdx + Conn.prev], rcx
.unlinked:
    dec     qword [rbx + Worker.nconns]
    mov     rdi, [rbp + Conn.out_buf]
    call    free wrt ..plt
    mov     rdi, rbp
    call    free wrt ..plt
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; housekeeping — once per second. Internal.
; @param  rdi  Worker*
; @remarks Re-arms a paused listener, ends lingering connections whose peer
;          never sent FIN, sends GOAWAY to idle connections, and (worker 0
;          only) returns retired store memory even when no writes happen.
; =============================================================================
housekeeping:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    mov     rbx, rdi
    cmp     qword [rbx + Worker.accept_paused], 0
    je      .conns
    cmp     qword [rbx + Worker.draining], 0
    jne     .conns
    mov     rdi, rbx
    mov     rsi, [rbx + Worker.listen_fd]
    mov     edx, LISTEN_TAG
    call    epoll_add_in
    mov     qword [rbx + Worker.accept_paused], 0
.conns:
    mov     r13, [rbx + Worker.cfg]
    mov     rbp, [rbx + Worker.conns]
.next:
    test    rbp, rbp
    jz      .reclaim
    mov     r12, [rbp + Conn.next]      ; the current one may be freed below
    mov     r14, [rbx + Worker.now]
    sub     r14, [rbp + Conn.last_active]
    test    qword [rbp + Conn.flags], CONN_LINGER
    jz      .check_idle
    cmp     r14, LINGER_NS
    jb      .advance
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy
    jmp     .advance
.check_idle:
    mov     rax, [r13 + Config.idle_timeout_ns]
    test    rax, rax
    jz      .advance
    cmp     r14, rax
    jb      .advance
    mov     rdi, [rbp + Conn.session]
    mov     esi, NGHTTP2_NO_ERROR
    call    nghttp2_session_terminate_session wrt ..plt
    mov     rdi, rbx
    mov     rsi, rbp
    call    flush_or_close
.advance:
    mov     rbp, r12
    jmp     .next
.reclaim:
    cmp     qword [rbx + Worker.id], 0
    jne     .out
    mov     rdi, [rbx + Worker.table]
    call    kv_store_reclaim wrt ..plt
.out:
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; flush_or_close — flush a connection and apply the result. Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; =============================================================================
flush_or_close:
    push    rbx
    push    rbp
    sub     rsp, 8
    mov     rbx, rdi
    mov     rbp, rsi
    call    conn_flush
    test    rax, rax
    jz      .out
    mov     rdi, rbx
    mov     rsi, rbp
    js      .destroy
    call    conn_linger
    jmp     .out
.destroy:
    call    conn_destroy
.out:
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; begin_drain — start graceful shutdown of this worker. Internal.
; @param  rdi  Worker*
; @remarks Closes the listener (new connections go to no one: the process is
;          stopping) and sends GOAWAY carrying the last processed stream id,
;          so every call already received still completes.
; =============================================================================
begin_drain:
    push    rbx
    push    rbp
    push    r12
    mov     rbx, rdi
    mov     qword [rbx + Worker.draining], 1
    mov     rax, [rbx + Worker.cfg]
    mov     rax, [rax + Config.grace_ns]
    add     rax, [rbx + Worker.now]
    mov     [rbx + Worker.drain_deadline], rax
    mov     rdi, [rbx + Worker.listen_fd]
    test    rdi, rdi
    js      .conns
    mov     eax, SYS_close              ; also removes it from epoll
    syscall
    mov     qword [rbx + Worker.listen_fd], -1
.conns:
    mov     rbp, [rbx + Worker.conns]
.next:
    test    rbp, rbp
    jz      .out
    mov     r12, [rbp + Conn.next]
    test    qword [rbp + Conn.flags], CONN_LINGER
    jnz     .advance
    mov     rdi, [rbp + Conn.session]
    call    nghttp2_session_get_last_proc_stream_id wrt ..plt
    mov     rdi, [rbp + Conn.session]
    mov     esi, NGHTTP2_FLAG_NONE
    mov     edx, eax                    ; last stream id we will still answer
    mov     ecx, NGHTTP2_NO_ERROR
    xor     r8d, r8d
    xor     r9d, r9d
    call    nghttp2_submit_goaway wrt ..plt
    mov     rdi, rbx
    mov     rsi, rbp
    call    flush_or_close
.advance:
    mov     rbp, r12
    jmp     .next
.out:
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; close_all_now — force-close every connection (grace period expired).
; Internal.  @param rdi Worker*
; =============================================================================
close_all_now:
    push    rbx
    push    rbp
    push    r12
    mov     rbx, rdi
.next:
    mov     rbp, [rbx + Worker.conns]
    test    rbp, rbp
    jz      .out
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy
    jmp     .next
.out:
    pop     r12
    pop     rbp
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
