# ADR 0009: Flush output once per event batch (a two-phase event loop)

- **Status:** accepted; amended once (see [Amendments](#amendments)):
  an early send for large batches and one `epoll_wait` per pass,
  2026-11-15
- **Date:** 2026-09-27
- **Amends:** [ADR 0003](0003-concurrency-model.md) (the worker loop)

## Context

A kernel-inclusive profile of one worker under E1's GET load
(`benchmarks/results/2026-09-26-profile-memory`) put **48% of the server's
core** in `__wake_up_common_lock`. The call graph showed where it came
from:

- The server's `sendto` → loopback transmit → softirq on the *server's*
  CPU → the client socket's `tcp_rcv_established` → `sock_def_readable` →
  waking the client thread asleep in `epoll_wait`.
- Under Hyper-V (WSL2), waking a sleeping vCPU is an IPI that exits the
  VM: about 4.5 µs per send.

The question that followed: can that be changed? A related requirement:
the server must stay event-driven, spending no CPU while it waits, in the
manner of green threads or C# async/await.

**The server already was event-driven, and never spins.**

- A connection is a state machine (h2n.asm's state in `Conn`, the gRPC
  streams) resumed by epoll readiness. That is what async/await compiles
  to, minus the heap-allocated continuations and thread hand-offs.
- With nothing ready, a worker sleeps in `epoll_wait`. Measured
  (`idleprobe.py`): 4 workers holding 1,000 idle HTTP/2 connections used
  10 ms of CPU in 10 s. They made 40 voluntary context switches (one wake
  per worker per 1 s housekeeping tick) and no syscall other than
  `epoll_wait`.
- The 48% is therefore not waiting. It is work: the kernel delivering our
  bytes to the co-located client and waking it, inside our `sendto`.

**But the loop's output order multiplied that work.**

- Each connection was flushed right after its own read: read A, process
  A, `sendto` A; then read B …
- A client thread owning several connections (kvbench: 8 threads × 8
  connections; any pooled or multiplexed client) received its responses
  ~90 µs apart. It processed one connection, fell asleep again, and was
  woken again for the next: up to once per connection per batch.
- Redis avoids this by writing all pending replies once per event-loop
  pass (`beforeSleep` → `handleClientsWithPendingWrites`).

## Decision

`worker_main` handles each epoll batch in two phases:

1. **Input:** every ready connection is read and processed.
   `conn_event` runs `kv_h2n_read`, which parses, dispatches, answers the
   GET batch and frames every response into the connection's output
   buffer. Nothing is sent. `conn_event` reports whether the connection
   survived. A destroyed one has its event entry cleared: epoll reports an
   fd once per batch, and processing one connection never touches another,
   so no other entry can dangle.
2. **Output:** every surviving, non-lingering connection of the batch is
   flushed back to back (`flush_or_close`: send, pump the pend list,
   back-pressure, linger or destroy). Then the loop sleeps.

A client thread's responses now arrive within one short burst. It wakes
once and finds the others already there.

The drain doorbell is also deferred to after phase 2. Before, processing
it mid-batch ran `begin_drain`, which can destroy a connection on a send
error. If that connection's own event was still ahead in the batch, it was
used after being freed. That latent use-after-free is gone.

What stays immediate:

- a new connection's server preface (`conn_open`);
- the idle-timeout and drain GOAWAYs (housekeeping and `begin_drain` flush
  what they queue).

## Measurements

Paired and concurrent (`ab3.py`, `benchmarks/results/2026-09-27-two-phase`):

- A (flush per connection) and B (two phases) ran at the same time.
- Each server was pinned to its own vCPU with its own client vCPU set, and
  the slots swapped every block.
- 6 blocks, fresh servers, 1M keys, 64 connections × 16 in flight, one
  core each.
- The VM ran nothing else, checked before and after, and ~7 of 32 CPUs
  were busy outside it.

Numbers are B/A geomeans with their range. "6/6" means B was better in
every block (exact sign test p = 0.031).

| Client shape | GET/s | p50 | p99 | server CPU/request | client wakeups/request |
|---|---|---|---|---|---|
| **8 threads × 8 connections** (a pooled client) | **1.257×** [1.217–1.338] 6/6: 1.81M → 2.24M | **0.799×** 6/6 | **0.790×** 6/6 | **0.796×** 6/6: 610 → 493 ns | **0.643×** 6/6: 33.3 → 21.9 per 1k |
| **16 threads × 4 connections** (the harness's kvbench) | **1.118×** [1.031–1.156] 6/6: 1.48M → 1.69M | 0.851× 6/6 | 0.857× 6/6 | 0.894× 6/6 | 0.888× (5/6) |
| 64 threads × 1 connection (control) | 1.041× [1.030–1.056] 6/6 | 0.958× 6/6 | 0.975× | 0.962× 6/6 | **1.001×** |
| busy-polling client (control) | 1.026× (5/6) | 0.922× 6/6 | 0.856× 6/6 | 0.975× (5/6) | none to save |

An earlier run of the same script gave the same picture: 8 × 8 was
1.263× [1.246–1.277] 6/6, with wakeups 0.666×. It may have overlapped
another session's benchmarks in the same VM, so it is kept only as
corroboration (`attempt1-possible-overlap/`).

The mechanism checks out, and **the gain scales with the connections a
client thread owns:**

- 8 per thread: wakeups per request −36%, throughput +26%.
- 4 per thread: −11% and +12%.
- 1 per thread: wakeups identical (exactly one per send) and +4%.
- No sleeping clients: nothing to save.

The +3–4% the controls keep is phase locality: all parsing happens first,
then all sending, which keeps each phase's code and data warm.

## Consequences

- One core serves more GETs with lower p50 and p99. How many more depends
  on the clients: +12% with the harness's 4 connections per client thread,
  +26% with 8, +4% with 1. The official one-core comparison was re-run on
  this revision (`benchmarks/results/2026-09-27-two-phase`).
- **Latency bound:** a response now waits until its batch's input is
  processed, at most `MAX_EVENTS` (256) connections of at most one
  `READ_CHUNK` each.
  - On a saturated core that queueing existed anyway, and the measured p50
    and p99 fell.
  - At low load a batch is a handful of events.
- **What remains is inherent to co-located, sleeping clients.** Each
  sleeping client thread must be woken once per burst. B still pays ~22
  wakeups per 1,000 requests with 8 connections per thread. Against a
  busy-polling client that is ~125–210 ns of server CPU per request,
  depending on host load.
  - Remote clients cost the server nothing of the kind: their NIC and
    kernel wake them on their own CPUs.
  - Co-located clients that spin before sleeping, or busy-poll, avoid the
    rest.
- Housekeeping and drain behave as before. The drain now starts at the
  end of the batch that carried the doorbell, microseconds later, and the
  mid-batch use-after-free cannot happen.

## Alternatives rejected

- **A dedicated sender thread** would move the send work to another core,
  not remove it. The per-core efficiency, which is the goal, gets worse.
- **Delaying sends across batches** (Nagle-like timers) adds latency on an
  idle server for no gain on a busy one. One batch is already the natural
  unit.
- **RPS/RFS on loopback** would move the peer's receive processing to other
  CPUs through the same kind of IPI. It is also host configuration, not a
  server design choice.

## Amendments

### An early send for large batches, and one `epoll_wait` per pass (2026-11-15)

**1. One exception to "nothing is sent until the whole batch has been
read".** When a connection's queued replies plus the next one would pass
the 64 KiB high-water mark, the queue is sent at once
([ADR 0007](0007-native-http2-engine.md), amendment of the same date).

- Why it does not undo this decision: the gain here came from a client
  thread being woken once per batch instead of once per connection. A
  connection that has more than 64 KiB to deliver wakes its reader
  whenever that data arrives, and the reader stays busy reading it. What
  the early send avoids is copying every further reply of the read into a
  private buffer.
- Small-reply batches never reach the mark (16 replies of 139 bytes are
  2.2 KiB), so the workloads measured above are untouched: the in-process
  request path and its wire bytes are identical for them.
- The early send does none of the flush's other work — no pump, no epoll
  interest change, no close. Phase 2 still runs for the connection and
  finds an empty buffer or the unsent rest.

**2. The loop enters its one `epoll_wait` with the real timeout.** It used
to poll first (timeout 0) and block only when nothing was ready, going
QSBR-offline only around the blocking call.

- The poll saved the rejoin's `XCHG` on a busy pass and cost a whole
  syscall before every sleep. Measured: 95.0 ns per empty `epoll_wait`
  against 8.4 ns for the offline/online pair
  (`benchmarks/results/2026-11-15-asm-audit/syscount.out`).
- Now every pass announces offline, waits (1 s, or 100 ms while
  draining), and comes back online. An offline announcement is also a
  quiescent state, so `kv_qsbr_quiescent` is no longer called by the loop,
  and reclamation never waits on a worker that is inside `epoll_wait`.
- Counted under an open-loop, unpipelined load (`strace -c`): `epoll_wait`
  calls per request 1.46 → 0.85, all syscalls 3.58 → 2.97.
- Timed where the worker never sleeps, so only the added `XCHG` could
  show: server CPU per GET 1.008× with blocking clients at saturation
  (3 of 6 blocks) and 0.995× with one request in flight under
  busy-polling clients (5 of 8) — no difference either way.
- **Not shown:** an end-to-end gain when the worker does sleep. On the
  test VM an unpipelined request pays about 20 µs of cross-vCPU wakeup,
  which hides 90 ns. The claim is the counted syscall.
