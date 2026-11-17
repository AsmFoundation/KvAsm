# Export probe: snapshot, scan and matching costs (2026-09-29)

Design-stage evidence for
[docs/proposals/export-watch.md](../../../docs/proposals/export-watch.md).
It measures, on the server's real store code, the numbers that decide how an
Export gets a consistent view and what a pattern-filtered scan costs.

- **What:** [`export_probe.c`](export_probe.c), built and run by
  [`run.sh`](run.sh) in the `kvasm-dev` container. The store modules are
  assembled fresh from `server/src` into a scratch directory.
- **Revision:** 0aa7f65. **Host:** i9-13900K, WSL2, THP `always`, one pinned
  vCPU (4), 5 repetitions, medians.
- **Workload:** 16-byte keys `k:%014llu`, 100-byte values, capacity = key
  count, so the table has the server's default load (63.6% of slots).
- **Isolation:** `docker events` for the run window shows only this run's
  two `exec`s: no other session touched the container. Other programs on
  the Windows host were not controlled, so treat absolute numbers as
  indicative. The conclusions rest on orders of magnitude.
- **Raw output:** [`output.txt`](output.txt). Every count is checked in the
  probe: a scan that skipped a record would have exited non-zero.

## Results

| | 1M keys (16 MiB table, 4,096 pages) | 4M (64 MiB) | 16M (256 MiB) |
|---|---|---|---|
| **collect**: copy every live record pointer (prefaulted array) | **2.34 ms** (2.34 ns/key) | 9.00 ms | **37.2 ms** |
| collect into a fresh array (page faults included) | 2.14 ms | 8.85 ms | 72.8 ms |
| tablecopy: `memcpy` of the whole bucket array | 1.21 ms | 2.97 ms | 16.3 ms |
| **page-pf**: one cold 4 KiB page, hot destination | **192 ns** | 176 ns | 199 ns |
| **page-fault**: the same into a fresh 4 KiB-page mapping | **836 ns** | 851 ns | 1,392 ns |
| occupancy: tags only | 1.13 ns/key | 0.99 | 1.09 |
| prefix: read key + 10-byte compare | 11.4 ns/key | 13.4 | 23.4 |
| dfa: read key + byte-class DFA (glob `k:*7?`) | 23.4 ns/key | 26.0 | 41.4 |
| **emit**: every key and value into 64 KiB batches | **24.9 ns/key, 4.9 GB/s** | 28.5, 4.3 GB/s | **40.3, 3.0 GB/s** |

## What it decides

1. **A snapshot taken under the writer lock is out.**
   - Copying the live pointers holds every writer for about 2.3 ns per
     key: 2.3 ms at 1M keys and 37-73 ms at 16M.
   - Today a Put holds that lock for at most ~450 ns. That bound comes from
     the 2.23M SET/s that 4 cores push through this single lock
     (`results/2026-09-27-standard`), so the stall is at least 5,000× a
     write's lock hold.
   - Even a bare `memcpy` of the bucket array takes 1.2 ms at 1M keys and
     16 ms at 16M.
2. **Copy-on-write of 4 KiB bucket pages bounds each writer instead.**
   - The first write to a page during an export pays one page copy: about
     0.2 µs, or 0.84-1.4 µs when that also faults in the destination page.
   - That happens once per page per export, at most 4,096 times for the
     default table.
   - A pool of prefaulted pages would remove the fault. The proposal
     makes it an optimization gated by measured write p99.
3. **An export is network-bound, not scan-bound.**
   - Producing output runs at 3.0-4.9 GB/s on one core, far above what one
     gRPC stream carries.
   - Pattern matching adds 10-20 ns per key over a plain prefix compare.
   - A filtered export still scans every key, because a hash table has no
     key order. That is about 0.4-0.7 s of one worker's CPU at 16M keys,
     spread over event-loop passes.

## Not measured here

- The cost of the event-history append in a writer's critical section. It
  needs the implementation; the proposal's gate G1 measures it.
- A 2 MiB (THP) snapshot mapping. A first touch there would zero 2 MiB, not
  4 KiB, which is why the proposal maps copies with `MADV_NOHUGEPAGE`.
- Anything over the network.
