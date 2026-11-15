# Second pass: what is left to improve, and why wakeups at all (2026-09-27)

Two questions:

- "Do a second pass, what else can we improve?"
- "Why are we at all spending time on wakeup? Isn't this all event driven,
  so we just 'enter' into the action?"

**Host:** i9-13900K, WSL2. During this pass another project's job
(20 to 36 `python.exe` processes) kept **25 to 30 of
32 logical CPUs** busy outside the VM, so timing measurements were
meaningless. Everything below is therefore a **count**, which host load
cannot change:

- context switches and system calls per request;
- exact user-space instructions per request, under callgrind.

Timing follow-ups for a quiet host are listed at the end.

## 1. Why an event-driven server still pays for wakeups

Event-driven means a thread with nothing to do *sleeps* in `epoll_wait`
instead of spinning. That is why an idle KvNode uses no CPU (measured the
same day: 4 workers, 1,000 idle connections, 10 ms of CPU in 10 s). The
price of sleeping is that someone must wake you. A sleeping thread is on no
CPU at all. When its event arrives, the kernel must mark it runnable,
queue it on a CPU and, if that CPU is idle, interrupt it (an IPI) so it
switches to the thread.

`loadprobe.sh` counts both sides' voluntary context switches under E1's
load (one worker, 16 client threads × 64 connections). Every such switch
is a block-then-wake:

| per 1,000 requests | wakeups |
|---|---|
| **KvNode's worker** | **0.00** |
| kvbench's threads (the client) | ~27 |

So **our worker already just enters the action.** Under load it never
sleeps: each `epoll_wait` returns the next batch at once.

The wakeups on our profile are the **client's**. kvbench is event-driven
too, so its threads sleep between responses. On loopback there is no
network card between the two processes. Our `sendto` therefore runs the
client socket's receive path in softirq on *our* CPU, and it ends by waking
the client thread. Under Hyper-V that wakeup of a halted vCPU exits the VM
and costs microseconds.

With a real network, `sendto` hands the packet to the NIC and returns. The
client's NIC and kernel wake the client on the client's machine, and
KvNode's CPU never sees it.

There are three ways to make it zero:

- **remote clients** (production);
- **clients that don't sleep while they wait:** in the same session, a
  busy-polling client took KvNode from 582 to 382 ns per GET
  (`../2026-09-26-profile-memory`);
- **kernel RPS/RFS on loopback,** which only moves the work to another CPU,
  via its own IPI.

The one server-side lever was to send a batch's output in one burst, so a
client thread wakes once per batch and not once per connection. That is
done (ADR 0009, +12–26% by client shape).

System calls per request are already minimal. Under the same load,
counted with kernel tracepoints (which slowed the server, so only the
ratios count), there was **one `read` and one `sendto` per connection
event, i.e. per 16 requests**, and one `epoll_wait` per ~50 connection
events.

## 2. Exact instructions per request (callgrind)

**Method** (`cgrun.sh`, `cgcount.py`, `cgprofile.py`):

- The server runs under callgrind in a helper image (the toolchain plus
  valgrind), sharing the dev container's network, so kvbench can reach it.
- `callgrind_control` zeroes the counters after a 20k-key preload, and
  dumps after an 8 s workload. `vgdb` needs `--cap-add=SYS_PTRACE`.
- The instruction total is divided by the exact number of requests: calls
  of `get_execute`, `kv_store_put`, or the ping replier.
- Callgrind reads KvNode's DWARF lines but not its function names: NASM
  emits no function entries, so functions appear as addresses. The scripts
  map them with `nm -n`.
- A function entered by a tail `jmp` is charged to its caller. So
  `request_end` includes `kv_grpc_dispatch`, and `kv_store_hash` includes
  SipHash.

`instruction-profile.txt`, final tree. User-space instructions per **GET:
1,425**:

| Part | Instructions | Share | Where |
|---|---|---|---|
| **SipHash-2-4-128 of the key** | 254 | 18% | `kv_store_hash` |
| HTTP/2 frame parsing | ~280 | 20% | `h2_process` 155, `header_block` 59, `decode_fields` 45, `blocks_equal` 21 |
| response framing | ~230 | 16% | `reply_fast` 85, three frame headers and two tiny HPACK blocks 90, `kv_h2n_reply_parts` 26, the value copy 21 |
| stream lifecycle | ~210 | 15% | open with zeroing, map insert and remove, close, unlink, free |
| store lookup | ~160 | 11% | `kv_store_get` 77, the two prefetch stages 67, key compare 18 |
| gRPC and protobuf | ~150 | 11% | dispatch and batch 112, protobuf decode 72 |

Instructions are not time. SipHash runs at a high IPC, while the store's
160 instructions cost ~60 ns in stalls. On a quiet host
(`../2026-09-27-store-lookup`), one GET is 269 ns of server time: 139 ns
user, of which the store is 69, and 130 ns kernel.

## 3. What changed

**PUT, DELETE and health replies no longer allocate** (`grpc.asm`
`.bool_reply`). Their whole message is 5 or 7 bytes. It used to be put in a
pool block, framed from there, and freed with the stream. Now it is built
on the stack and framed by `kv_h2n_reply_parts`, exactly like a GET miss.
`stream_free` also skips absent buffers instead of calling `kv_pool_free`
on NULL.

`instructions-per-request.txt`:

| per request | before | after |
|---|---|---|
| GET | 1,428 | 1,425 |
| **SET** | 1,812 | **1,789** (−23), pool calls 2+2 → 1+1 |
| **health check (N3's no-op)** | 867 | **846** (−21, −2.4%), pool calls 2+2 → 1+1 |

One protocol-visible consequence: the Trailers-Only `INTERNAL` "server out
of memory" reply is gone. If the server cannot frame or buffer any reply,
it now closes the connection, as it already did for GETs.
`docs/PROTOCOL.md` says so.

**Measured and rejected:** zeroing each 184-byte Stream inline (12
SSE2 stores) instead of `kv_pool_calloc`'s glibc `memset`. It saved
nothing (a GET was 1,424 instructions with it against 1,428 before),
because glibc's AVX2 memset is already cheap at this size. Reverted, and
the finding is noted at the call.

**A test fixed:** `make itest` phase 2 slept 0.3 s before SIGTERM and
assumed both drain clients had connected by then. With 30 CPUs busy that
race was lost once: the in-flight stream arrived after GOAWAY
(`last_stream=0`) and was never accepted.

The clients now send a PING after setting up their connection and stream,
and print `ready` on its ACK. The server processes a connection's frames
in order, and the helper puts queued frames on the wire before the PING,
because nghttp2 would otherwise send the PING first. `run.sh` waits for
`ready`.

## 4. What is left, by measured size

These use the server's own cost on a quiet host, a busy-polling client,
269 ns per GET, unless noted.

1. **Store memory stalls, 69 ns.**
   - A 2 MiB record slab is +6% throughput (measured with glibc tunables,
     `../2026-09-27-store-lookup`), and it also saves the 20 bytes of
     malloc overhead per 140-byte record.
   - On hosts that back guest memory with huge pages, the page-walk stalls
     mostly vanish.
2. **Kernel TCP/IP, 65 ns, plus the socket-lookup and softirq share of
   "other", 23 ns.**
   - For co-located clients, a **Unix-domain-socket listener** would skip
     the TCP/IP stack on both sides: an estimated ~50–60 ns of server time
     (~20%).
   - For remote clients this cost is inherent.
3. **SipHash, 254 instructions, ~12 ns.**
   - **SipHash-1-3** has 9 rounds against 14, so about −90 instructions
     and ~−4 ns. Rust's HashMap and CPython use it for the same threat
     model. It amends ADR 0004, so it is your call.
   - An **AVX2 4-lane** batch across a read would save ~5–8 ns, but needs
     the batch restructured.
4. **Writes.**
   - A SET is only 26% more instructions than a GET (1,789 vs 1,425), yet
     it took ~4× the time in the saturated probe. Its bucket and old
     record are cold, and PUTs execute at dispatch, outside the GET batch's
     prefetch pipeline.
   - Batching PUTs like GETs would prefetch at parse time and apply them
     under one writer-lock hold per read. That is the analogue of the +28%
     that batching gave GETs (ADR 0005), and it would also cut writer-lock
     traffic at 4+ cores.
   - This needs a quiet-host measurement first: the last published write
     numbers predate the native HTTP/2 engine.
5. **System calls, 13 ns, plus copies, 11 ns.**
   - An opt-in io_uring backend would submit phase 2's sends as one batch.
   - Docker's default seccomp profile blocks io_uring, so it cannot be the
     default.
6. **HTTP/2 and gRPC code.**
   - Fused fast paths for response framing and unary requests: an
     estimated −100 to −150 instructions per GET (~5–10 ns).
   - This is the most protocol-critical code: every change must keep
     h2spec, the fuzzer and grpc-dotnet green.
7. **The last periodic wakeup.** A worker wakes once per second for
   housekeeping, even when idle. Making it deadline-driven (sleep until
   the next idle timeout, linger deadline or pending reclamation) would
   remove it. That saves 0.08% of a core, so it is listed, not done.

## Files

| File | What |
|---|---|
| `cgrun.sh` | one callgrind measurement: variant × workload (get, set, ping) |
| `cgcount.py` | instructions per request and per-request call counts from a dump |
| `cgprofile.py` | self instructions per function per request |
| `instructions-per-request.txt`, `instruction-profile.txt` | the results above |
| `callgrind-{before,after}-{get,set,ping}.out`, `kvnode-{before,after}.nm` | the raw dumps and the symbol tables that name their addresses |
| `loadprobe.sh` | server and client wakeups per request under load |

## To run on a quiet host

- **Timing of this pass's change:** expected 1–2% on SET and health-check
  throughput. Check it with a concurrent A/B.
- **The write path:** SET throughput at 1 and 4 cores on today's server,
  then write batching behind a concurrent A/B.
- **The two-phase loop (ADR 0009) at 2 and 4 cores.**
- **SipHash-1-3, if you want it:** an A/B plus the differential test
  against a C reference.
