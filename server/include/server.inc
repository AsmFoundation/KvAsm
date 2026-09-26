; =============================================================================
; server.inc — structures shared by the server modules (config, worker,
; connection, gRPC stream) and their tunables.
;
; Ownership rules (the whole concurrency story of the network layer):
;   * Config and the nghttp2 callback table are built by main before any
;     worker starts and are read-only afterwards — shared by all threads.
;   * Everything else (Worker, Conn, Stream, nghttp2 sessions) belongs to
;     exactly one worker thread for its whole life; no locks are needed and
;     none are taken. Workers share only the store, which has its own
;     concurrency protocol (store.asm).
; =============================================================================
%ifndef SERVER_INC
%define SERVER_INC

%define READ_CHUNK          65536       ; bytes per recv() into the worker buffer
%define OUT_INITIAL         65536       ; initial per-connection output buffer
%define OUT_HIGH_WATER      65536       ; stop pulling from nghttp2 above this
%define MAX_EVENTS          256         ; epoll events per wait
%define GRPC_PREFIX         5           ; compressed flag + 4-byte length

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
endstruc

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
    .callbacks:         resq 1          ; nghttp2_session_callbacks* (shared)
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
    .pool:              resb Pool_size  ; this worker's allocator (sessions, streams)
    .mem:               resq 5          ; nghttp2_mem {user_data, malloc, free, calloc, realloc}
endstruc

; ---- Conn: one HTTP/2 connection ------------------------------------------------
%define CONN_WANT_OUT       1           ; epoll interest is EPOLLOUT (output pending)
%define CONN_LINGER         2           ; session done, write side shut, draining input
struc Conn
    .fd:                resq 1
    .flags:             resq 1
    .session:           resq 1          ; nghttp2_session*
    .worker:            resq 1          ; owning Worker*
    .streams:           resq 1          ; list of live Stream
    .out_buf:           resq 1
    .out_cap:           resq 1
    .out_len:           resq 1          ; bytes queued in out_buf
    .out_off:           resq 1          ; bytes of out_buf already sent
    .buffered:          resq 1          ; request bytes buffered across streams
    .last_active:       resq 1          ; monotonic ns of the last received bytes
    .prev:              resq 1
    .next:              resq 1
    ; Stream cache: nghttp2 callbacks name streams by id, and resolving an id
    ; is a hash-map lookup; the headers and data of one request arrive
    ; back to back, so remembering the last stream answers almost every call.
    .cur_id:            resq 1          ; id of cur_stream
    .cur_stream:        resq 1          ; Stream* or 0 (cleared when it closes)
    ; GETs parsed from the current read, answered after it (prefetch batch).
    .batch_head:        resq 1
    .batch_tail:        resq 1
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
; Queued in Conn.batch: answered by kv_grpc_run_batch after the current read.
%define ST_BATCHED          64
; The peer reset a batched stream: run_batch frees it instead of answering.
%define ST_CLOSED           128
%define M_UNKNOWN           0
%define M_GET               1
%define M_PUT               2
%define M_DELETE            3
%define M_HEALTH            4           ; grpc.health.v1.Health/Check
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
    ; A batched GET (ST_BATCHED) keeps what its deferred lookup needs.
    .key:               resq 1          ; points into req_buf
    .key_len:           resq 1
    .h1:                resq 1
    .h2:                resq 1
    .batch_next:        resq 1
endstruc

%endif
