# The store lookup: 93 ns, 69 ns or 43 ns? (2026-09-27)

The question was: the profile of 2026-09-26 put the store lookup at 93 ns
per GET, "mostly waiting on memory and page-table walks" — can it get down
to 50 ns?

This directory takes that number apart. All runs used the committed
server, revision `864a130`, built from a clean export. They ran on a
**quiet host**: the Ray job that had kept ~20 of 32 logical CPUs busy on
2026-09-26 had stopped, and ~4 CPUs were busy outside the VM. The runs are
on vCPU 2 of the dev container, i9-13900K under WSL2.

## Answer

| Where the lookup runs | 4 KiB-page records (as shipped) | 2 MiB-page records |
|---|---|---|
| in the server, busy-polling client (profile share × ns/GET) | **69 ns** of 269 | **62 ns** of 264 |
| alone: the same code in the same batched pipeline, 1M keys | **43.5 ns** | **40.3 ns** |
| alone, without SipHash | 32.5 ns | 30.5 ns |
| alone, 8 Ki keys (everything in L2), without SipHash | 9.0 ns | 9.0 ns |

1. **The 93 ns was the host.** Twenty busy CPUs outside the VM made every
   DRAM miss dearer. On the quiet host the same profile shows 69 ns, and
   one core serves **3.71M GET/s** (269 ns per GET) with a busy-polling
   client. Earlier it was 2.62M.
2. **The store itself is already at 43.5 ns.** That breaks down as SipHash
   ~12 ns (21.1 − 9.0 in L2), compute ~9 ns, and memory ~23 ns. Inside the
   server it costs ~25 ns more: the kernel's and the protocol's work
   between batches evicts cached lines and page-table entries, so the same
   misses cost more.
3. **Huge-page records are a measured win.**
   - In the server: store 69 → 62 ns, and the record-prefetch stage
     9.1% → 6.7% of the core.
   - Throughput, concurrent paired A/B (`ab-hugepage-records.out`):
     **1.059× [1.038–1.080], 6/6 blocks, sign p = 0.031.**
   - Alone: 43.5 → 40.3 ns, 11/11 repetitions.
4. **This VM caps it.** Under WSL2, guest huge pages shorten page walks
   but do not extend the TLB's reach (`tlb_chase.out`). A chase over
   16 MiB, the table's size, costs 9.3 ns per step on 2 MiB pages against
   4.1 ns when the pages fit the TLB. So Hyper-V backs this guest with
   4 KiB pages, and each TLB entry still covers 4 KiB. On bare metal, or a
   VM backed by huge pages, the table would need 8 TLB entries and the
   records ~80. The ~17 ns that the bucket prefetch spends on page walks
   would mostly go away.

**Verdict: 50 ns inside the server is within reach, but not on this VM
alone.**

- **Here:** huge-page records (62 ns) plus a cheaper hash (SipHash-1-3:
  ~4 ns less, estimated from its 9 rounds against 14; or AVX2 four-lane
  hashing across a batch: ~5–8 ns less) lead to about **54–58 ns**.
- **On a host that backs memory with huge pages:** the remaining walk
  stalls shrink as well, which should take it under 50.
- **The store alone** is already at 40–43 ns.

## Why `bench_store` says ~80 ns

`bench_store` (ADR 0006) measured 78.6 ns for `b16` at 1M keys on the same
quiet host, against 43.5 ns here. It is ~10 ns slower even in L2 (31.5 vs
21.1 ns). Its absolute numbers include its own bookkeeping:

- **Key formatting in the timed loop:** a hex loop writes the key byte by
  byte, and then SipHash reads it back as 8-byte words, which store
  forwarding cannot serve.
- **A verification step** that also reads a random slot of an 8 MB
  `version[]` array (+3.4 ns here).
- **A store mutated by earlier runs:** in each repetition every pattern
  runs back to back, and `zmix` replaces records.

Its paired *ratios*, the result it was built for, are unaffected. Its
absolute figure is not the store's share of a request.

## Files

| File | What |
|---|---|
| `lookup_decomp.c` → `lookup_decomp.out` | the server's batched pipeline on the real store objects: {SipHash, precomputed hashes} × {glibc 4 KiB-page records, a 2 MiB-page slab with glibc's exact chunk spacing}, at 8 Ki and 1 Mi keys, 11 rotated repetitions. The allocator switch is `--wrap=malloc`/`--wrap=free`, so only the store's own calls are redirected. The output also holds `bench_store`'s `b16` rows from the same session |
| `tlb_chase.c` → `tlb_chase.out` | a dependent chase through one line per 4 KiB page, on 4 KiB vs 2 MiB pages, from 4 MiB to 1 GiB |
| `thp_check.sh` → `thp_check.out` | proof that the tunables put the server's records on huge pages: AnonHugePages 14,336 → 174,080 KiB with 1M keys |
| `prof_setup2.sh`, `profile-busypoll-*-records.txt` | kernel-inclusive profiles of one server core, busy-polling client, without and with huge-page records. Method and report script as in `../2026-09-26-profile-memory` |
| `ab4.py` → `ab-hugepage-records.out` | a concurrent paired A/B: the same binary, B with `GLIBC_TUNABLES=glibc.malloc.arena_max=1:glibc.malloc.hugetlb=1:glibc.malloc.top_pad=268435456`, vCPU slots swapped every block |

## How to get huge-page records in production

- **Tunables** work today with no code change. But `arena_max=1` makes
  every worker's Puts share one malloc arena and its lock. That is fine at
  one worker, and unmeasured for multi-core writes.
- **The durable version** is a record slab carved from prefaulted 2 MiB
  chunks in `store.asm`: per-writer free lists, returned through QSBR. It
  keeps the win without the shared lock and also cuts the malloc overhead
  per key (160-byte chunks for 140-byte records). It is the cookbook's §1
  lead, now with a measured +6%.
