; =============================================================================
; store.asm — the KvAsm key-value store: records, writer mutex, QSBR memory
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
;   * A batch of writes may share one acquisition (ADR 0011): the server's
;     workers allocate and fill each Put's record (kv_store_rec_alloc) and
;     prefetch every write's buckets and old record while parsing a read,
;     then apply them all between kv_store_write_begin and
;     kv_store_write_end with kv_store_put_locked / kv_store_delete_locked.
;     The mutex changes hands once per batch, and the index updates under it
;     hit warm cache lines instead of stalling every other writer on DRAM.
;     Retirement and reclamation leave the mutex entirely: the records a
;     hold unlinks go to the writer's own QSBR slot, are stamped with one
;     epoch increment after the unlock, and are freed by that writer from
;     its slot's own limbo (slot_reclaim) — in parallel with other writers.
;     The global limbo below remains for the per-write functions (snapshot
;     load, Upload's publish, tests).
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
;
; Streaming (Upload/Download) adds two mechanisms:
;   * Write-once records: kv_store_rec_alloc reserves and allocates a record
;     whose value the caller fills in place (an upload writes each chunk
;     straight into it), kv_store_publish makes it visible atomically like
;     a Put, and kv_store_rec_free abandons an unpublished one. The value is
;     copied once, from the network buffer into its final place.
;   * Pins: a Download reads one record across many event-loop passes, i.e.
;     across quiescent states, so QSBR alone could free it under the reader
;     once a writer replaces it. kv_store_pin records the record in a small
;     table (under the writer mutex); reclamation defers a pinned record
;     instead of freeing it, and the last kv_store_unpin frees it. Why a
;     table instead of a count in the record: a count would need a new
;     field (8 bytes on every record for a rare operation) and readers
;     writing shared record memory; with no pin held the table costs
;     reclamation one compare. Pinning is O(1) expected (hashing), and the
;     pin/unpin pair takes the writer mutex twice per download — noise next
;     to a transfer.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

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
extern kv_siphash64
extern kv_aes_available
extern kv_aeshash_setup
extern kv_aeshash128
extern kv_cuckoo_get
extern kv_cuckoo_upsert
extern kv_cuckoo_remove
extern kv_cuckoo_prefetch
extern kv_cuckoo_prefetch_records
extern kv_cuckoo_locate
extern kv_cuckoo_prefetch_records_at
extern kv_cuckoo_get_at
extern kv_mutex_lock
extern kv_mutex_unlock

; The key hash in force (kv_set_hash_mode). Readers only load it; on its own
; cache line so nothing written at run time ever shares it.
section .data
align 64
kv_hash_mode:   db KV_HASH_SIPHASH
                times 63 db 0

section .text

global kv_store_create
global kv_store_destroy
global kv_store_hash
global kv_set_hash_mode
global kv_hash_best_mode
global kv_hash_mode_get
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
global kv_store_locate
global kv_store_prefetch_records_at
global kv_store_get_at
global kv_store_enable_inval
global kv_store_rec_alloc
global kv_store_rec_free
global kv_store_publish
global kv_store_pin
global kv_store_unpin
global kv_store_write_begin
global kv_store_write_end
global kv_store_put_locked
global kv_store_delete_locked
global kv_store_reclaim_local
global kv_store_rec_alloc_local

%define PIN_HASH_MUL        0x9E3779B97F4A7C15  ; Fibonacci hashing of record pointers
%define PIN_MIN_SLOTS       16                  ; first pin table (grows by doubling)

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
;          huge pages requested: zeroed by the kernel, fewer TLB misses on
;          large tables. Every whole 2 MiB region is written once here (see
;          the prefault below), so with THP the table is RESIDENT FROM
;          STARTUP, whatever the key count: 64 bytes x nbuckets (16 MiB at
;          the default capacity of 1M, 2 GiB at 100M). Without THP only one
;          4 KiB page per region is touched now, the rest as keys arrive.
;          nbuckets = next power of two >= capacity/5, i.e. at most ~83% of
;          the 6-slot buckets are ever used, which keeps cuckoo paths short.
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

    ; QSBR reader slots (at least one, never read if 0): the shared observed
    ; line plus the owner's private reclamation lines (QsbrSlot, ADR 0011),
    ; zeroed = offline, empty limbo, nothing pending.
    mov     rax, r13
    test    rax, rax
    jnz     .have_readers
    mov     eax, 1
.have_readers:
    imul    rax, rax, QsbrSlot_size
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
    ; The AES hash's key block, whenever the CPU can run it — whatever hash is
    ; selected, so kv_set_hash_mode never meets a table without one. A CPU
    ; without AES-NI leaves hash_key 0 (calloc), and the mode cannot be AES
    ; there (kv_set_hash_mode refuses).
    call    kv_aes_available wrt ..plt
    test    eax, eax
    jz      .hash_ready
    mov     edi, 64
    mov     esi, AESKEY_BYTES           ; 448: a multiple of the alignment, as aligned_alloc wants
    call    aligned_alloc wrt ..plt
    test    rax, rax
    jz      .fail
    mov     [rbx + Table.hash_key], rax
    mov     rdi, r14                    ; the seed
    mov     rsi, rax
    call    kv_aeshash_setup wrt ..plt
.hash_ready:
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
    push    rbp                         ; walks the record-cache classes
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; 6 pushes + 8: rsp 16-aligned for calls
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
    jz      .slots
    mov     r13, [r12 + Rec.next]
    mov     rdi, r12
    call    free wrt ..plt
    mov     r12, r13
    jmp     .limbo_next
.slots:
    ; Each slot's own limbo (ADR 0011): records its owner's batched writes
    ; retired and the owner had not freed yet. Every owner is gone by now
    ; (main destroys the store after joining the workers), so nothing else
    ; touches these lists. A partly created store may have no slots.
    mov     r14, [rbx + Table.readers]
    test    r14, r14
    jz      .pins
    mov     r15, [rbx + Table.n_readers]
.slot_next:
    test    r15, r15
    jz      .pins
    mov     r12, [r14 + QsbrSlot.limbo_head]
.slot_rec:
    test    r12, r12
    jz      .slot_cache
    mov     r13, [r12 + Rec.next]
    mov     rdi, r12
    call    free wrt ..plt
    mov     r12, r13
    jmp     .slot_rec
.slot_cache:
    ; Then the slot's record cache: freed records kept for reuse, one list
    ; per class through Rec.next; rbp walks the classes.
    xor     ebp, ebp
.cache_class:
    cmp     ebp, REC_CLASSES
    jae     .slot_done
    mov     r12, [r14 + QsbrSlot.cache_head + rbp*8]
.cache_rec:
    test    r12, r12
    jz      .cache_next
    mov     r13, [r12 + Rec.next]
    mov     rdi, r12
    call    free wrt ..plt
    mov     r12, r13
    jmp     .cache_rec
.cache_next:
    inc     ebp
    jmp     .cache_class
.slot_done:
    add     r14, QsbrSlot_size
    dec     r15
    jmp     .slot_next
.pins:
    ; A record whose grace period ended while pinned is in neither the index
    ; nor any limbo: its deferred pin entry is its only owner. (A pinned
    ; record still live or still in a limbo was freed above, and its entry is
    ; not deferred, so nothing is freed twice.) Normally empty: main destroys
    ; the store after every worker — and so every Download — is gone.
    mov     r12, [rbx + Table.pins]
    test    r12, r12
    jz      .scratch
    mov     r13, [rbx + Table.pin_mask]
    inc     r13                         ; slots
.pin_next:
    cmp     qword [r12 + PinEnt.rec], 0
    je      .pin_skip
    cmp     qword [r12 + PinEnt.deferred], 0
    je      .pin_skip
    mov     rdi, [r12 + PinEnt.rec]
    call    free wrt ..plt
.pin_skip:
    add     r12, PinEnt_size
    dec     r13
    jnz     .pin_next
    mov     rdi, [rbx + Table.pins]
    call    free wrt ..plt
.scratch:
    mov     rdi, [rbx + Table.inval]
    call    free wrt ..plt              ; 0 when caching was never enabled
    mov     rdi, [rbx + Table.hash_key]
    call    free wrt ..plt              ; 0 on a CPU without AES-NI
    mov     rdi, [rbx + Table.readers]
    call    free wrt ..plt              ; free(NULL) is a no-op
    mov     rdi, [rbx + Table.bfs]
    call    free wrt ..plt
    mov     rdi, [rbx + Table.visit]
    call    free wrt ..plt
    mov     rdi, rbx
    call    free wrt ..plt
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
.none:
    ret

; =============================================================================
; kv_set_hash_mode — choose the keyed hash that places keys.
;
; @param  edi  KV_HASH_SIPHASH (0) or KV_HASH_AES (1)
; @returns eax = the previous mode, or -EINVAL (mode unchanged) for any other
;          value — and for KV_HASH_AES on a CPU without AES-NI, where the
;          hash's first instruction would raise #UD.
; @remarks Process-wide, like kv_set_scan_mode. Footgun: a key hashes to a
;          different bucket under each mode, so the mode must be set before
;          the first key of ANY table is hashed and never again while a
;          table holds keys — lookups of keys stored under the other mode
;          would miss. Not synchronized: startup, or a test between tables.
;          Both modes are keyed by the table's seed and nobody outside the
;          process can predict either; only speed differs (ADR 0004).
; =============================================================================
kv_set_hash_mode:
    cmp     edi, KV_HASH_MAX
    ja      .bad
    cmp     edi, KV_HASH_AES
    jne     .set
    push    rdi                         ; one push: aligned for the call
    call    kv_aes_available wrt ..plt
    pop     rdi
    test    eax, eax
    jz      .bad
.set:
    movzx   eax, byte [rel kv_hash_mode]
    mov     [rel kv_hash_mode], dil
    ret
.bad:
    mov     eax, -EINVAL
    ret

; =============================================================================
; kv_hash_best_mode — the fastest key hash this CPU runs.
; @returns eax = KV_HASH_AES when kv_aes_available(), else KV_HASH_SIPHASH.
; @remarks What KVASM_HASH=auto selects (main.asm). Measured per 16-byte key:
;          7.6 core cycles for AES when hashes overlap against 50.0 for
;          SipHash-2-4 (benchmarks/results/2026-11-15-asm-audit).
; =============================================================================
kv_hash_best_mode:
    sub     rsp, 8                      ; aligned for the call
    call    kv_aes_available wrt ..plt  ; 1 = KV_HASH_AES, 0 = KV_HASH_SIPHASH
    add     rsp, 8
    ret

; =============================================================================
; kv_hash_mode_get — the key hash in force.
; @returns eax = KV_HASH_SIPHASH or KV_HASH_AES.
; =============================================================================
kv_hash_mode_get:
    movzx   eax, byte [rel kv_hash_mode]
    ret

; =============================================================================
; kv_store_hash — the store's keyed hash of a key.
;
; @param  rdi  Table*   @param rsi  key   @param rdx  len
; @param  rcx  uint64_t out[2] (may be NULL)
; @returns rax = h1, rdx = h2 (also written to out when non-NULL). The bucket
;          index comes from h1's low bits and the tag from h2's top byte.
;          Under KV_HASH_AES they are the two words of one AES-128 result;
;          under KV_HASH_SIPHASH h2 == h1, one 64-bit SipHash-2-4 word (its
;          low bits and its top byte are disjoint; kv_siphash64).
; @remarks Callers hash once and pass (h1, h2) to get/put/delete, so the
;          per-process secret never leaves the Table. The mode is a
;          perfectly predicted branch on a byte nobody writes after startup.
;          The AES path clobbers xmm0-xmm2 (see kv_aeshash128).
; =============================================================================
kv_store_hash:
    cmp     byte [rel kv_hash_mode], KV_HASH_SIPHASH
    jne     .aes
    lea     r8, [rdi + Table.seed0]
    mov     rdi, rsi
    mov     rsi, rdx
    mov     rdx, r8
    jmp     kv_siphash64 wrt ..plt
.aes:
    mov     r8, [rdi + Table.hash_key]
    mov     rdi, rsi
    mov     rsi, rdx
    mov     rdx, r8
    jmp     kv_aeshash128 wrt ..plt

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
; kv_store_locate — kv_store_prefetch that returns the two bucket addresses.
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns rax = bucket 1, rdx = bucket 2: what kv_store_prefetch_records_at
;          and kv_store_get_at take, so a caller that visits a key three
;          times (the batched request path) derives them once. Valid for the
;          table's life. See kv_cuckoo_locate.
; =============================================================================
kv_store_locate:
    jmp     kv_cuckoo_locate wrt ..plt

; =============================================================================
; kv_store_prefetch_records_at — kv_store_prefetch_records for located buckets.
; @param  rdi  bucket 1   @param rsi  bucket 2 (kv_store_locate)   @param rdx  h2
; @returns nothing. A hint, like kv_store_prefetch_records.
; =============================================================================
kv_store_prefetch_records_at:
    jmp     kv_cuckoo_prefetch_records_at wrt ..plt

; =============================================================================
; kv_store_get_at — kv_store_get for located buckets.
; @param  rdi  bucket 1   @param rsi  bucket 2 (kv_store_locate, same key)
; @param  rdx  key   @param rcx  len   @param r8  h2
; @returns rax = Rec*, or 0: exactly kv_store_get's answer and lifetime rules
;          (the caller is online in QSBR; the record is valid until its next
;          quiescent or offline announcement).
; @remarks Footgun: the buckets are trusted. Pass only what kv_store_locate
;          returned for this key's (h1, h2) in this table.
; =============================================================================
kv_store_get_at:
    jmp     kv_cuckoo_get_at wrt ..plt

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
; kv_store_reclaim — maintenance: free every record in the GLOBAL limbo whose
; grace period has ended.
;
; @param  rdi  Table*
; @returns rax = number of records still waiting in the global limbo
;          afterwards.
; @remarks Writes normally drain limbo incrementally; call this periodically
;          (e.g. from a worker timer) so memory is returned even when writes
;          stop. Takes the writer mutex. It does not touch the slots' own
;          limbos (ADR 0011): those are their owners' unsynchronized state,
;          reclaimed by kv_store_write_end and kv_store_reclaim_local.
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
    mov     rax, [rdi + Table.pin_used]
    mov     [rsi + KvStats.pinned], rax
    mov     rax, [rdi + Table.pin_deferred]
    mov     [rsi + KvStats.deferred], rax
    ; Since ADR 0011 most retired records wait in their writer's own slot,
    ; and that slot frees them: limbo and freed add every slot's counters
    ; (racy reads of owner state, as approximate as the rest of this set).
    mov     r8, [rdi + Table.readers]
    test    r8, r8
    jz      .out                        ; a partly created store
    mov     r9, [rdi + Table.n_readers]
.slot:
    test    r9, r9
    jz      .out
    mov     rax, [r8 + QsbrSlot.limbo_count]
    add     [rsi + KvStats.limbo_count], rax
    mov     rax, [r8 + QsbrSlot.freed]
    add     [rsi + KvStats.freed], rax
    add     r8, QsbrSlot_size
    dec     r9
    jmp     .slot
.out:
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
    imul    rsi, rsi, QsbrSlot_size     ; slot address (QsbrSlot is 640 bytes)
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
    imul    rsi, rsi, QsbrSlot_size     ; slot address (QsbrSlot is 640 bytes)
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
    imul    rsi, rsi, QsbrSlot_size     ; slot address (QsbrSlot is 640 bytes)
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
; @remarks Advances the epoch E -> E+1 with LOCK XADD and stamps the record
;          with E: the locked instruction is the full fence that orders the
;          unlink before any later scan of reader slots (see the file header).
;          Why an atomic increment and not load + XCHG: since ADR 0011 the
;          writers' slots also advance the epoch outside the mutex
;          (kv_store_write_end), and a load + store pair could then move the
;          epoch backwards between them.
; =============================================================================
store_retire:
    mov     qword [rsi + Rec.next], 0
    mov     eax, 1
    lock xadd [rdi + Table.epoch], rax  ; rax = E, the epoch before the increment
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
    dec     r13d                        ; bounded work: a deferral counts like a free
    mov     rcx, [rax + Rec.next]
    mov     [rbx + Table.limbo_head], rcx
    dec     qword [rbx + Table.limbo_count]
    ; A pinned record's grace period has ended, but a Download still reads it
    ; across quiescent states: its pin entry takes it over (deferred) and the
    ; last kv_store_unpin frees it. Its bytes stay counted in mem_used until
    ; then. With no pin held anywhere this is one compare.
    cmp     qword [rbx + Table.pin_used], 0
    jne     .check_pin
.free_it:
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
.check_pin:
    mov     r10, rax                    ; the record (pin_find keeps r10/r11)
    mov     rdi, rbx
    mov     rsi, rax
    call    pin_find
    test    rax, rax
    jz      .not_pinned
    mov     qword [rax + PinEnt.deferred], 1
    inc     qword [rbx + Table.pin_deferred]
    jmp     .free_next
.not_pinned:
    mov     rax, r10
    jmp     .free_it
.emptied:
    mov     qword [rbx + Table.limbo_tail], 0
.done:
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_rec_alloc — reserve and allocate a record whose value the caller
; fills in place, to be made visible later by kv_store_publish (Upload).
;
; @param  rdi  Table*   @param rsi  key   @param rdx  key len
; @param  rcx  value len (the value area is left uninitialized)
; @returns rax = Rec* with header and key written — the value area is at
;          data + align8(key_len), val_len bytes — or -E2BIG if a length
;          exceeds 2^31-1, or -ENOMEM if the memory limit would be exceeded
;          or allocation failed.
; @remarks Why: a streamed value arrives chunk by chunk; writing each chunk
;          straight into its final record copies it once, where kv_store_put
;          would need the whole value contiguous first. The bytes count
;          against KVASM_MAX_MEMORY_MB from this call on — so a client
;          cannot declare more than the store may hold and then hold it — and
;          the record belongs to the caller until kv_store_publish succeeds:
;          every other outcome MUST end in kv_store_rec_free, or the memory
;          reservation leaks. Readers cannot see it before it is published.
;          Lock-free (the reservation is a LOCK XADD, like kv_store_put's).
; =============================================================================
kv_store_rec_alloc:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 16                     ; [0] record size; 5 pushes + 16: aligned
    mov     rbx, rdi
    mov     r12, rsi                    ; key
    mov     r13, rdx                    ; key len
    mov     r14, rcx                    ; value len
    mov     eax, MAX_FIELD_LEN
    cmp     r13, rax
    ja      .e2big
    cmp     r14, rax
    ja      .e2big
    lea     r15, [r13 + 7]
    and     r15, -8                     ; key area, padded to 8
    lea     rax, [r15 + r14 + Rec_size]
    mov     [rsp], rax
    ; Reserve first, exactly as kv_store_put does, so concurrent writers and
    ; uploads cannot overshoot the limit between check and allocation.
    mov     rcx, rax
    lock xadd [rbx + Table.mem_used], rcx
    add     rcx, rax
    cmp     rcx, [rbx + Table.mem_limit]
    ja      .nomem_release
    mov     rdi, rax
    call    malloc wrt ..plt
    test    rax, rax
    jz      .nomem_release
    mov     qword [rax + Rec.next], 0
    mov     qword [rax + Rec.aux], 0
    mov     [rax + Rec.key_len], r13d
    mov     [rax + Rec.val_len], r14d
    test    r13, r13
    jz      .out
    ; Zero the last word of the padded key area before copying the key, so
    ; padding bytes are deterministic (never stale heap contents).
    mov     qword [rax + Rec.data + r15 - 8], 0
    mov     r14, rax                    ; (the value length is stored already)
    lea     rdi, [rax + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    mov     rax, r14
    jmp     .out
.nomem_release:
    mov     rcx, [rsp]
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rax, -ENOMEM
    jmp     .out
.e2big:
    mov     rax, -E2BIG
.out:
    add     rsp, 16
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_rec_alloc_local — kv_store_rec_alloc for a worker, served from
; its slot's record cache when it can be (ADR 0011).
;
; @param  rdi  Table*   @param esi  the caller's own QSBR slot id
; @param  rdx  key      @param rcx  key len
; @param  r8   value len (the value area is left uninitialized)
; @returns rax = Rec* with header and key written (the value area is at
;          data + align8(key_len)), or -E2BIG / -ENOMEM: kv_store_rec_alloc's
;          contract, and the record is published, abandoned (kv_store_rec_free)
;          or later freed exactly like one of its records.
; @remarks A record of up to 1,032 bytes comes from the slot's cache of
;          records its own reclamation freed — LIFO, so the most recently
;          freed and warmest one — or else from malloc asked for its class's
;          whole chunk, so a later request of the same class fits the block
;          whoever ends up caching it. Bigger records are malloc'd exactly.
;          Only the slot's owner may call this: the cache is unsynchronized.
;          The memory reservation is kv_store_rec_alloc's LOCK XADD; a cached
;          block is not counted while it waits, like free memory.
; =============================================================================
kv_store_rec_alloc_local:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [0] record size; 6 pushes + 8: aligned
    mov     rbx, rdi                    ; Table*
    mov     ebp, esi                    ; slot id
    mov     r12, rdx                    ; key
    mov     r13, rcx                    ; key len
    mov     r14, r8                     ; value len
    mov     eax, MAX_FIELD_LEN
    cmp     r13, rax
    ja      .e2big
    cmp     r14, rax
    ja      .e2big
    lea     r15, [r13 + 7]
    and     r15, -8                     ; key area, padded to 8
    lea     rax, [r15 + r14 + Rec_size]
    mov     [rsp], rax
    ; Reserve first, as kv_store_rec_alloc does, so concurrent writers cannot
    ; overshoot the limit between check and allocation.
    mov     rcx, rax
    lock xadd [rbx + Table.mem_used], rcx
    add     rcx, rax
    cmp     rcx, [rbx + Table.mem_limit]
    ja      .nomem_release
    mov     rdi, [rsp]
    call    rec_class                   ; rax = class or -1, rdx = chunk bytes
    test    rax, rax
    js      .malloc_exact               ; above the cached classes
    mov     r8, rax                     ; class
    mov     r9, rdx                     ; chunk
    mov     rdi, rbx
    mov     esi, ebp
    call    slot_of                     ; rax = QsbrSlot* (clobbers only rax)
    mov     rcx, [rax + QsbrSlot.cache_head + r8*8]
    test    rcx, rcx
    jz      .malloc_class
    mov     rdx, [rcx + Rec.next]       ; pop the warmest
    mov     [rax + QsbrSlot.cache_head + r8*8], rdx
    dec     dword [rax + QsbrSlot.cache_count + r8*4]
    sub     [rax + QsbrSlot.cache_bytes], r9
    mov     rax, rcx
    jmp     .fill
.malloc_class:
    lea     rdi, [r9 - 8]               ; the class's whole chunk, glibc's usable size
    call    malloc wrt ..plt
    test    rax, rax
    jz      .nomem_release
    jmp     .fill
.malloc_exact:
    mov     rdi, [rsp]
    call    malloc wrt ..plt
    test    rax, rax
    jz      .nomem_release
.fill:
    mov     qword [rax + Rec.next], 0
    mov     qword [rax + Rec.aux], 0
    mov     [rax + Rec.key_len], r13d
    mov     [rax + Rec.val_len], r14d
    test    r13, r13
    jz      .out
    ; Zero the last word of the padded key area before copying the key, so
    ; padding bytes are deterministic (never stale heap or cached contents).
    mov     qword [rax + Rec.data + r15 - 8], 0
    mov     r14, rax                    ; (the value length is stored already)
    lea     rdi, [rax + Rec.data]
    mov     rsi, r12
    mov     rdx, r13
    call    memcpy wrt ..plt
    mov     rax, r14
    jmp     .out
.nomem_release:
    mov     rcx, [rsp]
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rax, -ENOMEM
    jmp     .out
.e2big:
    mov     rax, -E2BIG
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
; rec_class — the record-cache class of an allocation size. Internal, leaf.
; @param  rdi  bytes requested (Rec header + padded key + value)
; @returns rax = class (0 .. REC_CLASSES-1), or -1 when the size is above the
;          cached classes; rdx = glibc's chunk size for the request.
; @remarks glibc rounds every request to a chunk of max(32, align16(n + 8))
;          bytes and its usable size is the chunk minus 8, so two requests
;          with the same chunk size fit each other's blocks: the class is the
;          chunk size. This binary links glibc statically, so the rounding is
;          fixed with it; blocks this store asks for itself are requested at
;          the full chunk anyway (kv_store_rec_alloc_local).
; =============================================================================
rec_class:
    lea     rdx, [rdi + 8 + 15]
    and     rdx, -16
    mov     eax, 32
    cmp     rdx, rax
    cmovb   rdx, rax                    ; glibc's minimum chunk
    mov     rax, rdx
    shr     rax, 4
    sub     rax, 2                      ; 32 -> 0, 48 -> 1, ..., 1040 -> 63
    cmp     rax, REC_CLASSES
    jb      .ok
    mov     rax, -1
.ok:
    ret

; =============================================================================
; slot_cache_put — keep a freed record in the slot's record cache, or free
; it. Internal; the caller owns the slot, and the record is past its grace
; period (no reader can reach it).
; @param  rdi  QsbrSlot*   @param rsi  Rec*
; @param  rdx  its size (Rec header + padded key + value)
; @returns nothing. Clobbers the caller-saved registers.
; @remarks Kept only within the caps (REC_CACHE_PER_CLASS records per class,
;          REC_CACHE_BYTES per slot), so a worker that frees much more than
;          it allocates of some size hands the excess back to glibc instead
;          of hoarding it. The link goes into Rec.next of the dead record.
; =============================================================================
slot_cache_put:
    push    rbx
    push    r12
    sub     rsp, 8                      ; 2 pushes + 8: rsp 16-aligned for the call
    mov     rbx, rdi
    mov     r12, rsi
    mov     rdi, rdx
    call    rec_class                   ; rax = class or -1, rdx = chunk bytes
    test    rax, rax
    js      .free
    cmp     dword [rbx + QsbrSlot.cache_count + rax*4], REC_CACHE_PER_CLASS
    jae     .free
    mov     rcx, [rbx + QsbrSlot.cache_bytes]
    add     rcx, rdx
    cmp     rcx, REC_CACHE_BYTES
    ja      .free
    mov     [rbx + QsbrSlot.cache_bytes], rcx
    mov     rcx, [rbx + QsbrSlot.cache_head + rax*8]
    mov     [r12 + Rec.next], rcx
    mov     [rbx + QsbrSlot.cache_head + rax*8], r12
    inc     dword [rbx + QsbrSlot.cache_count + rax*4]
    jmp     .out
.free:
    mov     rdi, r12
    call    free wrt ..plt
.out:
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_rec_free — abandon a record from kv_store_rec_alloc that was never
; published (a failed, cancelled or interrupted upload).
;
; @param  rdi  Table*   @param rsi  Rec* (NULL is a no-op)
; @returns nothing.
; @remarks Frees it at once and returns its bytes to the memory reservation:
;          no grace period is needed because no reader ever saw it. Never
;          call it for a published record (that one belongs to the store now
;          and is retired by the next replace or delete). Lock-free.
; =============================================================================
kv_store_rec_free:
    test    rsi, rsi
    jz      .none
    push    rbx
    push    r12                         ; 2 pushes + 8: aligned for the call
    sub     rsp, 8
    mov     rbx, rdi
    mov     ecx, [rsi + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    mov     edx, [rsi + Rec.val_len]
    add     rcx, rdx
    add     rcx, Rec_size
    mov     r12, rcx                    ; size, needed after free
    mov     rdi, rsi
    call    free wrt ..plt
    neg     r12
    lock xadd [rbx + Table.mem_used], r12
    add     rsp, 8
    pop     r12
    pop     rbx
.none:
    ret

; =============================================================================
; kv_store_publish — make a filled record from kv_store_rec_alloc visible:
; insert it, or atomically replace the key's current record.
;
; @param  rdi  Table*   @param rsi  Rec* (fully written)
; @param  rdx  h1       @param rcx  h2 (kv_store_hash of the record's key)
; @returns rax = 1 if the key was created, 0 if an existing value was
;          replaced, or -ENOSPC if the key is new and the store is at
;          capacity — in which case the record is still the caller's (free
;          it with kv_store_rec_free).
; @remarks Exactly kv_store_put's locked section: one pointer store publishes
;          the record, so readers see the old or the new value, never a mix;
;          the replaced record is retired (freed after a grace period, or
;          after its last unpin). On success the store owns the record.
; =============================================================================
kv_store_publish:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; [0] result; 4 pushes + 8: aligned
    mov     rbx, rdi
    mov     r12, rsi                    ; record
    mov     r13, rdx                    ; h1
    mov     r14, rcx                    ; h2
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, r13
    mov     rcx, r14
    call    kv_cuckoo_upsert wrt ..plt
    test    rax, rax
    js      .refused                    ; -ENOSPC
    jz      .created
    ; Replaced: same order as kv_store_put (see hcache.asm) — the new pointer
    ; is published, the key's generation bumped, only then the old record
    ; retired.
    mov     r12, rax                    ; old record (the new one is the store's now)
    mov     rdi, rbx
    mov     rsi, r13                    ; h1
    call    inval_bump
    mov     rdi, rbx
    mov     rsi, r12
    call    store_retire
    mov     qword [rsp], 0
    jmp     .reclaim
.created:
    mov     qword [rsp], 1
    jmp     .reclaim
.refused:
    mov     [rsp], rax
.reclaim:
    mov     rdi, rbx
    xor     esi, esi
    call    store_reclaim_step
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    mov     rax, [rsp]
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_write_begin — take the writer mutex for a batch of writes.
;
; @param  rdi  Table*
; @returns nothing; returns once the caller holds the writer mutex.
; @remarks The batched write path (grpc.asm kv_grpc_run_batch, ADR 0011): a
;          worker applies every Put and Delete of one read under ONE
;          acquisition with kv_store_put_locked / kv_store_delete_locked,
;          then calls kv_store_write_end. Why: per-write locking moves the
;          mutex's cache line between cores on every write and serializes
;          each write's cache misses behind it (a store probe fell from 4.51M
;          puts/s with one writer to 2.07M with four, golden-services P3).
;          Only index updates happen under the mutex: the records a hold
;          replaces are handed to the caller's QSBR slot and retired and
;          freed by kv_store_write_end after the mutex is released. Hold it
;          briefly — every other writer waits meanwhile — and for at most
;          SLOT_PENDING_MAX writes (more still work, but the excess is retired
;          the old way, under the mutex). Not recursive. Clobbers the
;          caller-saved registers.
; =============================================================================
kv_store_write_begin:
    lea     rdi, [rdi + Table.wlock]
    jmp     kv_mutex_lock wrt ..plt

; =============================================================================
; slot_of — the address of QSBR slot `rid`. Internal, leaf.
; @param  rdi  Table*   @param esi  slot id (< n_readers)
; @returns rax = QsbrSlot*. Clobbers nothing else.
; =============================================================================
slot_of:
    mov     eax, esi
    imul    rax, rax, QsbrSlot_size
    add     rax, [rdi + Table.readers]
    ret

; =============================================================================
; slot_pend — hand a record the current hold unlinked to the caller's slot,
; to be retired once the hold ends. Internal; the caller holds the writer
; mutex.
; @param  rdi  Table*   @param esi  slot id   @param rdx  Rec* (unlinked)
; @returns nothing. Clobbers rax, rcx, rsi.
; @remarks The record must not be freed before kv_store_write_end retires it
;          and its grace period passes: readers may still hold it. A full
;          pending list (a hold of more than SLOT_PENDING_MAX writes) retires
;          the record into the global limbo instead, under the mutex as before
;          ADR 0011 — correct, merely slower.
; =============================================================================
slot_pend:
    call    slot_of
    mov     rcx, [rax + QsbrSlot.pending_n]
    cmp     rcx, SLOT_PENDING_MAX
    jae     .global
    mov     [rax + QsbrSlot.pending + rcx*8], rdx
    inc     rcx
    mov     [rax + QsbrSlot.pending_n], rcx
    ret
.global:
    mov     rsi, rdx
    jmp     store_retire                ; rdi = Table*; clobbers rax, rcx

; =============================================================================
; kv_store_put_locked — insert or replace; the caller holds the writer mutex
; (kv_store_write_begin).
;
; @param  rdi  Table*
; @param  esi  the caller's QSBR slot id (the same for the whole hold)
; @param  rdx  Rec* from kv_store_rec_alloc, key and value fully written
; @param  rcx  h1   @param r8  h2 (kv_store_hash of the record's key)
; @returns rax = 1 if the key was created, 0 if an existing value was
;          replaced — the store owns the record in both cases — or -ENOSPC
;          if the key is new and the store is at capacity: the record is then
;          still the caller's, to free with kv_store_rec_free.
; @remarks Same order as kv_store_publish (hcache.asm's proof needs it): the
;          index update publishes the record with one pointer store, then the
;          key's invalidation generation is bumped, and only after that can
;          the replaced record ever be freed — it goes to the slot's pending
;          list now and is retired by kv_store_write_end. Prefetch the key's
;          buckets and old record first (kv_store_prefetch, then
;          kv_store_prefetch_records), or this stalls on them while every
;          other writer waits.
; =============================================================================
kv_store_put_locked:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; 4 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12d, esi                   ; slot id
    mov     r13, rcx                    ; h1, for the invalidation bump
    mov     rsi, rdx                    ; kv_cuckoo_upsert(Table*, rec, h1, h2)
    mov     rdx, rcx
    mov     rcx, r8
    call    kv_cuckoo_upsert wrt ..plt
    test    rax, rax
    js      .out                        ; -ENOSPC: the record stays the caller's
    jz      .created
    mov     r14, rax                    ; replaced: the old record
    mov     rdi, rbx
    mov     rsi, r13
    call    inval_bump
    mov     rdi, rbx
    mov     esi, r12d
    mov     rdx, r14
    call    slot_pend                   ; retired when the hold ends
    xor     eax, eax
    jmp     .out
.created:
    mov     eax, 1
.out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_delete_locked — remove a key; the caller holds the writer mutex
; (kv_store_write_begin).
;
; @param  rdi  Table*   @param esi  the caller's QSBR slot id
; @param  rdx  key      @param rcx  key len
; @param  r8   h1       @param r9   h2 (kv_store_hash of the key)
; @returns rax = 1 if the key existed and was removed, 0 if it was absent.
; @remarks kv_store_delete without the mutex and the reclamation. The
;          unlinked record goes to the slot's pending list (see
;          kv_store_put_locked), never freed here: readers that found it may
;          still be copying its value.
; =============================================================================
kv_store_delete_locked:
    push    rbx
    push    r12
    push    r13
    push    r14
    sub     rsp, 8                      ; 4 pushes + 8: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12d, esi                   ; slot id
    mov     r13, r8                     ; h1, for the invalidation bump
    mov     rsi, rdx                    ; kv_cuckoo_remove(Table*, key, len, h1, h2)
    mov     rdx, rcx
    mov     rcx, r8
    mov     r8, r9
    call    kv_cuckoo_remove wrt ..plt
    test    rax, rax
    jz      .out                        ; absent: 0
    mov     r14, rax                    ; the unlinked record
    mov     rdi, rbx
    mov     rsi, r13
    call    inval_bump
    mov     rdi, rbx
    mov     esi, r12d
    mov     rdx, r14
    call    slot_pend
    mov     eax, 1
.out:
    add     rsp, 8
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_write_end — release the writer mutex, then retire the hold's
; unlinked records into the caller's slot and free what is due there.
;
; @param  rdi  Table*
; @param  esi  the caller's QSBR slot id (as passed to this hold's *_locked
;              calls)
; @returns nothing; the mutex is released.
; @remarks Everything after the unlock runs in parallel with other writers:
;          * ONE LOCK XADD advances the epoch E -> E+1 for the whole hold, and
;            every record the hold unlinked is stamped E and appended to the
;            slot's own limbo. The unlinks happened before the unlock and the
;            locked increment is a full fence after them, so a reader that
;            loads an epoch > E loads it after the unlinks and cannot reach
;            any of them — the file header's QSBR argument, with the stamp
;            taken after the unlinks instead of under the mutex. (Per-write
;            retirement advanced the epoch once per record, and every
;            advance invalidated the epoch line every worker reads at each
;            quiescent announcement.)
;          * slot_reclaim then frees up to 2n + RECLAIM_BATCH of the slot's
;            records whose grace period ended: twice the hold's own n, so a
;            backlog drains while the steady state keeps pace.
; =============================================================================
kv_store_write_end:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r12d, esi
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    mov     rdi, rbx
    mov     esi, r12d
    call    slot_of
    mov     r13, rax                    ; QsbrSlot*, owned by this thread
    mov     rcx, [r13 + QsbrSlot.pending_n]
    test    rcx, rcx
    jz      .reclaim                    ; nothing replaced (inserts, misses)
    mov     eax, 1
    lock xadd [rbx + Table.epoch], rax  ; rax = E: the stamp for the whole hold
    xor     edx, edx                    ; i
    mov     r8, [r13 + QsbrSlot.limbo_tail]
.append:
    mov     rsi, [r13 + QsbrSlot.pending + rdx*8]
    mov     qword [rsi + Rec.next], 0   ; the record's line is warm: the hold
    mov     [rsi + Rec.aux], rax        ; just compared its key
    test    r8, r8
    jz      .first
    mov     [r8 + Rec.next], rsi
    jmp     .linked
.first:
    mov     [r13 + QsbrSlot.limbo_head], rsi
.linked:
    mov     r8, rsi
    inc     rdx
    cmp     rdx, rcx
    jb      .append
    mov     [r13 + QsbrSlot.limbo_tail], r8
    add     [r13 + QsbrSlot.limbo_count], rcx
    mov     qword [r13 + QsbrSlot.pending_n], 0
.reclaim:
    lea     rdx, [rcx*2 + RECLAIM_BATCH]
    mov     rdi, rbx
    mov     rsi, r13
    xor     ecx, ecx                    ; rate-limited: this runs after every hold
    call    slot_reclaim
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_reclaim_local — free records of one slot's limbo whose grace
; period ended (ADR 0011).
;
; @param  rdi  Table*   @param esi  slot id   @param rdx  budget: the most
;              records to free (or hand to a pin) in this call
; @returns rax = records still waiting in the slot's limbo.
; @remarks Only the slot's owner may call this — or anyone once the owner is
;          gone (tests drain finished writers' slots this way): the slot's
;          limbo is unsynchronized owner state. Workers call it from their
;          housekeeping tick, so the records of a worker that stopped writing
;          come back within a tick; kv_store_write_end runs the same code
;          after every hold. Unlike that call, this one always rescans the
;          readers (forced, as kv_store_reclaim is): the rate limit counts
;          calls, and at one tick per second it would hold an idle worker's
;          last records for SCAN_EVERY seconds. kv_store_reclaim covers only
;          the global limbo.
; =============================================================================
kv_store_reclaim_local:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: rsp 16-aligned for calls
    mov     rbx, rdi
    mov     r13, rdx
    call    slot_of
    mov     r12, rax
    mov     rdi, rbx
    mov     rsi, r12
    mov     rdx, r13
    mov     ecx, 1                      ; forced: always rescan the readers
    call    slot_reclaim
    mov     rax, [r12 + QsbrSlot.limbo_count]
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; slot_reclaim — free records of one slot's limbo whose grace period ended.
; Internal; the caller owns the slot (or the owner is gone).
;
; @param  rdi  Table*   @param rsi  QsbrSlot*
; @param  rdx  budget: the most records to free or defer in this call
; @param  ecx  force: 1 = rescan the readers whenever the cached bound does
;              not cover the head; 0 = rate-limited (the per-hold call)
; @returns rax = records freed.
; @remarks The bound is the slot's own: min(epoch at scan time, observed
;          epoch of every online reader), rescanned when the head's stamp is
;          not below the cached bound — when forced, else at most every
;          SCAN_EVERY calls unless SCAN_LIMBO_FORCE records wait. The cap by the scan-time epoch
;          matters as in store_reclaim_step: records stamped after the scan
;          wait for the next one even with no reader online.
;          Frees happen OUTSIDE the writer mutex (workers reclaim in
;          parallel), one record ahead prefetched so the next stamp check does
;          not stall after free() returns; the bytes come back to mem_used
;          with one LOCK XADD per call.
;          Pins: a record a Download pinned must be deferred, not freed, and
;          the pin table is writer-mutex state. pin_used is read AFTER the
;          reader scan: a Download pins under the mutex before its worker's
;          next quiescent announcement, and x86-TSO makes that worker's
;          stores visible in program order, so once the scan has seen the
;          announcement that ended a record's grace period, that pin is
;          visible too (and a record past its grace period can no longer be
;          found, hence never newly pinned). With no pin anywhere — the
;          common case — records are freed without the mutex; otherwise this
;          call takes the mutex and checks each one, as store_reclaim_step
;          does. Bounded: budget records plus one O(n_readers) scan.
; =============================================================================
slot_reclaim:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8                      ; [0] next record (pinned path); aligned
    mov     rbx, rdi                    ; Table*
    mov     r12, rsi                    ; QsbrSlot*
    mov     r13, rdx                    ; budget
    xor     r14d, r14d                  ; bytes freed
    xor     r15d, r15d                  ; records freed
    mov     rax, [r12 + QsbrSlot.limbo_head]
    test    rax, rax
    jz      .done
    mov     rdx, [rax + Rec.aux]
    cmp     rdx, [r12 + QsbrSlot.min_epoch]
    jb      .bound_ok                   ; the cached bound already covers the head
    test    ecx, ecx
    jnz     .scan                       ; forced (the housekeeping tick)
    cmp     qword [r12 + QsbrSlot.limbo_count], SCAN_LIMBO_FORCE
    jae     .scan
    cmp     qword [r12 + QsbrSlot.scan_budget], 0
    je      .scan
    dec     qword [r12 + QsbrSlot.scan_budget]
    jmp     .done
.scan:
    mov     qword [r12 + QsbrSlot.scan_budget], SCAN_EVERY
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
    mov     [r12 + QsbrSlot.min_epoch], r8
.bound_ok:
    cmp     qword [rbx + Table.pin_used], 0 ; after the scan (see remarks)
    jne     .pinned_path

    ; ---- no pin anywhere: free without the mutex ----
    mov     rbp, [r12 + QsbrSlot.limbo_head]
.free_next:
    test    rbp, rbp
    jz      .emptied
    test    r13, r13
    jz      .stop
    mov     rcx, [rbp + Rec.aux]
    cmp     rcx, [r12 + QsbrSlot.min_epoch]
    jae     .stop                       ; FIFO: everything behind is newer
    dec     r13
    mov     rdi, rbp
    mov     rbp, [rbp + Rec.next]
    test    rbp, rbp
    jz      .no_prefetch
    prefetcht0 [rbp]                    ; the next stamp, while free() runs
.no_prefetch:
    mov     ecx, [rdi + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    mov     edx, [rdi + Rec.val_len]
    add     rcx, rdx
    add     rcx, Rec_size
    add     r14, rcx
    inc     r15
    mov     rsi, rdi                    ; the record
    mov     rdx, rcx                    ; its size
    mov     rdi, r12                    ; this slot
    call    slot_cache_put              ; kept for this slot's next allocation, or freed
    jmp     .free_next
.stop:
    mov     [r12 + QsbrSlot.limbo_head], rbp
    jmp     .account
.emptied:
    mov     qword [r12 + QsbrSlot.limbo_head], 0
    mov     qword [r12 + QsbrSlot.limbo_tail], 0
.account:
    sub     [r12 + QsbrSlot.limbo_count], r15
    add     [r12 + QsbrSlot.freed], r15
    test    r14, r14
    jz      .done
    neg     r14
    lock xadd [rbx + Table.mem_used], r14
.done:
    mov     rax, r15
    add     rsp, 8
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

    ; ---- some record is pinned somewhere: check each under the mutex ----
.pinned_path:
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rbp, [r12 + QsbrSlot.limbo_head]
.p_next:
    test    rbp, rbp
    jz      .p_emptied
    test    r13, r13
    jz      .p_stop
    mov     rcx, [rbp + Rec.aux]
    cmp     rcx, [r12 + QsbrSlot.min_epoch]
    jae     .p_stop
    dec     r13
    mov     rax, [rbp + Rec.next]
    mov     [rsp], rax                  ; pin_find clobbers rcx, rdx, r8, r9
    mov     rdi, rbx
    mov     rsi, rbp
    call    pin_find
    test    rax, rax
    jz      .p_free
    ; Pinned: its pin entry takes it over (deferred) and the last
    ; kv_store_unpin frees it; its bytes stay counted until then.
    mov     qword [rax + PinEnt.deferred], 1
    inc     qword [rbx + Table.pin_deferred]
    dec     qword [r12 + QsbrSlot.limbo_count]
    jmp     .p_advance
.p_free:
    mov     ecx, [rbp + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    mov     edx, [rbp + Rec.val_len]
    add     rcx, rdx
    add     rcx, Rec_size
    add     r14, rcx
    inc     r15
    mov     rdi, rbp
    call    free wrt ..plt
.p_advance:
    mov     rbp, [rsp]
    jmp     .p_next
.p_stop:
    mov     [r12 + QsbrSlot.limbo_head], rbp
    jmp     .p_unlock
.p_emptied:
    mov     qword [r12 + QsbrSlot.limbo_head], 0
    mov     qword [r12 + QsbrSlot.limbo_tail], 0
.p_unlock:
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    jmp     .account

; =============================================================================
; kv_store_pin — keep a record alive across quiescent states (Download).
;
; @param  rdi  Table*
; @param  rsi  Rec* returned by kv_store_get during the caller's CURRENT
;              online period (before its next quiescent/offline announcement)
; @returns rax = 0, or -ENOMEM when the pin table could not grow (the record
;          is then not pinned; nothing changed).
; @remarks Safety argument: the lookup that returned the record happened
;          while the caller was online, so the record cannot be freed before
;          the caller's next quiescent announcement (QSBR). This function
;          records the pin under the writer mutex before that announcement;
;          reclamation checks pins under the same mutex, so whenever it later
;          reaches the record — still live, still in limbo, or past its grace
;          period — it sees the pin and defers instead of freeing. Pins
;          nest: every kv_store_pin needs one kv_store_unpin.
; =============================================================================
kv_store_pin:
    push    rbx
    push    r12
    sub     rsp, 8                      ; [0] result; 2 pushes + 8: aligned
    mov     rbx, rdi
    mov     r12, rsi
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     rsi, r12
    call    pin_find
    test    rax, rax
    jnz     .nested
    ; A new entry: keep the load factor <= 1/2 so probes stay short.
    cmp     qword [rbx + Table.pins], 0
    je      .grow
    mov     rax, [rbx + Table.pin_used]
    lea     rax, [rax * 2 + 2]          ; 2 * (used + 1)
    mov     rcx, [rbx + Table.pin_mask]
    inc     rcx
    cmp     rax, rcx
    jbe     .insert
.grow:
    mov     rdi, rbx
    call    pin_grow
    test    eax, eax
    jnz     .nomem
.insert:
    mov     r8, [rbx + Table.pins]
    mov     rdx, [rbx + Table.pin_mask]
    mov     rax, r12
    mov     rcx, PIN_HASH_MUL
    imul    rax, rcx
    shr     rax, 32
    and     rax, rdx                    ; home slot
.probe:
    mov     rcx, rax
    shl     rcx, 5                      ; * PinEnt_size
    cmp     qword [r8 + rcx + PinEnt.rec], 0
    je      .slot
    inc     rax
    and     rax, rdx
    jmp     .probe
.slot:
    mov     [r8 + rcx + PinEnt.rec], r12
    mov     qword [r8 + rcx + PinEnt.count], 1
    mov     qword [r8 + rcx + PinEnt.deferred], 0
    inc     qword [rbx + Table.pin_used]
    mov     qword [rsp], 0
    jmp     .unlock
.nested:
    inc     qword [rax + PinEnt.count]
    mov     qword [rsp], 0
    jmp     .unlock
.nomem:
    mov     qword [rsp], -ENOMEM
.unlock:
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    mov     rax, [rsp]
    add     rsp, 8
    pop     r12
    pop     rbx
    ret

; =============================================================================
; kv_store_unpin — release one pin taken by kv_store_pin.
;
; @param  rdi  Table*   @param rsi  Rec* (pinned)
; @returns nothing.
; @remarks The last unpin of a record that reclamation already deferred (it
;          was replaced or deleted and its grace period is over) frees it
;          here, under the writer mutex, and returns its bytes to the memory
;          accounting. The last unpin of a record still live, or still in
;          limbo, only forgets the pin; reclamation frees it normally later.
;          Unpinning a record that is not pinned is a caller bug and is
;          ignored (never a crash, never a double free).
; =============================================================================
kv_store_unpin:
    push    rbx
    push    r12
    push    r13                         ; 3 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, rsi
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_lock wrt ..plt
    mov     rdi, rbx
    mov     rsi, r12
    call    pin_find
    test    rax, rax
    jz      .unlock                     ; not pinned: ignored
    dec     qword [rax + PinEnt.count]
    jnz     .unlock                     ; other pins remain
    mov     r13, [rax + PinEnt.deferred]
    mov     rdi, rbx
    mov     rsi, rax
    call    pin_remove
    dec     qword [rbx + Table.pin_used]
    test    r13, r13
    jz      .unlock
    ; Deferred: nobody else can reach it (retired, grace period over, last pin
    ; gone), so it is freed now, exactly as reclamation would have.
    dec     qword [rbx + Table.pin_deferred]
    mov     ecx, [r12 + Rec.key_len]
    add     rcx, 7
    and     rcx, -8
    mov     edx, [r12 + Rec.val_len]
    add     rcx, rdx
    add     rcx, Rec_size
    neg     rcx
    lock xadd [rbx + Table.mem_used], rcx
    mov     rdi, r12
    call    free wrt ..plt
    inc     qword [rbx + Table.st_freed]
.unlock:
    lea     rdi, [rbx + Table.wlock]
    call    kv_mutex_unlock wrt ..plt
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; pin_find — the pin entry of a record. Internal, leaf; the caller holds the
; writer mutex.
; @param  rdi  Table*   @param rsi  Rec*
; @returns rax = PinEnt*, or 0 when the record is not pinned (or no pin
;          table exists yet). Clobbers rcx, rdx, r8, r9 — NOT r10/r11, which
;          store_reclaim_step keeps the record in.
; @remarks Fibonacci hashing of the pointer (records are 16-byte aligned, so
;          the low bits carry nothing; the multiply spreads the rest), then
;          linear probing; the table is at most half full, so a miss ends at
;          an empty slot within a few probes.
; =============================================================================
pin_find:
    mov     r8, [rdi + Table.pins]
    test    r8, r8
    jz      .none
    mov     rdx, [rdi + Table.pin_mask]
    mov     rax, rsi
    mov     rcx, PIN_HASH_MUL
    imul    rax, rcx
    shr     rax, 32
    and     rax, rdx                    ; home slot
.probe:
    mov     rcx, rax
    shl     rcx, 5                      ; * PinEnt_size
    mov     r9, [r8 + rcx + PinEnt.rec]
    test    r9, r9
    jz      .none
    cmp     r9, rsi
    je      .found
    inc     rax
    and     rax, rdx
    jmp     .probe
.found:
    lea     rax, [r8 + rcx]
    ret
.none:
    xor     eax, eax
    ret

; =============================================================================
; pin_grow — double the pin table (or create it) and rehash. Internal; the
; caller holds the writer mutex.
; @param  rdi  Table*
; @returns eax = 0, or -1 when out of memory (the old table stays intact).
; @remarks Entries move with their count and deferred flag unchanged, so a
;          deferred record keeps its owner through the move.
; =============================================================================
pin_grow:
    push    rbx
    push    r12
    push    r13
    push    r14
    push    r15                         ; 5 pushes: aligned for calls
    mov     rbx, rdi
    mov     r12, [rbx + Table.pins]     ; old table, or 0
    xor     r13d, r13d                  ; old slot count
    mov     r14d, PIN_MIN_SLOTS         ; new slot count
    test    r12, r12
    jz      .alloc
    mov     r13, [rbx + Table.pin_mask]
    inc     r13
    lea     r14, [r13 * 2]
.alloc:
    mov     rdi, r14
    mov     esi, PinEnt_size
    call    calloc wrt ..plt
    test    rax, rax
    jz      .nomem
    mov     r15, rax                    ; new table
    dec     r14                         ; new mask
    xor     ecx, ecx                    ; old slot index
.next:
    cmp     rcx, r13
    jae     .done
    mov     r8, rcx
    shl     r8, 5
    add     r8, r12                     ; old entry
    mov     r9, [r8 + PinEnt.rec]
    test    r9, r9
    jz      .skip
    mov     rax, r9
    mov     rdx, PIN_HASH_MUL
    imul    rax, rdx
    shr     rax, 32
    and     rax, r14                    ; home slot in the new table
.probe:
    mov     rdx, rax
    shl     rdx, 5
    cmp     qword [r15 + rdx + PinEnt.rec], 0
    je      .place
    inc     rax
    and     rax, r14
    jmp     .probe
.place:
    mov     r10, [r8 + PinEnt.rec]
    mov     [r15 + rdx + PinEnt.rec], r10
    mov     r10, [r8 + PinEnt.count]
    mov     [r15 + rdx + PinEnt.count], r10
    mov     r10, [r8 + PinEnt.deferred]
    mov     [r15 + rdx + PinEnt.deferred], r10
.skip:
    inc     rcx
    jmp     .next
.done:
    mov     [rbx + Table.pins], r15
    mov     [rbx + Table.pin_mask], r14
    mov     rdi, r12
    call    free wrt ..plt              ; free(NULL) for the first table
    xor     eax, eax
    jmp     .out
.nomem:
    mov     eax, -1
.out:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbx
    ret

; =============================================================================
; pin_remove — delete an entry from the pin table. Internal, leaf; the
; caller holds the writer mutex (and adjusts pin_used).
; @param  rdi  Table*   @param rsi  PinEnt* (in use)
; @returns nothing. Clobbers rax, rcx, rdx, rsi, r8-r11.
; @remarks Backward-shift deletion instead of tombstones (h2n.asm's
;          map_remove uses the same rule): each entry after the hole moves
;          back into it when the hole lies on that entry's probe path, so the
;          table never degrades however many downloads come and go.
; =============================================================================
pin_remove:
    mov     r8, [rdi + Table.pins]
    mov     rdx, [rdi + Table.pin_mask]
    mov     rcx, rsi
    sub     rcx, r8
    shr     rcx, 5                      ; i: index of the hole
    mov     r9, rcx                     ; j scans forward from the hole
.shift:
    inc     r9
    and     r9, rdx
    mov     rax, r9
    shl     rax, 5
    mov     r10, [r8 + rax + PinEnt.rec]
    test    r10, r10
    jz      .clear                      ; end of the cluster: the hole stays empty
    mov     r11, r10
    mov     rsi, PIN_HASH_MUL
    imul    r11, rsi
    shr     r11, 32
    and     r11, rdx                    ; home of the entry at j
    mov     rsi, r9
    sub     rsi, r11
    and     rsi, rdx                    ; dist(home -> j)
    mov     r11, r9
    sub     r11, rcx
    and     r11, rdx                    ; dist(hole -> j)
    cmp     r11, rsi
    ja      .shift                      ; the hole is not on its probe path: stays
    ; Move entry j into the hole i; j becomes the new hole.
    mov     r11, rcx
    shl     r11, 5
    mov     rsi, [r8 + rax + PinEnt.rec]
    mov     [r8 + r11 + PinEnt.rec], rsi
    mov     rsi, [r8 + rax + PinEnt.count]
    mov     [r8 + r11 + PinEnt.count], rsi
    mov     rsi, [r8 + rax + PinEnt.deferred]
    mov     [r8 + r11 + PinEnt.deferred], rsi
    mov     rcx, r9
    jmp     .shift
.clear:
    shl     rcx, 5
    mov     qword [r8 + rcx + PinEnt.rec], 0
    mov     qword [r8 + rcx + PinEnt.count], 0
    mov     qword [r8 + rcx + PinEnt.deferred], 0
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
