; =============================================================================
; benchabi.asm — the assembler's offsets of the Worker / Conn / Config fields
; that bench_h2.c pokes directly.
;
; Why: bench_h2.c builds a Worker and a Conn by hand (no sockets, no epoll)
; to drive h2n.asm in process. Their layouts live in server.inc only, so the
; offsets are exported from the same include files the variant under test was
; assembled with — a layout change in a variant can then never desynchronize
; the harness. The order is bench_h2.c's BA_* enum; append only.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"

section .rodata
align 8
global bench_abi
bench_abi:
    dq Worker_size                      ;  0
    dq Worker.table                     ;  1
    dq Worker.cfg                       ;  2
    dq Worker.read_buf                  ;  3
    dq Worker.now                       ;  4
    dq Conn_size                        ;  5
    dq Conn.fd                          ;  6
    dq Conn.worker                      ;  7
    dq Conn.out_buf                     ;  8
    dq Conn.out_cap                     ;  9
    dq Conn.out_len                     ; 10
    dq Conn.out_off                     ; 11
    dq Conn.h2 + H2.send_window         ; 12
    dq Conn.h2 + H2.nstreams            ; 13
    dq Config_size                      ; 14
    dq Worker.id                        ; 15
    dq Conn.h2 + H2.pend_head           ; 16
    dq Worker.st_requests               ; 17
    dq 0xABCDEF0123456789               ; sentinel: catches a short array

section .note.GNU-stack noalloc noexec nowrite progbits
