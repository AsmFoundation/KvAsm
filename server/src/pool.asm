; =============================================================================
; pool.asm — per-worker size-class allocator (malloc-shaped, Pool* last).
;
; Why: profiling KvAsm under GET load showed ~12% of user CPU in glibc
; malloc/free — its first HTTP/2 layer (nghttp2) allocated a stream,
; outbound items and frame buffers per request, and so did KvAsm (Stream,
; request and response buffers). Today's users are the Stream objects,
; request bodies, small responses, stream maps, header-block and carry
; buffers and HPACK rings. Every one of those objects is created and
; destroyed on the same worker thread (a connection never migrates), so a
; lock-free, unsynchronized pool per worker serves them in a few
; instructions:
;
;   * 7 size classes, 64 .. 4096 bytes per block (16-byte header included);
;   * free blocks form one LIFO list per class (a freed block is reused first,
;     while it is still in cache);
;   * new blocks are carved from 256 KiB chunks by bumping a pointer;
;   * larger requests go straight to malloc, tagged so free can tell.
;
; Block layout: [u64 class or POOL_LARGE][u64 user size (large only)][user ...]
; The 16-byte header keeps user pointers 16-byte aligned, as malloc's are.
; Memory is recycled inside the pool and returned to the system only by
; kv_pool_destroy (when the worker exits): the pool's size follows the
; worker's peak concurrency, not its lifetime traffic.
;
; Threading: none. A Pool must only be used by its owning thread.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"
%include "server.inc"

extern malloc
extern free
extern memcpy
extern memset

section .rodata
align 8
global kv_pool_struct_size
kv_pool_struct_size:    dq Pool_size    ; for C tests that embed a Pool

section .text

global kv_pool_malloc
global kv_pool_free
global kv_pool_calloc
global kv_pool_realloc
global kv_pool_destroy

; -----------------------------------------------------------------------------
; POOL_CLASS size_reg, out_reg — class index of a user size, or -1 if the
; block would exceed POOL_MAX_BLOCK. Uses rax.
; index = max(0, floor(log2(size + header - 1)) - (POOL_MIN_SHIFT - 1)).
; The size is range-checked *before* adding the header, so a near-2^64 size
; cannot wrap around into a small class.
; -----------------------------------------------------------------------------
%macro POOL_CLASS 2
    cmp     %1, POOL_MAX_BLOCK - POOL_HDR
    ja      %%large
    lea     rax, [%1 + POOL_HDR - 1]    ; total - 1, >= POOL_HDR - 1 > 0
    bsr     %2, rax
    sub     %2, POOL_MIN_SHIFT - 1
    jns     %%done
    xor     %2, %2                      ; total <= 64: the smallest class
    jmp     %%done
%%large:
    mov     %2, -1
%%done:
%endmacro

; =============================================================================
; kv_pool_malloc — allocate `size` bytes from a pool.
; @param  rdi  size in bytes
; @param  rsi  Pool*
; @returns rax = 16-byte-aligned pointer, or 0 when out of memory (malloc's
;          contract, which every caller relies on: 0 means "fail this
;          operation", never a crash).
; =============================================================================
kv_pool_malloc:
    POOL_CLASS rdi, rcx
    test    rcx, rcx
    js      .large
    mov     rax, [rsi + Pool.free + rcx*8]
    test    rax, rax
    jz      .carve
    mov     rdx, [rax]                  ; LIFO pop: next free block
    mov     [rsi + Pool.free + rcx*8], rdx
    ret                                 ; the header still holds the class
.carve:
    mov     edx, 1 << POOL_MIN_SHIFT
    shl     rdx, cl                     ; block size of this class
    mov     rax, [rsi + Pool.bump]
    lea     r8, [rax + rdx]
    cmp     r8, [rsi + Pool.bump_end]
    ja      .refill                     ; also taken for a fresh pool (bump_end 0)
    mov     [rsi + Pool.bump], r8
    mov     [rax], rcx                  ; header: class
    add     rax, POOL_HDR
    ret
.refill:
    ; Chunk exhausted (its tail, smaller than one block, is abandoned): link a
    ; new chunk in front of the chunk list, then retry the carve.
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for the call
    mov     rbx, rdi
    mov     r12, rsi
    mov     edi, POOL_CHUNK
    call    malloc wrt ..plt
    test    rax, rax
    jz      .refill_fail
    mov     rcx, [r12 + Pool.chunks]
    mov     [rax], rcx                  ; chunk header: next chunk (16 bytes, alignment kept)
    mov     [r12 + Pool.chunks], rax
    lea     rcx, [rax + POOL_HDR]
    mov     [r12 + Pool.bump], rcx
    add     rax, POOL_CHUNK
    mov     [r12 + Pool.bump_end], rax
    mov     rdi, rbx
    mov     rsi, r12
    pop     r13
    pop     r12
    pop     rbx
    jmp     kv_pool_malloc              ; fits now: a chunk holds many blocks
.refill_fail:
    pop     r13
    pop     r12
    pop     rbx
    xor     eax, eax
    ret
.large:
    push    rbx                         ; 1 push: rsp 16-aligned for the call
    mov     rbx, rdi
    add     rdi, POOL_HDR
    jc      .large_fail                 ; size + header does not fit 64 bits
    call    malloc wrt ..plt
    test    rax, rax
    jz      .large_out
    mov     qword [rax], POOL_LARGE
    mov     [rax + 8], rbx              ; user size, needed by realloc
    add     rax, POOL_HDR
.large_out:
    pop     rbx
    ret
.large_fail:
    xor     eax, eax
    pop     rbx
    ret

; =============================================================================
; kv_pool_free — return a block to its pool (NULL is ignored).
; @param  rdi  pointer from this pool, or NULL
; @param  rsi  Pool*
; @returns nothing.
; @remarks Must be *called* (ABI-aligned stack): large blocks tail-jump into
;          glibc free, which requires the standard entry alignment.
; =============================================================================
kv_pool_free:
    test    rdi, rdi
    jz      .out
    mov     rax, [rdi - POOL_HDR]
    cmp     rax, POOL_LARGE
    je      .large
    mov     rdx, [rsi + Pool.free + rax*8]
    mov     [rdi], rdx                  ; LIFO push
    mov     [rsi + Pool.free + rax*8], rdi
.out:
    ret
.large:
    sub     rdi, POOL_HDR
    jmp     free wrt ..plt              ; tail call keeps our caller's alignment

; =============================================================================
; kv_pool_calloc — zeroed allocation of nmemb * size bytes.
; @param  rdi  nmemb   @param rsi  size   @param rdx  Pool*
; @returns rax = pointer, or 0 when the product overflows or out of memory.
; =============================================================================
kv_pool_calloc:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for calls
    mov     r12, rdx                    ; Pool* (mul clobbers rdx)
    mov     rax, rdi
    mul     rsi                         ; rdx:rax = nmemb * size
    jc      .fail                       ; product needs more than 64 bits
    mov     rbx, rax
    mov     rdi, rax
    mov     rsi, r12
    call    kv_pool_malloc
    test    rax, rax
    jz      .out
    mov     r12, rax
    mov     rdi, rax
    xor     esi, esi
    mov     rdx, rbx
    call    memset wrt ..plt
    mov     rax, r12
    jmp     .out
.fail:
    xor     eax, eax
.out:
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_pool_realloc — resize a block, preserving min(old, new) bytes.
; @param  rdi  pointer (NULL = allocate)   @param rsi  new size
; @param  rdx  Pool*
; @returns rax = pointer (possibly unchanged), or 0 when out of memory — the
;          old block is then left intact, as realloc specifies. Size 0 frees
;          the block and returns 0.
; =============================================================================
kv_pool_realloc:
    test    rdi, rdi
    jnz     .have
    mov     rdi, rsi
    mov     rsi, rdx
    jmp     kv_pool_malloc
.have:
    test    rsi, rsi
    jnz     .resize
    sub     rsp, 8                      ; align: kv_pool_free may enter glibc free
    mov     rsi, rdx
    call    kv_pool_free
    add     rsp, 8
    xor     eax, eax
    ret
.resize:
    mov     rax, [rdi - POOL_HDR]       ; capacity of the current block
    cmp     rax, POOL_LARGE
    je      .large_cap
    mov     rcx, rax
    mov     eax, 1 << POOL_MIN_SHIFT
    shl     rax, cl
    sub     rax, POOL_HDR
    jmp     .have_cap
.large_cap:
    mov     rax, [rdi - POOL_HDR + 8]
.have_cap:
    cmp     rsi, rax
    ja      .grow
    mov     rax, rdi                    ; fits (or shrinks): keep the block
    ret
.grow:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; 4 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi                    ; old block
    mov     r12, rax                    ; old capacity (bytes to copy; < new size)
    mov     r13, rdx                    ; Pool*
    mov     rdi, rsi
    mov     rsi, rdx
    call    kv_pool_malloc
    test    rax, rax
    jz      .grow_out
    mov     r14, rax
    mov     rdi, rax
    mov     rsi, rbx
    mov     rdx, r12
    call    memcpy wrt ..plt
    mov     rdi, rbx
    mov     rsi, r13
    call    kv_pool_free
    mov     rax, r14
.grow_out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_pool_destroy — give every chunk back to the system and reset the pool.
; @param  rdi  Pool*
; @returns nothing.
; @remarks Only when no block of this pool is in use any more (the worker has
;          destroyed all its connections). Large blocks were freed by their
;          owners already.
; =============================================================================
kv_pool_destroy:
    push    rbx
    push    r12
    sub     rsp, 8
    mov     r12, rdi
    mov     rbx, [r12 + Pool.chunks]
.next:
    test    rbx, rbx
    jz      .done
    mov     rdi, rbx
    mov     rbx, [rbx]
    call    free wrt ..plt
    jmp     .next
.done:
    mov     rdi, r12
    xor     esi, esi
    mov     edx, Pool_size
    call    memset wrt ..plt
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
