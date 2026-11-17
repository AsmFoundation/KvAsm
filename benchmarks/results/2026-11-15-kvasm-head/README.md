# The standard suite for KvAsm at main (850cfd9), paired with the 2026-09-27 image (2026-11-15)

**What:** the owner's "re-measure the full suite's KvAsm (not the other
stores)" for the code on `main` now.
- **The code:** [ADR 0011](../../../docs/adr/0011-batched-writes-per-worker-reclamation.md)
  (batched writes) plus the audit's request-path changes (`8111e66` …
  `850cfd9`: AES-NI key hash, protobuf fast path, inline streams, the
  16 KiB fix).
- **Same design as the ADR 0011 session**
  ([2026-11-15-kvasm-standard](../2026-11-15-kvasm-standard/README.md)):
  two KvAsm builds in the same blocks, because this host had moved since
  2026-09-27 ([pass 1](../2026-11-15-kvasm-pass1/README.md)).

```sh
KVASM_BENCH_IMAGE=kvasm:850cfd9 python benchmarks/run.py --profile standard \
    --systems kvasm --control-image kvasm:0.1.0 --e4-rates 418000,836000,1254000 \
    --out benchmarks/results/2026-11-15-kvasm-head
```

- **`kvasm`:** `kvasm:850cfd9`, built from `main` at `5b1161d`, whose
  server code is `850cfd9`'s. The build passed every server suite and
  h2spec 146/146.
- **`kvasm-control`:** `kvasm:0.1.0` (`3728b25d…`), the image the
  2026-09-27 suite measured.
- **Recorded:** clean tree; 272 runs, all valid.
- **A clean session:** 222 of 272 run windows were clean, and no other
  work ran in the VM (`vmwatch-verdict.txt`).
  - The 50 flags are blips: 0.25–0.8 vCPU of side load, or the host's idle
    daemons just over 20 ms per interval.
  - A median 4.4 CPUs were busy outside the VM.

## Result: new ÷ the 2026-09-27 image, paired today

Paired by block, with a sign test. 6 blocks reach p = 0.031 only when all 6
agree; 3-repetition cells (E4, E6, E7) can never be significant.

| Cell | Main | Control | Main ÷ control |
|---|---|---|---|
| **E2, 100% SET, 4 cores** | **4.43M/s** | 1.41M/s | **3.14×** [2.77–3.31], 6/6 |
| **E2, 50% GET, 4 cores** | **4.28M/s** | 2.28M/s | **1.88×** [1.84–1.91], 6/6 |
| E2, 90% GET / Zipf | 4.71 / 4.76M/s | 4.48 / 4.40M/s | 1.05× (n.s.) / **1.08×** (6/6) |
| **E6, 16 KiB values** | **646k/s** | 357k/s | **1.81×** [1.75–1.93], 3/3 |
| E6, 1 KiB values | 3.84M/s | 3.50M/s | 1.10× [1.01–1.10], 3/3 |
| **N1, 1 core** | **1.01M/s**, 993 ns | 957k/s, 1,049 ns | **1.06×** GET/s, **0.95×** CPU per GET (6/6) |
| E1, GET, 1 / 2 / 4 / 8 cores | 0.99 / 2.47 / 4.92 / 5.75M | 0.94 / 2.34 / 4.89 / 5.63M | **1.05** (6/6) / 1.06 / 1.00 / 1.02. CPU per GET at 8 cores **0.92×** (6/6) |
| S1, GET, 4 cores (10 blocks) | 4.84M/s | 4.73M/s | 1.02× (n.s.) |
| E3, no pipelining (256×1) | 506k/s | 503k/s | 1.01× (n.s.) |
| E5, no-op | 1.21M/s, 835 ns | 1.21M/s, 834 ns | 1.00× (n.s.) |
| N3, no-op | 1.13M/s, 887 ns | 1.17M/s, 854 ns | **0.96×**, CPU **1.04×** (6/6) |
| E8, kvbench / h2load, one key | 5.11 / 3.25M/s | 4.99 / 3.25M/s | 1.02 / 1.00 (n.s.) |

- **Writes:** 3.14× on 100% SET and 1.88× on 50/50. The ADR 0011 build
  measured 2.99× and 1.71× (in a noisier session). The request-path work
  adds a little on top, mostly on the mixed workload.
- **16 KiB values:** 1.81×. This is the audit's fix, confirmed in the
  suite's placement.
- **One core:** +5–6% GET/s at −5% CPU per GET. 8 cores: −8% CPU per
  GET. At 4 cores, throughput and CPU per GET are level (1.00× and 0.97×,
  n.s.).
- **The no-op health check got ~4% dearer** in N3 (6/6 blocks: CPU per
  check 887 vs 854 ns). E5, the same cell measured separately, shows
  1.00×. A small cost on the cheapest call, which the request-path
  changes did not target, and the one regression this session shows.
- **E7 (3 repetitions)** moves both ways: 64×1 1.21×, 64×16 1.11×, 64×4
  0.86×, 16×1 0.93×. None is significant. E4's latencies at fixed load are
  within noise.

## Against the other stores: estimates carried over from 2026-09-27

new ÷ X ≈ (main ÷ control, paired today) × (control ÷ X, paired on
2026-09-27). This assumes only that the build's gain over the old image is
the same on both days' hosts (`transfer.py` → `transfer.md`; brackets
multiply the extremes of both factors).

| Cell | ÷ Redis | ÷ Valkey | ÷ Dragonfly | ÷ Garnet | ÷ Memcached |
|---|---|---|---|---|---|
| **100% SET, 4 cores** | **2.72×** | **3.95×** | **2.83×** | **1.87×** [1.61–2.17] | **4.56×** |
| **50% GET, 4 cores** | **2.68×** | **3.17×** | **3.05×** | **1.85×** [1.78–2.01] | **3.01×** |
| 90% GET, 4 cores | 2.80× | 2.67× | 3.31× | 2.03× | 2.73× |
| 100% GET, 4 cores (S1) | 3.14× | 2.83× | 3.76× | 2.34× | 3.01× |
| 100% GET, 1 core (N1) | **1.33×** | **1.31×** | — | — | — |
| 1 KiB values | 3.21× | 3.30× | 4.01× | 1.94× | 2.48× |
| **16 KiB values** | 1.89× | 3.19× | 1.96× | **0.90×** | **0.88×** |
| No-op (E5) | 1.02× | 1.11× | 1.15× | 1.64× | 3.89× |

- **Writes and mixes:** these were the "behind" lines of 2026-09-27 (SET:
  0.87× Redis, 0.60× Garnet; 50/50: level with Garnet). The estimates now
  put KvAsm ahead of every store on both. W1, measured directly on a
  loaded host, agrees: SET 2.79× Redis … 1.72× Garnet.
- **16 KiB values:** from 0.50× to an estimated 0.90× Garnet and 0.88×
  Memcached. That is close to level but still behind, the one cell where
  KvAsm trails.

**The same, as numbers on the 2026-09-27 host**
(main ÷ control × KvAsm's 2026-09-27 median; estimates under the same
assumption):

| Cell | 2026-09-27 | Main, estimated |
|---|---|---|
| 100% SET, 4 cores | 2.23M | **7.0M** |
| 50% GET, 4 cores | 3.79M | **7.1M** |
| 90% GET, 4 cores | 7.66M | 8.0M |
| 100% GET, 4 cores (S1) | 8.84M | 9.0M (client-bound there; the client tops out near 9.3M) |
| 100% GET, 1 core (N1) | 1.76M | 1.86M |
| 1 KiB / 16 KiB values | 5.59M / 481k | 6.1M / 870k |

## The host

- **This session's absolute numbers are not the 2026-09-27 host's.** The
  control image ran at 0.47–0.98× its 2026-09-27 throughput, depending on
  the cell (`transfer.md`, column "control ÷ 09-27"). S1 and N1 sat at
  about 0.53×.
- **Outside load was not the cause:** it was lower today. The likeliest
  cause is where Hyper-V places the VM's vCPUs on this hybrid CPU.
- **The pairing makes every ratio here immune to it.**

## Files

| File | What |
|---|---|
| `raw.jsonl`, `environment.json`, `REPORT.md`, `charts/`, `run.log` | The session and analyze.py's report. Every comparison table has "KvAsm ÷ KvAsm control" rows |
| `transfer.py` → `transfer.md` | Every cell: main ÷ control, control ÷ 2026-09-27, and the carried-over estimates |
| `compare_sessions.py` → `compare.md` | The unpaired cross-session view, distorted by the host's move; kept for completeness |
| `vmwatch.sh`, `vmwatch.log`, `vmwatch_report.py` → `vmwatch-verdict.txt` | The in-VM watcher (`b969e23fb08a`) and its verdict |

```sh
python vmwatch_report.py --log vmwatch.log --runs raw.jsonl --watch b969e23fb08a \
    --dev bae3fd5a0c43 --idle 577cf5134f8d,57e53854c733,a19fb32fe59d,buildx --offset 0
python transfer.py . ../2026-09-27-standard > transfer.md
```
