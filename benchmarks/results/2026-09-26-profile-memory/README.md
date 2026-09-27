# One KvNode core, profiled — and its memory (2026-09-26)

> **Update 2026-09-27:** the absolute times below were measured while ~20
> of the host's 32 logical CPUs were busy outside the VM, which made every
> DRAM miss dearer. On a quiet host the same server with a busy-polling
> client does **3.71M GET/s on one core (269 ns per GET)**, with the store
> lookup at **69 ns**, not 93. The store alone costs 43.5 ns, and
> huge-page records are worth +6% (paired, p = 0.031). The shares and the
> conclusions below still hold. See `../2026-09-27-store-lookup`.

These are the answers to three questions, measured on the revision that
removed nghttp2 from the server
([ADR 0008](../../../docs/adr/0008-remove-nghttp2-from-the-server.md)):

- What is the memory use of such an app?
- What are the hotspots of compute?
- Can we use SIMD?

It also holds the paired A/B of the one change the profile led to: a third
prefetched record line (ADR 0005 amendment).

**Host:** i9-13900K (8P + 16E cores, 32 threads, AVX2, no AVX-512), Docker
Desktop on WSL2 (kernel 5.15). The machine was shared. A Ray optimization
job (36 `python.exe` processes) and other programs kept **~19.6 of 32
logical CPUs** busy outside the VM throughout. Absolute throughput is
therefore lower and noisier than on a quiet host. The profile *shares* and
the memory numbers are not affected.

## Files

| File | What |
|---|---|
| `memprobe.py` → `memprobe.out` | memory scenarios on fresh servers: idle with 1/4/32 workers, capacity alone, 1M keys at 16/100/1,024-byte values, 1,000 idle HTTP/2 connections then one Get each, and a 64×16 GET load |
| `prof_setup.sh` | one worker pinned to vCPU 2 under E1's load (1M keys, 100 B, 64×16) from kvbench on vCPUs 16–31; `BUSY=1` adds `--busy-poll` |
| `prof_report.py` | groups a flat `perf report` into hotspots and converts shares to ns per request with the `/proc` user/kernel split; `nm -n` maps NASM macro-local labels to their functions |
| `profile-blocking.txt` | the profile with kvbench's normal, blocking client |
| `profile-busypoll.txt` | the profile with a busy-polling client (the server's own cost) |
| `profile-busypoll-3lines.txt` | the same, with the third record line prefetched |
| `callgraph-wakeup.txt` | the call chain behind the blocking profile's top symbol |
| `ab.py` → `ab-sequential-aborted.out` | the first A/B design (sequential pairs), stopped: see below |
| `ab2.py` → `ab-concurrent.out` | the A/B that counts: A and B run at the same time |

The profiles sample the server's vCPU system-wide with `cpu-clock` (WSL2
has no PMU). They run from a privileged helper container that shares the
dev container's PID namespace, because inside the dev container
`perf_event_paranoid=2` hides kernel symbols:

```sh
docker commit kvnode-dev kvnode-perf-helper:local      # the dev container has perf
docker run --rm --privileged --pid=container:kvnode-dev -v "$PWD:/src" kvnode-perf-helper:local \
  perf record -e cpu-clock -F 4999 -a -C 2 -o /src/server/build/prof.data -- sleep 12
```

## Memory

Resident memory, from `/proc/PID/smaps_rollup` (`VmRSS` is stale on this
kernel):

| Scenario | Resident |
|---|---|
| capacity 1,000 (almost no table), 1 worker | **740 KiB** in total: the binary's code pages 536 KiB (page cache, shared), the rest stacks, heap, data |
| default capacity 1M, 1 / 4 / 32 workers | 15,064 / 15,100 / 15,508 KiB: the table is 14,336 KiB of huge pages (7 × 2 MiB), and each idle worker ~14 KiB |
| capacity 10M / 100M, empty | 129,756 KiB / 2,095,812 KiB: the table is resident from startup |
| + 1M keys × 100-byte values | +158,424 KiB = **162.2 bytes per key** (a 140-byte record in a 160-byte malloc chunk) |
| + 1M keys × 16-byte values | 66.3 bytes per key (56-byte record, 64-byte chunk) |
| + 200k keys × 1,024-byte values | 1,083.2 bytes per key (1,064-byte record) |
| 1,000 idle HTTP/2 connections | +9,144 KiB = **~9.1 KiB per connection** (below) |
| … after one Get each | ~9.3 KiB per connection |
| … all closed | 5,100 KiB of the 10,072 stays: the pool keeps its chunks for reuse (peak-sized, by design) |
| 1M keys loaded, then 64×16 GET load | 173,504 → 174,260 KiB peak (+756 KiB of streams, request and output buffers) |

An idle connection breaks down as:

- the 848-byte connection record, with the HTTP/2 state embedded;
- the first touched page of its 64 KiB output buffer. Only touched pages
  count, and production stops at 64 KiB;
- a 4 KiB HPACK entry ring (128 × 24 bytes, a 4 KiB pool class);
- a 16-slot stream map.

Kernel socket memory is separate: `/proc/net/sockstat` showed 997 pages
for 2,000 idle loopback sockets (client and server sides), about 2 KiB
each.

With the table's share (16 MiB ÷ 1M = 16.8 bytes), a 100-byte-value key
costs 178 bytes. The published E1 comparison for the same data: Redis 174,
Valkey 173, Dragonfly 159, Memcached 196, Garnet 284 bytes per key.

## Hotspots

### Blocking client

kvbench as used by every published result.

1,719,339 GET/s on one core = **582 ns** of server-core time per GET. The
`/proc` split is 24.6% user (143 ns) and 75.4% kernel (439 ns).

| Hotspot | Share | ns/GET |
|---|---|---|
| kernel: waking the client (`__wake_up_common_lock`, see below) | 48.5% | 282 |
| kernel: TCP/IP loopback stack | 15.3% | 89 |
| KvNode: store lookup | 11.2% | 65 |
| KvNode: HTTP/2 (`h2n.asm`) | 6.1% | 36 |
| kernel: other (softirq entry, socket lookup, …) | 6.1% | 35 |
| kernel: syscall entry/exit + VFS | 2.8% | 16 |
| KvNode: gRPC + protobuf | 1.9% | 11 |
| kernel: user↔kernel copies | 1.9% | 11 |
| glibc memcpy/memset | 1.7% | 10 |
| kernel: slab/page allocation | 1.6% | 10 |
| KvNode: event loop | 1.5% | 9 |
| KvNode: pool | 1.0% | 6 |

**The top symbol is not KvNode's work.** `callgraph-wakeup.txt` shows the
path: the server's `sendto` → `tcp_write_xmit` → `ip_finish_output2` →
(loopback) `do_softirq` → `net_rx_action` → `tcp_v4_rcv` →
`tcp_rcv_established` → `sock_def_readable` → `__wake_up_common_lock`.

Over loopback, the server's send runs the *client's* TCP receive on the
server's CPU. It then wakes the kvbench thread sleeping in `epoll_wait` on
another vCPU. Under Hyper-V that wakeup is an IPI, which exits the VM.
Samples pile up on `__wake_up_common_lock` because the wakeup runs with
interrupts disabled (`spin_lock_irqsave`), and a `cpu-clock` sample lands
where interrupts come back on. Per send, one sends per 16 responses, the
wakeup costs ~4.5 µs. With remote clients, the NIC does the waking on the
client's machine instead.

### Busy-polling client

`kvbench --busy-poll`: client threads never sleep, so no send wakes anyone.

2,620,040 GET/s on one core = **382 ns** per GET. The `/proc` split is
53.6% user (205 ns) and 46.4% kernel (177 ns).

| Hotspot | Share | ns/GET |
|---|---|---|
| KvNode: store lookup | 24.5% | 93 |
| kernel: TCP/IP loopback stack | 22.5% | 86 |
| KvNode: HTTP/2 (`h2n.asm`) | 14.7% | 56 |
| kernel: other | 7.8% | 30 |
| KvNode: gRPC + protobuf | 5.6% | 21 |
| glibc memcpy/memset (AVX2) | 5.3% | 20 |
| kernel: syscall entry/exit + VFS | 4.3% | 16 |
| kernel: user↔kernel copies | 3.8% | 15 |
| kernel: scheduler/IRQ | 3.4% | 13 |
| kernel: slab/page allocation | 3.0% | 11 |
| KvNode: event loop | 2.7% | 10 |
| KvNode: pool | 2.0% | 8 |

Top user symbols:

| Symbol | Share | What it is |
|---|---|---|
| `kv_cuckoo_prefetch_records.sse2_slot` | 8.2% | the tag scan waiting for its bucket line, and the record prefetches holding fill buffers |
| `kv_cuckoo_prefetch` | 6.5% | two `prefetcht0`s. A prefetch that misses the TLB must page-walk before it retires, and under WSL2 that walk is nested (guest huge pages need not be host huge pages) |
| `__memmove_avx_unaligned_erms` | 4.2% | the value copy, which stalls on an unprefetched third record line |
| `kv_siphash128` (final + block) | 4.9% | the only arithmetic hotspot. Scalar SipHash-2-4-128 is latency-bound |

The rest is below 1.2% each.

## SIMD

What already uses it, and why little is left:

- **Tag matching:** SSE2 (`pcmpeqb` + `pmovmskb`) compares both buckets'
  12 tags at once. It is the default (ADR 0006). Its samples are memory
  stalls, not compare work.
- **Copies and zeroing:** glibc's AVX2 `memmove`/`memset` (IFUNC), plus
  inline overlapping moves for the 5–13-byte response head and the
  precomputed header blocks.
- **HPACK:** the Huffman decoding that was SIMD-shaped is skipped
  entirely by the header-block cache in steady state.
- **The ceiling:** of the 382 ns, the kernel is 46% and needs no SIMD.
  The store's 24% is almost all address-translation and DRAM stalls.
  Arithmetic or copying that SIMD could still shrink is ~10% of the core,
  and half of that is already AVX2.
- **Candidates**, with the cookbook §4 ranking:
  - AVX2 4-lane SipHash across a read's GET batch: ceiling 4.9%,
    estimate ~3%;
  - an inline SSE2 copy for values ≤128 bytes: <1% after the prefetch
    fix;
  - SSE2 header-block compare: <0.5%.

## The change the profile led to: a third record line

**The cause:** a 16-byte key and a 100-byte value make a 140-byte record
(a 24-byte header, the padded key, then the value). It starts on any
16-byte boundary, so it always spans three cache lines. The batch's
record stage prefetched two, and the value copy paid DRAM for the third.
Prefetching `[rec + 128]` too covers a whole record of up to 144 bytes at
every alignment.

**The profile** (`profile-busypoll-3lines.txt`, busy-polling client):

- `memmove`: 4.22% → **1.05%** of the core;
- user time per GET: 205 → 178 ns;
- the prefetch stage: 8.2% → 9.5%, because a third prefetch holds a third
  fill buffer.

Throughput in that single run was 2.62M → 2.80M GET/s. One run on this
host is not evidence, so the A/B follows.

**The A/B, first attempt** (`ab-sequential-aborted.out`): sequential
pairs on one vCPU, with a fresh server per run and the order alternating
per block. In every pair the *second* run was ~10% cheaper per request and
slower, whichever variant it was. That is host steal and scheduling, and
it is larger than the effect under test, so the run was stopped.

**The A/B that counts** (`ab-concurrent.out`): A (two lines) and B (three
lines) run at the same time, each on its own pinned vCPU (2 or 6) with its
own client vCPUs (16–23 or 24–31). The slots swap every block, so a faster
slot cancels. Each run: fresh servers, 1M keys preloaded, 64×16, 3 s
warmup, 10 s measured.

| Config | B/A geomean | Range | B faster | Sign p |
|---|---|---|---|---|
| blocking client, 100-byte values | 1.001 | 0.992–1.010 | 2/6 | 0.688 |
| busy-polling client (6 threads), 100-byte values | 1.025 | 0.958–1.071 | 4/6 | 0.688 |
| blocking client, 16-byte values (third line wasted) | 0.993 | 0.968–1.015 | 3/6 | 1.000 |

**Verdict: within noise everywhere.** In the busy-polling runs the slots
differed by ~3%, and two sets of spinning clients oversubscribed the host
(block 3 ran at ~1M/s for both). The change stays for the stall it
removes, recorded as 🧪 (applied, effect within noise) in the cookbook
and ADR 0005.
