# Services probe: more golden services, and the assembly behind them (10M keys)

- **Date:** 2026-10-01, revision a8dec3a (store code as committed).
- **Why:** after the [golden catalog](../2026-09-29-golden-probe/README.md)
  and the [functions plan](../../../docs/proposals/functions-plan.md), the
  ask was to find *more* key-value services within 10× of an exact get at
  10M keys, and to use assembly-level techniques and optional SIMD to make
  them, and the store itself, faster.
- **The proposal it supports:**
  [docs/proposals/golden-services.md](../../../docs/proposals/golden-services.md).
- **Files:**

  | File | What |
  |---|---|
  | `services_probe.c` | the probe: every structure, its reference check, its timing |
  | `run.sh` | builds the store's assembly and the probe in `kvasm-dev`, runs it |
  | `output.txt` | the certified 10M-key run quoted everywhere |
  | `control_golden_bar.sh`, `control.txt` | the golden probe's own bar, run the same hour |
  | `output-run1-repmovsq.txt` | the first certified run, kept as the exhibit of a probe bug (below); do not quote it |

## How to run

```bash
# full run, 10M keys, pinned to CPU 4, 5 repetitions (~40 s, ~3.2 GB)
MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
  kvasm-dev bash /src/benchmarks/results/2026-10-01-services-probe/run.sh 4 5
# smoke test: 1M keys, one repetition, every check, meaningless timings
MSYS_NO_PATHCONV=1 docker exec -e QUICK=1 kvasm-dev \
  bash /src/benchmarks/results/2026-10-01-services-probe/run.sh
```

- `run.sh` assembles `siphash`, `util`, `cuckoo`, `wlock` and `store` from
  `/src/server/src` with the server's NASM flags into a scratch directory.
  So the probe measures the checked-out store, and never touches
  `/src/server/build`, which other sessions share.
- The SIMD paths use per-function `target` attributes. The probe refuses
  to start without AVX2, FMA, POPCNT, BMI2 and SSE4.2. The AVX-VNNI and
  UMWAIT rows say so when the CPU, or the hypervisor, lacks them.

## What it measures

Every section checks its answers against a scalar or brute-force reference
before printing a number; any mismatch exits 1.

| Section | What | Checked against |
|---|---|---|
| bar | exact get: D (dependent chain), I (plain loop), B (the server's batched pipeline) | every lookup must hit |
| siphash4 | SipHash-2-4-128 of 4 and 8 keys in AVX2 lanes; the batched get with them | the store's assembly, bit for bit, for all 1,048,576 query keys |
| inline | keys ≤ 16 B and 8-byte values inside the 64-byte bucket (one miss); gets in D/I/B and increments | every answer and 100,000 absent keys; increments exact against stream counts |
| combining | concurrent replace-puts: the writer mutex vs flat combining, at 1/2/4/7 threads | every put must replace |
| waitpkg | one-way wake-up: PAUSE spin, UMONITOR/UMWAIT, futex | — |
| bitmaps | BITCOUNT (POPCNT, AVX2 lookup, Harley-Seal), AND-count, SETBIT, GETBIT, BITPOS, counts kept by SETBIT, range BITCOUNT | scalar POPCNT; block counts recomputed after every SETBIT pass; 4,096 ranges against scans |
| struct | HGET / HINCRBY through a 16-tag SSE2 field directory, against getting the whole 440-byte value | every field value; increments exact |
| sets | SISMEMBER (branchless binary search, AVX2 scan) and SINTERCARD (merges, AVX2 all-pairs blocks) | scalar results |
| kernels | sliding-window sum, MinHash agreement, int8 dot (VPMADDUBSW, VPDPBUSD), fp32 FMA dot | scalar results |
| topk | HeavyKeeper (2 × 4,096) + a 100-entry heap scanned with AVX2, on a Zipf(1) stream over 10M keys | exact counts of the stream: the reported keys must reach the 100th true count |
| quantiles | DDSketch, α = 1%: record; quantile by full walk and by 64-bucket groups | sorted exact quantiles: p50/p90/p99/p99.9 within 1% |
| simhash | near-duplicates (Hamming ≤ 3) among 10M 64-bit fingerprints: multi-index hashing (4 × 16-bit blocks), scalar vs AVX2 scan | brute force over all 10M; every planted near-duplicate found |
| geo | radius search over 10M points: PDEP Morton codes, a sorted code array, seeks by binary search or a 2^20 prefix table, PEXT box test | brute force over all 10M; PDEP vs shift-and-mask codes |

## Results (certified, `output.txt`)

**The bar of this run:** D 435.5 ns, I 406.0 ns, B 57.3 ns.

- **B matches the golden probe:** 54.1 ns certified on 2026-09-29, and
  56-64 ns in the control the same hour.
- **D and I are slower today on the machine itself, not in the probe:** the
  control read the golden probe's own D/I at 432-449 / 386-414 ns, against
  347/327 on 2026-09-29. The host was running a game client and desktop
  load; the VM was idle.
- **All ratios below compare within this run.**

| Service or technique | Measured | × bar | Golden? |
|---|---|---|---|
| SipHash-2-4-128, 8 AVX2 lanes | 5.6 ns per key vs 13.9 (0.40×) | — | — |
| batched get with 8-lane hashing | 56.8 vs 60.2 ns (paired, 0.943×) | 0.99 B | ✔ |
| inline table get, batched + AVX2 hash (16-B key, 8-B value) | 28.1 ns; the store with the same keys 53.1 | 0.49 B | ✔ |
| inline table get, dependent chain | 189.9 vs 349.2 ns (0.54×) | 0.44 D | ✔ |
| inline table increment, batched | 39.8 vs 58.1 ns (0.68×) | 0.69 B | ✔ |
| flat combining, 7 writer threads | 4.29 vs 2.24 M puts/s (1.91×) | — | — |
| UMWAIT wake-up | 103.5 ns vs PAUSE 114.4 (0.90×), futex 2.70 µs | — | — |
| BITCOUNT of a range via block counts | 67.6 ns vs a 7.44 µs scan (110×) | 0.16 D | ✔ |
| SETBIT keeping the counts | 10.1 ns | 0.18 B | ✔ |
| BITCOUNT of 1 MiB, Harley-Seal | 12.8 µs (82 GB/s) | 29 D | ✘ (O(1) with the kept total) |
| HGET one field (440-B struct value) | 59.6 ns; whole value 102.0 | 1.04 B | ✔ |
| HINCRBY one field | 62.5 ns | 1.09 B | ✔ |
| SISMEMBER, 64 elements, AVX2 | 2.1 ns | 0.04 B | ✔ |
| SINTERCARD 1,024 × 1,024, AVX2 | 552.8 ns vs merge 1.64 µs | 1.27 D | ✔ |
| int8 dot, 256 dims, AVX-VNNI | 1.5 ns (scalar 61.3; AVX2 3.2) | 0.03 B | ✔ |
| TopK update (HeavyKeeper) | 16.7 ns; 100/100 correct | 0.29 B | ✔ |
| DDSketch record / quantile | 16.7 / 16.2 ns; worst error 0.19% | 0.29 B / 0.04 D | ✔ |
| near-duplicates among 10M (MIH + AVX2) | 695 ns (brute force 10.8 ms) | 1.71 I | ✔ |
| geo, 200 m (15 results), prefix-table seeks | 697.5 ns (binary seeks 1.07 µs) | 1.72 I | ✔ |
| geo, 1000 m (371 results) | 6.18 µs | 15.2 I | ✘ (output-bound) |

## Method notes: four probe bugs found and fixed before the certified run

Each would have printed a wrong number without failing a check. They are
recorded so the next probe avoids them.

1. **A variable-size copy compiled to `rep movsq`.** The bar copied `vlen`
   bytes; GCC emitted `rep movsq`, whose startup cost added about 25 ns to
   every batched get. The first certified run read B = 83.4 ns
   (`output-run1-repmovsq.txt`); the golden probe's own bar, run as a
   control, read 56-64.
   - Fix: constant-size copies (inlined vector moves, as the golden probe).
   - The 440-byte whole-value copy calls glibc's `memcpy` through a pointer
     instead, as the server does (`call memcpy wrt ..plt`), so the inlined
     `rep movsq` cannot handicap it against HGET.
2. **Pure kernels were hoisted out of the timing loops.** GCC proved
   `pop_scalar(A, n)` pure, computed it once and multiplied: a 1 MiB
   popcount read 4.5 TB/s.
   - Fix: a compiler barrier (`asm volatile("" ::: "memory")`) in every
     iteration that repeats a call with the same arguments.
3. **The "scalar" kernels were neither scalar nor called.** At -O2 GCC
   auto-vectorized them (SSE2) and inlined them into the driver, while the
   AVX2 versions (other target) could not inline.
   - Fix: both sides `noinline`, scalar references
     `optimize("no-tree-vectorize")`.
   - An earlier variant changed one operand element per iteration; its
     narrow store made the next 256-bit load miss store-forwarding, a stall
     only the SIMD rows paid. Removed.
4. **Two SIMD kernels were slower for avoidable reasons.**
   - The AVX2 SipHash read each key's tail with a variable-length `memcpy`
     (a library call per key, about 5 ns). Fix: one overlapping 8-byte load
     ending at the key's last byte.
   - The VNNI dot product used one accumulator, so every `VPDPBUSD` waited
     on the previous one's 5-cycle latency, and the AVX2 pair (whose chain
     is a 1-cycle add) beat it. Fix: four accumulators. It now beats the
     AVX2 pair 2.1× (1.5 vs 3.2 ns).

## Certification

- **The window was certified by a host-side wrapper**, a successor of the
  golden probe's (session scratchpad, `certified_run2.sh`). It required 3
  consecutive idle polls, 5 s apart:
  - kvasm-dev had no process besides pid 1, zombies, and six process trees
    stuck in D state since 2026-09-29;
  - VM CPU below 2% over 2 s, from `/proc/stat` deltas;
  - host CPU below 20%.

  It then ran the probe and accepted it only if `docker events` for the
  window showed no foreign `exec_create` or `start`.
- **The idle check reads only `/proc/<pid>/stat`, with a shell builtin
  under `timeout`.** `ps` now hangs in this VM: it reads the stuck
  processes' `cmdline`, which blocks on their memory map.
- **The VM's load average is useless as an idle signal.** Linux counts
  D-state tasks in it, so the stuck processes hold it at 6 or more.
- Window: 2026-10-01T17:21:07Z .. 17:21:45Z, foreign events 0, VM CPU
  0.2%, host CPU 15.3%.

## Limits

- One machine: i9-13900K under WSL2 / Docker Desktop, P-cores only, no PMU,
  so no cache-miss counters. Host load moves absolute latency (see the
  control), so compare ratios within a run.
- Synthetic data:
  - uniform keys, except TopK's Zipf(1) stream;
  - uniform geo points (clustered cities would scan more codes per query);
  - random fingerprints (real SimHash codes cluster, and clusters lengthen
    the posting lists).
- Single-threaded except the combining rows. Thread safety of each service
  is designed and reasoned in the proposal, not stress-tested here.
- The inline table, struct values, bitmaps, sets, sketches and the geo and
  near-duplicate indexes are prototypes in C next to the store, not store
  code.
