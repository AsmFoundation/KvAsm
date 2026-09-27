# Why 8 cores does not double on this host (2026-09-28)

The question: in the 2026-09-27 standard session, E1 went from 4.98M GET/s
at 4 cores to 5.99M at 8. Why does doubling the cores not double the
throughput?

That session's report inferred from utilization that KvNode's 8-core cells
are client-bound: KvNode's cores were only 0.52–0.70 busy while kvbench's
client was 80–92% busy ([its README](../2026-09-27-standard/README.md),
*The client is the ceiling*). This A/B tests that directly: the same
server and workload, and only the client's CPU budget changes.

## Setup

- **Server:** KvNode on vCPUs 0–7 with 8 workers. Image
  `sha256:3728b25dfe86`, the same one the standard session used; its server
  code is `a0264f7`, and the repository was at `18adf13`.
- **Workload:** E1's. 100% GET, 100-byte values and 1M preloaded keys, with
  64 connections × 16 in flight, i.e. 1,024 requests in flight in both arms.
- **A, the suite's client:** kvbench on vCPUs 16–31, with 16 threads.
- **B, a bigger client:** kvbench on vCPUs 8–31, with 24 threads. vCPUs
  8–15 are never used by the suite, so B adds 8 vCPUs without taking any
  from the server.
- **Protocol:** 6 blocks with the order alternating (AB, BA, …), a fresh
  server per run, 5 s warmup and 10 s measured. Everything else is
  `run.py`'s own `run_kvbench_cell`. The CPU-clock factor was measured at
  the start: 1.0671.
- **Host:** 5.4–6.8 logical CPUs busy outside the VM, the quiet level of the
  standard session's S1.
- **VM:** nothing else ran. No other kvnode session was busy, and the dev
  container was idle before and after. `docker-events.txt` lists every
  Docker event in the window: only the test's own containers appear, and
  there was no `exec` into `kvnode-dev`.

## Result

`ab_client.py` → `ab_client.out` (per run) and `ab_client.jsonl` (raw).

| 8 cores, 8 workers | A: client on 16 vCPUs | B: client on 24 vCPUs | B ÷ A (6 blocks) |
|---|---|---|---|
| GET/s | **9.72M** (9.67–9.89M) | 8.60M (8.19–8.70M) | **0.877×**, lower in 6/6, p = 0.031 |
| KvNode's cores busy | 0.69 (0.69–0.71) | 0.86 (0.84–0.91) | 1.23×, higher in 6/6, p = 0.031 |
| KvNode CPU per GET | 568 ns | 810 ns | 1.43× |
| client busy | **0.97** (0.97–0.98) | 0.68 (0.66–0.69) | |
| client CPU per request | 1,596 ns | 1,888 ns | 1.18× |

Values are medians, with the range of runs in parentheses.

1. **The standard client is saturated at 8 cores.** It was 97–98% busy
   while KvNode used 69% of its cores. A's 9.72M is the highest 8-core
   KvNode figure measured so far. E1's 5.99M ran with 7 workers, as chosen
   by E0 under load, and with ~16 CPUs of outside load.
2. **A bigger client on the same machine is slower, not faster.**
   - B was 12% slower in every block.
   - KvNode did get busier, but each GET cost 43% more of its CPU, and each
     request cost the client 18% more of its CPU.
   - B's outside load read about 0.6 CPU higher. With every vCPU busy, some
     hypervisor overhead lands outside `vmmemWSL`, so this is an effect of
     B rather than a confound. At the suite's load slope of 2–3% per CPU,
     it could account for about 2% at most.
3. **Why: the host is out of physical cores.**
   - The i9-13900K has 24 cores behind its 32 logical CPUs: 8 P-cores with
     two hyperthreads each, and 16 E-cores.
   - An 8-vCPU server plus a 16-vCPU client already keeps 24 busy. The 8
     spare vCPUs add hyperthread siblings, not cores. Using them slowed both
     sides, as B shows.
   - The same effect appears from 4 to 8 server vCPUs: 435 ns of CPU per
     GET at 4 cores (S1, same image, quiet host) against 568 at 8 (A).
     That comparison is unpaired, across sessions nine hours apart, so it
     is indicative only.
   - A second contributor, not tested here: a server that waits for
     requests (31% idle in A) gets fewer requests per epoll batch. Each
     `epoll_wait`, `read`, `sendto` and client wakeup is then spread over
     fewer requests.
4. **So on this host, 8 cores cannot double 4 cores.**
   - The gRPC client needs ~1,600 ns of CPU per request against KvNode's
     ~570, so a saturated 8-core server would need about 2.8 × 8 ≈ 22
     vCPUs of client, which this host cannot spare.
   - From 4 to 8 cores on the quiet host, KvNode went from 8.84M (S1) to
     9.72M (+10%), bounded by the client and by the shared cores.
   - KvNode's 8-core figures are floors. At A's per-GET cost, 8 fully busy
     cores would serve about 8 ÷ 568 ns ≈ 14M GET/s, if that cost held at
     saturation.
   - Measuring it needs the client on a second machine.

## Files

| File | What |
|---|---|
| `ab_client.py` | the A/B: arms, blocks, per-run figures, paired sign tests (imports `benchmarks/run.py` and `suite.py`) |
| `ab_client.out` | its console output: the per-run lines and the summary above |
| `ab_client.jsonl` | one raw record per run |
| `docker-events.txt` | every Docker event during the A/B, the proof that nothing else ran in the VM |

Reproduce, from the repository root, with the standard session's images:

```sh
python benchmarks/results/2026-09-28-client-ceiling/ab_client.py 6 | tee ab_client.out
```
