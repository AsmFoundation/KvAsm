; =============================================================================
; calib.asm — the cycle-time calibration loop of the measurement harnesses
; (bench_h2.c), on its own so that a harness can link it beside any server
; variant (hashes.asm, which also has it, defines hash symbols that a variant
; with its own aeshash.asm already provides).
; =============================================================================

default rel
bits 64

section .text

; =============================================================================
; calib_add — rdi iterations of a one-cycle dependency chain.
; @param  rdi  iterations (> 0)
; @returns rax = iterations (the chain's value).
; @remarks ADD and the fused DEC/JNZ both have one cycle of latency and the
;          loop carries two independent chains, so the loop runs at exactly
;          one iteration per core cycle: time / iterations = cycle time.
;          Use short runs and their minimum: a long run spans time slices and
;          reads slow.
; =============================================================================
global calib_add
calib_add:
    xor     eax, eax
.loop:
    add     rax, 1
    dec     rdi
    jnz     .loop
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
