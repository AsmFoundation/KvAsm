; =============================================================================
; cuckoo.asm — bucketized cuckoo hash index with lock-free readers.
;
; Layout: 2^k buckets of one cache line each (see Bucket in kvasm.inc):
; version word, six one-byte tags, six record pointers. Every key has exactly
; two candidate buckets, so a lookup reads at most two cache lines — O(1) in
; the worst case, and the two addresses are known up front so the CPU fetches
; both lines in parallel.
;
; Placement ("partial-key cuckoo hashing", as in MemC3):
;   tag = h2 >> 56 (0 is reserved for "empty", so 0 becomes 1)
;   b1  = h1 & mask
;   b2  = b1 ^ ((((tag * K) >> 32) | 1) & mask)       K = 0x9E3779B97F4A7C15
; The alternate bucket depends only on (bucket, tag), so relocating a record
; never needs its key or hash: path search touches bucket lines only. The
; offset is forced odd, hence b2 != b1, and alt(alt(b)) == b.
;
; Concurrency contract:
;   * Readers (kv_cuckoo_get) run lock-free from any thread. They read a
;     bucket's version, tags and slots with plain loads and re-check both
;     versions before reporting a miss (x86-TSO keeps the loads in order; no
;     fences). A hit needs no re-check: the pointer was read atomically, the
;     record is immutable and QSBR keeps it alive, and the key is compared
;     exactly — so the hit was true at the instant the pointer was read.
;   * Writers (upsert/remove) must be serialized by the caller (store.asm's
;     mutex). They bump a bucket's version to odd, modify it, and bump it back
;     to even; relocations copy a record to its new slot before clearing the
;     old one, so a key is never absent from both of its buckets.
;
; Worst-case work per operation is bounded (O(1) in the number of keys):
; lookups/removals touch 2 buckets; inserts additionally run a breadth-first
; path search capped at MAX_BFS queue entries (<= MAX_BFS bucket reads).
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

%define ALT_MUL     0x9E3779B97F4A7C15      ; tag -> bucket-offset mixer
%define GEN_LIMIT   0x0000000000FFFFFF      ; 24-bit visited-set generation

extern kv_keyeq

; -----------------------------------------------------------------------------
; Scan mode: how readers match a key's tag against the candidate buckets.
;   KV_SCAN_SWAR (0)  one 64-bit SWAR match per bucket (GPRs only); B2's line
;                     is touched only when B1 has no match.
;   KV_SCAN_SSE2 (1)  both buckets' tag words in one XMM register, one
;                     PCMPEQB/PMOVMSKB for all 12 slots. SSE2 is part of the
;                     x86-64 baseline, so this adds no CPU requirement.
; Set once at startup (kv_set_scan_mode) before any reader runs; readers only
; load it, so its line is never written while shared. Writers (upsert,
; remove) always use SWAR: they are serialized and rare on the read path.
; -----------------------------------------------------------------------------
%define KV_SCAN_SWAR        0
%define KV_SCAN_SSE2        1
%define KV_SCAN_MAX         1

section .data
align 64
global kv_scan_mode
kv_scan_mode:   db KV_SCAN_SWAR
                times 63 db 0           ; own cache line: nothing else shares it

section .text

global kv_cuckoo_get
global kv_cuckoo_upsert
global kv_cuckoo_remove
global kv_cuckoo_prefetch
global kv_cuckoo_prefetch_records
global kv_cuckoo_locate
global kv_cuckoo_prefetch_records_at
global kv_cuckoo_get_at
global kv_set_scan_mode

; =============================================================================
; kv_set_scan_mode — choose the readers' tag-scan implementation.
;
; @param  edi  KV_SCAN_SWAR (0) or KV_SCAN_SSE2 (1)
; @returns eax = the previous mode, or -EINVAL (mode unchanged) for any other
;          value.
; @remarks Not synchronized: call it while no thread reads the store (startup,
;          or between benchmark phases). Both modes return identical results;
;          only speed differs, which is what bench_store measures.
; =============================================================================
kv_set_scan_mode:
    cmp     edi, KV_SCAN_MAX
    ja      .bad
    movzx   eax, byte [rel kv_scan_mode]
    mov     [rel kv_scan_mode], dil
    ret
.bad:
    mov     eax, -EINVAL
    ret

; -----------------------------------------------------------------------------
; LOCATE — derive tag and both bucket pointers for (h1, h2).
;   %1 Table* register (preserved)   %2 h1 register   %3 h2 register
;   out: rbx = tag (1..255), rbp = tag broadcast to all 8 bytes,
;        r14 = bucket-1 pointer, r15 = bucket-2 pointer
;   clobbers: rax, r9, r10
; -----------------------------------------------------------------------------
%macro LOCATE 3
    mov     rbx, %3
    shr     rbx, 56
    mov     eax, 1
    test    ebx, ebx
    cmovz   ebx, eax                    ; tag 0 is reserved for empty slots
    mov     r9, [%1 + Table.mask]
    mov     r14, %2
    and     r14, r9                     ; b1
    mov     rax, ALT_MUL
    imul    rax, rbx
    shr     rax, 32
    or      rax, 1                      ; odd offset => b2 != b1
    and     rax, r9
    xor     rax, r14                    ; b2
    mov     r10, [%1 + Table.buckets]
    shl     r14, 6
    add     r14, r10
    shl     rax, 6
    lea     r15, [r10 + rax]
    mov     rbp, 0x0101010101010101
    imul    rbp, rbx                    ; tag in every byte, for SWAR matching
%endmacro

; -----------------------------------------------------------------------------
; BUCKETS_OF table, h1, h2 — the two candidate bucket addresses of a key into
; r8 (B1) and r10 (B2), and the tag into rcx: exactly LOCATE's arithmetic,
; on caller-saved registers only (for the leaf prefetch functions).
; Uses rax, rcx, r8, r9, r10.
; -----------------------------------------------------------------------------
%macro BUCKETS_OF 3
    mov     rcx, %3
    shr     rcx, 56
    mov     eax, 1
    test    ecx, ecx
    cmovz   ecx, eax                    ; tag 0 is reserved for empty slots
    mov     r9, [%1 + Table.mask]
    mov     r8, %2
    and     r8, r9                      ; b1
    mov     rax, ALT_MUL
    imul    rax, rcx
    shr     rax, 32
    or      rax, 1
    and     rax, r9
    xor     rax, r8                     ; b2
    mov     r10, [%1 + Table.buckets]
    shl     r8, 6
    add     r8, r10
    shl     rax, 6
    add     r10, rax
%endmacro

; =============================================================================
; kv_cuckoo_prefetch — start loading a key's two candidate buckets.
;
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns nothing. Clobbers rax, rcx, r8, r9, r10.
; @remarks Why: under GET load the bucket's first cache miss was the single
;          hottest instruction in KvAsm (50% of the lookup's samples, on the
;          tag load). A request can be hashed long before it is looked up;
;          issuing both bucket loads at parse time lets the misses of a whole
;          batch of requests overlap instead of stalling one after another.
;          A pure hint: reads only the Table header, never faults.
; =============================================================================
kv_cuckoo_prefetch:
    BUCKETS_OF rdi, rsi, rdx
    prefetcht0 [r8]
    prefetcht0 [r10]
    ret

; =============================================================================
; kv_cuckoo_prefetch_records — second prefetch stage: records of tag matches.
;
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns nothing. Clobbers rax, rcx, rdx, rsi, rdi, r8 .. r11.
; @remarks Meant to run once the buckets arrived (after kv_cuckoo_prefetch
;          and other work): scans both tag words with the exact SWAR match (or
;          one SSE2 compare, per kv_scan_mode) and prefetches the first three
;          lines of each candidate record — the key compare and the value copy
;          of the real lookup then hit the cache.
;          Why three: a record (24-byte header, padded key, value) starts on
;          any 16-byte boundary, so lines [rec], [rec + 64] and [rec + 128]
;          cover a whole record of up to 144 bytes at every alignment — a
;          16-byte key with a value of up to ~104 bytes (the benchmarks' 100
;          makes 140). Measured with two lines (1 core, 100-byte values,
;          busy-polling client so the profile shows only server work): the
;          value copy took the third line's DRAM miss, glibc memmove was 4.2%
;          of the core; with three, 1.05%. Throughput, paired and concurrent
;          (6 blocks): 1.001x with a blocking client, 1.025x busy-polling
;          (4/6 blocks), 0.993x with 16-byte values — within noise either
;          way, so it is kept for the stall it removes, not for a proven
;          gain. For a smaller record the third line is a wasted hint (one
;          extra line of memory traffic per candidate); a larger value's
;          remaining lines stream in behind the copy.
;          Racing writers are harmless: tags and pointers are read with plain
;          aligned loads, and a prefetch of a stale or freed address is only
;          a wasted hint (prefetch never faults). No versions are checked —
;          correctness stays with kv_cuckoo_get. The SSE2 path also clobbers
;          xmm0 and xmm1 (caller-saved in the System V ABI).
; =============================================================================
kv_cuckoo_prefetch_records:
    BUCKETS_OF rdi, rsi, rdx
.located:                               ; r8 = B1, r10 = B2, rcx = tag (kv_cuckoo_prefetch_records_at)
    mov     r11, 0x0101010101010101
    imul    r11, rcx                    ; tag in every byte
    cmp     byte [rel kv_scan_mode], KV_SCAN_SWAR
    jne     .sse2
    mov     rdi, r8
    call    .bucket                     ; B1 (a local call; .bucket makes none)
    mov     rdi, r10                    ; B2: fall through, .bucket's ret returns to our caller
.bucket:
    mov     rax, [rdi + Bucket.tags]
    xor     rax, r11                    ; matching tag bytes become 0
    mov     rdx, 0x7F7F7F7F7F7F7F7F
    mov     rsi, rax
    and     rsi, rdx
    add     rsi, rdx
    or      rsi, rax
    or      rsi, rdx
    not     rsi                         ; exact: 0x80 exactly where a byte was 0
    mov     rdx, BK_TAGMASK
    and     rsi, rdx                    ; only the six slot tags
    jz      .none
.match:
    bsf     rcx, rsi
    shr     ecx, 3                      ; byte index = slot index
    mov     rax, [rdi + Bucket.slot + rcx*8]
    prefetcht0 [rax]                    ; header + key (+ value start)
    prefetcht0 [rax + 64]               ; rest of a typical small value
    prefetcht0 [rax + 128]              ; its tail line (see @remarks: 3 lines)
    lea     rax, [rsi - 1]
    and     rsi, rax                    ; clear the lowest match (baseline x86-64, no BMI)
    jnz     .match
.none:
    ret

    ; SSE2: B1's tag word in the low half, B2's in the high half, one byte
    ; compare for all 12 slots. Mask bits 0-5 are B1's slots, 8-13 B2's; the
    ; two unused tag bytes of each word are masked off (and hold 0, which is
    ; never a key's tag anyway).
.sse2:
    movq    xmm1, r11
    punpcklqdq xmm1, xmm1               ; tag in all 16 bytes
    movq    xmm0, [r8 + Bucket.tags]
    movhps  xmm0, [r10 + Bucket.tags]
    pcmpeqb xmm0, xmm1
    pmovmskb eax, xmm0
    and     eax, 0x3F3F
    jz      .sse2_done
.sse2_match:
    bsf     ecx, eax
    mov     rdx, r8
    cmp     ecx, 8
    jb      .sse2_slot
    mov     rdx, r10                    ; bits 8..13: slot (bit - 8) of B2
    sub     ecx, 8
.sse2_slot:
    mov     rsi, [rdx + Bucket.slot + rcx*8]
    prefetcht0 [rsi]                    ; 0 (slot cleared meanwhile) is a harmless hint
    prefetcht0 [rsi + 64]
    prefetcht0 [rsi + 128]              ; the tail line (see @remarks: 3 lines)
    lea     ecx, [rax - 1]
    and     eax, ecx                    ; next match
    jnz     .sse2_match
.sse2_done:
    ret

; =============================================================================
; kv_cuckoo_locate — a key's two candidate buckets, with both lines requested.
;
; @param  rdi  Table*   @param rsi  h1   @param rdx  h2
; @returns rax = bucket 1, rdx = bucket 2 (addresses inside the table).
; @remarks kv_cuckoo_prefetch that also hands the addresses back, for a caller
;          that will come back to the same key: the batched request path
;          prefetches at parse time, prefetches the records later and looks
;          the key up last, and deriving the addresses from (h1, h2) each
;          time cost a multiply and ~17 instructions twice over per Get.
;          The addresses stay valid for the table's life — it is sized once
;          and never moves — but say nothing about the buckets' contents:
;          the consumers below read them under the usual protocol.
;          Clobbers rcx, r8, r9, r10.
; =============================================================================
kv_cuckoo_locate:
    BUCKETS_OF rdi, rsi, rdx
    prefetcht0 [r8]
    prefetcht0 [r10]
    mov     rax, r8
    mov     rdx, r10
    ret

; =============================================================================
; kv_cuckoo_prefetch_records_at — kv_cuckoo_prefetch_records for buckets that
; kv_cuckoo_locate already found.
;
; @param  rdi  bucket 1   @param rsi  bucket 2   @param rdx  h2
; @returns nothing. Clobbers rax, rcx, rdx, rsi, rdi, r8 .. r11 (and xmm0,
;          xmm1 on the SSE2 path), as kv_cuckoo_prefetch_records does.
; @remarks Footgun: the buckets must be the ones kv_cuckoo_locate returned
;          for the same key and table. Nothing here can check that; wrong
;          addresses only waste the hints (a prefetch never faults), but the
;          same mistake in kv_cuckoo_get_at would read arbitrary memory.
; =============================================================================
kv_cuckoo_prefetch_records_at:
    mov     rcx, rdx
    shr     rcx, 56
    mov     eax, 1
    test    ecx, ecx
    cmovz   ecx, eax                    ; tag 0 is reserved for empty slots (as BUCKETS_OF)
    mov     r8, rdi
    mov     r10, rsi
    jmp     kv_cuckoo_prefetch_records.located

; =============================================================================
; kv_cuckoo_get_at — kv_cuckoo_get for buckets that kv_cuckoo_locate already
; found.
;
; @param  rdi  bucket 1   @param rsi  bucket 2 (of this key, in this table)
; @param  rdx  const uint8_t *key   @param rcx  size_t len
; @param  r8   h2 (its top byte is the tag)
; @returns rax = Rec* whose key equals (key, len), or 0 if absent.
; @remarks The same contract, concurrency protocol and scan-mode dispatch as
;          kv_cuckoo_get: it builds that function's frame and registers and
;          continues in its body, so there is one lookup implementation.
;          Footgun: buckets of another key or table make it dereference
;          whatever those addresses hold — pass exactly what
;          kv_cuckoo_locate returned for (h1, h2) of this key.
; =============================================================================
kv_cuckoo_get_at:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24                     ; kv_cuckoo_get's frame: [rsp] / [rsp+8] = bucket versions
    mov     r12, rdx
    mov     r13, rcx
    mov     r14, rdi
    mov     r15, rsi
    mov     rbx, r8
    shr     rbx, 56
    mov     eax, 1
    test    ebx, ebx
    cmovz   ebx, eax                    ; tag 0 is reserved for empty slots (as LOCATE)
    mov     rbp, 0x0101010101010101
    imul    rbp, rbx                    ; tag in every byte
    cmp     byte [rel kv_scan_mode], KV_SCAN_SWAR
    je      kv_cuckoo_get.retry
    movq    xmm7, rbp
    punpcklqdq xmm7, xmm7               ; tag in all 16 bytes
    jmp     kv_cuckoo_get_sse2.retry

; =============================================================================
; kv_cuckoo_get — lock-free lookup, callable from any thread.
;
; @param  rdi  Table*
; @param  rsi  const uint8_t *key
; @param  rdx  size_t len
; @param  rcx  h1 (SipHash-128 low word of the key)
; @param  r8   h2 (SipHash-128 high word of the key)
; @returns rax = Rec* whose key equals (key, len), or 0 if absent.
; @remarks The caller must be an online QSBR reader (store.asm) and must stop
;          using the returned record before its next quiescent/offline
;          announcement. Spins (PAUSE) while a writer holds a bucket odd;
;          writers keep that window to a handful of stores.
;          Dispatches on kv_scan_mode (a perfectly predicted branch): the SSE2
;          variant below is an exact replacement with the same contract.
; =============================================================================
kv_cuckoo_get:
    cmp     byte [rel kv_scan_mode], KV_SCAN_SWAR
    jne     kv_cuckoo_get_sse2
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24                     ; [rsp] = version of B1, [rsp+8] = of B2
    mov     r12, rsi
    mov     r13, rdx
    LOCATE  rdi, rcx, r8
.retry:
    mov     rax, [r14 + Bucket.ver]
    test    al, 1
    jnz     .busy
    mov     [rsp], rax
    mov     rax, [r15 + Bucket.ver]
    test    al, 1
    jnz     .busy
    mov     [rsp + 8], rax
    mov     rbx, r14
    call    cuckoo_scan_bucket
    test    rax, rax
    jnz     .out
    mov     rbx, r15
    call    cuckoo_scan_bucket
    test    rax, rax
    jnz     .out
    ; A miss is only trustworthy if neither bucket changed while we scanned:
    ; a concurrent relocation could have moved the key between the buckets.
    mov     rcx, [r14 + Bucket.ver]
    cmp     rcx, [rsp]
    jne     .retry
    mov     rcx, [r15 + Bucket.ver]
    cmp     rcx, [rsp + 8]
    jne     .retry
    xor     eax, eax
.out:
    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
.busy:
    pause
    jmp     .retry

; =============================================================================
; kv_cuckoo_get_sse2 — kv_cuckoo_get with one SSE2 tag compare over both
; candidate buckets. Internal (reached through kv_cuckoo_get's dispatch).
;
; @param  rdi  Table*   @param rsi  key   @param rdx  len
; @param  rcx  h1       @param r8   h2
; @returns rax = Rec* whose key equals (key, len), or 0 if absent.
; @remarks Same contract and concurrency protocol as the SWAR path: both
;          versions are read (and must be even) before the tags, and a miss
;          is reported only if neither version changed during the scan.
;          Differences, which bench_store measures:
;            * B1's and B2's tag words share one XMM register, so all 12
;              slots are matched by PCMPEQB + PMOVMSKB without a per-bucket
;              branch;
;            * both tag lines are always loaded (the SWAR path skips B2 when
;              B1 hits). Under the batched GET path both lines are prefetched
;              anyway; a lone lookup pays for B2's line, in parallel with B1.
;          Candidates are confirmed exactly as before (slot non-zero, same
;          length, kv_keyeq), so a tag collision never returns a wrong record.
;          Uses r9-r11 and xmm0/xmm7 as scratch: kv_keyeq clobbers only rax,
;          rcx and r8, so the match mask survives in r11 across calls.
; =============================================================================
kv_cuckoo_get_sse2:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 24                     ; [rsp] = version of B1, [rsp+8] = of B2
    mov     r12, rsi
    mov     r13, rdx
    LOCATE  rdi, rcx, r8
    movq    xmm7, rbp
    punpcklqdq xmm7, xmm7               ; tag in all 16 bytes
.retry:
    mov     rax, [r14 + Bucket.ver]
    test    al, 1
    jnz     .busy
    mov     [rsp], rax
    mov     rax, [r15 + Bucket.ver]
    test    al, 1
    jnz     .busy
    mov     [rsp + 8], rax
    movq    xmm0, [r14 + Bucket.tags]   ; loads stay after the version loads (TSO)
    movhps  xmm0, [r15 + Bucket.tags]
    pcmpeqb xmm0, xmm7
    pmovmskb r11d, xmm0
    and     r11d, 0x3F3F                ; bits 0-5: B1 slots, bits 8-13: B2 slots
.next:
    test    r11d, r11d
    jz      .miss
    bsf     ecx, r11d
    lea     eax, [r11 - 1]
    and     r11d, eax                   ; consume this candidate
    mov     r10, r14
    cmp     ecx, 8
    jb      .slot
    mov     r10, r15
    sub     ecx, 8
.slot:
    mov     r9, [r10 + Bucket.slot + rcx*8]
    test    r9, r9
    jz      .next                       ; cleared concurrently: not a candidate
    mov     edx, [r9 + Rec.key_len]
    cmp     rdx, r13
    jne     .next
    lea     rdi, [r9 + Rec.data]
    mov     rsi, r12
    call    kv_keyeq wrt ..plt          ; clobbers rax, rcx, r8 only
    test    eax, eax
    jz      .next
    mov     rax, r9
    jmp     .out
.miss:
    ; As in the SWAR path: a relocation could have moved the key between the
    ; buckets while we scanned, so a miss needs both versions unchanged.
    mov     rcx, [r14 + Bucket.ver]
    cmp     rcx, [rsp]
    jne     .retry
    mov     rcx, [r15 + Bucket.ver]
    cmp     rcx, [rsp + 8]
    jne     .retry
    xor     eax, eax
.out:
    add     rsp, 24
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
.busy:
    pause
    jmp     .retry

; =============================================================================
; kv_cuckoo_upsert — insert a record or replace the record with the same key.
;
; @param  rdi  Table* (caller holds the writer lock)
; @param  rsi  Rec* rec — fully initialized; its key is rec->data[0..key_len)
; @param  rdx  h1 of the key
; @param  rcx  h2 of the key
; @returns rax = previous Rec* if the key existed (now replaced; the caller
;          must retire it, never free it directly), 0 if the record was
;          inserted as a new key, or -ENOSPC if the key is new and the table
;          is at max_records or no free slot is reachable within the bounded
;          path search.
; @remarks Maintains count and statistics. Records are published only inside
;          an odd-version window, so readers never accept a half-updated
;          bucket as stable.
; =============================================================================
kv_cuckoo_upsert:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 40                     ; [0]=Table* [8]=rec [16]=tag [24]=b1 [32]=b2
    mov     [rsp], rdi
    mov     [rsp + 8], rsi
    lea     r12, [rsi + Rec.data]
    mov     r13d, [rsi + Rec.key_len]
    LOCATE  rdi, rdx, rcx
    mov     [rsp + 16], rbx
    mov     rax, r14                    ; bucket pointers -> indices (path search)
    sub     rax, [rdi + Table.buckets]
    shr     rax, 6
    mov     [rsp + 24], rax
    mov     rax, r15
    sub     rax, [rdi + Table.buckets]
    shr     rax, 6
    mov     [rsp + 32], rax

    ; Existing key? Replace in place (same key => same tag and buckets).
    mov     rbx, r14
    call    cuckoo_scan_bucket
    test    rax, rax
    jnz     .replace
    mov     rbx, r15
    call    cuckoo_scan_bucket
    test    rax, rax
    jnz     .replace

    mov     rdi, [rsp]
    mov     rax, [rdi + Table.count]
    cmp     rax, [rdi + Table.max_records]
    jae     .full

    mov     rbx, r14
    call    cuckoo_empty_slot
    jns     .publish
    mov     rbx, r15
    call    cuckoo_empty_slot
    jns     .publish

    ; Both candidate buckets are full: find a relocation path.
    mov     rdi, [rsp]
    mov     rsi, [rsp + 8]
    mov     rdx, [rsp + 16]
    mov     rcx, [rsp + 24]
    mov     r8, [rsp + 32]
    call    cuckoo_bfs_insert
    test    rax, rax
    jnz     .full
    jmp     .inserted

.publish:                               ; rbx = bucket, rcx = free slot
    mov     rdx, [rsp + 8]
    mov     rax, [rsp + 16]
    call    cuckoo_publish
.inserted:
    mov     rdi, [rsp]
    inc     qword [rdi + Table.count]
    inc     qword [rdi + Table.st_inserts]
    xor     eax, eax
    jmp     .out

.replace:                               ; rax = old rec, rbx = bucket, rcx = slot
    mov     rdx, [rsp + 8]
    inc     qword [rbx + Bucket.ver]    ; odd: readers of this bucket retry
    mov     [rbx + Bucket.slot + rcx*8], rdx
    inc     qword [rbx + Bucket.ver]    ; even
    mov     rdi, [rsp]
    inc     qword [rdi + Table.st_replaces]
    jmp     .out

.full:
    mov     rdi, [rsp]
    inc     qword [rdi + Table.st_full]
    mov     rax, -ENOSPC
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
; kv_cuckoo_remove — unlink the record with the given key.
;
; @param  rdi  Table* (caller holds the writer lock)
; @param  rsi  const uint8_t *key
; @param  rdx  size_t len
; @param  rcx  h1
; @param  r8   h2
; @returns rax = the unlinked Rec* (the caller must retire it, never free it
;          directly: readers may still hold it), or 0 if the key is absent.
; =============================================================================
kv_cuckoo_remove:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 8
    mov     [rsp], rdi
    mov     r12, rsi
    mov     r13, rdx
    LOCATE  rdi, rcx, r8
    mov     rbx, r14
    call    cuckoo_scan_bucket
    test    rax, rax
    jnz     .found
    mov     rbx, r15
    call    cuckoo_scan_bucket
    test    rax, rax
    jz      .out
.found:                                 ; rax = rec, rbx = bucket, rcx = slot
    inc     qword [rbx + Bucket.ver]    ; odd
    mov     r8, rcx                     ; slot index
    shl     ecx, 3                      ; bit offset of its tag byte
    mov     r9, 0xFF
    shl     r9, cl
    not     r9
    and     [rbx + Bucket.tags], r9     ; tag byte -> 0 (empty)
    mov     qword [rbx + Bucket.slot + r8*8], 0
    inc     qword [rbx + Bucket.ver]    ; even
    mov     rdi, [rsp]
    dec     qword [rdi + Table.count]
    inc     qword [rdi + Table.st_deletes]
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
; cuckoo_scan_bucket — find the record with key (r12, r13) in one bucket.
; Internal; custom convention so callers keep state in rbx/rbp/r12-r15.
;
; @param  rbx  Bucket*
; @param  rbp  tag broadcast to all 8 bytes
; @param  r12  key pointer      @param r13  key length
; @returns rax = matching Rec* or 0; rcx = its slot index when rax != 0.
; @remarks Tags pre-filter candidates (1/255 false-positive rate per slot);
;          every candidate is confirmed by exact key comparison, so a tag or
;          hash collision can never return the wrong record. A slot seen
;          empty although its tag matched was cleared concurrently: skipped.
;          Clobbers rax, rcx, rdx, rsi, rdi, r8-r11.
; =============================================================================
cuckoo_scan_bucket:
    mov     r9, [rbx + Bucket.tags]
    xor     r9, rbp                     ; matching tag bytes become 0x00
    mov     r10, 0x7F7F7F7F7F7F7F7F
    mov     r11, r9
    and     r11, r10
    add     r11, r10
    or      r11, r9
    or      r11, r10
    not     r11                         ; 0x80 in each zero byte (exact)
    mov     r10, BK_TAGMASK
    and     r11, r10                    ; only slots 0..5 exist
.next:
    test    r11, r11
    jz      .none
    bsf     rcx, r11
    shr     ecx, 3
    mov     r10, rcx                    ; slot index survives kv_keyeq
    lea     rax, [r11 - 1]
    and     r11, rax                    ; consume this hit
    mov     r9, [rbx + Bucket.slot + r10*8]
    test    r9, r9
    jz      .next
    mov     edx, [r9 + Rec.key_len]
    cmp     rdx, r13
    jne     .next
    lea     rdi, [r9 + Rec.data]
    mov     rsi, r12
    call    kv_keyeq wrt ..plt          ; clobbers rax, rcx, r8 only
    test    eax, eax
    jz      .next
    mov     rax, r9
    mov     rcx, r10
    ret
.none:
    xor     eax, eax
    ret

; =============================================================================
; cuckoo_empty_slot — first free slot of a bucket. Internal.
;
; @param  rbx  Bucket*
; @returns rcx = slot index (0..5) with SF clear, or rcx = -1 with SF set, so
;          callers branch with JNS/JS directly.
; @remarks A slot is free when its tag byte is 0. Clobbers r9-r11, rcx.
; =============================================================================
cuckoo_empty_slot:
    mov     r9, [rbx + Bucket.tags]
    mov     r10, 0x7F7F7F7F7F7F7F7F
    mov     r11, r9
    and     r11, r10
    add     r11, r10
    or      r11, r9
    or      r11, r10
    not     r11
    mov     r10, BK_TAGMASK
    and     r11, r10
    jz      .none
    bsf     rcx, r11
    shr     rcx, 3                      ; 0..5 => SF clear
    ret
.none:
    mov     rcx, -1                     ; MOV does not set flags:
    test    rcx, rcx                    ; set SF explicitly
    ret

; =============================================================================
; cuckoo_publish — place a record into a free slot. Internal, writer only.
;
; @param  rbx  Bucket*      @param rcx  free slot index
; @param  rdx  Rec*         @param rax  tag (1..255)
; @returns nothing. Clobbers rcx, r8.
; @remarks The pointer is stored before the tag, so a reader that sees the new
;          tag also sees the pointer. Either order would be safe (the odd
;          version forces retries); this one avoids pointless retries.
; =============================================================================
cuckoo_publish:
    inc     qword [rbx + Bucket.ver]
    mov     [rbx + Bucket.slot + rcx*8], rdx
    shl     ecx, 3
    movzx   r8d, al
    shl     r8, cl
    or      [rbx + Bucket.tags], r8
    inc     qword [rbx + Bucket.ver]
    ret

; =============================================================================
; cuckoo_bfs_insert — insert via the shortest relocation path. Internal.
;
; Breadth-first search from both candidate buckets over "record in slot s of
; bucket W may move to alt(W, tag_s)" edges until a bucket with a free slot is
; found, then relocates records backwards along the path (each record is
; copied into the hole first, then cleared from its old slot) and finally
; places the new record into the freed root slot.
;
; @param  rdi  Table*   @param rsi  Rec* new record   @param rdx  tag
; @param  rcx  b1 index @param r8   b2 index
; @returns rax = 0 on success, -ENOSPC if no path exists within MAX_BFS
;          queue entries (the table is effectively full around this key).
; @remarks Bounded work: at most MAX_BFS buckets are read and each bucket is
;          enqueued once (exact visited set), so a path never contains the
;          same bucket twice. Touches bucket lines only — never records.
; =============================================================================
cuckoo_bfs_insert:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, 40                     ; [0]=Table [8]=rec [16]=tag [24]=bucket base
    mov     [rsp], rdi
    mov     [rsp + 8], rsi
    mov     [rsp + 16], rdx
    mov     r15, [rdi + Table.mask]
    mov     rax, [rdi + Table.buckets]
    mov     [rsp + 24], rax
    mov     r12, [rdi + Table.bfs]      ; queue base
    inc     qword [rdi + Table.st_bfs]

    ; New generation for the visited set; clear the set on 24-bit wrap so a
    ; stale stamp can never alias the current search.
    mov     rax, [rdi + Table.visit_gen]
    inc     rax
    cmp     rax, GEN_LIMIT
    jb      .gen_ok
    mov     rdx, [rdi + Table.visit]
    mov     r9d, VISIT_SLOTS
.clear:
    mov     qword [rdx], 0
    add     rdx, 8
    dec     r9d
    jnz     .clear
    mov     eax, 1
.gen_ok:
    mov     [rdi + Table.visit_gen], rax
    shl     rax, 40
    mov     rsi, rax                    ; visit_add: generation stamp
    mov     rdi, [rdi + Table.visit]    ; visit_add: set base (rdi no longer Table*)

    ; Roots: both candidate buckets of the new key.
    mov     [r12 + BfsEntry.bucket], rcx
    mov     dword [r12 + BfsEntry.parent], -1
    mov     dword [r12 + BfsEntry.pslot], 0
    mov     [r12 + BfsEntry_size + BfsEntry.bucket], r8
    mov     dword [r12 + BfsEntry_size + BfsEntry.parent], -1
    mov     dword [r12 + BfsEntry_size + BfsEntry.pslot], 0
    mov     rax, rcx
    call    visit_add
    mov     rax, r8
    call    visit_add
    xor     r13d, r13d                  ; head
    mov     r14d, 2                     ; tail

.dequeue:
    cmp     r13, r14
    jae     .no_path
    mov     rax, r13
    shl     rax, 4
    mov     rbp, [r12 + rax + BfsEntry.bucket]      ; bucket index
    mov     rbx, rbp
    shl     rbx, 6
    add     rbx, [rsp + 24]                          ; Bucket*
    call    cuckoo_empty_slot
    jns     .found                                   ; rcx = hole
    ; Expand only when all six children fit, so the queue never overflows.
    lea     rax, [r14 + BK_SLOTS]
    cmp     rax, MAX_BFS
    ja      .advance
    mov     r9, [rbx + Bucket.tags]
    xor     r10d, r10d                               ; slot s
.child:
    mov     rax, r9
    mov     ecx, r10d
    shl     ecx, 3
    shr     rax, cl
    movzx   eax, al                                  ; tag of slot s (non-zero: bucket full)
    mov     rdx, ALT_MUL
    imul    rax, rdx
    shr     rax, 32
    or      rax, 1
    and     rax, r15
    xor     rax, rbp                                 ; where that record could go
    call    visit_add                                ; keeps rax, r9, r10, rdi, rsi
    jz      .next_child                              ; already queued
    mov     rdx, r14
    shl     rdx, 4
    mov     [r12 + rdx + BfsEntry.bucket], rax
    mov     [r12 + rdx + BfsEntry.parent], r13d
    mov     [r12 + rdx + BfsEntry.pslot], r10d
    inc     r14
.next_child:
    inc     r10d
    cmp     r10d, BK_SLOTS
    jb      .child
.advance:
    inc     r13
    jmp     .dequeue

.found:                                 ; r13 = entry with a hole, rcx = hole
    mov     r8, rcx                     ; hole slot in the current bucket
    mov     r9, r13                     ; current entry
.move_up:
    mov     rax, r9
    shl     rax, 4
    movsxd  rdx, dword [r12 + rax + BfsEntry.parent]
    test    rdx, rdx
    js      .at_root
    mov     r10d, [r12 + rax + BfsEntry.pslot]      ; s: slot in parent W
    mov     rbx, [r12 + rax + BfsEntry.bucket]
    shl     rbx, 6
    add     rbx, [rsp + 24]                          ; X: destination bucket
    mov     rax, rdx
    shl     rax, 4
    mov     rbp, [r12 + rax + BfsEntry.bucket]
    shl     rbp, 6
    add     rbp, [rsp + 24]                          ; W: source bucket
    mov     r11, [rbp + Bucket.slot + r10*8]         ; record moving W[s] -> X[hole]
    mov     rax, [rbp + Bucket.tags]
    mov     ecx, r10d
    shl     ecx, 3
    shr     rax, cl
    movzx   eax, al                                  ; its tag
    inc     qword [rbx + Bucket.ver]                 ; both buckets odd
    inc     qword [rbp + Bucket.ver]
    mov     [rbx + Bucket.slot + r8*8], r11          ; copy first ...
    mov     ecx, r8d
    shl     ecx, 3
    shl     rax, cl
    or      [rbx + Bucket.tags], rax
    mov     ecx, r10d
    shl     ecx, 3
    mov     rax, 0xFF
    shl     rax, cl
    not     rax
    and     [rbp + Bucket.tags], rax                 ; ... then clear the source
    mov     qword [rbp + Bucket.slot + r10*8], 0
    inc     qword [rbx + Bucket.ver]                 ; both even
    inc     qword [rbp + Bucket.ver]
    mov     rdi, [rsp]
    inc     qword [rdi + Table.st_moves]
    mov     r8d, r10d                                ; vacated slot = next hole
    mov     r9, rdx                                  ; continue from the parent
    jmp     .move_up
.at_root:
    mov     rax, r9
    shl     rax, 4
    mov     rbx, [r12 + rax + BfsEntry.bucket]
    shl     rbx, 6
    add     rbx, [rsp + 24]
    mov     rcx, r8
    mov     rdx, [rsp + 8]
    mov     rax, [rsp + 16]
    call    cuckoo_publish
    xor     eax, eax
    jmp     .out
.no_path:
    mov     rax, -ENOSPC
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
; visit_add — add a bucket index to the path search's visited set. Internal.
;
; @param  rax  bucket index (< 2^40; guaranteed by kv_store_create)
; @param  rdi  set base (u64[VISIT_SLOTS])
; @param  rsi  generation stamp (generation << 40, never 0)
; @returns ZF=1 if the index was already in the set, ZF=0 if it was added.
;          rax, rdi, rsi, r9, r10 are preserved.
; @remarks Exact open-addressing set (linear probing, <= 50% load because at
;          most MAX_BFS = VISIT_SLOTS/2 indices are added per search). Entries
;          carry the search generation in bits 40..63, so entries from older
;          searches read as free and the set never needs clearing between
;          searches. Clobbers rcx, rdx, r11.
; =============================================================================
visit_add:
    mov     rcx, rax
    imul    rcx, rcx, 0x5BD1E995        ; scramble the index
    shr     rcx, 7
    and     ecx, VISIT_SLOTS - 1
.probe:
    mov     rdx, [rdi + rcx*8]
    mov     r11, rdx
    shr     r11, 40
    shl     r11, 40                     ; generation part of the entry
    cmp     r11, rsi
    jne     .insert                     ; older generation (or 0) => free
    shl     rdx, 24
    shr     rdx, 24                     ; index part
    cmp     rdx, rax
    je      .present
    inc     ecx
    and     ecx, VISIT_SLOTS - 1
    jmp     .probe
.insert:
    mov     rdx, rsi
    or      rdx, rax                    ; non-zero (stamp >= 2^40) => ZF=0
    mov     [rdi + rcx*8], rdx
    ret
.present:
    cmp     eax, eax                    ; ZF=1
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
