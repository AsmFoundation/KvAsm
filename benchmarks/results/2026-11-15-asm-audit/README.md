# Assembly audit: which operations can be made faster (2026-11-15)

A pass over every hot path of the server at `6a31d0a` (v0.2.0) to find
work that can be removed, with each candidate measured and the strongest
ones prototyped. **Nothing here is merged.** Four prototypes pass the
whole test suite and are in [`patches/`](patches/); the rest are measured
candidates.

- **Revision:** `6a31d0a`, built from a clean `git archive` in a private
  directory of the dev container. The main working tree was not touched
  (another session was changing the write path in it).
- **Host:** i9-13900K, Docker Desktop on WSL2, shared during the whole
  session with another session's benchmarks (vCPUs 2–5 and 16–31) and two
  Windows VMs. Sequential end-to-end runs of identical binaries differed
  by up to ±40%, so the audit rests on instruments that do not depend on
  the host being quiet (next section).

## Instruments

| Instrument | What it gives | Files |
|---|---|---|
| callgrind with a count per instruction | exact user-space instructions per request, per function and per source line; independent of load | `cgrun.sh`, `cgannot.py` → `instructions-per-request.txt`, `get-annotated-head.txt` |
| in-process request-path harness | ns per request of h2n + grpc + store with no socket and no syscall; the minimum over short interleaved runs repeats to under 1% | `bench_h2.c`, `benchabi.asm`, `build_bench.sh`, `abbench.sh` → `bench_h2.out` |
| per-request counters of a real server | requests/s, server CPU per request (schedstat), minor page faults per request | `getcost.sh` → `getcost.out` |
| concurrent paired A/B | two builds measured at the same time on two pinned vCPUs, slots swapped per block; cancels load that varies over time | `abpair.sh` → `abpair.out` |
| syscall counts | `strace -c` under an open-loop load; `epollcost.c` for the cost of one call | `syscount.sh` → `syscount.out` |
| hash comparison | latency and throughput of each candidate hash, after checking every one against reference vectors | `hashes.asm`, `hashbench.c` → `hashbench.out` |

The harness verifies itself: every variant fed the same input must print
the same output digest (the bytes on the wire are identical), the output
size of every batch is checked, and the server's own request counter must
equal the requests sent.

## Where a request's instructions go (HEAD)

| Request | Instructions | Largest items |
|---|---|---|
| GET, 100-byte value | 1,446 | key hash 249; frame parsing 282 (`h2_process`, `header_block`, `decode_fields`, `blocks_equal`); stream open/close 241; reply framing 187; lookup and prefetch 160; dispatch 93; protobuf 72 |
| health check (no store work) | 862 | the same protocol path: 60% of a GET is not about the key at all |
| SET, 100-byte value | 1,806 | key hash 249; glibc `malloc` + `free` of the record 236; protobuf 132 |
| GET, 16,384-byte value | 35,026 | two copies of the value (32,735), one `malloc`/`free`, 0.69 heap growths per request |

In process (no kernel, 1,024 keys in cache) a GET's whole user-space path
takes 121 ns on this vCPU, about 355 core cycles; a health check 72 ns.

## Findings, ranked

| # | Operation | Finding | Measured | Status |
|---|---|---|---|---|
| 1 | GET of a value ≥ 16,374 bytes, and any reply after the first 64 KiB of a read's output | the fast reply path is never taken: `malloc`, copy, pend list, second copy, `free` | **1.82× GET/s, 0.541× server CPU per request** (16 KiB, concurrent A/B, 6/6 blocks); page faults 1.9 → 0 per request | patch 0001, tests pass |
| 2 | Key hash (every Get, Put, Delete, Upload, Download) | SipHash-2-4-**128** costs 71–87 cycles per 16-byte key, 17% of a GET's instructions; the second 64-bit word is not needed | 64-bit output: −7.7% of the GET path, 50–66 cycles; AES-NI: **−19.3%**, 8–44 cycles | patch 0004 (64-bit), tests pass; AES measured only |
| 3 | Reply framing of every unary call | six helper calls build 30 constant bytes | −6.2% of the GET path, **−16.1%** of a health check; bytes identical | patch 0003, tests pass |
| 4 | Event loop, whenever the worker is not saturated | a non-blocking `epoll_wait` before every sleep | 95 ns per sleep against 8.4 ns for what replaces it; `epoll_wait` calls per unpipelined request 1.46 → 0.85 | patch 0002, tests pass; not timed end to end |
| 5 | Stream open/close of every unary call | 241 instructions: zeroed 192-byte object, map insert/find/remove, list, four nested calls to free | not prototyped | candidate |
| 6 | Frame parsing | 282 instructions for two frames whose header block is already cached | not prototyped | candidate |
| 7 | Protobuf decode | 72 instructions for one field (GET), 132 for two (SET): a generic varint loop per tag and length | not prototyped | candidate |
| 8 | Lookup | the two bucket addresses are derived three times per GET (≈ 20 instructions each, with a multiply) | not prototyped | candidate |
| 9 | GET of a value > 64 KiB | still the buffered path (patch 0001 stops at 65,536 bytes) | 65,525 bytes: 14.8 µs/request; 65,526 bytes: 25.7 µs | candidate |
| 10 | Upload | the partial frame at the end of every read is copied out to the carry buffer and back in at the next read | extra copying equal to 13–14% of the uploaded bytes (8.5 KiB per 64 KiB upload, 147 KiB per 1 MiB) | candidate |
| 11 | SET | glibc `malloc`/`free` per record: 236 instructions and heap churn | (the write path is another session's work) | known |

Together, patches 0001–0004: **GET 1,446 → 1,280 instructions (−11.5%)**,
health check 862 → 755, SET 1,806 → 1,638; in process GET 121.2 → 104.7
ns (**−13.6%**), health check 72.1 → 60.7 ns (−15.8%).

### 1. Large replies never took the fast path

`reply_fast` required the whole gRPC message to fit one HTTP/2 frame and
the output buffer to be below 64 KiB. A 16,384-byte value makes a
16,395-byte message — 11 bytes over the default frame — so every such GET
went through `reply_slow`. The profile shows it exactly: `reply_slow` 1.00
calls per request, `pend_push` 1.00, two value copies, `malloc` and `free`
once each, glibc growing the heap 0.69 times per request
(`instructions-per-request.txt`). Natively that is 1.9 minor page faults
per request: the freed 16 KiB blocks are trimmed back to the kernel and
faulted in again.

The second condition hurts values that do fit a frame: 16,000-byte values
with 16 in flight make 262 KiB per read, and after the first 64 KiB every
reply was buffered the same way (1.2 faults per request).

Patch 0001: a message of up to 64 KiB is split over as many DATA frames
as it needs, copied once from the record; and when the buffer is past the
high-water mark it is handed to the socket at once instead of buffering
the reply (one refused attempt per read at most).

| 16,384-byte values, one worker | HEAD | patches |
|---|---|---|
| GET/s, concurrent A/B, 6 blocks | 96–122 k | 173–226 k (**1.82×**, 6/6) |
| server CPU per request | 8.4–10.6 µs | 4.4–5.9 µs (**0.541×**, range 0.479–0.566) |
| minor page faults per request (sequential runs, `getcost.out`) | 1.90–1.97 | 0.000 |
| instructions per request | 35,026 | 17,843 |

16,000-byte values: 102–146 k → 223–232 k GET/s with patch 0001 alone
(`getcost.out`). Every value returned was checked by kvbench in every run
(0 errors, 0 bad values), across the frame boundary (16,373 / 16,374
bytes) and the direct-path limit (65,525 / 65,526).

This matches the "unexplained 16 KiB regression" of the 2026-09-27 suite
(691 k → 481 k at 4 cores when the native engine replaced nghttp2, 0.50×
Garnet): the native engine buffers every such reply. Why the nghttp2 build
did not — presumably it framed a message across frames straight from its
data provider — was not verified here. Whether the fix closes the gap to
Garnet and Memcached needs the suite on a quiet host.

### 2. The key hash

`hashbench.out`, 16-byte key, core cycles (latency when the hash is the
critical path / throughput when consecutive hashes overlap), minimum of
three runs; every function first checked against the reference C SipHash,
the FIPS-197 AES-128 vector and a guard page:

| Hash | Latency | Throughput |
|---|---|---|
| SipHash-2-4-128 (today) | 86.6 | 70.6 |
| SipHash-2-4-64 | 66.0 | 50.0 |
| SipHash-1-3-128 | 60.3 | 44.5 |
| SipHash-1-3-64 | 43.2 | 28.2 |
| AES-128 as a PRF (AES-NI) | 43.8 | 7.6 |

Inside the real request path (`bench_h2.out`): the 64-bit SipHash-2-4
takes 7.7% off a GET (121.2 → 111.9 ns), the AES hash 19.3% (→ 97.9 ns).

- **Patch 0004** keeps SipHash-2-4 and its key and takes the bucket index
  from the low bits and the tag from the top byte of one 64-bit word. The
  table never needed two independent words; keys are still compared
  exactly. It changes where keys land, not what is stored (no snapshot or
  wire format depends on the hash). `test_siphash` gains the published
  64-bit vectors, a C reference and the overread proof.
- **AES** (`patches/measurement-only-aes-hash.patch`) is CMAC-shaped: one
  AES-128 encryption of the padded key under a secret length mask, chained
  for keys over 16 bytes. It is the larger win and a stronger primitive,
  but it needs CPUID dispatch with SipHash as the fallback and a key block
  per table; the measurement variant has neither (one static key block: it
  cannot run the unit tests, which create several stores). It amends
  ADR 0004, so it is the owner's decision.

### 3. The reply template

After a connection's first response, every unary response is the same 30
bytes around the message. Patch 0003 writes them with constant stores in
`reply_fast` and keeps the general path for every other encoder state.
Framing instructions per GET 187 → 82; the output digest of 32,000
responses is identical to HEAD's.

### 4. The pre-poll

`worker_main` polled with timeout 0 and blocked only when nothing was
ready. That saves the QSBR rejoin (an `xchg`) on a busy pass and costs a
whole syscall before every sleep. Measured: 95.0 ns per empty
`epoll_wait`, 8.4 ns for the offline/online pair (`syscount.out`); under
an open-loop unpipelined load `epoll_wait` calls fell from 1.46 to 0.85
per request, all syscalls from 3.58 to 2.97.

Not shown: an end-to-end gain. In this VM every unpipelined request pays
about 20 µs of cross-vCPU wakeup, which hides 90 ns.

## What was not resolved

- **Small values end to end.** Concurrent paired A/B, 100-byte values:
  server CPU per request 0.984× with all four patches (5/8 blocks, range
  0.954–1.019) against 0.993× for HEAD against itself (range 0.970–1.018).
  The expected effect is about −1.5% in this shape (the user-space path is
  ~11% of a request's CPU here; the rest is kernel and wakeups), which is
  below what this host could resolve today. The instruction counts and
  the in-process timings are exact; a quiet-host run should confirm the
  end-to-end figure before any claim is made.
- **Patch 0002** changes how the worker enters the kernel. Its syscall
  saving is counted, its timing is not. It deserves the same quiet-host
  A/B, saturated and unsaturated.
- **Cycles.** The vCPU ran at 2.8–2.9 GHz-equivalent; a P-core at full
  speed runs the same path in roughly half the time. Ratios carry over;
  nanoseconds do not.

## Patches

Against `6a31d0a`; each is documented in the code it adds.

| Patch | Files | Tests |
|---|---|---|
| `0001-direct-multiframe-replies.patch` | `h2n.asm` | full suite, h2spec 146/146, fuzzing |
| `0002-one-epoll-wait-per-pass.patch` | `worker.asm` | full suite, h2spec 146/146, fuzzing |
| `0003-reply-template.patch` (after 0001) | `h2n.asm` | full suite, h2spec 146/146, fuzzing (alone on HEAD, and in all-four) |
| `0004-siphash64-placement.patch` | `siphash.asm`, `store.asm`, `test_siphash.c` | full suite, h2spec 146/146, fuzzing (as part of all-four) |
| `all-four.patch` | the four above as one | full suite, h2spec 146/146, fuzzing |
| `measurement-only-aes-hash.patch` | `aeshash.asm`, `store.asm`, `Makefile` | not shippable: see §2 |

Still missing before 0001 could ship: an end-to-end test that pipelines
GETs of multi-frame values past the high-water mark against a slow reader
(the existing suite passes, but does not aim at the early drain).

## Reproducing

```sh
# inside kvnode-dev, a variant built in /pc/<name>/server by `make all`
bash build_bench.sh head && bash build_bench.sh all
bash abbench.sh get 13 10 head all            # in-process ns per request
bash getcost.sh /pc/all/server/build/kvnode all 2000 16384 "" 5
bash abpair.sh /pc/head/server/build/kvnode /pc/all/server/build/kvnode 6 2000 16384 ""
# from the host
AUDIT_WORK=<dir with the binaries> bash cgrun.sh kvnode-head get 20000 100 "" ""
python cgannot.py cg-kvnode-head-get.out.1 kvnode-head.nm get_execute
python cgannot.py cg-kvnode-head-get.out.1 kvnode-head.nm get_execute --src <repo>/server/src reply_fast
```

`callgrind-*.out` are the raw dumps behind the tables; `kvnode-*.nm` map
their addresses to functions.
