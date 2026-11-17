; =============================================================================
; wlock.asm — the writers' mutex (futex-based, Drepper "mutex3" + bounded spin).
;
; States of the 32-bit lock word: 0 = free, 1 = held, 2 = held with (possible)
; sleepers. Uncontended lock/unlock cost one LOCK'd instruction each and never
; enter the kernel. Under contention a waiter spins briefly (writes hold the
; lock for well under a microsecond), then sleeps in futex_wait instead of
; burning a core. Readers never touch this lock.
;
; Reference: Ulrich Drepper, "Futexes Are Tricky" (mutex3).
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

%define SPIN_LIMIT  128                 ; ~128 PAUSEs before sleeping

section .text

global kv_mutex_lock
global kv_mutex_unlock

; =============================================================================
; kv_mutex_lock — acquire the mutex.
;
; @param  rdi  uint32_t *lock (4-byte aligned, initialized to 0)
; @returns nothing; returns only once the caller owns the lock.
; @remarks Not recursive: locking twice from one thread deadlocks. EINTR and
;          spurious futex wake-ups simply loop. Clobbers rax, rcx, rdx, rsi,
;          r8, r10, r11.
; =============================================================================
kv_mutex_lock:
    xor     eax, eax
    mov     ecx, 1
    lock cmpxchg [rdi], ecx             ; 0 -> 1: fast path
    jnz     .contended
    ret
.contended:
    mov     r8d, SPIN_LIMIT
.spin:
    pause
    mov     eax, [rdi]
    test    eax, eax
    jnz     .spin_again                 ; still held: keep spinning (read-only)
    mov     ecx, 1
    lock cmpxchg [rdi], ecx             ; looks free: try 0 -> 1
    jz      .acquired
.spin_again:
    dec     r8d
    jnz     .spin
.sleep_loop:
    ; Mark "held, maybe sleepers". If the previous value was 0 we now own it
    ; (in state 2, which only costs one unnecessary wake at unlock).
    mov     eax, 2
    xchg    [rdi], eax
    test    eax, eax
    jz      .acquired
    mov     esi, FUTEX_WAIT_PRIVATE
    mov     edx, 2                      ; sleep only while the word is still 2
    xor     r10d, r10d                  ; no timeout
    mov     eax, SYS_futex
    syscall                             ; rdi preserved by the kernel
    jmp     .sleep_loop
.acquired:
    ret

; =============================================================================
; kv_mutex_unlock — release the mutex held by the caller.
;
; @param  rdi  uint32_t *lock
; @returns nothing.
; @remarks If the word was 2 (sleepers possible) it is reset to 0 and one
;          sleeper is woken; it re-competes (fairness is not guaranteed, which
;          is fine for short critical sections). Clobbers rax, rdx, rsi, rcx,
;          r11.
; =============================================================================
kv_mutex_unlock:
    mov     eax, -1
    lock xadd [rdi], eax                ; eax = previous value
    cmp     eax, 1
    jne     .wake                       ; was 2: someone may be sleeping
    ret
.wake:
    mov     dword [rdi], 0
    mov     esi, FUTEX_WAKE_PRIVATE
    mov     edx, 1
    mov     eax, SYS_futex
    syscall
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
