; =============================================================================
; pbstream.inc — the resumable decoder of gRPC-framed protobuf message
; streams (proto.asm kv_pb_stream_feed), shared with its users (grpc.asm's
; Upload). A layout change here is an ABI change for tests/test_proto.c,
; which mirrors PbStream and the event codes.
;
; Why a streaming decoder: a client-streaming call (Upload) is an unbounded
; sequence of messages that arrives in DATA frames split at arbitrary bytes.
; Buffering each whole message first would copy every chunk twice and hold a
; message-sized buffer per stream. This decoder instead keeps a few bytes of
; state per stream and reports each field as an event the moment its bytes
; are complete — and hands over length-delimited payloads (the upload's data)
; as pointers into the caller's buffer, piece by piece, so the caller can
; copy them straight to their final place.
; =============================================================================
%ifndef PBSTREAM_INC
%define PBSTREAM_INC

; ---- decoder states (PbStream.state) -------------------------------------------
%define PSS_PREFIX          0           ; collecting the 5-byte gRPC message prefix
%define PSS_TAG             1           ; collecting a field tag varint
%define PSS_VARINT          2           ; collecting a wire-type-0 value varint
%define PSS_LEN             3           ; collecting a wire-type-2 length varint
%define PSS_BYTES           4           ; passing a length-delimited payload through
%define PSS_FIXED           5           ; collecting a fixed32/fixed64 value

; ---- events reported to the handler (esi), with their arguments ----------------
;   handler(ctx rdi, event esi, tag rdx, a1 rcx, a2 r8) -> eax 0 to continue,
;   or a negative code that stops decoding and is returned by the feed.
%define PBE_MSG_BEGIN       1           ; rcx = message length (prefix excluded)
%define PBE_VARINT          2           ; rdx = tag, rcx = value
%define PBE_FIXED32         3           ; rdx = tag, rcx = value (zero-extended)
%define PBE_FIXED64         4           ; rdx = tag, rcx = value
%define PBE_BYTES_BEGIN     5           ; rdx = tag, rcx = payload length
%define PBE_BYTES           6           ; rdx = tag, rcx = pointer, r8 = length (> 0)
%define PBE_BYTES_END       7           ; rdx = tag
%define PBE_MSG_END         8

; ---- decoder errors (negative returns of kv_pb_stream_feed) ---------------------
%define PB_ERR_MALFORMED    (-1)        ; bad varint, field 0, tag >= 2^32, wire type
                                        ; 3/4/6/7, or a field running past its message
%define PB_ERR_COMPRESSED   (-2)        ; message prefix flag 1 (compressed)
%define PB_ERR_FLAG         (-3)        ; message prefix flag > 1 (undefined)

; ---- PbStream: one stream's decoder state (zeroed = at a message boundary) -------
struc PbStream
    .state:     resq 1                  ; PSS_*
    .mleft:     resq 1                  ; bytes left in the current message
    .tag:       resq 1                  ; the field being decoded (number << 3 | type)
    .fleft:     resq 1                  ; PSS_BYTES: payload left; PSS_FIXED: bytes to collect
    .hlen:      resq 1                  ; bytes collected in hbuf
    .msgs:      resq 1                  ; messages whose prefix was complete
    .hbuf:      resb 16                 ; prefix (5), varint (<= 10) or fixed (4/8) bytes
endstruc

%endif
