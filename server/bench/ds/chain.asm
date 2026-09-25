; =============================================================================
; chain.asm — bake-off candidate: separate chaining with intrusive linked lists.
;
; The classic hash table: 2^k head pointers (load factor <= 1), each bucket a
; singly linked list threaded through Rec.next, the key hash h1 cached in
; Rec.aux so most non-matching nodes are rejected without a key comparison.
; Complexity: O(1) expected, O(chain length) worst case — every list step is
; a dependent pointer chase (one potential cache miss per node).
;
; Single-threaded (the bake-off measures data-structure cost only). Records
; are owned by the caller; the structure never allocates or frees them.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"

struc Chain
    .heads:     resq 1                  ; Rec*[nb], anonymous mapping (zeroed)
    .mask:      resq 1
    .count:     resq 1
    .nb:        resq 1
endstruc

extern calloc
extern free
extern kv_keyeq

section .text

global bd_chain_create
global bd_chain_destroy
global bd_chain_get
global bd_chain_upsert
global bd_chain_remove
global bd_chain_heads

; =============================================================================
; bd_chain_create — empty table with next_pow2(capacity) heads.
; @param  rdi  capacity (>= 1)
; @returns rax = Chain*, or 0 on allocation failure.
; =============================================================================
bd_chain_create:
    push    rbx
    push    r12
    push    r13
    mov     r12, rdi
    cmp     r12, 2
    jae     .cap_ok
    mov     r12d, 2
.cap_ok:
    lea     rcx, [r12 - 1]
    bsr     rcx, rcx
    inc     ecx
    mov     r13d, 1
    shl     r13, cl                     ; nb = next power of two >= capacity
    mov     edi, 1
    mov     esi, Chain_size
    call    calloc wrt ..plt
    test    rax, rax
    jz      .out
    mov     rbx, rax
    mov     [rbx + Chain.nb], r13
    lea     rax, [r13 - 1]
    mov     [rbx + Chain.mask], rax
    xor     edi, edi
    lea     rsi, [r13*8]
    mov     edx, PROT_READ | PROT_WRITE
    mov     r10d, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
    mov     r8, -1
    xor     r9d, r9d
    mov     eax, SYS_mmap
    syscall
    cmp     rax, -4095
    jae     .fail
    mov     [rbx + Chain.heads], rax
    mov     rdi, rax
    lea     rsi, [r13*8]
    mov     edx, MADV_HUGEPAGE
    mov     eax, SYS_madvise
    syscall
    mov     rax, rbx
    jmp     .out
.fail:
    mov     rdi, rbx
    call    free wrt ..plt
    xor     eax, eax
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; bd_chain_destroy — release the table (not the records).
; @param  rdi  Chain*
; =============================================================================
bd_chain_destroy:
    push    rbx
    mov     rbx, rdi
    mov     rdi, [rbx + Chain.heads]
    mov     rsi, [rbx + Chain.nb]
    shl     rsi, 3
    mov     eax, SYS_munmap
    syscall
    mov     rdi, rbx
    call    free wrt ..plt
    pop     rbx
    ret

; =============================================================================
; bd_chain_heads — expose the head array for statistics in the harness.
; @param  rdi  Chain*   @param rsi  uint64_t *nb_out
; @returns rax = Rec** heads.
; =============================================================================
bd_chain_heads:
    mov     rax, [rdi + Chain.nb]
    mov     [rsi], rax
    mov     rax, [rdi + Chain.heads]
    ret

; =============================================================================
; bd_chain_get — find the record with the given key.
; @param  rdi  Chain*  @param rsi  key  @param rdx  len
; @param  rcx  h1      @param r8   h2 (unused)
; @returns rax = Rec* or 0.
; =============================================================================
bd_chain_get:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     rax, [rdi + Chain.mask]
    and     rax, rcx
    mov     rdx, [rdi + Chain.heads]
    mov     rbx, [rdx + rax*8]
.walk:
    test    rbx, rbx
    jz      .miss
    cmp     [rbx + Rec.aux], r14        ; cached hash: rejects almost every non-match
    jne     .next
    mov     eax, [rbx + Rec.key_len]
    cmp     rax, r13
    jne     .next
    lea     rdi, [rbx + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jnz     .hit
.next:
    mov     rbx, [rbx + Rec.next]
    jmp     .walk
.hit:
    mov     rax, rbx
    jmp     .out
.miss:
    xor     eax, eax
.out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; bd_chain_upsert — insert, or replace the record with an equal key in place.
; @param  rdi  Chain*  @param rsi  Rec* rec  @param rdx  h1  @param rcx  h2 (unused)
; @returns rax = replaced Rec* or 0 when inserted.
; =============================================================================
bd_chain_upsert:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     r15, rdi
    mov     r12, rsi                    ; new record
    mov     r14, rdx                    ; h1
    mov     [r12 + Rec.aux], r14
    mov     rax, [r15 + Chain.mask]
    and     rax, r14
    mov     rdx, [r15 + Chain.heads]
    lea     rbp, [rdx + rax*8]          ; pp = &heads[b]
    mov     r13, rbp                    ; remember the head slot
.walk:
    mov     rbx, [rbp]
    test    rbx, rbx
    jz      .insert
    cmp     [rbx + Rec.aux], r14
    jne     .next
    mov     eax, [rbx + Rec.key_len]
    cmp     eax, [r12 + Rec.key_len]
    jne     .next
    lea     rdi, [rbx + Rec.data]
    lea     rsi, [r12 + Rec.data]
    mov     edx, eax
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .next
    mov     rax, [rbx + Rec.next]       ; replace node in place
    mov     [r12 + Rec.next], rax
    mov     [rbp], r12
    mov     rax, rbx
    jmp     .out
.next:
    lea     rbp, [rbx + Rec.next]
    jmp     .walk
.insert:                                ; push front
    mov     rax, [r13]
    mov     [r12 + Rec.next], rax
    mov     [r13], r12
    inc     qword [r15 + Chain.count]
    xor     eax, eax
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
; bd_chain_remove — unlink the record with the given key.
; @param  rdi  Chain*  @param rsi  key  @param rdx  len
; @param  rcx  h1      @param r8   h2 (unused)
; @returns rax = unlinked Rec* or 0.
; =============================================================================
bd_chain_remove:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     r15, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     rax, [r15 + Chain.mask]
    and     rax, r14
    mov     rdx, [r15 + Chain.heads]
    lea     rbp, [rdx + rax*8]
.walk:
    mov     rbx, [rbp]
    test    rbx, rbx
    jz      .miss
    cmp     [rbx + Rec.aux], r14
    jne     .next
    mov     eax, [rbx + Rec.key_len]
    cmp     rax, r13
    jne     .next
    lea     rdi, [rbx + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    kv_keyeq wrt ..plt
    test    eax, eax
    jz      .next
    mov     rax, [rbx + Rec.next]
    mov     [rbp], rax
    dec     qword [r15 + Chain.count]
    mov     rax, rbx
    jmp     .out
.next:
    lea     rbp, [rbx + Rec.next]
    jmp     .walk
.miss:
    xor     eax, eax
.out:
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
