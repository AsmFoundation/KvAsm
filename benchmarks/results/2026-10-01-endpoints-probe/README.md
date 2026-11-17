# Endpoints probe: what the storage design makes cheap, and SIMD work on values (10M keys)

- **Date:** 2026-10-01, revision 783cf4a (store code as committed).
- **Why:** the ask was to revisit how KvAsm hashes and stores, where SIMD
  can be used, and then find more endpoints.
  - The re-read of `siphash.asm`, `cuckoo.asm`, `store.asm` and `grpc.asm`
    showed that a GET is memory-bound: two dependent misses, a bucket line
    then a record.
  - SIMD inside the lookup can only shave the hash and the compares.
  - Values are opaque bytes the server never computes on.

  So the next endpoints come from what the bucket and record layout makes
  O(1), and from value-level work where SIMD runs at GB/s and returns a few
  bytes instead of whole values.
- **The plan it supports:**
  [docs/proposals/functions-plan.md](../../../docs/proposals/functions-plan.md):
  Part II §3 (the store as built, and where SIMD pays) and Group J (the
  twelve functions measured here).
- **Files:**

  | File | What |
  |---|---|
  | `endpoints_probe.c` | the probe: every structure, its reference check, its timing |
  | `run.sh` | assembles the store **and the server's CRC-32C** (`crc32c.asm`) in `kvasm-dev`, builds and runs |
  | `output.txt` | the certified 10M-key run |

## How to run

```bash
# full run, 10M keys, pinned to CPU 4, 5 repetitions (~40 s, ~3 GB)
MSYS_NO_PATHCONV=1 docker exec -e REV="$(git rev-parse --short HEAD)" \
  kvasm-dev bash /src/benchmarks/results/2026-10-01-endpoints-probe/run.sh 4 5
# smoke test: 1M keys, one repetition, every check, meaningless timings
MSYS_NO_PATHCONV=1 docker exec -e QUICK=1 kvasm-dev \
  bash /src/benchmarks/results/2026-10-01-endpoints-probe/run.sh
```

The probe refuses to start without AVX2, BMI1, SSE4.2, PCLMULQDQ and
SHA-NI.

## What it measures, and how each is checked

| Section | What | Checked against |
|---|---|---|
| bar | exact get: D (dependent), I (plain loop), B (the server's batched pipeline); Stat (lengths only) and GetRange (16 bytes) on the same pipeline | every lookup must hit |
| etag | CRC32C of 100 B to 64 KiB with the server's assembly (folding vs SSE4.2); GetIfChanged with an on-the-fly CRC | a bitwise CRC reference on 2,000 random buffers; unchanged values must never report a change |
| snapshot | 16 keys read as one snapshot (double collect over bucket versions): the cost at rest, and a writer racing a reader | the writer's order defines every possible state; validated reads must never be torn, and plain reads must tear (or the test proves nothing) |
| scan | every key by bucket cursor, no index: 1 thread with keys, then with values; 7 threads by hash shard | every key exactly once (count and an order-independent signature sum) |
| cells | max register (LOCK CMPXCHG), 64-flag word (LOCK OR), against LOCK XADD; counter arrays (168 × int64): increment at an index; AVX2 sum and max | exact registers from the stream; the kernels against scalar code |
| json | one field out of a JSON value: an AVX2 + PCLMULQDQ structural scan (simdjson's method) against a scalar tokenizer, with the same path walker; JsonGet on a store of 1M documents | every structural position equal for 4,000 generated documents; every extracted field equal to the generator's value. The documents include escaped quotes, escaped backslashes before a closing quote, brackets inside strings and random whitespace |
| sha | SHA-256 with SHA-NI against scalar FIPS 180-4 code; PutBlob (digest, exists, put); FindMissingBlobs (batched exists of digests) | the FIPS vectors ("", "abc", the 448-bit one) and 500 random lengths against the scalar code |
| grep | a substring filter over 256 MiB of text values: AVX2 first/last-byte candidates against glibc memmem | equal match counts |

## Results (certified, `output.txt`)

**The bars:** D 411.4 ns, I 369.5 ns, B 57.5 ns. They are in line with
the services probe the same day (435.5 / 406.0 / 57.3). That day's
control showed host load moving D and I.

| Endpoint or kernel | Measured | × bar |
|---|---|---|
| Stat, batched | 58.8 ns (1.00× the get beside it) | 1.02 B |
| GetRange 16 bytes, batched | 58.7 ns (1.00×) | 1.02 B |
| CRC32C of 1 KiB / 64 KiB, folding | 11.6 ns (88 GB/s) / 720 ns (91 GB/s); SSE4.2: 40.2 ns / 4.29 µs | — |
| GetIfChanged, unchanged (on-the-fly CRC, no value bytes) | 67.5 ns (1.12× the get beside it) | 1.17 B |
| snapshot of 16 keys, per key, at rest | 68.2 ns (1.01× plain GetMany) | 1.19 B |
| 16 keys under a racing writer | plain: 21,904 of 200,000 reads torn; validated: 0 torn, 1.52 attempts | — |
| Iterate keys, 1 thread | 18.5 ns per key (54 M keys/s) | 0.32 B |
| Iterate keys + values, 1 thread | 19.5 ns per key (5.1 GB/s of values) | 0.34 B |
| ScanShard, 7 threads | 2.9 ns per key: **340 M keys/s, 10M keys in 29 ms** | 0.05 B |
| max register / flag word, batched | 43.1 / 47.1 ns (0.91× / 1.00× an increment) | 0.75 / 0.82 B |
| ArrayIncrement / ArraySum (168 × int64), batched | 71.7 / 116.4 ns | 1.25 / 2.02 B |
| sum / max of 168 int64 in cache | AVX2 6.3 / 18.2 ns vs scalar 29.2 / 48.5 | — |
| JSON structural index, 668 B / 4,969 B documents | AVX2 + PCLMULQDQ 2.44 / 2.83 GB/s vs scalar 1.04 / 1.60 GB/s (2.3× / 1.8×) | — |
| JsonGet, late field, 668 B / 4,969 B | 253 ns / 1.75 µs (scalar 491 ns / 3.36 µs) | 0.68 / 4.73 I |
| JsonGet, early field | 31-80 ns (1.6-2.0× the scalar) | ≤ 0.22 I |
| JsonGet end to end, 1M documents of ~1 KB | 369.2 ns: 3.34× the whole-document get on the server, a few bytes on the wire instead of ~1 KB | 6.42 B |
| SHA-256, 4 KiB / 1 MiB | SHA-NI 1.66 µs / 415 µs (2.5 GB/s) vs scalar 10.5 µs / 2.65 ms (6.3-6.4×) | — |
| PutBlob 4 KiB (digest + exists + put) | 1.77 µs | 4.79 I |
| FindMissingBlobs, 32-byte digests, batched | 33.8 ns per digest | 0.59 B |
| grep a rare needle / a common one, 256 MiB | AVX2 19.6 / 3.9 GB/s vs memmem 5.0 / 1.6 GB/s (3.9× / 2.5×) | — |

## Notes

- **The race test needed a realistic reader.** With 16 hot keys in cache a
  reader is far faster than a writer, which can then never overtake it, so
  no read could tear (the first smoke run stopped on the probe's own guard).
  - The reader now spends ~0.1 µs per key, a cold miss's cost at 10M keys.
  - The writer pauses ~10 µs between rounds. A writer that never pauses
    starves any validated read (the seqlock livelock); the plan (J4) falls
    back to reading under the writer lock after a few attempts.
- **The max-register row is mostly the no-write path.** Each repetition
  replays the same candidates, so after the first one a candidate rarely
  raises its register and the update is a plain load. The median of five
  therefore measures mostly unchanged registers. A raising update adds one
  `LOCK CMPXCHG`, about an increment's cost. The flag-word row always
  writes (`LOCK OR`).
- **The JSON speedup is about 2×, not simdjson's.** The SIMD tokenizer is a
  pull iterator (one structural per call), and the path walker drives it.
  - simdjson flattens each block's structurals into an index array first,
    and reaches several GB/s on full parses.
  - This prototype's 2.4-2.8 GB/s is a floor for that technique, not its
    ceiling.
- **Run-to-run variance of in-cache kernels was about ±30%** between the
  smoke and certified runs (CPU frequency, host load). Every SIMD/scalar
  pair alternates within a run, so the ratios are paired.
- **Certification:** the same wrapper as the services probe.
  - Idle checks: kvasm-dev processes from `/proc/*/stat` only, VM CPU from
    `/proc/stat` deltas, host CPU.
  - `docker events` for the window.
  - Window: 2026-10-01T20:16:17Z .. 20:16:49Z, foreign 0, VM CPU 0.0%, host
    CPU 14.7%.

## Limits

- One machine (i9-13900K, WSL2, no PMU); synthetic data. Real JSON varies
  more in shape, and real text in byte statistics: the substring filter's
  speed depends on how common the needle's first and last bytes are (3.9
  vs 19.6 GB/s here).
- Prototypes in C next to the store, not store code. The scan's guarantee
  under concurrent writes (the relocation log, J5), and the snapshot's
  sequence word and writer-lock fallback (J4), are designed in the plan but
  not stress-tested here.
