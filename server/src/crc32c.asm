; =============================================================================
; crc32c.asm — CRC-32C (Castagnoli), the integrity check of Upload/Download.
;
; What: the CRC of iSCSI, ext4 and SCTP (RFC 3720 §12.1): reflected
; polynomial 0x82F63B78, register preset to all ones, result inverted. The
; functions here are zlib-style: pass 0 for a fresh checksum and the previous
; result to continue one, so crc(a || b) == crc(crc(a), b) — which is how a
; value that arrives in chunks is checked chunk by chunk.
;
; Why CRC-32C: it is the checksum large-object stores already speak (Google
; Cloud Storage, S3's checksum option, gRPC-adjacent tooling), every client
; language has it, and x86-64 CPUs since 2008 compute it in hardware (the
; SSE4.2 `crc32` instruction), so an end-to-end check costs little next to
; the copy it rides on.
;
; Four implementations with identical results, chosen once at startup
; (kv_crc32c_set_mode, from KVASM_CRC32C; `auto` takes the first the CPU
; has, kv_crc32c_best_mode):
;   vpclmul  carry-less folding on four 256-bit registers (eight independent
;            128-bit lanes) with VPCLMULQDQ + AVX2 (Ice Lake / Zen 3 and
;            later): ~4-5x sse42 on a transfer chunk, as fast as the copy it
;            rides on (tests/test_crc32c.c prints the measurement).
;   pclmul   the same folding on eight 128-bit SSE registers with PCLMULQDQ
;            (every x86-64 CPU since 2010 — Westmere, Bulldozer — including
;            the AVX-512 Xeons that lack VPCLMULQDQ). Plain SSE encodings, so
;            it needs no AVX at all.
;   sse42    `crc32` on 8 bytes per instruction: ONE dependency chain of a
;            3-cycle instruction, ~2.7 bytes per cycle (~14.6 GB/s at 5.5
;            GHz), which a profile of the upload path showed to be 49% of a
;            worker's CPU: why the folding paths exist. Still the choice
;            below 256 bytes, where folding's setup and finish do not pay.
;            SSE4.2 is NOT part of the x86-64 baseline (it is x86-64-v2).
;   table    slicing-by-8 with plain 64-bit registers and eight 1 KiB tables:
;            runs on every x86-64 CPU. It is also the mode before any
;            kv_crc32c_set_mode call, so an uninitialized process is slow but
;            never wrong.
; Each exists in two shapes: checksum only (kv_crc32c*) and fused copy +
; checksum (kv_crc32c_copy*), which moves bytes and checks them in one pass
; over memory — the shape the transfer paths need (an upload chunk into its
; record, a download chunk into the output buffer). The copy has a third
; entry, kv_crc32c_copy_nt, for a destination that is cold and not read soon
; (a large upload's record): the folding modes then write it with
; non-temporal stores, skipping the read of every destination line (1.9x on
; that pattern) — and 2x SLOWER into a hot destination, so callers choose it
; per transfer.
;
; Folding, in short (Intel, "Fast CRC Computation for Generic Polynomials
; Using PCLMULQDQ", 2009, reflected form): CRC is linear, so a 128-bit block
; V of the message may be replaced by any W with W = V * x^D (mod P) placed D
; bits later, and XORed into the data there. With V = V_H * x^64 + V_L,
; W = V_H * (x^(D+64) mod P) + V_L * (x^D mod P): two 64x32-bit carry-less
; multiplies. Eight lanes 128 bits apart fold by D = 1024 each, independent
; of each other, so the multiplier's latency hides behind the other lanes.
; At the end the lanes fold into one 128-bit block, which the crc32
; instruction reduces to 32 bits (no Barrett step): the register value is
; exactly the CRC of a message whose only non-zero bits are that block.
;
; The tables and the fold constants are computed by the assembler (NASM
; preprocessor loops below): no generated source file, no startup work, and
; nothing to initialize. The unit test (tests/test_crc32c.c) checks every
; implementation against a bitwise reference and the RFC 3720 vectors.
;
; Threading: every function is a pure computation; kv_crc32c_mode is written
; only by kv_crc32c_set_mode before any worker thread starts.
; =============================================================================

default rel
bits 64

%include "kvasm.inc"

%define CRC32C_POLY     0x82F63B78      ; reflected Castagnoli polynomial
%define CRC32C_NORMAL   0x1EDC6F41      ; the same polynomial, normal bit order (x^32 implicit)
%define CRC_MODE_TABLE  0               ; slicing-by-8, baseline x86-64
%define CRC_MODE_SSE42  1               ; SSE4.2 crc32 instruction
%define CRC_MODE_PCLMUL 2               ; 128-bit folding, PCLMULQDQ (+ SSE4.2 finish)
%define CRC_MODE_VPCLMUL 3              ; 256-bit folding, VPCLMULQDQ + AVX2
%define CRC_MODE_MAX    3
%define ENODEV          19              ; mode not supported by this CPU
; Below this many bytes the folding paths hand over to the crc32 chain: their
; fixed cost (8 lanes to set up, 3 fold steps to combine, the finish) is
; ~60 cycles, what the chain spends on ~160 bytes.
%define FOLD_MIN        256

section .rodata
; -----------------------------------------------------------------------------
; crc_tab — eight 256-entry tables for slicing-by-8, table k at crc_tab + k*1024.
; Entry i of table k is the raw (unconditioned) reflected CRC-32C register
; after 8*(k+1) bit steps starting from the value i: table 0 is the classic
; one-byte table, and table k folds in k further zero bytes, which is exactly
; what slicing-by-8 needs to consume 8 input bytes with 8 independent loads.
; Computing each entry from scratch (instead of from table k-1) keeps the
; preprocessor free of cross-references; it costs ~74k bit steps at assembly
; time, a fraction of a second.
; -----------------------------------------------------------------------------
align 64
crc_tab:
%assign k 0
%rep 8
  %assign i 0
  %rep 256
    %assign c i
    %rep 8 * (k + 1)
      %if c & 1
        %assign c (c >> 1) ^ CRC32C_POLY
      %else
        %assign c (c >> 1)
      %endif
    %endrep
    dd c
    %assign i i + 1
  %endrep
  %assign k k + 1
%endrep

; -----------------------------------------------------------------------------
; FOLD_K e — emit one pclmul operand: x^e mod P, bit-reflected into bits
; 32..63 of a qword (degree d of the remainder at bit 63 - d).
; Why that placement: in reflected form a 64-bit data half stands for
; sum(bit_i * x^(63-i)), and PCLMULQDQ sums bit products at i + j, so for the
; product to land aligned with the 128-bit data block (bit k = x^(127-k)) the
; constant's bit j must stand for x^(64-j). Degrees 1..64 fit, degree 0 does
; not — hence the operand holds x * (x^e mod P) with e one less than the
; shift wanted: e = D + 63 for the first (high-order) half of a block folded
; D bits, e = D - 1 for the second half.
; The remainder comes from e doubling steps (shift, reduce by P when bit 32
; appears); e is at most 1087 here, a few thousand preprocessor steps.
; -----------------------------------------------------------------------------
%macro FOLD_K 1
  %assign %%r 1
  %rep %1
    %assign %%r ((%%r << 1) & 0xFFFFFFFF) ^ (((%%r >> 31) & 1) * CRC32C_NORMAL)
  %endrep
  %assign %%rr 0
  %assign %%i 0
  %rep 32
    %assign %%rr %%rr | (((%%r >> %%i) & 1) << (31 - %%i))
    %assign %%i %%i + 1
  %endrep
  dq %%rr << 32
%endmacro

; Fold constants per distance D (bits): [x^(D+63) mod P, x^(D-1) mod P] =
; the multipliers of a lane's low qword (the earlier, high-order half: V_H)
; and high qword (V_L), in the order PCLMULQDQ's immediates 0x00 / 0x11 pick
; them. 1024: a lane to the same lane 128 bytes on (the main loops);
; 512 / 256 / 128: the tree that combines the eight lanes at the end, and the
; 16-byte loop that follows it.
align 16
fold_k1024: FOLD_K 1087
            FOLD_K 1023
fold_k512:  FOLD_K 575
            FOLD_K 511
fold_k256:  FOLD_K 319
            FOLD_K 255
fold_k128:  FOLD_K 191
            FOLD_K 127

section .data
align 64
global kv_crc32c_mode
; The mode byte dispatch reads on every call: alone on its cache line, and
; written only before workers start, so it is effectively read-only shared.
kv_crc32c_mode: db CRC_MODE_TABLE
                times 63 db 0

section .text

global kv_crc32c
global kv_crc32c_copy
global kv_crc32c_copy_nt
global kv_crc32c_copy_vpclmul_nt
global kv_crc32c_copy_pclmul_nt
global kv_crc32c_sw
global kv_crc32c_hw
global kv_crc32c_pclmul
global kv_crc32c_vpclmul
global kv_crc32c_copy_sw
global kv_crc32c_copy_hw
global kv_crc32c_copy_pclmul
global kv_crc32c_copy_vpclmul
global kv_crc32c_hw_available
global kv_crc32c_pclmul_available
global kv_crc32c_vpclmul_available
global kv_crc32c_best_mode
global kv_crc32c_set_mode

; =============================================================================
; kv_crc32c — CRC-32C of a buffer, continuing a previous result.
;
; @param  edi  crc: 0 for a new checksum, or the result of a previous call
;              over the bytes that precede this buffer
; @param  rsi  const void *buf (may be NULL when len is 0)
; @param  rdx  size_t len
; @returns eax = the CRC-32C of everything checksummed so far (upper half of
;          rax zero).
; @remarks Dispatches on kv_crc32c_mode (perfectly predicted branches: the
;          mode never changes once workers run); every target gives
;          identical results. Reads exactly [buf, buf + len). Clobbers eax
;          before the jump, which no target reads as an argument.
; =============================================================================
kv_crc32c:
    movzx   eax, byte [rel kv_crc32c_mode]
    cmp     eax, CRC_MODE_VPCLMUL
    je      kv_crc32c_vpclmul
    cmp     eax, CRC_MODE_PCLMUL
    je      kv_crc32c_pclmul
    cmp     eax, CRC_MODE_SSE42
    je      kv_crc32c_hw
    jmp     kv_crc32c_sw

; =============================================================================
; kv_crc32c_copy — copy a buffer and return the continued CRC-32C of it.
;
; @param  edi  crc to continue (0 for a new checksum)
; @param  rsi  void *dst (len writable bytes; must not overlap src)
; @param  rdx  const void *src
; @param  rcx  size_t len
; @returns eax = the continued CRC-32C; dst holds a copy of src.
; @remarks One pass over the data: each word or vector is loaded once,
;          stored and folded into the checksum, so checking a chunk that
;          must be copied anyway costs little more than the copy (measured
;          in tests/test_crc32c.c). Overlapping buffers give an unspecified
;          copy (the checksum is still that of src as read). Dispatch as in
;          kv_crc32c.
; =============================================================================
kv_crc32c_copy:
    movzx   eax, byte [rel kv_crc32c_mode]
    cmp     eax, CRC_MODE_VPCLMUL
    je      kv_crc32c_copy_vpclmul
    cmp     eax, CRC_MODE_PCLMUL
    je      kv_crc32c_copy_pclmul
    cmp     eax, CRC_MODE_SSE42
    je      kv_crc32c_copy_hw
    jmp     kv_crc32c_copy_sw

; =============================================================================
; kv_crc32c_copy_nt — kv_crc32c_copy for a destination that is cold and not
; read soon (an upload's record): the folding modes write it with
; non-temporal stores.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C; dst holds a copy of src.
; @remarks Only the folding modes have NT variants (~1.9x on that pattern,
;          see kv_crc32c_copy_vpclmul_nt); the sse42 and table modes are
;          bounded by their own checksum chain, not by the destination, so
;          they keep their temporal copy. Using this for a HOT destination
;          halves the speed: callers choose it per transfer (grpc.asm: large
;          uploads only). Every target ends with the NT stores drained
;          (sfence) or never issued them, so a caller may publish dst right
;          after.
; =============================================================================
kv_crc32c_copy_nt:
    movzx   eax, byte [rel kv_crc32c_mode]
    cmp     eax, CRC_MODE_VPCLMUL
    je      kv_crc32c_copy_vpclmul_nt
    cmp     eax, CRC_MODE_PCLMUL
    je      kv_crc32c_copy_pclmul_nt
    cmp     eax, CRC_MODE_SSE42
    je      kv_crc32c_copy_hw
    jmp     kv_crc32c_copy_sw

; =============================================================================
; kv_crc32c_hw — kv_crc32c with the SSE4.2 crc32 instruction.
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Requires SSE4.2 (kv_crc32c_hw_available): on a CPU without it the
;          instruction faults (#UD). Exported for the unit test, which
;          compares it with the table version; the server reaches it only
;          through kv_crc32c after kv_crc32c_set_mode verified the CPU.
;          One dependency chain of `crc32 r64, m64` (3-cycle latency): about
;          2.7 bytes per cycle. That chain turned out to bound the transfer
;          paths (49% of an upload worker's CPU, 2026-09-30 profile), so
;          buffers of FOLD_MIN bytes and more go to the folding paths where
;          the CPU has them; this remains the small-buffer path and the
;          fallback for SSE4.2 CPUs without PCLMULQDQ.
; =============================================================================
kv_crc32c_hw:
    mov     eax, edi
    not     eax                         ; preset: all ones (zlib-style API)
    cmp     rdx, 8
    jb      .tail
.words:
    crc32   rax, qword [rsi]            ; 8 bytes; the result is zero-extended
    add     rsi, 8
    sub     rdx, 8
    cmp     rdx, 8
    jae     .words
.tail:
    test    rdx, rdx
    jz      .done
.byte:
    crc32   eax, byte [rsi]
    inc     rsi
    dec     rdx
    jnz     .byte
.done:
    not     eax                         ; final inversion
    ret

; =============================================================================
; kv_crc32c_copy_hw — kv_crc32c_copy with the SSE4.2 crc32 instruction.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src.
; @remarks Requires SSE4.2 (see kv_crc32c_hw). Unaligned 8-byte loads and
;          stores: both buffers may start anywhere. Clobbers r8, rcx, rdx,
;          rsi.
; =============================================================================
kv_crc32c_copy_hw:
    mov     eax, edi
    not     eax
    cmp     rcx, 8
    jb      .tail
.words:
    mov     r8, [rdx]
    mov     [rsi], r8
    crc32   rax, r8
    add     rdx, 8
    add     rsi, 8
    sub     rcx, 8
    cmp     rcx, 8
    jae     .words
.tail:
    test    rcx, rcx
    jz      .done
.byte:
    movzx   r8d, byte [rdx]
    mov     [rsi], r8b
    crc32   eax, r8b
    inc     rdx
    inc     rsi
    dec     rcx
    jnz     .byte
.done:
    not     eax
    ret

; -----------------------------------------------------------------------------
; CRC_TAIL copy — the end every folding body shares: fold the last < 16 bytes
; into eax (the raw register, NOT yet inverted) with the crc32 instruction,
; copying them too when copy = 1, then invert and return.
; In: rdx src cursor, rcx bytes left (< 16), rsi dst cursor (copy only).
; Clobbers r8, rcx, rdx, rsi.
; -----------------------------------------------------------------------------
%macro CRC_TAIL 1
    cmp     rcx, 8
    jb      %%bytes
    mov     r8, [rdx]
%if %1
    mov     [rsi], r8
    add     rsi, 8
%endif
    crc32   rax, r8
    add     rdx, 8
    sub     rcx, 8
%%bytes:
    test    rcx, rcx
    jz      %%done
%%byte:
    movzx   r8d, byte [rdx]
%if %1
    mov     [rsi], r8b
    inc     rsi
%endif
    crc32   eax, r8b
    inc     rdx
    dec     rcx
    jnz     %%byte
%%done:
    not     eax                         ; final inversion
    ret
%endmacro

; -----------------------------------------------------------------------------
; VFOLD acc, tmp, data, k — one fold step on two 128-bit lanes at once (ymm
; operands) or one (xmm operands): per lane, acc = acc.lo64 * k.lo64 ^
; acc.hi64 * k.hi64 ^ data, k holding the distance's constants (broadcast to
; both lanes for ymm). tmp is scratch; register renaming lets consecutive
; calls share one tmp without serializing the lanes.
; -----------------------------------------------------------------------------
%macro VFOLD 4
    vpclmulqdq %2, %1, %4, 0x00
    vpclmulqdq %1, %1, %4, 0x11
    vpxor   %1, %1, %2
    vpxor   %1, %1, %3
%endmacro

; -----------------------------------------------------------------------------
; VSTORE mem, ymm, nt — a 32-byte store: vmovdqu, or with nt = 1 the
; non-temporal vmovntdq (mem must then be 32-byte aligned), which writes
; around the cache instead of reading the destination line first.
; -----------------------------------------------------------------------------
%macro VSTORE 3
%if %3
    vmovntdq %1, %2
%else
    vmovdqu %1, %2
%endif
%endmacro

; -----------------------------------------------------------------------------
; CRC_VPCLMUL_BODY copy [, nt] — kv_crc32c[_copy]_vpclmul after the length check.
; In: edi crc (as passed: not inverted yet), rdx src, rcx len (>= FOLD_MIN),
; rsi dst (copy = 1 only). Out: eax, via CRC_TAIL.
; Steps: (1) the first 128 bytes into ymm0-ymm3 (8 lanes), the inverted crc
; XORed into the first 4 bytes (how a register value enters the message);
; (2) 128 bytes per iteration, each lane folded 1024 bits onto its next
; block; (3) a 3-step tree folds the 8 lanes into one 128-bit block;
; (4) remaining whole 16-byte blocks folded 128 bits at a time; (5) the
; block reduced by two crc32 instructions from a zero register, then the
; tail. Each block is loaded once, stored (copy) and folded from the same
; register. vzeroupper runs before the scalar finish, so no caller pays an
; AVX-to-SSE transition penalty. Clobbers ymm0-ymm11, ymm15, r8, r9, rcx,
; rdx, rsi.
; -----------------------------------------------------------------------------
%macro CRC_VPCLMUL_BODY 1-2 0
    not     edi                         ; preset: all ones (zlib-style API)
    vmovdqu ymm0, [rdx]
    vmovdqu ymm1, [rdx + 32]
    vmovdqu ymm2, [rdx + 64]
    vmovdqu ymm3, [rdx + 96]
%if %1
    VSTORE  [rsi], ymm0, %2
    VSTORE  [rsi + 32], ymm1, %2
    VSTORE  [rsi + 64], ymm2, %2
    VSTORE  [rsi + 96], ymm3, %2
    add     rsi, 128
%endif
    vmovd   xmm4, edi                   ; VEX: bits 32..255 of ymm4 zeroed
    vpxor   ymm0, ymm0, ymm4            ; the register enters as the first 4 bytes
    vbroadcasti128 ymm15, [rel fold_k1024]
    add     rdx, 128
    sub     rcx, 128
    cmp     rcx, 128
    jb      %%combine
%%loop:
    vmovdqu ymm4, [rdx]
    vmovdqu ymm5, [rdx + 32]
    vmovdqu ymm6, [rdx + 64]
    vmovdqu ymm7, [rdx + 96]
%if %1
    VSTORE  [rsi], ymm4, %2
    VSTORE  [rsi + 32], ymm5, %2
    VSTORE  [rsi + 64], ymm6, %2
    VSTORE  [rsi + 96], ymm7, %2
    add     rsi, 128
%endif
    VFOLD   ymm0, ymm8, ymm4, ymm15
    VFOLD   ymm1, ymm9, ymm5, ymm15
    VFOLD   ymm2, ymm8, ymm6, ymm15
    VFOLD   ymm3, ymm9, ymm7, ymm15
    add     rdx, 128
    sub     rcx, 128
    cmp     rcx, 128
    jae     %%loop
%%combine:
    ; Lanes (byte offsets in the last 128-byte block): ymm0 = 0,16; ymm1 =
    ; 32,48; ymm2 = 64,80; ymm3 = 96,112. ymm0 onto ymm2 and ymm1 onto ymm3
    ; (64 bytes on, in parallel), then that onto ymm3's (32 on), then lane
    ; 96 onto lane 112 (16 on).
    vbroadcasti128 ymm15, [rel fold_k512]
    VFOLD   ymm0, ymm8, ymm2, ymm15     ; lanes now at 64, 80
    VFOLD   ymm1, ymm9, ymm3, ymm15     ; lanes now at 96, 112
    vbroadcasti128 ymm15, [rel fold_k256]
    VFOLD   ymm0, ymm8, ymm1, ymm15     ; lanes at 96, 112
    vmovdqu xmm15, [rel fold_k128]      ; also the 16-byte loop's constants
    vextracti128 xmm1, ymm0, 1          ; the lane at 112
    VFOLD   xmm0, xmm8, xmm1, xmm15     ; one block, at 112
    cmp     rcx, 16
    jb      %%finish
%%loop16:
    vmovdqu xmm4, [rdx]
%if %1
    vmovdqu [rsi], xmm4
    add     rsi, 16
%endif
    VFOLD   xmm0, xmm8, xmm4, xmm15
    add     rdx, 16
    sub     rcx, 16
    cmp     rcx, 16
    jae     %%loop16
%%finish:
    vmovq   r8, xmm0
    vpextrq r9, xmm0, 1
%if %2
    sfence                              ; NT stores are weakly ordered: drain them before returning
%endif
    vzeroupper
    ; The block is all that is left of the message (every earlier bit was
    ; folded into it), so the CRC register over just its 16 bytes, from 0,
    ; is the register over everything so far.
    xor     eax, eax
    crc32   rax, r8
    crc32   rax, r9
    CRC_TAIL %1
%endmacro

; =============================================================================
; kv_crc32c_vpclmul — kv_crc32c by 256-bit carry-less folding.
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Requires AVX2 + VPCLMULQDQ with the OS saving YMM state
;          (kv_crc32c_vpclmul_available): anywhere else it faults (#UD).
;          Under FOLD_MIN bytes it is kv_crc32c_hw (tail call). Reads exactly
;          [buf, buf + len) with unaligned loads. Exported for the unit test;
;          the server reaches it through kv_crc32c.
; =============================================================================
kv_crc32c_vpclmul:
    cmp     rdx, FOLD_MIN
    jb      kv_crc32c_hw
    mov     rcx, rdx                    ; the body's registers: rcx len, rdx src
    mov     rdx, rsi
    CRC_VPCLMUL_BODY 0

; =============================================================================
; kv_crc32c_copy_vpclmul — kv_crc32c_copy by 256-bit carry-less folding.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src; dst holds its copy.
; @remarks Requirements, the small-length hand-over (to kv_crc32c_copy_hw)
;          and the exact reads as kv_crc32c_vpclmul; writes exactly
;          [dst, dst + len) with unaligned 32-byte stores (temporal: the
;          upload's record and the download's output buffer are both read
;          again soon).
; =============================================================================
kv_crc32c_copy_vpclmul:
    cmp     rcx, FOLD_MIN
    jb      kv_crc32c_copy_hw
    CRC_VPCLMUL_BODY 1

; -----------------------------------------------------------------------------
; NT_HEAD align — the entry of a non-temporal copy: bytes up to the next
; `align`-byte boundary of dst go through the crc32 chain (8 bytes per step,
; then single bytes), so the folding body that follows stores whole,
; aligned cache lines; the chain's result is handed to the body as its
; initial CRC, in the API form (edi, not inverted) the body expects.
; Why whole lines (align 64): measured on the upload pattern, 32-byte
; vmovntdq blocks starting mid-line ran at 32-39 GB/s, line-aligned ones at
; 40-44 — a write-combining buffer only streams out cheaply when full.
; In/out: edi crc, rsi dst, rdx src, rcx len (all advanced past the head).
; Needs rcx >= align - 1 + FOLD_MIN (checked by the caller). Clobbers eax,
; r8, r9.
; -----------------------------------------------------------------------------
%macro NT_HEAD 1
    mov     eax, edi
    not     eax
    mov     r9, rsi
    neg     r9
    and     r9d, %1 - 1                 ; bytes until dst is aligned
    jz      %%aligned
    sub     rcx, r9
%%qwords:
    cmp     r9d, 8
    jb      %%bytes
    mov     r8, [rdx]
    mov     [rsi], r8
    crc32   rax, r8                     ; zero-extends: rax stays the 32-bit register
    add     rdx, 8
    add     rsi, 8
    sub     r9d, 8
    jmp     %%qwords
%%bytes:
    test    r9d, r9d
    jz      %%aligned
%%byte:
    movzx   r8d, byte [rdx]
    mov     [rsi], r8b
    crc32   eax, r8b
    inc     rdx
    inc     rsi
    dec     r9d
    jnz     %%byte
%%aligned:
    not     eax
    mov     edi, eax
%endmacro

; =============================================================================
; kv_crc32c_copy_vpclmul_nt — kv_crc32c_copy_vpclmul writing its 128-byte
; blocks with non-temporal stores.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src; dst holds its copy.
; @remarks For a destination that is cold and will not be read soon — an
;          upload's record: measured in tests/test_crc32c.c on 16 KiB calls
;          from a hot buffer into a 64 MiB span, 40-44 GB/s against 16-18
;          for the temporal copy, which must first read every destination
;          line (read-for-ownership) — but the copy is left out of the
;          cache, so into a hot destination it is 2x SLOWER (42 vs 88 GB/s):
;          never for download output. The first 0..63 bytes go through the
;          crc32 chain (NT_HEAD) so every vmovntdq pair fills one aligned
;          cache line; the 16-byte loop and the tail store normally. Ends
;          with sfence: the NT stores are globally visible, in order, before
;          the caller's next store — the record's publish, which other
;          threads acquire. Same CPU requirements as kv_crc32c_copy_vpclmul;
;          under FOLD_MIN + 63 bytes it is kv_crc32c_copy_hw.
; =============================================================================
kv_crc32c_copy_vpclmul_nt:
    cmp     rcx, FOLD_MIN + 63
    jb      kv_crc32c_copy_hw
    NT_HEAD 64
    CRC_VPCLMUL_BODY 1, 1

; -----------------------------------------------------------------------------
; PFOLD acc, data — kv_crc32c_pclmul's fold step on one 128-bit lane, in
; plain SSE (PCLMULQDQ is destructive: the copy into xmm9 keeps acc for the
; second multiply; renaming makes the shared xmm9 free). Constants in xmm15.
; -----------------------------------------------------------------------------
%macro PFOLD 2
    movdqa  xmm9, %1
    pclmulqdq xmm9, xmm15, 0x00         ; acc.lo * K.lo
    pclmulqdq %1, xmm15, 0x11           ; acc.hi * K.hi
    pxor    %1, xmm9
    pxor    %1, %2
%endmacro

; -----------------------------------------------------------------------------
; PSTORE mem, xmm, nt — a 16-byte store: movdqu, or with nt = 1 the
; non-temporal movntdq (SSE2; mem must then be 16-byte aligned).
; -----------------------------------------------------------------------------
%macro PSTORE 3
%if %3
    movntdq %1, %2
%else
    movdqu  %1, %2
%endif
%endmacro

; -----------------------------------------------------------------------------
; PLOAD acc, off, copy, nt — the first pass: 16 bytes at src + off into acc
; (and to dst + off when copying, non-temporally when nt = 1).
; -----------------------------------------------------------------------------
%macro PLOAD 4
    movdqu  %1, [rdx + %2]
%if %3
    PSTORE  [rsi + %2], %1, %4
%endif
%endmacro

; -----------------------------------------------------------------------------
; PSTEP acc, off, copy, nt — one main-loop lane: the next 16 bytes at
; src + off (stored to dst + off when copying) folded onto acc.
; -----------------------------------------------------------------------------
%macro PSTEP 4
    movdqu  xmm8, [rdx + %2]
%if %3
    PSTORE  [rsi + %2], xmm8, %4
%endif
    PFOLD   %1, xmm8
%endmacro

; -----------------------------------------------------------------------------
; CRC_PCLMUL_BODY copy [, nt] — kv_crc32c[_copy]_pclmul after the length
; check: the algorithm of CRC_VPCLMUL_BODY on eight xmm accumulators
; (xmm0-xmm7, one lane each), legacy SSE encodings only (SSE2 + PCLMULQDQ +
; SSE4.1 pextrq + SSE4.2 crc32), so it runs on pre-AVX CPUs. nt = 1 writes
; the 128-byte blocks with movntdq (dst 16-byte aligned by NT_HEAD) and ends
; with sfence. Clobbers xmm0-xmm9, xmm15, r8, r9, rcx, rdx, rsi.
; -----------------------------------------------------------------------------
%macro CRC_PCLMUL_BODY 1-2 0
    not     edi
    PLOAD   xmm0, 0, %1, %2
    PLOAD   xmm1, 16, %1, %2
    PLOAD   xmm2, 32, %1, %2
    PLOAD   xmm3, 48, %1, %2
    PLOAD   xmm4, 64, %1, %2
    PLOAD   xmm5, 80, %1, %2
    PLOAD   xmm6, 96, %1, %2
    PLOAD   xmm7, 112, %1, %2
%if %1
    add     rsi, 128
%endif
    movd    xmm8, edi                   ; zero-extends: bits 32..127 cleared
    pxor    xmm0, xmm8                  ; the register enters as the first 4 bytes
    movdqa  xmm15, [rel fold_k1024]
    add     rdx, 128
    sub     rcx, 128
    cmp     rcx, 128
    jb      %%combine
%%loop:
    PSTEP   xmm0, 0, %1, %2
    PSTEP   xmm1, 16, %1, %2
    PSTEP   xmm2, 32, %1, %2
    PSTEP   xmm3, 48, %1, %2
    PSTEP   xmm4, 64, %1, %2
    PSTEP   xmm5, 80, %1, %2
    PSTEP   xmm6, 96, %1, %2
    PSTEP   xmm7, 112, %1, %2
%if %1
    add     rsi, 128
%endif
    add     rdx, 128
    sub     rcx, 128
    cmp     rcx, 128
    jae     %%loop
%%combine:
    ; Lane xmm<i> sits at byte 16*i of the last block: 0-3 onto 4-7 (64 on),
    ; then onto 6-7 (32 on), then 6 onto 7 (16 on); the results land in the
    ; low registers.
    movdqa  xmm15, [rel fold_k512]
    PFOLD   xmm0, xmm4                  ; at 64
    PFOLD   xmm1, xmm5                  ; at 80
    PFOLD   xmm2, xmm6                  ; at 96
    PFOLD   xmm3, xmm7                  ; at 112
    movdqa  xmm15, [rel fold_k256]
    PFOLD   xmm0, xmm2                  ; at 96
    PFOLD   xmm1, xmm3                  ; at 112
    movdqa  xmm15, [rel fold_k128]
    PFOLD   xmm0, xmm1                  ; one block, at 112
    cmp     rcx, 16
    jb      %%finish
%%loop16:
    movdqu  xmm8, [rdx]
%if %1
    movdqu  [rsi], xmm8
    add     rsi, 16
%endif
    PFOLD   xmm0, xmm8
    add     rdx, 16
    sub     rcx, 16
    cmp     rcx, 16
    jae     %%loop16
%%finish:
    movq    r8, xmm0
    pextrq  r9, xmm0, 1
%if %2
    sfence                              ; NT stores are weakly ordered: drain them before returning
%endif
    xor     eax, eax                    ; see CRC_VPCLMUL_BODY's finish
    crc32   rax, r8
    crc32   rax, r9
    CRC_TAIL %1
%endmacro

; =============================================================================
; kv_crc32c_pclmul — kv_crc32c by 128-bit carry-less folding.
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Requires PCLMULQDQ, SSE4.1 and SSE4.2
;          (kv_crc32c_pclmul_available), no AVX. Under FOLD_MIN bytes it is
;          kv_crc32c_hw (tail call). Reads exactly [buf, buf + len).
; =============================================================================
kv_crc32c_pclmul:
    cmp     rdx, FOLD_MIN
    jb      kv_crc32c_hw
    mov     rcx, rdx
    mov     rdx, rsi
    CRC_PCLMUL_BODY 0

; =============================================================================
; kv_crc32c_copy_pclmul — kv_crc32c_copy by 128-bit carry-less folding.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src; dst holds its copy.
; @remarks As kv_crc32c_pclmul (small lengths go to kv_crc32c_copy_hw);
;          writes exactly [dst, dst + len).
; =============================================================================
kv_crc32c_copy_pclmul:
    cmp     rcx, FOLD_MIN
    jb      kv_crc32c_copy_hw
    CRC_PCLMUL_BODY 1

; =============================================================================
; kv_crc32c_copy_pclmul_nt — kv_crc32c_copy_pclmul writing its 128-byte
; blocks with non-temporal stores (movntdq).
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src; dst holds its copy.
; @remarks The 128-bit counterpart of kv_crc32c_copy_vpclmul_nt, with the
;          same use (a cold destination that is not read soon: an upload's
;          record), head (0..63 bytes, to a cache-line boundary: 36-43 GB/s
;          on the upload pattern against 16-17 temporal) and closing sfence.
;          Same CPU requirements as kv_crc32c_copy_pclmul; under
;          FOLD_MIN + 63 bytes it is kv_crc32c_copy_hw.
; =============================================================================
kv_crc32c_copy_pclmul_nt:
    cmp     rcx, FOLD_MIN + 63
    jb      kv_crc32c_copy_hw
    NT_HEAD 64
    CRC_PCLMUL_BODY 1, 1

; -----------------------------------------------------------------------------
; SLICE8 w — fold one 8-byte little-endian word (in register w, a 64-bit GPR
; other than rax, rcx, r10, r11) into the running register value in eax.
; Standard slicing-by-8: the low 4 bytes are XORed with the register, then
; each of the 8 bytes indexes its own table (byte j of the word uses table
; 7 - j) and the eight entries are XORed together. Clobbers w, rcx, r10,
; r11; uses r9 = crc_tab.
; Bytes are extracted with shifts and the low-byte registers only: the legacy
; high-byte registers (ch) cannot be encoded in an instruction that also
; needs a REX prefix, which every use of r10/r11 does.
; -----------------------------------------------------------------------------
%macro SLICE8 1
    mov     rcx, %1
    xor     ecx, eax                    ; first 4 bytes ^ register (clears bits 32-63)
    shr     %1, 32                      ; last 4 bytes
    movzx   r10d, cl
    mov     eax, [r9 + 7 * 1024 + r10 * 4]
    mov     r11d, ecx
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 6 * 1024 + r10 * 4]
    mov     r11d, ecx
    shr     r11d, 16
    movzx   r10d, r11b
    xor     eax, [r9 + 5 * 1024 + r10 * 4]
    shr     ecx, 24
    xor     eax, [r9 + 4 * 1024 + rcx * 4]
    mov     r11, %1
    movzx   r10d, r11b
    xor     eax, [r9 + 3 * 1024 + r10 * 4]
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 2 * 1024 + r10 * 4]
    shr     r11d, 8
    movzx   r10d, r11b
    xor     eax, [r9 + 1 * 1024 + r10 * 4]
    shr     r11d, 8
    xor     eax, [r9 + r11 * 4]         ; table 0: the byte with no zero bytes after it
%endmacro

; -----------------------------------------------------------------------------
; SLICE1 b — fold one byte (in the low 8 bits of 32-bit register b, not eax,
; ecx or r10) into eax with table 0. Clobbers ecx, r10; uses r9 = crc_tab.
; -----------------------------------------------------------------------------
%macro SLICE1 1
    mov     ecx, eax
    xor     ecx, %1
    movzx   r10d, cl
    shr     eax, 8
    xor     eax, [r9 + r10 * 4]
%endmacro

; =============================================================================
; kv_crc32c_sw — kv_crc32c with slicing-by-8 tables (any x86-64 CPU).
;
; @param  edi  crc to continue   @param rsi  buf   @param rdx  len
; @returns eax = the continued CRC-32C.
; @remarks Plain 64-bit registers only (the "no SIMD requirement" path).
;          Eight table lookups per 8 bytes, all independent of each other,
;          so they overlap in the load ports; the tables (8 KiB) stay in L1
;          while a chunk is processed. Clobbers rcx, rdx, rsi, r8-r11.
; =============================================================================
kv_crc32c_sw:
    mov     eax, edi
    not     eax
    lea     r9, [crc_tab]
    cmp     rdx, 8
    jb      .tail
.words:
    mov     r8, [rsi]
    SLICE8  r8
    add     rsi, 8
    sub     rdx, 8
    cmp     rdx, 8
    jae     .words
.tail:
    test    rdx, rdx
    jz      .done
.byte:
    movzx   r8d, byte [rsi]
    SLICE1  r8d
    inc     rsi
    dec     rdx
    jnz     .byte
.done:
    not     eax
    ret

; =============================================================================
; kv_crc32c_copy_sw — kv_crc32c_copy with slicing-by-8 tables.
;
; @param  edi  crc   @param rsi  dst   @param rdx  src   @param rcx  len
; @returns eax = the continued CRC-32C of src.
; @remarks Each word is stored to dst from the register it was folded from,
;          so src is read once. The length moves to rdi because SLICE8
;          clobbers rcx. Clobbers rcx, rdx, rsi, rdi, r8-r11.
; =============================================================================
kv_crc32c_copy_sw:
    mov     eax, edi
    not     eax
    mov     rdi, rcx                    ; length (rcx is SLICE8 scratch)
    lea     r9, [crc_tab]
    cmp     rdi, 8
    jb      .tail
.words:
    mov     r8, [rdx]
    mov     [rsi], r8                   ; store before SLICE8 consumes r8
    SLICE8  r8
    add     rdx, 8
    add     rsi, 8
    sub     rdi, 8
    cmp     rdi, 8
    jae     .words
.tail:
    test    rdi, rdi
    jz      .done
.byte:
    movzx   r8d, byte [rdx]
    mov     [rsi], r8b
    SLICE1  r8d
    inc     rdx
    inc     rsi
    dec     rdi
    jnz     .byte
.done:
    not     eax
    ret

; =============================================================================
; kv_crc32c_hw_available — does this CPU have the SSE4.2 crc32 instruction?
;
; @returns eax = 1 if CPUID leaf 1 reports SSE4.2 (ECX bit 20), else 0.
; @remarks crc32 works on general-purpose registers only, so unlike AVX it
;          needs no operating-system support check (XGETBV): the CPUID bit is
;          the whole answer. CPUID is slow (~100 cycles, a VM exit under a
;          hypervisor): call it once at startup, never per operation.
; =============================================================================
kv_crc32c_hw_available:
    push    rbx                         ; cpuid writes ebx (callee-saved)
    mov     eax, 1
    xor     ecx, ecx
    cpuid
    xor     eax, eax
    bt      ecx, 20
    setc    al
    pop     rbx
    ret

; =============================================================================
; kv_crc32c_pclmul_available — can kv_crc32c_pclmul run here?
;
; @returns eax = 1 if CPUID leaf 1 reports PCLMULQDQ (ECX bit 1), SSE4.1
;          (bit 19, for pextrq) and SSE4.2 (bit 20, for the crc32 finish),
;          else 0.
; @remarks Legacy SSE state is always enabled on x86-64, so no XGETBV check
;          is needed (unlike the AVX path). Call once at startup (CPUID is a
;          VM exit under a hypervisor).
; =============================================================================
kv_crc32c_pclmul_available:
    push    rbx                         ; cpuid writes ebx (callee-saved)
    mov     eax, 1
    xor     ecx, ecx
    cpuid
    xor     eax, eax
    and     ecx, (1 << 1) | (1 << 19) | (1 << 20)
    cmp     ecx, (1 << 1) | (1 << 19) | (1 << 20)
    sete    al
    pop     rbx
    ret

; =============================================================================
; kv_crc32c_vpclmul_available — can kv_crc32c_vpclmul run here?
;
; @returns eax = 1 if everything the 256-bit path executes is available AND
;          enabled, else 0: PCLMULQDQ, SSE4.1, SSE4.2, AVX and OSXSAVE (CPUID
;          leaf 1 ECX bits 1, 19, 20, 28, 27), the OS saving XMM and YMM
;          state (XCR0 bits 1 and 2, read with XGETBV), and AVX2 and
;          VPCLMULQDQ (leaf 7 subleaf 0: EBX bit 5, ECX bit 10).
; @remarks The XCR0 test matters: a CPU with AVX under an OS (or a
;          hypervisor's guest configuration) that does not save YMM
;          registers must not use them — the CPUID bits alone would say yes.
;          Leaf 7 is read only when leaf 0 reports it exists (older CPUs
;          return another leaf's data for out-of-range requests). Call once
;          at startup.
; =============================================================================
kv_crc32c_vpclmul_available:
    push    rbx
    xor     eax, eax
    xor     ecx, ecx
    cpuid                               ; eax = highest basic leaf
    cmp     eax, 7
    jb      .no
    mov     eax, 1
    xor     ecx, ecx
    cpuid
    and     ecx, (1 << 1) | (1 << 19) | (1 << 20) | (1 << 27) | (1 << 28)
    cmp     ecx, (1 << 1) | (1 << 19) | (1 << 20) | (1 << 27) | (1 << 28)
    jne     .no
    xor     ecx, ecx
    xgetbv                              ; XCR0 in edx:eax (legal: OSXSAVE is set)
    and     eax, 6
    cmp     eax, 6
    jne     .no
    mov     eax, 7
    xor     ecx, ecx
    cpuid
    bt      ebx, 5                      ; AVX2
    jnc     .no
    bt      ecx, 10                     ; VPCLMULQDQ
    jnc     .no
    mov     eax, 1
    pop     rbx
    ret
.no:
    xor     eax, eax
    pop     rbx
    ret

; =============================================================================
; kv_crc32c_best_mode — the fastest mode this CPU runs (what `auto` means).
;
; @returns eax = CRC_MODE_VPCLMUL (3), CRC_MODE_PCLMUL (2), CRC_MODE_SSE42
;          (1) or CRC_MODE_TABLE (0), the first whose instructions are
;          available.
; @remarks Runs up to three CPUID probes: once at startup.
; =============================================================================
kv_crc32c_best_mode:
    push    rbx                         ; keeps the mode; aligns the stack for calls
    mov     ebx, CRC_MODE_VPCLMUL
    call    kv_crc32c_vpclmul_available
    test    eax, eax
    jnz     .done
    mov     ebx, CRC_MODE_PCLMUL
    call    kv_crc32c_pclmul_available
    test    eax, eax
    jnz     .done
    mov     ebx, CRC_MODE_SSE42
    call    kv_crc32c_hw_available
    test    eax, eax
    jnz     .done
    mov     ebx, CRC_MODE_TABLE
.done:
    mov     eax, ebx
    pop     rbx
    ret

; =============================================================================
; kv_crc32c_set_mode — choose the implementation behind kv_crc32c*.
;
; @param  edi  CRC_MODE_TABLE (0), CRC_MODE_SSE42 (1), CRC_MODE_PCLMUL (2)
;              or CRC_MODE_VPCLMUL (3)
; @returns eax = the previous mode, -EINVAL for an unknown mode, or -ENODEV
;          when the mode's instructions are not available on this CPU (mode
;          unchanged in both error cases).
; @remarks Not synchronized: call it before any thread computes checksums
;          (main does, from KVASM_CRC32C). Refusing an unsupported mode is
;          what keeps a forced setting from crashing a worker with #UD at
;          the first upload: main turns -ENODEV into a startup error.
; =============================================================================
kv_crc32c_set_mode:
    cmp     edi, CRC_MODE_MAX
    ja      .bad                        ; unsigned: negative modes too
    cmp     edi, CRC_MODE_TABLE
    je      .set                        ; the tables run everywhere
    push    rdi                         ; also realigns the stack for the calls
    cmp     edi, CRC_MODE_SSE42
    je      .probe_sse42
    cmp     edi, CRC_MODE_PCLMUL
    je      .probe_pclmul
    call    kv_crc32c_vpclmul_available
    jmp     .probed
.probe_pclmul:
    call    kv_crc32c_pclmul_available
    jmp     .probed
.probe_sse42:
    call    kv_crc32c_hw_available
.probed:
    pop     rdi
    test    eax, eax
    jz      .nodev
.set:
    movzx   eax, byte [rel kv_crc32c_mode]
    mov     [rel kv_crc32c_mode], dil
    ret
.nodev:
    mov     eax, -ENODEV
    ret
.bad:
    mov     eax, -EINVAL
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
