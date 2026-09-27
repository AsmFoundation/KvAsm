# The two-phase event loop: fewer client wakeups (2026-09-27)

This session answers two questions about the 48% of a loaded core that the
previous profile put in `__wake_up_common_lock`, the server's `sendto`
waking the co-located client (`../2026-09-26-profile-memory`):

- "Can we change that?"
- Is KvNode event-driven, spending no CPU while it waits, the way green
  threads or C# async/await would be?

Decision record:
[ADR 0009](../../../docs/adr/0009-flush-once-per-event-batch.md).

**Host:** i9-13900K, Docker Desktop on WSL2. Before and after every
measurement the VM ran nothing but these benchmarks. That is checked with
`docker ps` and a process list, and no other kvnode Claude session was busy
(see *Attempt 1*). Outside the VM, 4–11 of 32 logical CPUs were busy
(docfx, a game, test hosts), recorded per run by the harness.

## 1. Event-driven, no CPU while waiting: measured

`idleprobe.py` → `idleprobe.out`: 4 workers, 1,000 HTTP/2 connections
open and silent, 10 s.

- **CPU: 10 ms in 10 s** (0.08% of one core, a single clock tick).
- **Context switches:** 40 voluntary, none involuntary. That is each
  worker waking once per second for housekeeping.
- **System calls (`strace -c -f`, 5 s):** only `epoll_wait`, 36 calls.

A connection is a state machine resumed by epoll readiness. That is what
C# async/await compiles to, minus the heap-allocated continuations and
thread hand-offs. With nothing ready, a worker sleeps in `epoll_wait`. The
48% was never waiting. It is work the kernel does inside our `sendto`:
delivering bytes to the co-located client, and waking its thread.

## 2. The change

Before, each connection was flushed right after its own read. A client
thread owning several connections got its responses ~90 µs apart, fell
asleep between them, and was woken once per connection.

`worker_main` now handles each epoll batch in two phases:

1. read and process every ready connection;
2. flush them all back to back.

The drain doorbell also waits until after phase 2. That removes a latent
use-after-free: a drain could destroy a connection whose event was still
ahead in the batch.

## 3. Concurrent paired A/B: the evidence for the change

`ab3.py` → `ab3.out`. A = flush per connection (`864a130`/`ac100eb`), B =
two phases, both running at the same time.

- Each server is pinned to its own vCPU (2 or 6) with its own client vCPUs
  (16–23 or 24–31). The slots swap every block.
- Each block starts fresh servers with 1M keys preloaded, then runs 64
  connections × 16 in flight, 3 s warmup, 10 s measured.
- Client wakeups are the voluntary context switches of kvbench's threads
  over the window.

Numbers are B/A geomeans. 6/6 means B was better in every block (exact
sign test p = 0.031).

| Client shape | GET/s | p50 | p99 | server CPU/req | client wakeups/req |
|---|---|---|---|---|---|
| 8 threads × 8 connections | **1.257×** [1.217–1.338] 6/6 (1.81M → 2.24M) | 0.799× 6/6 | 0.790× 6/6 | 0.796× 6/6 | **0.643×** 6/6 |
| 16 × 4 (the harness's kvbench) | **1.118×** [1.031–1.156] 6/6 (1.48M → 1.69M) | 0.851× 6/6 | 0.857× 6/6 | 0.894× 6/6 | 0.888× (5/6) |
| 64 × 1 (control) | 1.041× [1.030–1.056] 6/6 | 0.958× 6/6 | 0.975× (5/6) | 0.962× 6/6 | **1.001×** |
| busy-polling client (control) | 1.026× (5/6) | 0.922× 6/6 | 0.856× 6/6 | 0.975× (5/6) | none to save |

**The gain scales with the connections a client thread owns, exactly as
the mechanism predicts.** With one connection per thread, wakeups per
request are unchanged: one per send. The +3–4% the controls keep is phase
locality, from parsing everything first and then sending everything.

## 4. Before and after, profiled

`profile-blocking-flush-per-connection.txt` and
`profile-blocking-two-phase.txt`. One worker on vCPU 2, 8 client threads ×
64 connections, blocking. Kernel symbols come from the privileged helper
container; the method is in `../2026-09-26-profile-memory`.

| | flush per connection | two phases |
|---|---|---|
| GET/s, one core | 1,975,929 | **2,339,619** |
| server time per GET | 506 ns | **427 ns** |
| waking the client (`__wake_up_common_lock`) | 46.0% of the core, ~233 ns | 43.1%, **~184 ns** |
| TCP/IP loopback | 85 ns | 76 ns |
| store lookup | 65 ns | 58 ns |
| HTTP/2 | 31 ns | 26 ns |

The wakeup cost per request fell by about a fifth, and every other part
got cheaper too (locality).

The wakeup share stays high because what remains cannot be removed on the
server. Every burst must still wake each *sleeping* client thread once,
and under Hyper-V each wakeup of a halted vCPU costs microseconds. Remote
clients never cost the server this: their own NIC and kernel wake them.
Co-located clients avoid it by spinning before they sleep, or by
busy-polling (the busy-poll control).

## 5. The one-core comparison, re-run

`raw.jsonl`, `REPORT.md` and `run.log`: N1 and N3 through the standard
harness. Server on vCPU 0, kvbench in its network namespace on vCPUs
16–31, 6 blocks, a fresh server per run.

| 1 core | KvNode | Redis 8.10.2 | Valkey 9.1.2 | KvNode ÷ Redis | KvNode ÷ Valkey |
|---|---|---|---|---|---|
| N1 GET/s | **1.65M** | 1.26M | 1.34M | 1.32× [0.82–1.57] | 1.23× [0.86–1.44] |
| N1 p50 | **545 µs** | 729 µs | 725 µs | **0.75×**, 6/6 | **0.75×**, 6/6 |
| N3 no-op round trips/s | 2.03M | 2.11M | 1.95M | 0.96× | 1.04×, 6/6 |

This session was noisy. Outside load during N1 had a median of 9 CPUs and
peaked at 19. In one N1 block KvNode's run collapsed to 1.18M, so the GET
ratios pass in only 5 of 6 blocks (p = 0.219). Latency passes in 6 of 6.

The no-op floor ties Redis here. Attempt 1 measured KvNode's no-op at
2.74–2.78M against the same image, while Redis and Valkey were steady
across both sessions. On this hybrid CPU, the hypervisor places vCPUs on
P- or E-cores from session to session (the harness's CPU-clock
calibration read 0.83 in one session and 1.09 in the other). Absolute
cross-session numbers are therefore not comparable. Section 3's A/B,
which runs both variants at the same instant, is the evidence for the
change. This table is only the current standing against Redis and Valkey.

## Attempt 1 (kept, not used)

`attempt1-possible-overlap/` holds a first A/B and a first N1/N3 session
from 07:43–08:04. Another Claude session benchmarked on the same VM
around then: it committed `ac100eb` at 08:03, with experiments on vCPUs
2/6 and client vCPUs 16–31. The outside-load monitor cannot see a
benchmark inside the VM. Both measurements were re-run once the VM was
verifiably idle.

Its A/B agreed with the re-run: 8 × 8 was 1.263× [1.246–1.277] 6/6, with
wakeups 0.666×. Its N1 showed KvNode 1.2× Redis, and its N3 KvNode
1.3–1.5× Redis.

## Files

| File | What |
|---|---|
| `idleprobe.py` → `idleprobe.out` | idle CPU, context switches and syscalls with 1,000 open connections |
| `ab3.py` → `ab3.out` | the concurrent A/B: throughput, latency, server CPU/req, client wakeups/req |
| `prof_setup3.sh`, `prof_report.py` | the profiling driver (`SRV=` picks the binary) and the grouping script |
| `profile-blocking-*.txt` | the before and after profiles |
| `raw.jsonl`, `environment.json`, `REPORT.md`, `charts/`, `run.log` | the N1/N3 harness session |
| `attempt1-possible-overlap/` | the first attempt, kept as raw data |
