; =============================================================================
; hcache.asm — a reader thread's private two-level record cache ("L1"/"L2")
; in front of the shared cuckoo table ("L3").
;
; Why: the table is shared by every worker and costs up to two bucket lines
; plus the record per lookup. A small per-thread cache sized to the CPU's
; L1d/L2 answers repeated reads of hot keys from lines this core already
; owns, never touching the table. Whether that beats the hardware caches,
; which already keep hot buckets and records close, is exactly what
; bench_store measures (docs/adr/0006); the cache is off unless configured.
;
; Layout: each level is a direct-mapped array of 32-byte HEntry (kvasm.inc):
;   L1 index = h2 & l1_mask           (low bits of h2)
;   L2 index = (h2 >> 20) & l2_mask   (different bits: conflicts decorrelate)
; A lookup checks L1, then L2 (a hit is promoted into L1), then the table
; (a hit fills both levels).
;
; Correctness — why a cached pointer can never be stale or freed:
;   Writers bump inval[(h1 >> 32) & mask] after publishing a replace/delete
;   in the index and before retiring the old record (store.asm).
;   Fill: the reader loads the generation g BEFORE its table lookup and
;   stores {h2, h1, rec, g}. If the writer's bump happened before that load,
;   TSO guarantees the lookup also sees the new index state (the index store
;   precedes the bump in the writer's order), so rec is current. If not,
;   the entry carries the old g and fails validation later: harmless.
;   Hit: an entry is used only if both hashes match (so the generation slot
;   is the one used at fill time) and entry.gen == inval[slot] now. Then no
;   replace/delete of the key has been bumped since the fill, so rec was not
;   yet retired when the reader loaded the generation. It cannot be freed
;   before this reader's next quiescent state (QSBR), and the reader finishes
;   with rec before announcing one. If the reader had already observed the
;   epoch that follows the retire, it would also see the bump (the retire's
;   XCHG orders it) and reject the entry.
;   Keys are still compared exactly on every hit: a cache never widens what
;   counts as "the same key".
;
; Threading: an HCache belongs to one thread; only the inval array is shared,
; read here and written by the lock-holding writer.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

%define L2_SHIFT    20                  ; h2 bits skipped by the L2 index

extern aligned_alloc
extern calloc
extern free
extern memset
extern kv_keyeq
extern kv_cuckoo_get

section .text

global kv_hcache_create
global kv_hcache_destroy
global kv_hcache_get
global kv_hcache_stats

; =============================================================================
; kv_hcache_create — allocate a private cache for one reader thread.
;
; @param  rdi  Table* (kv_store_enable_inval must have succeeded on it)
; @param  esi  l1_bits: 2^l1_bits L1 entries, 0 = no L1 (max HCACHE_BITS_MAX)
; @param  edx  l2_bits: 2^l2_bits L2 entries, 0 = no L2 (max HCACHE_BITS_MAX)
; @returns rax = HCache*, or 0 if the table has no inval array, a size is
;          out of range, both levels are 0, or memory is short (partial
;          allocations are released).
; @remarks Entries start empty (tag 0). An L1 of 2^10 entries is 32 KiB,
;          about a P-core's L1d; 2^16 L2 entries are 2 MiB, about its L2.
; =============================================================================
kv_hcache_create:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15                         ; 5 pushes: rsp 16-aligned for calls
    mov     r12, rdi
    mov     r13d, esi
    mov     r14d, edx
    xor     ebx, ebx                    ; HCache* (0 until allocated)
    cmp     qword [r12 + Table.inval], 0
    je      .fail
    cmp     r13d, HCACHE_BITS_MAX
    ja      .fail
    cmp     r14d, HCACHE_BITS_MAX
    ja      .fail
    mov     eax, r13d
    or      eax, r14d
    jz      .fail                       ; a cache with no level is a mistake
    mov     edi, 64
    mov     esi, HCache_size
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     rbx, rax
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, HCache_size
    call    memset wrt ..plt
    mov     [rbx + HCache.table], r12
    mov     qword [rbx + HCache.l1_mask], -1
    mov     qword [rbx + HCache.l2_mask], -1
    mov     edi, r13d
    call    alloc_level
    test    r13d, r13d
    jz      .l2
    test    rax, rax
    jz      .fail
    mov     [rbx + HCache.l1], rax
    mov     [rbx + HCache.l1_mask], rdx
.l2:
    mov     edi, r14d
    call    alloc_level
    test    r14d, r14d
    jz      .done
    test    rax, rax
    jz      .fail
    mov     [rbx + HCache.l2], rax
    mov     [rbx + HCache.l2_mask], rdx
.done:
    mov     rax, rbx
    jmp     .out
.fail:
    mov     rdi, rbx
    call    kv_hcache_destroy           ; NULL and half-built caches are fine
    xor     eax, eax
.out:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; alloc_level — zeroed, 64-byte aligned HEntry array of 2^bits entries.
; Internal.
; @param  edi  bits (0 = none)
; @returns rax = array or 0 (bits == 0 or out of memory), rdx = bits mask.
; =============================================================================
alloc_level:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for calls
    xor     eax, eax
    xor     ebx, ebx                    ; bits 0 => mask -1 (level disabled)
    test    edi, edi
    jz      .out
    mov     ecx, edi
    mov     ebx, 1
    shl     rbx, cl                     ; entries
    mov     r12, rbx
    shl     r12, 5                      ; bytes (32 per entry, >= 64 for bits >= 1)
    mov     edi, 64
    mov     rsi, r12
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .out
    push    rax
    push    rax
    mov     rdi, rax
    xor     esi, esi
    mov     rdx, r12
    call    memset wrt ..plt            ; tag 0 = empty
    pop     rax
    pop     rax
.out:
    lea     rdx, [rbx - 1]
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_hcache_destroy — free a private cache.
; @param  rdi  HCache* (NULL is a no-op)
; @returns nothing.
; @remarks Records it points to are owned by the table, never freed here.
; =============================================================================
kv_hcache_destroy:
    test    rdi, rdi
    jz      .none
    push    rbx
    mov     rbx, rdi
    mov     rdi, [rbx + HCache.l1]
    call    free wrt ..plt
    mov     rdi, [rbx + HCache.l2]
    call    free wrt ..plt
    mov     rdi, rbx
    call    free wrt ..plt
    pop     rbx
.none:
    ret

; =============================================================================
; kv_hcache_stats — copy the cache's counters.
; @param  rdi  HCache*   @param rsi  uint64_t out[4]:
;              L1 hits, L2 hits, table lookups, stale (invalidated) entries
; @returns nothing.
; =============================================================================
kv_hcache_stats:
    mov     rax, [rdi + HCache.st_l1_hits]
    mov     [rsi], rax
    mov     rax, [rdi + HCache.st_l2_hits]
    mov     [rsi + 8], rax
    mov     rax, [rdi + HCache.st_misses]
    mov     [rsi + 16], rax
    mov     rax, [rdi + HCache.st_stale]
    mov     [rsi + 24], rax
    ret

; =============================================================================
; kv_hcache_get — look a key up through L1, L2, then the table.
;
; @param  rdi  HCache*
; @param  rsi  const uint8_t *key   @param rdx  size_t len
; @param  rcx  h1                   @param r8   h2
; @returns rax = Rec* whose key equals (key, len), or 0 if absent.
; @remarks Same contract as kv_store_get: the caller is an online QSBR reader
;          and stops using the record before its next quiescent state.
;          Misses (absent keys) are not cached: every miss asks the table.
;          Entries whose generation moved are cleared when met ("stale").
; =============================================================================
kv_hcache_get:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [rsp] = generation before the table lookup
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     r15, r8
    ; rbp = &inval[(h1 >> 32) & mask]: the key's generation for this call.
    mov     rax, [rbx + HCache.table]
    mov     rbp, r14
    shr     rbp, 32
    and     rbp, [rax + Table.inval_mask]
    shl     rbp, 3
    add     rbp, [rax + Table.inval]

    ; ---- L1 ----
    mov     rdi, [rbx + HCache.l1]
    test    rdi, rdi
    jz      .l2
    mov     rax, r15
    and     rax, [rbx + HCache.l1_mask]
    shl     rax, 5
    add     rdi, rax                    ; L1 entry
    call    probe
    test    rax, rax
    jz      .l2
    inc     qword [rbx + HCache.st_l1_hits]
    jmp     .out

    ; ---- L2 ----
.l2:
    mov     rdi, [rbx + HCache.l2]
    test    rdi, rdi
    jz      .table
    mov     rax, r15
    shr     rax, L2_SHIFT
    and     rax, [rbx + HCache.l2_mask]
    shl     rax, 5
    add     rdi, rax                    ; L2 entry
    call    probe
    test    rax, rax
    jz      .table
    inc     qword [rbx + HCache.st_l2_hits]
    mov     rsi, [rdi + HEntry.gen]     ; promote the validated entry into L1
    mov     rdi, rax
    call    fill_l1
    jmp     .out

    ; ---- the shared table ----
.table:
    mov     rax, [rbp]
    mov     [rsp], rax                  ; generation BEFORE the lookup (fill safety)
    mov     rdi, [rbx + HCache.table]
    mov     rsi, r12
    mov     rdx, r13
    mov     rcx, r14
    mov     r8, r15
    call    kv_cuckoo_get wrt ..plt
    inc     qword [rbx + HCache.st_misses]
    test    rax, rax
    jz      .out                        ; absent keys are not cached
    mov     rdi, rax
    mov     rsi, [rsp]
    call    fill_l1
    mov     rdi, rax
    mov     rsi, [rsp]
    call    fill_l2
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
; probe — validate one cache entry against the key being looked up.
; Internal; runs inside kv_hcache_get's frame.
;
; @param  rdi  HEntry*
; @param  rbx  HCache*  r12/r13 key/len  r14 h1  r15 h2  rbp &generation
; @returns rax = the entry's record if it is valid for this key, else 0.
;          rdi (the entry) is preserved — saved in r10 around kv_keyeq — so
;          the caller can promote a validated entry.
; @remarks Order of checks: both hashes (cheap, and they pin the generation
;          slot), then the generation (the record is live only if it still
;          matches), then the exact key. A matching tag with a moved
;          generation is stale: its tag is cleared so it is not re-checked.
;          Called with rsp misaligned by 8, which is fine: kv_keyeq is a leaf
;          with no alignment needs. Clobbers rax, rcx, rdx, rsi, r8-r10.
; =============================================================================
probe:
    cmp     [rdi + HEntry.tag], r15
    jne     .none
    cmp     [rdi + HEntry.h1], r14
    jne     .none
    mov     rax, [rbp]
    cmp     [rdi + HEntry.gen], rax
    jne     .stale
    mov     r9, [rdi + HEntry.rec]      ; live: safe to dereference (see header)
    mov     edx, [r9 + Rec.key_len]
    cmp     rdx, r13
    jne     .none
    mov     r10, rdi                    ; kv_keyeq takes rdi/rsi as its operands
    lea     rdi, [r9 + Rec.data]
    mov     rsi, r12
    call    kv_keyeq wrt ..plt          ; clobbers rax, rcx, r8 only
    mov     rdi, r10
    test    eax, eax
    jz      .none
    mov     rax, r9
    ret
.stale:
    inc     qword [rbx + HCache.st_stale]
    mov     qword [rdi + HEntry.tag], 0
.none:
    xor     eax, eax
    ret

; =============================================================================
; fill_l1 / fill_l2 — store {h2, h1, rec, gen} into the key's slot of a
; level (no-op when the level is disabled). Internal; kv_hcache_get's frame.
;
; @param  rdi  Rec*   @param rsi  generation read before the rec was found
; @param  rbx  HCache*  r14 h1  r15 h2
; @returns rax = rdi (the record), so calls can be chained.
; @remarks Direct-mapped: the previous occupant is simply replaced.
;          Clobbers rcx, rdx.
; =============================================================================
fill_l1:
    mov     rdx, [rbx + HCache.l1]
    test    rdx, rdx
    jz      .done
    mov     rcx, r15
    and     rcx, [rbx + HCache.l1_mask]
    shl     rcx, 5
    add     rdx, rcx
    mov     [rdx + HEntry.tag], r15
    mov     [rdx + HEntry.h1], r14
    mov     [rdx + HEntry.rec], rdi
    mov     [rdx + HEntry.gen], rsi
.done:
    mov     rax, rdi
    ret

fill_l2:
    mov     rdx, [rbx + HCache.l2]
    test    rdx, rdx
    jz      .done
    mov     rcx, r15
    shr     rcx, L2_SHIFT
    and     rcx, [rbx + HCache.l2_mask]
    shl     rcx, 5
    add     rdx, rcx
    mov     [rdx + HEntry.tag], r15
    mov     [rdx + HEntry.h1], r14
    mov     [rdx + HEntry.rec], rdi
    mov     [rdx + HEntry.gen], rsi
.done:
    mov     rax, rdi
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
