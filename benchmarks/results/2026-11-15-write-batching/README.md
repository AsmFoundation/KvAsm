# Write batching and per-worker reclamation (ADR 0011), 2026-11-15

**Question:** why are writes so slow, when the server is assembly?

**Answer:** the assembly was not the problem. Each write spent its time
waiting:
- in DRAM misses and `free()` calls while holding the one writer mutex;
- at 4 workers, also in the other workers' glibc arena locks.

The fix and its measured effect are in
[ADR 0011](../../../docs/adr/0011-batched-writes-per-worker-reclamation.md).

## Headline

`solo.sh` runs each server alone on the machine:
- 4 workers on their own cores (vCPUs 2, 4, 6, 8; 1 worker: vCPU 2);
- kvbench on 16 vCPUs;
- 1M keys, 100-byte values, 64 connections × 16 in flight.

The original and the new server alternated for 3 pairs (`solo.out`).

| Workload | Original (0.2.0) | New (ADR 0011) | New / original, per pair | Server CPU per request |
|---|---|---|---|---|
| 100% SET, 4 workers | 1.21M/s | **3.21M/s** | **2.61×** (2.57, 2.67, 2.59) | 3,071 → 1,254 ns |
| 50% GET / 50% SET, 4 workers | 2.05M/s | **3.51M/s** | **1.77×** (1.87, 1.74, 1.72) | 1,884 → 1,123 ns |
| 100% SET, 1 worker | 0.65M/s | **0.78M/s** | **1.19×** (1.20, 1.20, 1.18) | 1,587 → 1,219 ns |

- **Writes now scale with workers end to end:**
  - New server, 1 → 4 workers: SET from 0.78M to 3.21M/s (4.1×), at nearly
    the same CPU per SET (1,219 and 1,254 ns).
  - The original: 0.65M → 1.21M (1.9×), its CPU per SET doubling (1,587
    → 3,071 ns).
- **A write now costs about what a read costs.** From the paired run
  `ab-final.out` (half a core per variant), server CPU per request at 4
  workers:

  | Server | SET | GET | SET ÷ GET |
  |---|---|---|---|
  | Original | 3,373 ns | 1,011 ns | 3.3× |
  | New | 1,178 ns | 1,013 ns | 1.16× |
- **The host was shared.**
  - One sample early in the session found 9.2 of 32 logical CPUs busy
    outside the VM.
  - Another session's audit
    ([2026-11-15-asm-audit](../2026-11-15-asm-audit/README.md)) ran
    measurements in the same VM during this session.
  - Absolute rates are therefore below the quiet-host suite of 2026-09-27;
    the paired ratios are the result.

## The diagnosis

Measured on the original server, then again after each step.

| Measurement | Result | Raw output |
|---|---|---|
| Kernel time per request, 1 worker, GET vs SET | 1,108 vs 1,278 ns. About equal: loopback TCP and client wakeups, not the write path | `faults-original.out` |
| User time per request, same runs | GET 394 ns, SET 1,105 ns: **the SET-specific cost** | `faults-original.out` |
| Page faults | 0.000 minor faults per SET, so glibc's heap trimming is not the cause | `faults-original.out` |
| Profile, original, 1 worker, 100% SET (user time) | `cuckoo_scan_bucket` 38.9% (bucket and old-record misses, taken under the mutex), `store_reclaim_step` 15.2% (cold retired records, freed under the mutex), glibc malloc/free 20.9% | `profile-original-1w.out` |
| Profile after step 2, 4 workers | glibc malloc/free at least 43.8% of user time (listed rows), with `__lll_lock_wait_private`: cross-thread frees waiting on other workers' arena locks | `profile-step2.out` |
| Profile, final, 4 workers | glibc's allocator down to ~1% (`_int_malloc` 1.1%); 3.54M SET/s at 1,185 ns per SET (user 450, kernel 735) | `profile-final-4w.out` |

**Still in the final profile** (4 workers, share of user time). These are
the candidates for a next step:

| Cost | Share | Where |
|---|---|---|
| The shared memory counter's LOCK XADD, one per Put (`KVNODE_MAX_MEMORY_MB` accounting) | 7.2% | 58% of `kv_store_rec_alloc_local` |
| Popping a record from the slot cache: its link is a cold line | 4.3% | 35% of `kv_store_rec_alloc_local` |
| Reading cold retired-record headers in `slot_reclaim` | 12.0% | `slot_reclaim.no_prefetch`, `.free_next` |
| Where the batch's prefetched bucket and record misses land | 14.8% | `kv_cuckoo_prefetch_records`, `kv_cuckoo_prefetch` |

User time is 38% of a SET's server CPU, and the kernel's socket work is the
other 62%. So even removing every user-space item above would save less
than a sixth of a SET's server CPU.

## The steps, each against the original

`abw.py` runs a paired, concurrent A/B with 6 blocks per configuration.
- **Shared physical cores:** both servers run at once, and each core used
  carries one thread of each on its two SMT siblings. The hardware is
  identical by construction.
- **Paired ratios per block,** with a sign test: 6/6 gives p = 0.031.
- **Absolute rates are not the headline's:** each variant gets half a
  core.
- **The first layout was discarded.** It gave each server its own vCPUs
  (`ab-batching-attempt1-slot-biased.out`), and in this VM a vCPU pair is
  one core's two SMT siblings.
  - At 4 workers the per-block B/A followed the slot, from 0.35 to 1.97.
  - The 1-worker SET ratio agreed in either slot (1.20–1.29).
  - An A/A check of that layout had already hinted at it: the original
    against itself, 2 blocks, 4 workers, 0.88 and 1.08, flipping with the
    slot (not saved).

| Workers : read ratio | Step 1: batching (`ab-batching.out`) | Step 2: + slot reclamation (`ab-step2.out`) | Final: + record cache (`ab-final.out`) |
|---|---|---|---|
| 1 : SET | 1.19× | 1.22× | **1.25×** |
| 4 : SET | 1.01× (n.s.) | 1.45× | **2.76×** (B's client saturated: a floor) |
| 4 : 50/50 | **0.85×** (regressed, 0/6) | 1.35× | **1.65×** |
| 1 : 90% GET | 1.02× | 1.05× | 1.02× |
| 1 : GET | 1.00 | 1.00 | 1.00 |
| 4 : GET | 1.005 (n.s.) | 1.00 | 0.99 (n.s.; client-bound) |
| 1 : SET, 256 × 1 (unpipelined) | 1.01 (n.s.) | 1.04 (n.s.) | 0.97 (n.s., 1/6) |

- **Step 1 was rejected by its own measurement.** Batching made the holds
  longer, and those holds still contained the cold frees. Waiters slept,
  and a batch's GETs waited behind its writes.
- **The glibc tcache was the probe for step 3.** Running the step-2 server
  under `GLIBC_TUNABLES=glibc.malloc.tcache_count=1024` (`tunables.sh`,
  `ab-tcache1024.out`, `profile-step2-tcache1024-4w.out`) gave:
  - 1.74× SET at 4 workers (6/6);
  - 1.20× on 50/50.

  That pinned the remaining cost on cross-thread frees. The store-owned
  record cache replaced the tunable; ADR 0011 §3 explains why.

## Store level (`server/bench/bench_write.c`)

1M keys, uniform replacements, threads pinned one per core, 3 repetitions
× 2 s per cell. `make bench-write` runs it. Its `--quick` mode verifies
values and memory accounting, and is part of `make test`.

| Writer threads | Per-write `kv_store_put` | Batched, after step 2 | Batched, final | Final ÷ per-write [range over repetitions] |
|---|---|---|---|---|
| 1 | 1.66–1.77M/s | 3.76M/s | **5.42M/s** | 3.11× [2.99–3.27] |
| 2 | 1.30–1.45M/s | 2.51M/s | **9.19M/s** | 6.43× [5.98–7.18] |
| 4 | 1.27–1.32M/s | 2.76M/s | **12.58M/s** | 10.00× [9.75–10.46] |
| 7 | 1.05–1.16M/s | 3.37M/s | 6.89M/s | 7.18× [5.69–9.06] |

- **Per-write locking gets slower with every writer** added.
- **Batching without the record cache did not scale.**
- **The final path scales 2.32× from 1 to 4 threads.**
- **7 threads fall back** on this host, whose other programs leave no spare
  physical cores. That cell's ratio also varies most.

## Files

| File | What it is |
|---|---|
| `abw.py` | The concurrent A/B driver. Configs are `WORKERS:READ_RATIO:VALUE[:CONNECTIONS:DEPTH]`; sibling-shared slots, CPU and client-saturation accounting, sign test |
| `solo.sh` | One server at a time with the whole client, A and B alternated: the headline numbers |
| `profile_set.sh` | perf cpu-clock profile of one server under 100% SET, plus the user/kernel split |
| `faults_set.sh` | Page faults and user/kernel time per request, GET or SET, optionally with extra environment (e.g. `GLIBC_TUNABLES`) |
| `tunables.sh` | Runs a binary under extra glibc tunables, as an A/B variant |
| `ab-batching-attempt1-slot-biased.out` | Step 1 with per-server vCPUs: discarded, kept for the slot-bias lesson |
| `ab-batching.out`, `ab-step2.out`, `ab-tcache1024.out`, `ab-final.out` | The A/B runs above |
| `solo.out` | The headline runs |
| `faults-original.out`, `profile-original-1w.out`, `profile-step2.out`, `profile-step2-tcache1024-4w.out`, `profile-final-4w.out` | The raw outputs behind the diagnosis tables, each headed by its command |

## Reproduce

Inside the `kvnode-dev` container (see CONTRIBUTING.md):
- the original binary at `/tmp/kvnode-A` (built from `6a31d0a`);
- the new one at `/tmp/kvnode-B`.

```sh
cd /src/benchmarks/results/2026-11-15-write-batching
python3 abw.py 6 1:0:100 4:0:100 4:0.5:100 1:0.9:100 1:1:100 4:1:100 1:0:100:256:1
bash solo.sh 3 4 0 /tmp/kvnode-A /tmp/kvnode-B
bash profile_set.sh /tmp/kvnode-B 4 8
make -C /src/server bench-write
```
