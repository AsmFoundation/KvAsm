; =============================================================================
; log.inc — LogBuf layout shared by log.asm and every caller that reserves a
; LogBuf inside a fixed stack frame.
;
; Why an include instead of only kv_logbuf_size(): a frame laid out with
; %define offsets (main.asm) must know the size at assembly time. Sharing the
; struc makes a LOG_CAP change resize those frames automatically, and the
; %error guards in the users turn a too-small slot into a build failure
; instead of a silent stack overwrite. Callers with dynamic frames
; (config.asm, snapshot.asm) may keep using kv_logbuf_size().
; =============================================================================

%ifndef KVASM_LOG_INC
%define KVASM_LOG_INC

%define LOG_CAP         1000            ; payload bytes per line

; One log line under construction. len counts bytes in data; appends never
; write past LOG_CAP (the extra 24 bytes hold the newline and let kv_fmt_u64
; render a full 20-digit number before the truncation check).
struc LogBuf
    .len:       resq 1
    .data:      resb LOG_CAP + 24
endstruc

; Bytes to reserve for one LogBuf, rounded to 16 so a frame that places it
; last stays ABI-aligned for calls. Equals kv_logbuf_size()'s return value.
LOGBUF_BYTES    equ (LogBuf_size + 15) & ~15

%endif
