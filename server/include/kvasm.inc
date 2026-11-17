; =============================================================================
; kvasm.inc — definitions shared by every KvAsm assembly module.
;
; Contents: Linux x86-64 syscall numbers and errno values used by raw
; syscalls, futex operations, and the in-memory layouts (struc) that more than
; one module reads. A layout change here is an ABI change for every module and
; for the C test harnesses that mirror these structs — update
; server/tests/kvasm_abi.h in the same commit and let test_abi catch drift.
; =============================================================================
%ifndef KVASM_INC
%define KVASM_INC

; ---- Linux x86-64 syscall numbers -------------------------------------------
%define SYS_read            0
%define SYS_write           1
%define SYS_close           3
%define SYS_fstat           5
%define SYS_mmap            9
%define SYS_munmap          11
%define SYS_madvise         28
%define SYS_sendto          44
%define SYS_shutdown        48
%define SYS_exit_group      231
%define SYS_futex           202
%define SYS_getrandom       318

; ---- errno values (raw syscalls return -errno) ------------------------------
%define EINTR               4
%define E2BIG               7
%define EAGAIN              11
%define ENOMEM              12
%define EINVAL              22
%define ENOSPC              28

; ---- mmap / madvise ---------------------------------------------------------
%define PROT_READ           1
%define PROT_WRITE          2
%define MAP_PRIVATE         0x02
%define MAP_ANONYMOUS       0x20
%define MAP_NORESERVE       0x4000
%define MADV_HUGEPAGE       14
%define HUGE_PAGE           0x200000    ; x86-64 transparent huge page (2 MiB)

; ---- key hash (store.asm kv_store_hash, aeshash.asm) ---------------------------
; Which keyed hash places keys (kv_set_hash_mode). Process-wide, set once at
; startup before any key is hashed: every table of the process hashes the
; same way, under its own key.
%define KV_HASH_SIPHASH     0           ; SipHash-2-4, one 64-bit word: runs on every x86-64
%define KV_HASH_AES         1           ; AES-128 through AES-NI (kv_aes_available)
%define KV_HASH_MAX         1
; A table's AES key block: the 11 round keys, then one secret mask per final
; block length 0..16 (aeshash.asm). 448 bytes = 7 cache lines, 64-byte aligned.
%define AESKEY_RK_BYTES     176
%define AESKEY_MASKS        17
%define AESKEY_BYTES        (AESKEY_RK_BYTES + AESKEY_MASKS * 16)

; ---- private-cache limits (hcache.asm / store.asm) ------------------------------
%define INVAL_BITS_MAX      24          ; 16 Mi generations = 128 MiB at most
%define HCACHE_BITS_MAX     24          ; per level: 16 Mi entries = 512 MiB at most

; ---- futex (process-private: never shared across processes) -----------------
%define FUTEX_WAIT_PRIVATE  128
%define FUTEX_WAKE_PRIVATE  129

; ---- Record -------------------------------------------------------------------
; A stored key/value pair. Immutable from the moment a pointer to it is
; published in a bucket until it is freed, so any thread may read key_len,
; val_len and data without synchronization while it is protected by QSBR.
;   next  writer-only link: limbo list, bake-off chains/tries. Readers never
;         read it.
;   aux   writer-only: retire epoch once unlinked (store); key hash h1 in the
;         bake-off structures. Readers never read it.
;   data  key bytes, zero-padded to a multiple of 8, followed by value bytes.
struc Rec
    .next:      resq 1
    .aux:       resq 1
    .key_len:   resd 1
    .val_len:   resd 1
    .data:
endstruc

; ---- Bucket -------------------------------------------------------------------
; One 64-byte cache line: a version word, six one-byte tags and six record
; pointers. The version is even when stable and odd while the (single, lock
; holding) writer modifies the bucket; readers accept what they read only if
; the version was even and unchanged (the Linux vDSO seqcount protocol).
%define BK_SLOTS            6
%define BK_TAGMASK          0x0000808080808080  ; SWAR hit bits of slots 0..5
struc Bucket
    .ver:       resq 1
    .tags:      resq 1
    .slot:      resq BK_SLOTS
endstruc

; ---- Cuckoo path search scratch ------------------------------------------------
%define MAX_BFS             256     ; queue entries => bounded (O(1)) insert work
%define VISIT_SLOTS         512     ; exact visited-bucket set, load <= 50%
struc BfsEntry
    .bucket:    resq 1              ; bucket index
    .parent:    resd 1              ; queue index of the parent entry, -1 = root
    .pslot:     resd 1              ; slot in the parent whose record moves here
endstruc

; ---- QSBR reader slot (one per reader thread, own cache line) -------------------
; Replaced records one hold of the writer mutex can hand to its slot
; (kv_store_put_locked / kv_store_delete_locked); a hold with more falls back
; to the global limbo. grpc.asm bounds a hold to exactly this many writes.
%define SLOT_PENDING_MAX    64

; Each slot's record cache (ADR 0011): records its own reclamation freed,
; kept for its own next allocations (kv_store_rec_alloc_local) instead of
; going back to glibc. Why: a record is allocated by the worker that wrote
; it and freed by the worker whose write replaced it, so with several workers
; nearly every free() crossed threads and took another worker's malloc arena
; lock — 44% of a worker's user time at 4 workers. Classes are glibc's
; chunk sizes, 32..1040 bytes in 16-byte steps (records up to 1,032 bytes:
; a 100-byte value with a 16-byte key is 140); bigger records bypass it.
; Bounded so a skewed worker cannot hoard memory: per class and per slot.
%define REC_CLASSES         64
%define REC_CACHE_PER_CLASS 256
%define REC_CACHE_BYTES     262144

; One thread's QSBR slot. Line 0 is the shared part: the owner stores its
; observed epoch there at every quiescent announcement, and every reclaiming
; thread's reader scan loads it. The rest is PRIVATE to the owner (ADR 0011):
; the records its batched writes replaced or deleted wait in this slot's own
; limbo and are freed by the owner, outside the writer mutex — on separate
; lines, so the owner's limbo bookkeeping never invalidates the line the
; scans read.
struc QsbrSlot
    .observed:      resq 1          ; 0 = offline, else last observed epoch
    .pad:           resb 56
    ; line 1: owner-private reclamation state
    .limbo_head:    resq 1          ; oldest retired record (FIFO through Rec.next)
    .limbo_tail:    resq 1
    .limbo_count:   resq 1
    .min_epoch:     resq 1          ; records stamped below this are free-able
    .scan_budget:   resq 1          ; batches until the next reader scan is due
    .freed:         resq 1          ; records this slot reclaimed (statistic)
    .pending_n:     resq 1          ; replaced records of the current mutex hold
    .pad1:          resq 1
    ; lines 2-9: those records, retired when the hold ends (kv_store_write_end)
    .pending:       resq SLOT_PENDING_MAX
    ; lines 10-22: the record cache, LIFO per class, linked through Rec.next
    .cache_bytes:   resq 1          ; chunk bytes held (<= REC_CACHE_BYTES)
    .cache_pad:     resq 7
    .cache_head:    resq REC_CLASSES
    .cache_count:   resd REC_CLASSES
endstruc

; ---- Table ---------------------------------------------------------------------
; Grouped by who writes each cache line so readers never share a line with
; state that writers update on every operation (false sharing).
struc Table
    ; line 0: immutable after creation, read by everyone
    .buckets:       resq 1          ; Bucket[nbuckets], 64-byte aligned (mmap)
    .mask:          resq 1          ; nbuckets - 1
    .seed0:         resq 1          ; SipHash key (random per process)
    .seed1:         resq 1
    .readers:       resq 1          ; QsbrSlot[n_readers]
    .n_readers:     resq 1
    .max_records:   resq 1
    .mem_limit:     resq 1
    ; line 1: writer mutex (touched by writers only)
    .wlock:         resd 1
    .pad1:          resb 60
    ; line 2: global QSBR epoch (written by the lock holder, read by readers)
    .epoch:         resq 1
    .pad2:          resb 56
    ; line 3: memory accounting (LOCK XADD by any writer, before locking)
    .mem_used:      resq 1
    .pad3:          resb 56
    ; lines 4+: state owned by the lock holder
    .count:         resq 1          ; live records
    .limbo_head:    resq 1          ; retired records waiting for a grace period
    .limbo_tail:    resq 1
    .limbo_count:   resq 1
    .min_epoch:     resq 1          ; records with retire epoch < this are free-able
    .scan_budget:   resq 1          ; rate limit for reader-slot scans
    .visit_gen:     resq 1          ; generation stamp of the visited set
    .bfs:           resq 1          ; BfsEntry[MAX_BFS]
    .visit:         resq 1          ; u64[VISIT_SLOTS]
    .nbuckets:      resq 1
    .st_inserts:    resq 1
    .st_replaces:   resq 1
    .st_deletes:    resq 1
    .st_bfs:        resq 1          ; inserts that needed a cuckoo path
    .st_moves:      resq 1          ; records relocated along cuckoo paths
    .st_full:       resq 1          ; inserts refused (capacity or no path)
    .st_freed:      resq 1          ; records reclaimed after a grace period
    .pad4:          resb 56         ; lines 4-6 end here (448)
    ; line 7: invalidation generations for readers' private caches (hcache.asm).
    ; Set once by kv_store_enable_inval before any reader or writer runs, then
    ; read-only; the array itself is bumped by writers under the mutex.
    .inval:         resq 1          ; u64[inval_mask + 1], or 0 when disabled
    .inval_mask:    resq 1
    ; The AES key hash's key block (aeshash.asm): AESKEY_BYTES derived from the
    ; seed by kv_store_create, or 0 on a CPU without AES-NI. Set before the
    ; table is shared, then read-only — on this line because every hash loads
    ; it and nothing here is written after startup.
    .hash_key:      resq 1
    .pad7:          resb 40         ; keep line 8 on its own cache line
    ; line 8: record pins (store.asm kv_store_pin). A Download keeps its record
    ; alive across the worker's quiescent states; QSBR alone would free a
    ; replaced record at the next grace period. Lock-holder state, like lines
    ; 4-6, on its own line so pin traffic never invalidates line 7, which
    ; private-cache readers load on every lookup.
    .pins:          resq 1          ; PinEnt[pin_mask + 1], or 0 before the first pin
    .pin_mask:      resq 1
    .pin_used:      resq 1          ; entries in use = distinct pinned records
    .pin_deferred:  resq 1          ; entries whose record is retired and past its grace
    .pad8:          resb 32         ; keep Table_size a multiple of 64 (576)
endstruc

; ---- PinEnt: one pinned record (store.asm pin table, 32 bytes) -------------------
; Open addressing with linear probing and backward-shift deletion, keyed by
; the record pointer. A record whose grace period ends while it is pinned is
; not freed by reclamation: its entry is marked deferred and owns it, and the
; last unpin frees it (so a deferred record lives in no list at all).
struc PinEnt
    .rec:       resq 1              ; Rec*, 0 = empty slot
    .count:     resq 1              ; pins held (>= 1 while in use)
    .deferred:  resq 1              ; 1 = retired and past its grace period: last unpin frees
    .pad:       resq 1
endstruc

; ---- HEntry: one private-cache entry (hcache.asm), half a cache line ------------
; tag = h2 of the key (0 = empty), h1 = its h1, rec = the record the table
; returned, gen = the key's invalidation generation read BEFORE that lookup.
; Both hashes must match before gen is compared: the generation slot is
; derived from h1, so a lookup for another key that merely shares h2 would
; otherwise validate against the wrong slot and could dereference a record
; that was retired and freed.
struc HEntry
    .tag:       resq 1
    .h1:        resq 1
    .rec:       resq 1
    .gen:       resq 1
endstruc

; ---- HCache: one reader thread's private two-level cache (hcache.asm) ----------
; Owned by exactly one thread (a worker): entries are never shared, so they
; need no synchronization. A level with mask -1 is disabled.
struc HCache
    .l1:            resq 1          ; HEntry[l1_mask + 1], 64-byte aligned, or 0
    .l1_mask:       resq 1
    .l2:            resq 1          ; HEntry[l2_mask + 1], or 0
    .l2_mask:       resq 1
    .table:         resq 1          ; Table* whose inval array validates entries
    .st_l1_hits:    resq 1
    .st_l2_hits:    resq 1
    .st_misses:     resq 1          ; answered by the table
    .st_stale:      resq 1          ; tag matched but a write invalidated it
    .pad:           resb 56         ; 128 bytes: two whole lines
endstruc

; ---- KvStats (C-facing snapshot of Table counters, see kv_store_stats) ----------
struc KvStats
    .count:         resq 1
    .nbuckets:      resq 1
    .max_records:   resq 1
    .mem_used:      resq 1
    .mem_limit:     resq 1
    .limbo_count:   resq 1
    .epoch:         resq 1
    .inserts:       resq 1
    .replaces:      resq 1
    .deletes:       resq 1
    .bfs:           resq 1
    .moves:         resq 1
    .full:          resq 1
    .freed:         resq 1
    ; appended with record pins (Download): both must be 0 once no stream
    ; holds a pin — main logs them at shutdown and the end-to-end suite
    ; checks that line, so a leaked pin fails the build.
    .pinned:        resq 1          ; distinct records pinned now
    .deferred:      resq 1          ; of those, retired and waiting for their last unpin
endstruc

%endif
