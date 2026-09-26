; =============================================================================
; store.asm — the KvNode key-value store: records, writer mutex, QSBR memory
; reclamation and the public API around the cuckoo index (cuckoo.asm).
;
; Concurrency model ("owner-less MemC3"):
;   * Any thread may read (kv_store_get) without locks or atomic
;     read-modify-writes, provided it is registered as a QSBR reader and
;     online (kv_qsbr_online) while it reads.
;   * Any thread may write (kv_store_put / kv_store_delete); writers are
;     serialized by one futex mutex. Record allocation and copying happen
;     BEFORE taking the mutex, so the critical section is only the index
;     update plus bounded reclamation.
;   * A replaced or deleted record is never freed immediately: readers on
;     other cores may still be reading it. It is "retired" into a FIFO limbo
;     list stamped with the current epoch; once every online reader has
;     announced a quiescent state in a later epoch, nobody can hold it, and it
;     is freed (quiescent-state-based reclamation, QSBR).
;
; QSBR ordering on x86-TSO (proof sketch):
;   writer: unlink R -> R.epoch = E -> XCHG epoch = E+1 (full fence) -> scan
;           readers' observed epochs.
;   reader: load epoch -> store observed (plain when already online, XCHG when
;           coming online) -> read the table.
;   A reader that published observed > E loaded epoch E+1, which is ordered
;   after the unlink store, so none of its later reads can reach R. A reader
;   seen offline (0) that comes online later fences (XCHG) before reading, so
;   it too reads after the unlink. Hence R may be freed once every online
;   reader's observed epoch is > E, capped by the epoch at scan time.
;
; Every operation does O(1) work in the number of keys: the index is O(1),
; reclamation frees at most RECLAIM_BATCH records per call, and reader scans
; are O(n_readers) and rate-limited.
; =============================================================================

default rel
bits 64

%include "kvnode.inc"

%define RECLAIM_BATCH       8           ; records freed per write (amortized drain)
%define RECLAIM_BATCH_FORCE 4096        ; per explicit kv_store_reclaim call
%define SCAN_EVERY          16          ; rescan readers at most every N writes...
%define SCAN_LIMBO_FORCE    64          ; ...unless this many records wait
%define MAX_CAPACITY        0x0000001000000000      ; 2^36 keys
%define MAX_READERS         4096
%define MAX_FIELD_LEN       0x7FFFFFFF  ; lengths are stored as u32

extern aligned_alloc
extern malloc
extern calloc
extern free
extern memcpy
extern memset
extern kv_siphash128
extern kv_cuckoo_get
extern kv_cuckoo_upsert
extern kv_cuckoo_remove
extern kv_cuckoo_prefetch
extern kv_cuckoo_prefetch_records
extern kv_mutex_lock
extern kv_mutex_unlock

section .text

global kv_store_create
global kv_store_destroy
global kv_store_hash
global kv_store_get
global kv_store_put
global kv_store_delete
global kv_store_reclaim
global kv_store_stats
global kv_qsbr_online
global kv_qsbr_offline
global kv_qsbr_quiescent
global kv_store_prefetch
global kv_store_prefetch_records
global kv_store_enable_inval

; =============================================================================
; kv_store_create — allocate an empty store.
;
; @param  rdi  capacity: maximum number of keys (1 .. 2^36)
; @param  rsi  n_readers: number of QSBR reader slots (0 .. 4096); every
;              thread that calls kv_store_get needs its own slot id
; @param  rdx  const uint8_t seed[16]: SipHash key; must be secret and random
;              (getrandom) so key placement is unpredictable from outside
; @param  rcx  mem_limit: max bytes of record memory (0 = unlimited)
; @returns rax = Table*, or 0 on invalid arguments or allocation failure
;          (everything allocated so far is released).
; @remarks Buckets come from an anonymous NORESERVE mapping with transparent
;          huge pages requested: zeroed by the kernel, touched lazily, and
;          fewer TLB misses on large tables. nbuckets = next power of two >=
;          capacity/5, i.e. at most ~83% of the 6-slot buckets are ever used,
;          which keeps cuckoo paths short.
; =============================================================================
kv_store_create:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     r12, rdi                    ; capacity
    mov     r13, rsi                    ; n_readers
    mov     r14, rdx                    ; seed
    mov     r15, rcx                    ; mem_limit
    xor     ebx, ebx                    ; Table* (0 until allocated)
    test    r12, r12
    jz      .fail
    mov     rax, MAX_CAPACITY
    cmp     r12, rax
    ja      .fail
    cmp     r13, MAX_READERS
    ja      .fail

    mov     edi, 64
    mov     esi, Table_size
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     rbx, rax
    mov     rdi, rbx
    xor     esi, esi
    mov     edx, Table_size
    call    memset wrt ..plt

    ; nbuckets = next_pow2(max(2, ceil(capacity / 5)))
    lea     rax, [r12 + 4]
    xor     edx, edx
    mov     ecx, 5
    div     rcx
    cmp     rax, 2
    jae     .have_min
    mov     eax, 2
.have_min:
    lea     rcx, [rax - 1]
    bsr     rcx, rcx
    inc     ecx
    mov     eax, 1
    shl     rax, cl                     ; smallest power of two >= the target
    mov     [rbx + Table.nbuckets], rax
    lea     rcx, [rax - 1]
    mov     [rbx + Table.mask], rcx

    ; buckets: zeroed anonymous memory, huge pages requested (best effort)
    shl     rax, 6
    mov     rbp, rax                    ; bytes
    xor     edi, edi
    mov     rsi, rbp
    mov     edx, PROT_READ | PROT_WRITE
    mov     r10d, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
    mov     r8, -1
    xor     r9d, r9d
    mov     eax, SYS_mmap
    syscall
    cmp     rax, -4095
    jae     .fail                       ; -errno
    mov     [rbx + Table.buckets], rax
    mov     rdi, rax
    mov     rsi, rbp
    mov     edx, MADV_HUGEPAGE
    mov     eax, SYS_madvise
    syscall                             ; failure only costs TLB efficiency
    ; Fault the table in with WRITES, one per 2 MiB-aligned region, while no
    ; reader exists. Measured on the WSL2 kernel (5.15): left to the lookups,
    ; a region's first touch is a read, which maps the huge zero page, and
    ; the insert that follows splits it into 4 KiB pages (thp_split_pmd +9,
    ; AnonHugePages 0 for a fully written 16 MiB table). A write to an
    ; untouched region allocates a real huge page instead, so every bucket
    ; lookup hits the TLB (8 entries per 16 MiB) rather than walking 4 KiB
    ; page tables. Without THP each write just faults in one 4 KiB page.
    ; Memory already zero stays zero; the table becomes resident at startup
    ; (16 MiB for the default capacity) instead of as keys arrive.
    mov     rdi, [rbx + Table.buckets]
    lea     rcx, [rdi + rbp]            ; end of the table
    add     rdi, HUGE_PAGE - 1
    and     rdi, -HUGE_PAGE             ; first aligned region
.prefault:
    lea     rax, [rdi + HUGE_PAGE]
    cmp     rax, rcx
    ja      .prefaulted                 ; only whole regions can be huge
    mov     byte [rdi], 0
    mov     rdi, rax
    jmp     .prefault
.prefaulted:

    ; QSBR reader slots, one cache line each (at least one, never read if 0)
    mov     rax, r13
    test    rax, rax
    jnz     .have_readers
    mov     eax, 1
.have_readers:
    shl     rax, 6
    mov     rbp, rax
    mov     edi, 64
    mov     rsi, rbp
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [rbx + Table.readers], rax
    mov     rdi, rax
    xor     esi, esi
    mov     rdx, rbp
    call    memset wrt ..plt            ; observed = 0: everyone starts offline
    mov     [rbx + Table.n_readers], r13

    mov     edi, MAX_BFS * BfsEntry_size
    call    malloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [rbx + Table.bfs], rax
    mov     edi, VISIT_SLOTS
    mov     esi, 8
    call    calloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [rbx + Table.visit], rax

    mov     rax, [r14]
    mov     [rbx + Table.seed0], rax
    mov     rax, [r14 + 8]
    mov     [rbx + Table.seed1], rax
    mov     [rbx + Table.max_records], r12
    test    r15, r15
    jnz     .have_limit
    mov     r15, -1                     ; unlimited
.have_limit:
    mov     [rbx + Table.mem_limit], r15
    mov     qword [rbx + Table.epoch], 1      ; 0 means "offline" in reader slots
    mov     rax, rbx
    jmp     .out
.fail:
    mov     rdi, rbx
    call    kv_store_destroy            ; handles NULL and partial tables
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
; kv_store_destroy — free the store and every record it owns.
;
; @param  rdi  Table* (NULL is a no-op; partially created tables are fine)
; @returns nothing.
; @remarks Only valid once no other thread uses the table (after workers are
;          joined): it frees live and retired records without grace periods.
; =============================================================================
kv_store_destroy:
    test    rdi, rdi
    jz      .none
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    mov     rbx, rdi
    mov     r12, [rbx + Table.buckets]
    test    r12, r12
    jz      .limbo
    mov     r13, [rbx + Table.nbuckets]
.bucket:
    xor     r14d, r14d
.slot:
    mov     rdi, [r12 + Bucket.slot + r14*8]
    test    rdi, rdi
    jz      .next_slot
    call    free wrt ..plt
.next_slot:
    inc     r14d
    cmp     r14d, BK_SLOTS
    jb      .slot
    add     r12, Bucket_size
    dec     r13
    jnz     .bucket
    mov     rdi, [rbx + Table.buckets]
    mov     rsi, [rbx + Table.nbuckets]
    shl     rsi, 6
    mov     eax, SYS_munmap
    syscall
.limbo:
    mov     r12, [rbx + Table.limbo_head]
.limbo_next:
    test    r12, r12
    jz      .scratch
    mov     r13, [r12 + Rec.next]
    mov     rdi, r12
    call    free wrt ..plt
    mov     r12, r13
    jmp     .limbo_next
.scratch:
    mov     rdi, [rbx + Table.inval]
    call    free wrt ..plt              ; 0 when caching was never enabled
    mov     rdi, [rbx + Table.readers]
    call    free wrt ..plt              ; free(NULL) is a no-op
    mov     rdi, [rbx + Table.bfs]
    call    free wrt ..plt
    mov     rdi, [rbx + Table.visit]
    call    free wrt ..plt
    mov     rdi, rbx
    call    free wrt ..plt
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
.none:
    ret

; =============================================================================
; kv_store_hash — the store's keyed hash of a key (SipHash-2-4-128).
;
; @param  rdi  Table*   @param rsi  key   @param rdx  len
; @param  rcx  uint64_t out[2] (may be NULL)
; @returns rax = h1, rdx = h2 (also written to out when non-NULL).
; @remarks Callers hash once and pass (h1, h2) to get/put/delete, so the
;          per-process secret seed never leaves the Table.
; =============================================================================
kv_store_hash:
    lea     r8, [rdi + Table.seed0]
    mov     rdi, rsi
    mov     rsi, rdx
    mov     rdx, r8
    jmp     kv_siphash128 wrt ..plt

; =============================================================================
; kv_store_get — lock-free lookup.
;
; @param  rdi  Table*  @param rsi  key  @param rdx  len
; @param  rcx  h1      @param r8   h2   (from kv_store_hash)
; @returns rax = Rec* (value at data + align8(key_len), val_len bytes), or 0.
; @remarks The calling thread must be online in QSBR (kv_qsbr_online) and
;          must not use the record after its next kv_qsbr_quiescent /
;          kv_qsbr_offline call: after that the record may be freed.
; =============================================================================
kv_store_get:
    jmp     kv_cuckoo_get wrt ..plt

; =============================================================================
; kv_store_prefetch — warm a key's two candidate buckets (a hint; see
; kv_cuckoo_prefetch).
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns nothing.
; =============================================================================
kv_store_prefetch:
    jmp     kv_cuckoo_prefetch wrt ..plt

; =============================================================================
; kv_store_prefetch_records — warm the records whose tags match (a hint; see
; kv_cuckoo_prefetch_records). Call after kv_store_prefetch had time to land.
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns nothing.
; =============================================================================
kv_store_prefetch_records:
    jmp     kv_cuckoo_prefetch_records wrt ..plt

; =============================================================================
; kv_store_put — insert or replace a key's value.
;
; @param  rdi  Table*  @param rsi  key  @param rdx  key len
; @param  rcx  value   @param r8   value len
; @param  r9   h1      @param [rsp+8] h2
; @returns rax = 1 if the key was created, 0 if an existing value was
;          replaced, -E2BIG if a length exceeds 2^31-1, -ENOMEM if the memory
;          limit would be exceeded or allocation failed, -ENOSPC if the key is
;          new and the store is at capacity.
; @remarks The record is allocated and filled before the writer mutex is
;          taken, so large values do not lengthen the critical section. The
;          replaced record is retired, never freed in place.
; =============================================================================
kv_store_put:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 40                     ; [0]=h2 [8]=size [16]=rec [24]=result [32]=keyspan
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     r15, r8
    mov     rbp, r9
    mov     rax, [rsp + 40 + 48 + 8]    ; h2 (7th argument)
    mov     [rsp], rax

    mov     eax, MAX_FIELD_LEN
    cmp     r13, rax
    ja      .e2big
    cmp     r15, rax
    ja      .e2big

    lea     rax, [r13 + 7]
    and     rax, -8                     ; key area, padded to 8
    mov     [rsp + 32], rax
    lea     rax, [rax + r15 + Rec_size]
    mov     [rsp + 8], rax              ; record size

    ; Reserve memory first so concurrent writers cannot overshoot the limit.
    mov     rcx, rax
    lock xadd [rbx + Table.mem_used], rcx
    add     rcx, rax
    cmp     rcx, [rbx + Table.mem_limit]
    ja      .nomem_release

    mov     rdi, rax
    call    malloc wrt ..plt
    test    rax, rax
    jz      .nomem_release
    mov     [rsp + 16], rax

    mov     qword [rax + Rec.next], 0
    mov     qword [rax + Rec.aux], 0
    mov     [rax + Rec.key_len], r13d
    mov     [rax + Rec.val_len], r15d
    test    r13, r13
    jz      .copy_value
    ; Zero the last word of the padded key area before copying the key, so
    ; padding bytes are deterministic (never stale heap contents).
    mov     rcx, [rsp + 32]
    mov     qword [rax + Rec.data + rcx - 8], 0
    lea     rdi, [rax + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
.copy_value:
    test    r15, r15
    jz      .locked_update
    mov     rax, [rsp + 16]
    mov     rcx, [rsp + 32]
    lea     rdi, [rax + Rec.data + rcx]
    mov     rsi, r14
    mov     rdx, r15
    call    memcpy wrt ..plt

.locked_update:
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     rsi, [rsp + 16]
    mov     rdx, rbp
    mov     rcx, [rsp]
    call    kv_cuckoo_upsert wrt ..plt
    test    rax, rax
    js      .refused                    ; -ENOSPC
    jz      .created
    ; Replaced. Order matters (see hcache.asm): the new pointer is already
    ; published, the key's generation is bumped next, and only then is the
    ; old record retired — so no private cache can validate a copy of the
    ; old record once it may be freed. r12 (key pointer) is free for reuse.
    mov     r12, rax
    mov     rdi, rbx
    mov     rsi, rbp                    ; h1
    call    inval_bump
    mov     rdi, rbx
    mov     rsi, r12
    call    store_retire                ; replaced: old record -> limbo
    mov     qword [rsp + 24], 0
    jmp     .reclaim
.created:
    mov     qword [rsp + 24], 1
    jmp     .reclaim
.refused:
    mov     [rsp + 24], rax
.reclaim:
    mov     rdi, rbx
    xor     esi, esi
    call    store_reclaim_step
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    mov     rax, [rsp + 24]
    test    rax, rax
    jns     .out
    ; Refused: the record was never published, so it can be freed directly.
    mov     rdi, [rsp + 16]
    call    free wrt ..plt
    mov     rcx, [rsp + 8]
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rax, [rsp + 24]
    jmp     .out
.nomem_release:
    mov     rcx, [rsp + 8]
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rax, -ENOMEM
    jmp     .out
.e2big:
    mov     rax, -E2BIG
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
; kv_store_delete — remove a key.
;
; @param  rdi  Table*  @param rsi  key  @param rdx  len
; @param  rcx  h1      @param r8   h2
; @returns rax = 1 if the key existed and was removed, 0 if it was absent.
; @remarks The record is retired (freed after a grace period), so concurrent
;          readers that already found it can finish using it safely.
; =============================================================================
kv_store_delete:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     rbx, rdi
    mov     r12, rsi
    mov     r13, rdx
    mov     r14, rcx
    mov     r15, r8
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, r13
    mov     rcx, r14
    mov     r8, r15
    call    kv_cuckoo_remove wrt ..plt
    xor     ebp, ebp
    test    rax, rax
    jz      .reclaim
    mov     r12, rax                    ; unlinked record (key pointer no longer needed)
    mov     rdi, rbx
    mov     rsi, r14                    ; h1: bump before retiring, as in kv_store_put
    call    inval_bump
    mov     rdi, rbx
    mov     rsi, r12
    call    store_retire
    mov     ebp, 1
.reclaim:
    mov     rdi, rbx
    xor     esi, esi
    call    store_reclaim_step
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    mov     eax, ebp
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; =============================================================================
; kv_store_reclaim — maintenance: free every retired record whose grace
; period has ended.
;
; @param  rdi  Table*
; @returns rax = number of records still waiting in limbo afterwards.
; @remarks Writes normally drain limbo incrementally; call this periodically
;          (e.g. from a worker timer) so memory is returned even when writes
;          stop. Takes the writer mutex.
; =============================================================================
kv_store_reclaim:
    push    rbx
    mov     rbx, rdi
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     esi, 1
    call    store_reclaim_step
    mov     rax, [rbx + Table.limbo_count]
    push    rax
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    pop     rax
    pop     rbx
    ret

; =============================================================================
; kv_store_stats — copy the store's counters.
;
; @param  rdi  Table*   @param rsi  KvStats* out
; @returns nothing.
; @remarks Lock-free and therefore approximate under concurrent writes (each
;          counter is read atomically, the set is not a single snapshot).
; =============================================================================
kv_store_stats:
    mov     rax, [rdi + Table.count]
    mov     [rsi + KvStats.count], rax
    mov     rax, [rdi + Table.nbuckets]
    mov     [rsi + KvStats.nbuckets], rax
    mov     rax, [rdi + Table.max_records]
    mov     [rsi + KvStats.max_records], rax
    mov     rax, [rdi + Table.mem_used]
    mov     [rsi + KvStats.mem_used], rax
    mov     rax, [rdi + Table.mem_limit]
    mov     [rsi + KvStats.mem_limit], rax
    mov     rax, [rdi + Table.limbo_count]
    mov     [rsi + KvStats.limbo_count], rax
    mov     rax, [rdi + Table.epoch]
    mov     [rsi + KvStats.epoch], rax
    mov     rax, [rdi + Table.st_inserts]
    mov     [rsi + KvStats.inserts], rax
    mov     rax, [rdi + Table.st_replaces]
    mov     [rsi + KvStats.replaces], rax
    mov     rax, [rdi + Table.st_deletes]
    mov     [rsi + KvStats.deletes], rax
    mov     rax, [rdi + Table.st_bfs]
    mov     [rsi + KvStats.bfs], rax
    mov     rax, [rdi + Table.st_moves]
    mov     [rsi + KvStats.moves], rax
    mov     rax, [rdi + Table.st_full]
    mov     [rsi + KvStats.full], rax
    mov     rax, [rdi + Table.st_freed]
    mov     [rsi + KvStats.freed], rax
    ret

; =============================================================================
; kv_qsbr_online — reader slot `rid` starts reading the table.
;
; @param  rdi  Table*   @param esi  reader slot id (< n_readers)
; @returns nothing.
; @remarks XCHG is a full fence: the slot's new epoch is globally visible
;          before any subsequent table read. Without it a writer could read
;          the slot as still offline and free a record this thread is about
;          to reach (StoreLoad reordering). Call once when a worker wakes up.
; =============================================================================
kv_qsbr_online:
    mov     rax, [rdi + Table.readers]
    mov     esi, esi
    shl     rsi, 6
    mov     rcx, [rdi + Table.epoch]
    xchg    [rax + rsi + QsbrSlot.observed], rcx
    ret

; =============================================================================
; kv_qsbr_quiescent — reader slot `rid` holds no record pointers right now.
;
; @param  rdi  Table*   @param esi  reader slot id
; @returns nothing.
; @remarks Plain store (the reader is already online): TSO orders it after
;          the thread's earlier table reads. Call between requests (once per
;          event-loop iteration); every retired record is freed only after
;          all online readers have done so.
; =============================================================================
kv_qsbr_quiescent:
    mov     rax, [rdi + Table.readers]
    mov     esi, esi
    shl     rsi, 6
    mov     rcx, [rdi + Table.epoch]
    mov     [rax + rsi + QsbrSlot.observed], rcx
    ret

; =============================================================================
; kv_qsbr_offline — reader slot `rid` stops reading (e.g. before blocking).
;
; @param  rdi  Table*   @param esi  reader slot id
; @returns nothing.
; @remarks An offline reader never delays reclamation, so a worker idling in
;          epoll_wait does not pin memory. It must not touch the table again
;          before kv_qsbr_online.
; =============================================================================
kv_qsbr_offline:
    mov     rax, [rdi + Table.readers]
    mov     esi, esi
    shl     rsi, 6
    mov     qword [rax + rsi + QsbrSlot.observed], 0
    ret

; =============================================================================
; kv_store_enable_inval — allocate the invalidation generations that let
; readers keep private caches of records (hcache.asm).
;
; @param  rdi  Table*
; @param  esi  bits: 2^bits generation counters (1 .. INVAL_BITS_MAX)
; @returns rax = 0, -EINVAL (bits out of range, or already enabled), or
;          -ENOMEM.
; @remarks Call once before any reader or writer runs (it is not
;          synchronized). Every replace or delete then bumps the counter of
;          the key's slot, (h1 >> 32) & mask, before retiring the old record.
;          Keys share slots, so a write also invalidates cached entries of
;          unrelated keys in the same slot: harmless (a miss), and rare with
;          enough bits. Costs one store per write; nothing when disabled.
; =============================================================================
kv_store_enable_inval:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     rax, -EINVAL
    cmp     qword [rbx + Table.inval], 0
    jne     .out
    test    esi, esi
    jz      .out
    cmp     esi, INVAL_BITS_MAX
    ja      .out
    mov     ecx, esi
    mov     r12d, 1
    shl     r12, cl                     ; counters
    lea     r13, [r12 * 8]              ; bytes
    cmp     r13, 64
    jae     .size_ok
    mov     r13d, 64                    ; aligned_alloc wants a multiple of 64
.size_ok:
    mov     edi, 64
    mov     rsi, r13
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     [rbx + Table.inval], rax
    lea     rcx, [r12 - 1]
    mov     [rbx + Table.inval_mask], rcx
    mov     rdi, rax
    xor     esi, esi
    mov     rdx, r13
    call    memset wrt ..plt            ; every key starts at generation 0
    xor     eax, eax
    jmp     .out
.nomem:
    mov     rax, -ENOMEM
.out:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; inval_bump — advance the invalidation generation of a key's slot.
; Internal; the caller holds the writer mutex.
;
; @param  rdi  Table*   @param rsi  h1 of the key just replaced or deleted
; @returns nothing. Clobbers rax, rcx.
; @remarks A plain increment suffices: writers are serialized, readers only
;          load the counter, and aligned 8-byte stores are atomic on x86-64.
;          Must run after the index update and before store_retire; under
;          x86-TSO a reader that sees the new generation therefore also sees
;          the new index state, and one that saw the old generation cannot
;          outlive the grace period that protects the old record (proof in
;          hcache.asm).
; =============================================================================
inval_bump:
    mov     rax, [rdi + Table.inval]
    test    rax, rax
    jz      .off                        ; caching disabled: nothing to do
    mov     rcx, rsi
    shr     rcx, 32
    and     rcx, [rdi + Table.inval_mask]
    inc     qword [rax + rcx*8]
.off:
    ret

; =============================================================================
; store_retire — move an unlinked record to the limbo list. Internal; the
; caller holds the writer mutex.
;
; @param  rdi  Table*   @param rsi  Rec* (already unlinked from the index)
; @returns nothing. Clobbers rax, rcx.
; @remarks Stamps the record with the current epoch, then advances the epoch
;          with XCHG — the full fence that orders the unlink before any later
;          scan of reader slots (see the file header).
; =============================================================================
store_retire:
    mov     rax, [rdi + Table.epoch]
    mov     qword [rsi + Rec.next], 0
    mov     [rsi + Rec.aux], rax
    mov     rcx, [rdi + Table.limbo_tail]
    test    rcx, rcx
    jz      .first
    mov     [rcx + Rec.next], rsi
    jmp     .linked
.first:
    mov     [rdi + Table.limbo_head], rsi
.linked:
    mov     [rdi + Table.limbo_tail], rsi
    inc     qword [rdi + Table.limbo_count]
    inc     rax
    xchg    [rdi + Table.epoch], rax
    ret

; =============================================================================
; store_reclaim_step — free retired records whose grace period ended.
; Internal; the caller holds the writer mutex.
;
; @param  rdi  Table*
; @param  esi  force: 1 = always rescan readers and free up to
;              RECLAIM_BATCH_FORCE records; 0 = rate-limited, RECLAIM_BATCH
; @returns nothing.
; @remarks min_epoch = min(epoch at scan time, observed epoch of every online
;          reader). Capping by the scan-time epoch matters: records retired
;          after the scan must wait for the next scan even if no reader is
;          online now. Bounded work: at most n_readers slot reads and a fixed
;          number of free() calls.
; =============================================================================
store_reclaim_step:
    push    rbx
    push    r12
    push    r13
    mov     rbx, rdi
    mov     r12d, esi
    mov     rax, [rbx + Table.limbo_head]
    test    rax, rax
    jz      .done
    ; Forced calls always rescan. Skipping the scan whenever the cached bound
    ; covers the head (the rate-limited shortcut below) would free only up to
    ; that stale bound and leave records retired after the last scan in limbo
    ; even with every reader offline — the documented contract is "always".
    test    r12d, r12d
    jnz     .scan
    mov     rcx, [rax + Rec.aux]
    cmp     rcx, [rbx + Table.min_epoch]
    jb      .free_batch                 ; the cached bound already covers the head
    cmp     qword [rbx + Table.limbo_count], SCAN_LIMBO_FORCE
    jae     .scan
    cmp     qword [rbx + Table.scan_budget], 0
    je      .scan
    dec     qword [rbx + Table.scan_budget]
    jmp     .done
.scan:
    mov     qword [rbx + Table.scan_budget], SCAN_EVERY
    mov     r8, [rbx + Table.epoch]     ; cap (see remarks)
    mov     r9, [rbx + Table.readers]
    mov     r10, [rbx + Table.n_readers]
.scan_next:
    test    r10, r10
    jz      .scan_done
    mov     r11, [r9 + QsbrSlot.observed]
    test    r11, r11
    jz      .scan_skip                  ; offline readers hold nothing
    cmp     r11, r8
    cmovb   r8, r11
.scan_skip:
    add     r9, QsbrSlot_size
    dec     r10
    jmp     .scan_next
.scan_done:
    mov     [rbx + Table.min_epoch], r8
.free_batch:
    mov     r13d, RECLAIM_BATCH
    test    r12d, r12d
    jz      .free_next
    mov     r13d, RECLAIM_BATCH_FORCE
.free_next:
    mov     rax, [rbx + Table.limbo_head]
    test    rax, rax
    jz      .emptied
    mov     rcx, [rax + Rec.aux]
    cmp     rcx, [rbx + Table.min_epoch]
    jae     .done                       ; FIFO: everything behind is newer
    test    r13d, r13d
    jz      .done
    dec     r13d
    mov     rcx, [rax + Rec.next]
    mov     [rbx + Table.limbo_head], rcx
    dec     qword [rbx + Table.limbo_count]
    mov     ecx, [rax + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    mov     edx, [rax + Rec.val_len]
    add     rcx, rdx
    add     rcx, Rec_size
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rdi, rax
    call    free wrt ..plt
    inc     qword [rbx + Table.st_freed]
    jmp     .free_next
.emptied:
    mov     qword [rbx + Table.limbo_tail], 0
.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
