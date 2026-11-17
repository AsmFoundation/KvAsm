; =============================================================================
; server.inc — structures shared by the server modules (config, worker,
; connection, gRPC stream) and their tunables.
;
; Ownership rules (the whole concurrency story of the network layer):
;   * Config and the HPACK/Huffman tables (kv_hpack_global_init) are built by
;     main before any worker starts and are read-only afterwards — shared by
;     all threads.
;   * Everything else (Worker, Conn with its HTTP/2 state, Stream) belongs to
;     exactly one worker thread for its whole life; no locks are needed and
;     none are taken. Workers share only the store, which has its own
;     concurrency protocol (store.asm).
; =============================================================================
%ifndef SERVER_INC
%define SERVER_INC

%include "hpack.inc"                    ; HpackDec, embedded in the HTTP/2 engine's H2

; Bytes per read() into the worker's buffer. 256 KiB, measured (2026-09-30,
; benchmarks/results/2026-09-30-streaming): against 64 KiB it lets one read
; complete several uploads, whose responses then leave in one send instead
; of one each (upload of 64 KiB values 1.87x; the partial-frame carry and
; the reads per byte shrink 4x too), while 512 KiB and 1 MiB lost 7-17% on
; large uploads (the buffer no longer stays in L2 beside the socket data the
; kernel copies in); GETs are flat across all four sizes.
%define READ_CHUNK          262144
%define OUT_INITIAL         65536       ; initial per-connection output buffer
%define OUT_HIGH_WATER      65536       ; stop producing output above this
; Fairness of long responses (Download): bytes one connection may frame from
; its pend list in one flush before worker.asm yields to the other
; connections of the worker (it resumes on the next loop pass). Without it
; a fast local reader of a 64 MiB download would keep the worker in one
; flush for tens of milliseconds while every other connection waits.
%define FLUSH_BUDGET        262144
%define MAX_EVENTS          256         ; epoll events per wait
%define GRPC_PREFIX         5           ; compressed flag + 4-byte length
; HTTP/2 flow control: the per-stream receive window we grant (large values
; need no WINDOW_UPDATE round trips) and the protocol's window ceiling
; (RFC 9113 §6.9.1).
%define STREAM_WINDOW       1048576
%define MAX_WINDOW          0x7FFFFFFF
; RFC 9113 §7 error code carried by a graceful GOAWAY (drain, idle timeout).
%define H2_NO_ERROR         0

; ---- Config: validated settings (see config.asm for the environment names) ---
struc Config
    .port:              resq 1
    .workers:           resq 1
    .capacity:          resq 1
    .max_key:           resq 1
    .max_value:         resq 1
    .max_memory:        resq 1          ; bytes, 0 = unlimited
    .max_streams:       resq 1          ; per connection
    .max_conns:         resq 1          ; per worker
    .idle_timeout_ns:   resq 1          ; 0 = never
    .grace_ns:          resq 1          ; shutdown drain deadline
    .conn_budget:       resq 1          ; buffered request bytes per connection
    .snapshot:          resq 1          ; char* path or 0
    .max_request:       resq 1          ; derived: largest acceptable gRPC message
    ; appended fields (C tests read Config only through kv_config_abi offsets)
    .scan_mode:         resq 1          ; KVASM_SCAN: 0 = swar, 1 = sse2 (kv_set_scan_mode)
    .cache_l1_bits:     resq 1          ; KVASM_CACHE_L1_BITS: 0 = no private L1
    .cache_l2_bits:     resq 1          ; KVASM_CACHE_L2_BITS: 0 = no private L2
    .max_upload:        resq 1          ; KVASM_MAX_UPLOAD_BYTES: largest value Upload stores
    .crc_mode:          resq 1          ; KVASM_CRC32C: a CRC_CFG_* word index
    .hash_mode:         resq 1          ; KVASM_HASH: a HASH_CFG_* word index
endstruc

; KVASM_HASH words (config.asm) — the index stored in Config.hash_mode.
; main.asm maps them to store.asm's KV_HASH_* modes.
%define HASH_CFG_AUTO       0
%define HASH_CFG_SIPHASH    1
%define HASH_CFG_AES        2

; KVASM_CRC32C words (config.asm) — the index stored in Config.crc_mode.
; New words go at the end: the indices are what main.asm maps to modes.
%define CRC_CFG_AUTO        0
%define CRC_CFG_SSE42       1
%define CRC_CFG_TABLE       2
%define CRC_CFG_PCLMUL      3
%define CRC_CFG_VPCLMUL     4

%define INVAL_BITS_DEFAULT  16          ; generation slots when private caches are on

; ---- Worker: one epoll event loop, pinned to nothing, owns its connections ----
; ---- per-worker allocator (pool.asm) ------------------------------------------
%define POOL_HDR            16          ; block header; keeps 16-byte alignment
%define POOL_MIN_SHIFT      6           ; smallest block: 64 bytes (header included)
%define POOL_CLASSES        7           ; 64, 128, ..., 4096
%define POOL_MAX_BLOCK      4096
%define POOL_CHUNK          (256 * 1024)
%define POOL_LARGE          0xFF        ; header class of a malloc'd block

struc Pool
    .free:              resq POOL_CLASSES ; LIFO free list per class
    .bump:              resq 1          ; next unused byte of the current chunk
    .bump_end:          resq 1
    .chunks:            resq 1          ; list of chunks (freed by kv_pool_destroy)
endstruc

struc Worker
    .id:                resq 1          ; also the QSBR reader slot
    .epfd:              resq 1
    .listen_fd:         resq 1          ; -1 once closed (draining)
    .event_fd:          resq 1          ; wake-up from main (shutdown)
    .table:             resq 1          ; KvTable* (shared)
    .cfg:               resq 1          ; Config* (shared, read-only)
    .conns:             resq 1          ; doubly linked list of Conn
    .nconns:            resq 1
    .draining:          resq 1          ; 1 after shutdown was requested
    .drain_deadline:    resq 1          ; monotonic ns
    .next_tick:         resq 1          ; monotonic ns of the next housekeeping
    .read_buf:          resq 1          ; READ_CHUNK bytes
    .events:            resq 1          ; epoll_event[MAX_EVENTS]
    .thread:            resq 1          ; pthread_t
    .st_accepted:       resq 1
    .st_requests:       resq 1
    .st_errors:         resq 1
    .now:               resq 1          ; monotonic ns, refreshed once per loop pass
    .accept_paused:     resq 1          ; 1 while out of fds (listener unregistered)
    .st_rejected:       resq 1          ; connections refused (limit or fds)
    .pool:              resb Pool_size  ; this worker's allocator (streams, maps, HPACK rings)
    .hcache:            resq 1          ; private L1/L2 record cache (hcache.asm), or 0
    ; Huffman output of the HPACK decoder (h2n.asm): decoding is synchronous,
    ; so one buffer serves every connection of the worker. malloc'd by the
    ; worker's first connection, freed with the worker.
    .h2_scratch:        resq 1
endstruc

; ---- H2: the HTTP/2 engine's per-connection state (h2n.asm, ADR 0007) ---------
; Embedded in Conn (no extra allocation or pointer chase on the hot path).
%define H2_RING             32          ; remembered ids per ring (power of two)
%define H2_HC_MAX           128         ; largest request header block cached
struc H2
    .state:             resq 1          ; preface / first SETTINGS / open (h2n.asm)
    .flags:             resq 1          ; H2F_*: closing, GOAWAY sent/received, ...
    .last_id:           resq 1          ; highest client stream id opened or refused
    .nstreams:          resq 1          ; streams in the map (count toward the limit)
    .send_window:       resq 1          ; signed: connection window for our DATA
    .recv_window:       resq 1          ; signed: what the peer may still send us
    .conn_window:       resq 1          ; target connection receive window
    .peer_window:       resq 1          ; peer's SETTINGS_INITIAL_WINDOW_SIZE
    .peer_frame:        resq 1          ; peer's SETTINGS_MAX_FRAME_SIZE
    .enc:               resq 1          ; response HPACK encoder state (H2E_*)
    .cont_id:           resq 1          ; stream of the header block being continued
    .cont_flags:        resq 1          ; END_STREAM / self-dependency of its HEADERS
    .cont_count:        resq 1          ; CONTINUATION frames in that block
    .hb_buf:            resq 1          ; header block assembly (pool block)
    .hb_len:            resq 1
    .hb_cap:            resq 1
    .carry:             resq 1          ; partial frame kept for the next read (pool)
    .carry_len:         resq 1
    .carry_cap:         resq 1
    .map:               resq 1          ; Stream*[map_mask + 1], open addressing by id
    .map_mask:          resq 1
    .pend_head:         resq 1          ; streams with response bytes waiting for
    .pend_tail:         resq 1          ;   flow-control window or buffer room
    .reset_tokens:      resq 1          ; rapid-reset budget (CVE-2023-44487)
    .reset_stamp:       resq 1          ; Worker.now of the last refill
    .rst_pos:           resq 1          ; next slot of rst_ring
    .closed_pos:        resq 1          ; next slot of closed_ring
    .rst_ring:          resd H2_RING    ; streams WE reset: their late frames are ignored
    .closed_ring:       resd H2_RING    ; streams closed normally or by the peer
    .hpack:             resb HpackDec_size ; this connection's HPACK decoder
    .map_displaced:     resq 1          ; map entries outside their home slot
    ; Request header-block cache (h2n.asm decode_fields): gRPC clients send
    ; byte-identical blocks for identical calls once their HPACK table is
    ; warm. A block equal to hc_bytes under the same table generation
    ; decodes to the same fields, so its recorded effects are replayed
    ; instead of decoding (one compare instead of HPACK + Huffman + checks).
    .hc_len:            resq 1          ; cached block length, 0 = empty
    .hc_gen:            resq 1          ; HpackDec.gen the entry is valid for
    .hc_flags:          resq 1          ; Stream.flags bits the block set
    .hc_method:         resq 1          ; Stream.method (route) it set
    .hc_h2flags:        resq 1          ; Stream.h2flags bits it set (HEAD)
    .hc_clen:           resq 1          ; Stream.clen it set (-1: none)
    .hc_result:         resq 1          ; decode_fields result: 0 ok, 1 malformed
    .hc_bytes:          resb H2_HC_MAX
endstruc

; ---- Conn: one HTTP/2 connection ------------------------------------------------
%define CONN_WANT_OUT       1           ; epoll interest is EPOLLOUT (output pending)
%define CONN_LINGER         2           ; session done, write side shut, draining input
; epoll interest is EPOLLIN | EPOLLOUT: a flush stopped at FLUSH_BUDGET with
; the socket still writable, so it resumes on the next pass — while input
; (WINDOW_UPDATE, RST_STREAM, new calls) is still read. Distinct from
; CONN_WANT_OUT, which means the socket is full and the connection must stop
; reading until the peer drains it (back-pressure).
%define CONN_WANT_BOTH      4
; The idle timeout expired once and its GOAWAY was queued (terminate mode).
; If the timeout expires AGAIN with no byte moved since, the peer has stopped
; reading at the TCP level — its receive window stays 0, so not even the
; GOAWAY can leave — and housekeeping destroys the connection outright.
; Without this, such a peer would hold its streams forever: a Download's
; pinned record (up to KVASM_MAX_UPLOAD_BYTES of memory), an Upload's
; reservation, a unary response's buffer.
%define CONN_IDLE_CLOSING   8
struc Conn
    .fd:                resq 1
    .flags:             resq 1
    .worker:            resq 1          ; owning Worker*
    .streams:           resq 1          ; list of live Stream
    .out_buf:           resq 1
    .out_cap:           resq 1
    .out_len:           resq 1          ; bytes queued in out_buf
    .out_off:           resq 1          ; bytes of out_buf already sent
    .buffered:          resq 1          ; request bytes buffered across streams
    .last_active:       resq 1          ; monotonic ns of the last bytes received,
                                        ; or sent (conn_flush: a long Download is
                                        ; progress even when the peer says nothing)
    .prev:              resq 1
    .next:              resq 1
    ; GETs parsed from the current read, answered after it (prefetch batch).
    .batch_head:        resq 1
    .batch_tail:        resq 1
    .h2:                resb H2_size    ; HTTP/2 engine state (h2n.asm)
    ; Value bytes reserved by this connection's Uploads in progress (their
    ; declared sizes), bounded by Config.conn_budget: a client cannot pin more
    ; memory than one connection's budget by declaring uploads it never sends.
    .up_bytes:          resq 1
endstruc

; ---- HdrField: one response header field, for the error-path replies ----------
; grpc.asm's static header tables (Trailers-Only gRPC errors, 405/415) are
; arrays of these, handed to kv_h2n_reply_headers / kv_h2n_reply_static;
; h2n.asm's encode_nv HPACK-encodes them. Strings are not NUL-terminated.
struc HdrField
    .name:              resq 1          ; const char*
    .value:             resq 1          ; const char*
    .namelen:           resq 1
    .valuelen:          resq 1
endstruc

; ---- Stream: one gRPC call --------------------------------------------------------
%define ST_POST             1           ; :method was POST
%define ST_CT_OK            2           ; content-type starts with application/grpc
%define ST_TOO_LARGE        4           ; request exceeded max_request: data dropped
%define ST_ENCODED          8           ; grpc-encoding other than identity
%define ST_RESPONDED        16          ; a response was submitted
; resp_buf points at static read-only text (never freed), and the response
; ends on its last DATA frame: a plain HTTP error (405/415), no gRPC trailers.
%define ST_STATIC_RESP      32
; Queued in Conn.batch: executed and answered by kv_grpc_run_batch after the
; current read (Get, Put and Delete; ADR 0005, ADR 0011).
%define ST_BATCHED          64
; The peer reset a batched stream: run_batch frees it instead of answering.
%define ST_CLOSED           128
; req_buf points into the worker's read buffer instead of a pool block (the
; native engine's zero-copy path for a body that arrived whole in one DATA
; frame): never freed, never counted in Conn.buffered, and valid only until
; the read that carried it has been fully processed — which covers dispatch
; and the batch (Get, Put, Delete) executed right after it.
%define ST_BORROWED         256
; The response body is generated on demand by kv_grpc_gen_fill instead of
; being read from resp_buf (Download: the value is framed straight from its
; pinned record as windows open). resp_len is the body's exact total length.
%define ST_GEN              512
%define M_UNKNOWN           0
%define M_GET               1
%define M_PUT               2
%define M_DELETE            3
%define M_HEALTH            4           ; grpc.health.v1.Health/Check
%define M_UPLOAD            5           ; client streaming: DATA consumed as it arrives
%define M_DOWNLOAD          6           ; server streaming: body generated (ST_GEN)
struc Stream
    .next:              resq 1
    .prev:              resq 1
    .conn:              resq 1
    .id:                resq 1          ; HTTP/2 stream id
    .flags:             resq 1
    .method:            resq 1
    .req_buf:           resq 1
    .req_len:           resq 1
    .req_cap:           resq 1
    .resp_buf:          resq 1          ; complete gRPC response message (prefix + protobuf)
    .resp_len:          resq 1
    .resp_off:          resq 1
    ; A batched call (ST_BATCHED: Get, Put or Delete) keeps what its deferred
    ; store operation needs.
    .key:               resq 1          ; points into req_buf (Get, Delete)
    .key_len:           resq 1
    .h1:                resq 1
    .h2:                resq 1
    ; The key's two candidate buckets (kv_store_locate at dispatch): the batch
    ; prefetches their records and a Get looks the key up through them, so
    ; the addresses are derived from (h1, h2) once instead of three times.
    .b1:                resq 1
    .b2:                resq 1
    .batch_next:        resq 1
    ; A batched Put's record, allocated and filled at dispatch, owned by the
    ; Stream until kv_grpc_run_batch publishes it (then 0). stream_free
    ; returns an unpublished one (kv_store_rec_free), so a stream reset or a
    ; connection lost before its batch ran stores nothing and leaks nothing.
    .wrec:              resq 1
    ; A batched Put's or Delete's outcome, set under the writer mutex and
    ; answered after it is released (kv_store_put_locked/_delete_locked).
    .wres:              resq 1
    ; HTTP/2 stream state, owned by h2n.asm.
    .send_window:       resq 1          ; signed: what the peer lets us send here
    .recv_window:       resq 1          ; signed: what we still let the peer send
    .h2flags:           resq 1          ; SN_* stream state bits (h2n.asm)
    .clen:              resq 1          ; declared content-length, or -1
    .data_len:          resq 1          ; DATA payload bytes received so far
    .pend_next:         resq 1          ; H2.pend list link
    ; Streaming calls only (Upload/Download): their transfer state (grpc.asm
    ; Xfer), or 0. A separate object so that unary calls — nearly every
    ; stream — keep their small Stream (still one 256-byte pool block).
    ; stream_free releases it: aborting an upload, unpinning a download.
    .xfer:              resq 1
endstruc

%endif
