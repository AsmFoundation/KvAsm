; =============================================================================
; kvnode.inc — definitions shared by every KvNode assembly module.
;
; Contents: Linux x86-64 syscall numbers and errno values used by raw
; syscalls, futex operations, and the in-memory layouts (struc) that more than
; one module reads. A layout change here is an ABI change for every module and
; for the C test harnesses that mirror these structs — update
; server/tests/kvnode_abi.h in the same commit and let test_abi catch drift.
; =============================================================================
%ifndef KVNODE_INC
%define KVNODE_INC

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
struc QsbrSlot
    .observed:  resq 1              ; 0 = offline, else last observed epoch
    .pad:       resb 56
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
    .pad4:          resb 56         ; keep Table_size a multiple of 64 (448)
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
endstruc

%endif
