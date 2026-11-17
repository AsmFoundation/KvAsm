; =============================================================================
; trie.asm — bake-off candidate: binary crit-bit trie over the key hash.
;
; The strongest binary tree for exact-match lookups: navigation needs no key
; comparisons at all — each internal node tests one bit of the 64-bit key
; hash h1 ("critical bit", lowest-first), and only the final leaf's key is
; compared. Depth is bounded by the hash width (<= 64, so O(1) in n formally)
; but is ~log2(n) in practice, and every level is a dependent load.
;
; Assembly tricks used (the "linked list to binary tree" hypothesis done as
; well as possible):
;   * Branchless descent: BT sets CF from the tested hash bit, SBB turns it
;     into 0/-1, AND 8 makes it a child offset — random bits never
;     mispredict a branch.
;   * Tagged pointers: a child pointer carries the child's critical-bit index
;     in its top byte (user-space addresses stay below 2^47 on Linux unless
;     explicitly requested) and bit 0 marks a leaf (records are 8-aligned), so
;     a node is just two pointers: 16 bytes, four per cache line.
;
; Leaves are records (Rec.aux caches h1). Keys with identical 64-bit h1 share
; a leaf through Rec.next, so correctness never depends on hash uniqueness.
; Single-threaded (the bake-off measures data-structure cost only). Records
; are owned by the caller.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

%define PTR_MASK    0x00FFFFFFFFFFFFFF  ; clears the critical-bit tag
%define NODE_SIZE   16

struc Trie
    .root:      resq 1                  ; tagged pointer, 0 = empty
    .pool:      resq 1                  ; node pool (anonymous mapping)
    .pool_cap:  resq 1                  ; nodes in the pool
    .pool_top:  resq 1                  ; next never-used node index
    .free:      resq 1                  ; free-list head (linked via child0)
    .count:     resq 1
endstruc

extern calloc
extern free
extern kv_keyeq

section .text

global bd_trie_create
global bd_trie_destroy
global bd_trie_get
global bd_trie_upsert
global bd_trie_remove
global bd_trie_root

; =============================================================================
; bd_trie_create — empty trie able to hold `capacity` distinct hashes.
; @param  rdi  capacity (internal nodes needed: at most capacity - 1)
; @returns rax = Trie*, or 0 on allocation failure.
; =============================================================================
bd_trie_create:
    push    rbx
    push    r12
    sub     rsp, 8
    mov     r12, rdi
    test    r12, r12
    jnz     .cap_ok
    mov     r12d, 1
.cap_ok:
    mov     edi, 1
    mov     esi, Trie_size
    call    calloc wrt ..plt
    test    rax, rax
    jz      .out
    mov     rbx, rax
    mov     [rbx + Trie.pool_cap], r12
    xor     edi, edi
    mov     rsi, r12
    shl     rsi, 4
    mov     edx, PROT_READ | PROT_WRITE
    mov     r10d, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
    mov     r8, -1
    xor     r9d, r9d
    mov     eax, SYS_mmap
    syscall
    cmp     rax, -4095
    jae     .fail
    mov     [rbx + Trie.pool], rax
    mov     rdi, rax
    mov     rsi, r12
    shl     rsi, 4
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
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; bd_trie_destroy — release the trie (not the records).
; @param  rdi  Trie*
; =============================================================================
bd_trie_destroy:
    push    rbx
    mov     rbx, rdi
    mov     rdi, [rbx + Trie.pool]
    mov     rsi, [rbx + Trie.pool_cap]
    shl     rsi, 4
    mov     eax, SYS_munmap
    syscall
    mov     rdi, rbx
    call    free wrt ..plt
    pop     rbx
    ret

; =============================================================================
; bd_trie_root — tagged root pointer, for depth statistics in the harness.
; @param  rdi  Trie*   @returns rax = root (0, leaf|1, or (bit<<56)|node).
; =============================================================================
bd_trie_root:
    mov     rax, [rdi + Trie.root]
    ret

; =============================================================================
; bd_trie_get — find the record with the given key.
; @param  rdi  Trie*  @param rsi  key  @param rdx  len
; @param  rcx  h1     @param r8   h2 (unused)
; @returns rax = Rec* or 0.
; =============================================================================
bd_trie_get:
    mov     rax, [rdi + Trie.root]
    test    rax, rax
    jz      .miss
    mov     r9, PTR_MASK
.down:
    test    al, 1
    jnz     .leaf
    mov     r10, rax
    shr     r10, 56                     ; critical bit of this node
    bt      rcx, r10                    ; CF = that bit of h1
    sbb     r11, r11                    ; 0 / -1
    and     r11d, 8                     ; child offset 0 / 8
    and     rax, r9
    mov     rax, [rax + r11]            ; the only load per level
    jmp     .down
.leaf:
    and     rax, -2                     ; strip the leaf flag
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8
    mov     rbx, rax
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
.walk:                                  ; collision list (normally one record)
    test    rbx, rbx
    jz      .not_found
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
    jnz     .found
.next:
    mov     rbx, [rbx + Rec.next]
    jmp     .walk
.found:
    mov     rax, rbx
    jmp     .done
.not_found:
    xor     eax, eax
.done:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret
.miss:
    xor     eax, eax
    ret

; =============================================================================
; bd_trie_upsert — insert, or replace the record with an equal key.
; @param  rdi  Trie*  @param rsi  Rec* rec  @param rdx  h1  @param rcx  h2 (unused)
; @returns rax = replaced Rec*, 0 when inserted, or -ENOMEM if the node pool
;          is exhausted (more distinct hashes than the capacity).
; =============================================================================
bd_trie_upsert:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     r15, rdi                    ; Trie*
    mov     r12, rsi                    ; rec
    mov     r14, rdx                    ; h1
    mov     [r12 + Rec.aux], r14
    mov     qword [r12 + Rec.next], 0
    mov     rax, [r15 + Trie.root]
    test    rax, rax
    jnz     .descend
    lea     rax, [r12 + 1]
    mov     [r15 + Trie.root], rax      ; first key: the root is a leaf
    inc     qword [r15 + Trie.count]
    xor     eax, eax
    jmp     .out

.descend:                               ; find the leaf h1 leads to; rbp = slot
    lea     rbp, [r15 + Trie.root]
    mov     r9, PTR_MASK
.down:
    mov     rax, [rbp]
    test    al, 1
    jnz     .at_leaf
    mov     r10, rax
    shr     r10, 56
    bt      r14, r10
    sbb     r11, r11
    and     r11d, 8
    and     rax, r9
    lea     rbp, [rax + r11]
    jmp     .down
.at_leaf:
    and     rax, -2
    mov     r13, rax                    ; L: first record of the leaf
    cmp     [r13 + Rec.aux], r14
    jne     .split

    ; Same 64-bit hash: replace an equal key in the collision list or push.
    mov     rbx, r13
    xor     ecx, ecx                    ; prev
.coll:
    test    rbx, rbx
    jz      .coll_push
    mov     eax, [rbx + Rec.key_len]
    cmp     eax, [r12 + Rec.key_len]
    jne     .coll_next
    push    rcx
    push    rcx                         ; keep 16-byte alignment for the call
    lea     rdi, [rbx + Rec.data]
    lea     rsi, [r12 + Rec.data]
    mov     edx, eax
    call    kv_keyeq wrt ..plt
    pop     rcx
    pop     rcx
    test    eax, eax
    jz      .coll_next
    mov     rax, [rbx + Rec.next]
    mov     [r12 + Rec.next], rax
    test    rcx, rcx
    jz      .coll_head
    mov     [rcx + Rec.next], r12
    jmp     .coll_replaced
.coll_head:
    lea     rax, [r12 + 1]
    mov     [rbp], rax
.coll_replaced:
    mov     rax, rbx
    jmp     .out
.coll_next:
    mov     rcx, rbx
    mov     rbx, [rbx + Rec.next]
    jmp     .coll
.coll_push:
    mov     [r12 + Rec.next], r13
    lea     rax, [r12 + 1]
    mov     [rbp], rax
    inc     qword [r15 + Trie.count]
    xor     eax, eax
    jmp     .out

.split:                                 ; new hash: insert a node at bit c
    mov     rbx, r14
    xor     rbx, [r13 + Rec.aux]
    bsf     rbx, rbx                    ; c = lowest differing bit
    lea     rbp, [r15 + Trie.root]
.find_slot:                             ; first pointer that is a leaf or bit > c
    mov     rax, [rbp]
    test    al, 1
    jnz     .make_node
    mov     r10, rax
    shr     r10, 56
    cmp     r10, rbx
    ja      .make_node
    bt      r14, r10
    sbb     r11, r11
    and     r11d, 8
    and     rax, r9
    lea     rbp, [rax + r11]
    jmp     .find_slot
.make_node:
    ; allocate from the free list, else from the pool
    mov     rdx, [r15 + Trie.free]
    test    rdx, rdx
    jz      .from_pool
    mov     rcx, [rdx]
    mov     [r15 + Trie.free], rcx
    jmp     .have_node
.from_pool:
    mov     rcx, [r15 + Trie.pool_top]
    cmp     rcx, [r15 + Trie.pool_cap]
    jae     .nomem
    lea     rdx, [rcx + 1]
    mov     [r15 + Trie.pool_top], rdx
    shl     rcx, 4
    mov     rdx, [r15 + Trie.pool]
    add     rdx, rcx
.have_node:                             ; rdx = node, rax = old subtree pointer
    bt      r14, rbx
    sbb     r11, r11
    and     r11d, 8                     ; offset of the new leaf's side
    lea     rcx, [r12 + 1]
    mov     [rdx + r11], rcx            ; new leaf
    xor     r11d, 8
    mov     [rdx + r11], rax            ; existing subtree keeps its own tag
    shl     rbx, 56
    or      rbx, rdx
    mov     [rbp], rbx                  ; link the node, tagged with bit c
    inc     qword [r15 + Trie.count]
    xor     eax, eax
    jmp     .out
.nomem:
    mov     rax, -ENOMEM
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
; bd_trie_remove — unlink the record with the given key.
; @param  rdi  Trie*  @param rsi  key  @param rdx  len
; @param  rcx  h1     @param r8   h2 (unused)
; @returns rax = unlinked Rec* or 0.
; @remarks Removing a leaf also removes its parent node: the parent's slot is
;          overwritten with the sibling pointer (which keeps its own tag) and
;          the node returns to the free list.
; =============================================================================
bd_trie_remove:
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
    mov     rax, [r15 + Trie.root]
    test    rax, rax
    jz      .miss
    lea     rbp, [r15 + Trie.root]      ; slot of the current pointer
    xor     esi, esi                    ; pslot: slot of the parent pointer
    mov     r9, PTR_MASK
.down:
    mov     rax, [rbp]
    test    al, 1
    jnz     .at_leaf
    mov     rsi, rbp
    mov     r10, rax
    shr     r10, 56
    bt      r14, r10
    sbb     r11, r11
    and     r11d, 8
    and     rax, r9
    lea     rbp, [rax + r11]
    jmp     .down
.at_leaf:
    mov     [rsp], rsi                  ; keep pslot across kv_keyeq calls
    and     rax, -2
    mov     rbx, rax                    ; r
    xor     ecx, ecx                    ; prev
.walk:
    test    rbx, rbx
    jz      .miss
    cmp     [rbx + Rec.aux], r14
    jne     .next
    mov     eax, [rbx + Rec.key_len]
    cmp     rax, r13
    jne     .next
    push    rcx
    push    rcx
    lea     rdi, [rbx + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    kv_keyeq wrt ..plt
    pop     rcx
    pop     rcx
    test    eax, eax
    jnz     .unlink
.next:
    mov     rcx, rbx
    mov     rbx, [rbx + Rec.next]
    jmp     .walk
.unlink:
    test    rcx, rcx
    jz      .head
    mov     rax, [rbx + Rec.next]       ; not the list head: splice
    mov     [rcx + Rec.next], rax
    jmp     .removed
.head:
    mov     rax, [rbx + Rec.next]
    test    rax, rax
    jz      .last
    inc     rax                         ; promote the next colliding record
    mov     [rbp], rax
    jmp     .removed
.last:                                  ; leaf disappears together with its parent
    mov     rsi, [rsp]
    test    rsi, rsi
    jnz     .with_parent
    mov     qword [r15 + Trie.root], 0
    jmp     .removed
.with_parent:
    mov     rdx, [rsi]
    and     rdx, r9                     ; parent node
    mov     rax, [rdx + 8]              ; sibling = child1 if we were child0 ...
    cmp     rbp, rdx
    je      .sib_ok
    mov     rax, [rdx]                  ; ... else child0
.sib_ok:
    mov     [rsi], rax
    mov     rcx, [r15 + Trie.free]
    mov     [rdx], rcx
    mov     [r15 + Trie.free], rdx
.removed:
    dec     qword [r15 + Trie.count]
    mov     rax, rbx
    jmp     .out
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
