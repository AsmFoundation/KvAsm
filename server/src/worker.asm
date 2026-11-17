; =============================================================================
; worker.asm — one event loop per worker thread (thread-per-core).
;
; Each worker owns: an epoll instance, its own listening socket (SO_REUSEPORT,
; so the kernel spreads new connections across workers — no accept lock and
; no hand-off), an eventfd doorbell for shutdown, and every connection it
; accepted, together with that connection's HTTP/2 state (h2n.asm, embedded
; in Conn). Nothing here is shared with other workers except the store, so no
; locks are taken.
;
; Per connection, I/O is non-blocking and level-triggered with half-duplex
; back-pressure: while output is pending the connection is watched for
; EPOLLOUT only (its input is not read), so a client that sends requests but
; never reads responses cannot make the server buffer unbounded replies.
;
; HTTP/2 is KvAsm's own engine (h2n.asm, ADR 0007): it reads, parses and
; frames responses straight into the connection's output buffer. Output is
; coalesced twice: everything one read produced leaves in one sendto
; (measured: ~1 syscall per 16 requests instead of 3 per request), and every
; connection of an epoll batch is flushed together, after the whole batch's
; input (worker_main's two phases, ADR 0009): a client thread owning several
; connections is then woken once per batch instead of once per connection —
; measured on one core: +26% GET/s with 8 connections per client thread,
; +12% with 4, +4% with 1, and lower p50/p99 in every case.
;
; Event-driven, never spinning: a connection is a state machine (h2n.asm's
; state in Conn, the gRPC streams) resumed by epoll readiness — what C#
; async/await compiles to, without its heap-allocated continuations or a
; thread hand-off. With nothing ready the worker sleeps in epoll_wait
; (measured: 4 workers holding 1,000 idle connections used 10 ms of CPU in
; 10 s, waking only for the 1 s housekeeping tick).
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

%include "kvasm.inc"
%include "server.inc"

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

extern aligned_alloc
extern malloc
extern calloc
extern free
extern memset
extern clock_gettime
extern pthread_create
extern pthread_join
extern kv_qsbr_online
extern kv_qsbr_offline
extern kv_qsbr_quiescent
extern kv_store_reclaim
extern kv_store_reclaim_local
extern kv_grpc_free_streams
extern kv_logb_init
extern kv_logb_str
extern kv_logb_u64
extern kv_logb_emit
extern kv_logbuf_size
extern kv_pool_destroy
extern kv_hcache_create
extern kv_hcache_destroy
extern kv_h2n_open
extern kv_h2n_close
extern kv_h2n_read
extern kv_h2n_pump
extern kv_h2n_finished
extern kv_h2n_goaway

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
;          group. A second kvasm started on the same port (host networking,
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
    push    r15                         ; unused; 6 pushes + 24 keep rsp 16-aligned
    sub     rsp, 24                     ; [0..16) sockaddr_in / epoll_event scratch
    ; Callers run kv_port_probe first: SO_REUSEPORT below would otherwise let
    ; this listener silently join a group owned by another process.
    mov     r12, rdi
    mov     r13, rsi
    mov     r14, rdx
    mov     edi, 64
    mov     esi, (Worker_size + 63) & ~63
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail_alloc_early
    mov     rbx, rax
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, (Worker_size + 63) & ~63
    call    memset wrt ..plt            ; also an empty Pool: all lists and chunks 0
    mov     [rbx + Worker.id], r12
    mov     [rbx + Worker.table], r13
    mov     [rbx + Worker.cfg], r14
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

    ; Private L1/L2 record cache (KVASM_CACHE_L1_BITS/L2_BITS; off by
    ; default, see ADR 0006). Main enabled the table's invalidation
    ; generations before any worker was created.
    mov     rax, [r14 + Config.cache_l1_bits]
    or      rax, [r14 + Config.cache_l2_bits]
    jz      .no_cache
    mov     rdi, r13
    mov     rsi, [r14 + Config.cache_l1_bits]
    mov     rdx, [r14 + Config.cache_l2_bits]
    call    kv_hcache_create wrt ..plt
    mov     [rbx + Worker.hcache], rax
    test    rax, rax
    jz      .fail_alloc
.no_cache:

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
    mov     rdi, [rbx + Worker.h2_scratch] ; NULL unless a native connection ran
    call    free wrt ..plt
    lea     rdi, [rbx + Worker.pool]    ; every session and stream is gone by now
    call    kv_pool_destroy wrt ..plt
    mov     rdi, [rbx + Worker.hcache]  ; NULL when caching is off
    call    kv_hcache_destroy wrt ..plt
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
; @remarks Each pass handles one epoll batch in two phases:
;            1. input: every ready connection is read and processed; the
;               answers are framed into its output buffer, nothing is sent;
;            2. output: every connection still alive is flushed, back to
;               back (flush_or_close), then the loop sleeps in epoll_wait.
;          Why (measured, ADR 0009): on one machine the sender's sendto also
;          runs the receiver's TCP input and wakes the receiver's thread if
;          it sleeps; under a hypervisor that wakeup is a VM-exiting IPI, and
;          with one flush per connection it cost ~48% of a loaded core. A
;          client thread owning several connections got woken once per
;          connection, because it fell asleep again between our spaced-out
;          sends. Sending the whole batch's output in one burst lets it wake
;          once and find the rest already there. Redis does the same (its
;          replies are written in beforeSleep). Cost: a response waits until
;          the batch's input is processed (bounded by MAX_EVENTS events of at
;          most READ_CHUNK bytes each), which is also when it would have
;          been queued behind the others anyway on a saturated core.
;          A drain request (the eventfd) is only noted during phase 1 and
;          started after phase 2: begin_drain may destroy connections, and a
;          destroyed connection whose event is still ahead in the batch
;          would be used after being freed.
; =============================================================================
worker_main:
    push    rbx
    push    rbp                         ; ebp: 1 = drain requested this batch
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [0] eventfd counter; 6 pushes + 8: aligned
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
    ; One epoll_wait per pass, entered offline: no record pointers are held
    ; here, so the worker leaves the readers' set for the wait (reclamation
    ; never waits on a sleeping worker) and rejoins it after — an offline
    ; announcement is also a quiescent state, so nothing else is needed.
    ; Why not poll first (timeout 0) and block only when nothing is ready, as
    ; this loop used to: the poll saved the rejoin's XCHG (~20 cycles) on a
    ; busy pass, but cost a whole extra syscall before every sleep — once per
    ; wakeup whenever the worker is not saturated, e.g. one of the four
    ; syscalls an unpipelined request needs.
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
    ; ---- phase 1: input. Read and process every ready connection. ----
    xor     ebp, ebp                    ; no drain requested yet
    xor     r15d, r15d
.next_event:
    cmp     r15, r14
    jae     .output
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
    test    eax, eax
    jz      .ev_done
    ; The connection was destroyed (freed): clear its event so phase 2
    ; skips it. epoll reports an fd at most once per batch, and processing
    ; one connection never touches another, so no other entry can dangle.
    lea     rax, [r15 + r15*2]
    shl     rax, 2
    add     rax, [rbx + Worker.events]
    mov     qword [rax + 4], 0          ; packed struct: unaligned store, fine on x86
    jmp     .ev_done
.ev_listen:
    mov     rdi, rbx
    call    accept_all                  ; new connections send their preface at once
    jmp     .ev_done
.ev_doorbell:
    mov     rdi, [rbx + Worker.event_fd]
    mov     rsi, rsp
    mov     edx, 8
    xor     eax, eax                    ; SYS_read: consume the counter
    syscall
    mov     ebp, 1                      ; begin_drain after phase 2 (see @remarks)
.ev_done:
    inc     r15
    jmp     .next_event

.output:
    ; ---- phase 2: output. Flush every surviving connection back to back. ----
    xor     r15d, r15d
.next_flush:
    cmp     r15, r14
    jae     .batch_done
    lea     rax, [r15 + r15*2]
    shl     rax, 2
    add     rax, [rbx + Worker.events]
    mov     rsi, [rax + 4]              ; Conn*, 0 (destroyed) or a tag
    cmp     rsi, EVENT_TAG
    jbe     .flush_done                 ; 0, LISTEN_TAG (1), EVENT_TAG (2)
    test    qword [rsi + Conn.flags], CONN_LINGER
    jnz     .flush_done                 ; output already sent; only reading to FIN
    mov     rdi, rbx
    call    flush_or_close              ; may linger or destroy this one only
.flush_done:
    inc     r15
    jmp     .next_flush
.batch_done:
    test    ebp, ebp
    jz      .after_events
    cmp     qword [rbx + Worker.draining], 0
    jne     .after_events               ; a second doorbell: already draining
    mov     rdi, rbx
    call    begin_drain

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
    sub     rsp, 32                     ; 5 pushes + 32: rsp 16-aligned for calls
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
    ; HTTP/2 state lives in Conn; kv_h2n_open queues the server preface:
    ; SETTINGS (stream concurrency, a 1 MiB stream window, an advisory
    ; header-list limit) and a WINDOW_UPDATE raising the connection window to
    ; the per-connection buffering budget.
    mov     rdi, rbp
    call    kv_h2n_open wrt ..plt
    test    eax, eax
    jnz     .free_conn

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
    ; kv_h2n_close copes with a half-done (or never started) kv_h2n_open: the
    ; Conn was calloc'd and Conn.worker is set, so unset parts are NULL.
    mov     rdi, rbp
    call    kv_h2n_close wrt ..plt
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
; conn_event — phase 1 for one connection: take its input. Internal.
; @param  rdi  Worker*   @param esi  epoll events   @param rdx  Conn*
; @returns eax = 0 (still open: worker_main flushes it in phase 2), or 1 (it
;          was destroyed and freed here: the caller must forget the pointer).
; @remarks Reads at most one chunk per event (level-triggered epoll re-reports
;          remaining data), which keeps one busy connection from starving the
;          others on the same worker. kv_h2n_read parses the chunk and frames
;          the answers into out_buf; nothing is sent here — worker_main sends
;          every connection's output together once the batch's input is done
;          (see its @remarks for why), so a request batch still costs one
;          read and (usually) one sendto. An EPOLLOUT-only event needs no
;          input work: phase 2's flush is all it needs.
; =============================================================================
conn_event:
    push    rbx
    push    rbp
    push    r12                         ; 3 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rbp, rdx                    ; Conn*
    mov     r12d, esi                   ; events
    test    qword [rbp + Conn.flags], CONN_LINGER
    jnz     .linger
    test    r12d, EPOLLIN
    jnz     .readable
    test    r12d, EPOLLERR | EPOLLHUP
    jnz     .destroy
    jmp     .alive                      ; writable: phase 2 flushes it
.readable:
    ; kv_h2n_read does the read itself (after a partial frame carried from
    ; the previous one), updates Conn.last_active, parses, runs the GET
    ; batch the frames queued and tops up the flow-control windows.
    ; EAGAIN/EINTR come back as 0: nothing new, the connection lives on.
    mov     rdi, rbx
    mov     rsi, rbp
    call    kv_h2n_read wrt ..plt
    test    rax, rax
    js      .destroy                    ; peer closed, socket error, out of memory
.alive:
    xor     eax, eax
    jmp     .out
.linger:
    ; Discard whatever the peer still sends until its FIN arrives.
    mov     rdi, [rbp + Conn.fd]
    mov     rsi, [rbx + Worker.read_buf]
    mov     edx, READ_CHUNK
    xor     eax, eax
    syscall
    test    rax, rax
    jg      .alive                      ; more may follow; wait for next event
    cmp     rax, -EAGAIN
    je      .alive
.destroy:
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy
    mov     eax, 1                      ; freed: the caller must not flush it
.out:
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; conn_flush — send pending output and frame more from the pend list.
; Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; @returns rax = 0 (idle or waiting for EPOLLOUT), 1 (the session has
;          nothing more to read or write: close gracefully), -1 (error:
;          the peer reset, or kv_h2n_pump ran out of memory).
; @remarks Most output never waits here: kv_h2n_read frames responses
;          straight into out_buf while it parses. What kv_h2n_pump adds are
;          bodies that were waiting for flow-control window or buffer room;
;          it stops at OUT_HIGH_WATER, so one connection cannot build an
;          unbounded buffer, and the rest stays on the pend list until the
;          next flush. On EAGAIN the connection switches to EPOLLOUT and stops
;          reading (half-duplex back-pressure, see the file header).
;          Fairness (Download): after FLUSH_BUDGET bytes framed from the pend
;          list, with more still pending, the flush stops although the socket
;          would take more: the connection is watched for EPOLLIN | EPOLLOUT
;          and continues on the next loop pass, after the worker's other
;          ready connections. A fast reader of a large download therefore
;          shares the worker instead of holding it for the whole transfer,
;          and its connection still reads input (WINDOW_UPDATE, RST_STREAM,
;          new calls) meanwhile — unlike a full socket, which stops reading.
; =============================================================================
conn_flush:
    push    rbx
    push    rbp
    sub     rsp, 8                      ; [0] bytes framed by the pump; aligned
    mov     rbx, rdi
    mov     rbp, rsi
    mov     qword [rsp], 0
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
    ; Output the peer accepts is activity too: a long Download whose window
    ; never runs out makes the client send nothing, and must not be cut by
    ; the idle timeout. A peer that stops reading makes no progress here, so
    ; KVASM_IDLE_TIMEOUT_SEC still ends a stalled transfer (and its pin).
    mov     rcx, [rbx + Worker.now]
    mov     [rbp + Conn.last_active], rcx
    jmp     .send_pending
.sent:
    xor     eax, eax
    mov     [rbp + Conn.out_len], rax
    mov     [rbp + Conn.out_off], rax
    ; Budget spent with streams still pending: yield (see @remarks). A stream
    ; that is only waiting for window costs one empty pass at most: the next
    ; pump frames nothing and the connection returns to EPOLLIN below.
    cmp     qword [rsp], FLUSH_BUDGET
    jb      .fill
    cmp     qword [rbp + Conn.h2 + H2.pend_head], 0
    je      .fill
    mov     rdi, rbx
    mov     rsi, rbp
    mov     edx, 2                      ; EPOLLIN | EPOLLOUT
    call    set_interest
    jmp     .idle
.fill:
    ; out_buf is empty: frame what the windows and OUT_HIGH_WATER allow from
    ; the pend list, send it, and repeat until nothing more can be framed.
    mov     rdi, rbp
    call    kv_h2n_pump wrt ..plt
    test    eax, eax
    js      .error
    mov     rax, [rbp + Conn.out_len]
    test    rax, rax
    jz      .drained
    add     [rsp], rax                  ; toward FLUSH_BUDGET
    jmp     .send_pending
.drained:
    mov     rdi, rbx
    mov     rsi, rbp
    xor     edx, edx
    call    set_interest                ; nothing pending: read again
    mov     rdi, rbp
    call    kv_h2n_finished wrt ..plt
    test    eax, eax
    jz      .idle
    mov     eax, 1                      ; GOAWAY exchanged, no stream left
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
    add     rsp, 8
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; set_interest — choose what epoll reports for a connection. Internal.
; @param  rdi  Worker*   @param rsi  Conn*
; @param  edx  0 = EPOLLIN (reading), 1 = EPOLLOUT only (the socket is full:
;              back-pressure), 2 = EPOLLIN | EPOLLOUT (a flush yielded at
;              FLUSH_BUDGET with the socket still writable)
; @returns nothing. Changes the registration only when the mode changes.
; @remarks Mode 1 is half-duplex on purpose: see the file header
;          (back-pressure). Mode 2 is not back-pressure — the peer is
;          keeping up — so input keeps flowing while the remaining output
;          waits for the next pass. The mode lives in Conn.flags
;          (CONN_WANT_OUT / CONN_WANT_BOTH).
; =============================================================================
set_interest:
    mov     rax, [rsi + Conn.flags]
    xor     ecx, ecx                    ; current mode
    test    eax, CONN_WANT_OUT
    jz      .not_out
    mov     ecx, 1
.not_out:
    test    eax, CONN_WANT_BOTH
    jz      .have_mode
    mov     ecx, 2
.have_mode:
    cmp     ecx, edx
    je      .same
    and     qword [rsi + Conn.flags], ~(CONN_WANT_OUT | CONN_WANT_BOTH)
    sub     rsp, 24
    mov     dword [rsp], EPOLLIN
    test    edx, edx
    jz      .mask_ok
    cmp     edx, 1
    jne     .both
    mov     dword [rsp], EPOLLOUT
    or      qword [rsi + Conn.flags], CONN_WANT_OUT
    jmp     .mask_ok
.both:
    mov     dword [rsp], EPOLLIN | EPOLLOUT
    or      qword [rsi + Conn.flags], CONN_WANT_BOTH
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
; @returns nothing; the Conn stays linked (CONN_LINGER) until conn_event sees
;          the peer's FIN or housekeeping's LINGER_NS expires.
; @remarks Frees the session state now, sends FIN (shutdown SHUT_WR) and keeps
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
    mov     rdi, rbp
    call    kv_h2n_close wrt ..plt      ; idempotent: conn_destroy calls it again
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
; @returns nothing; the Conn is freed, so the caller must have saved
;          Conn.next beforehand if it is walking the list.
; @remarks close() also removes the fd from epoll (no other reference to it
;          exists). kv_h2n_close releases the engine's state (stream map,
;          pend list, HPACK ring) without touching the gRPC streams, so those
;          are freed explicitly afterwards; both calls are idempotent, which
;          lets a lingering connection (already closed once) come through here.
; =============================================================================
conn_destroy:
    push    rbx
    push    rbp
    sub     rsp, 8
    mov     rbx, rdi
    mov     rbp, rsi
    mov     rdi, rbp
    call    kv_h2n_close wrt ..plt      ; idempotent (a lingering one was closed)
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
;          never sent FIN, sends GOAWAY to idle connections — and destroys
;          one still idle a timeout after that GOAWAY was queued (a peer
;          stalled at the TCP level, CONN_IDLE_CLOSING) — and returns
;          retired store memory even when no writes happen: every worker
;          its own slot's (ADR 0011), worker 0 also the global limbo.
;          "Idle" is no byte received or sent for KVASM_IDLE_TIMEOUT_SEC,
;          so a transfer that keeps moving is never cut, while one whose
;          peer stopped reading ends at most ~2 s (two passes) after the
;          timeout. With the timeout disabled (0) neither ever happens.
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
    ; Expired again with the idle GOAWAY still undelivered (last_active moves
    ; with every byte sent, so nothing left since): the peer stopped reading
    ; at the TCP level. That GOAWAY can never go out, and the connection would
    ; otherwise live forever holding its streams — a Download's pinned record,
    ; an Upload's reservation. Close it now, without the courtesy frame.
    test    qword [rbp + Conn.flags], CONN_IDLE_CLOSING
    jnz     .stuck
    or      qword [rbp + Conn.flags], CONN_IDLE_CLOSING
    ; Idle too long: GOAWAY(NO_ERROR) in terminate mode, so the connection
    ; closes as soon as the frame is out; streams still open are abandoned
    ; (an idle connection has none that made progress for the whole timeout).
    mov     rdi, rbp
    mov     esi, H2_NO_ERROR
    mov     edx, 1                      ; terminate: close once it is sent
    call    kv_h2n_goaway wrt ..plt     ; (out of memory: closed by the next error)
    mov     rdi, rbx
    mov     rsi, rbp
    call    flush_or_close
    jmp     .advance
.stuck:
    mov     rdi, rbx
    mov     rsi, rbp
    call    conn_destroy                ; frees its streams: unpins, releases reservations
.advance:
    mov     rbp, r12
    jmp     .next
.reclaim:
    ; This worker's own slot first (ADR 0011): its batched writes retired
    ; records there, and only it may free them — after a burst of writes the
    ; last batches' records would otherwise wait for its next write. The
    ; budget is the forced-reclamation one (store.asm RECLAIM_BATCH_FORCE).
    mov     rdi, [rbx + Worker.table]
    mov     esi, [rbx + Worker.id]
    mov     edx, 4096
    call    kv_store_reclaim_local wrt ..plt
    cmp     qword [rbx + Worker.id], 0
    jne     .out
    mov     rdi, [rbx + Worker.table]
    call    kv_store_reclaim wrt ..plt  ; the global limbo (per-write paths)
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
; @returns nothing. The Conn may be lingering (session finished: FIN sent,
;          reading to the peer's FIN) or destroyed and freed (send error) on
;          return, so a caller walking a list must have saved Conn.next.
; @remarks The one place output is sent from the event loop: worker_main's
;          phase 2 calls it for every connection of the batch, and
;          housekeeping (idle GOAWAY) and begin_drain (drain GOAWAY) for the
;          connections they queued a frame on.
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
    ; GOAWAY(last stream id, NO_ERROR): accepted streams still complete,
    ; new ones are ignored (RFC 9113 §6.8).
    mov     rdi, rbp
    mov     esi, H2_NO_ERROR
    xor     edx, edx                    ; graceful, not terminate
    call    kv_h2n_goaway wrt ..plt
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
