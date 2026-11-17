# The standard suite for KvAsm after ADR 0011, paired with the 2026-09-27 image (2026-11-15)

**Question:** after
[ADR 0011](../../../docs/adr/0011-batched-writes-per-worker-reclamation.md),
where does KvAsm stand in every cell of the standard suite? The other
stores were not re-measured, at the owner's request.

**Why a control:** this host had moved since 2026-09-27
([pass 1](../2026-11-15-kvasm-pass1/README.md)). The very image the
2026-09-27 suite measured now runs ~1.5× slower per GET. So the suite ran two
KvAsm builds in the same blocks:

```sh
KVASM_BENCH_IMAGE=kvasm:adr0011 python benchmarks/run.py --profile standard \
    --systems kvasm --control-image kvasm:0.1.0 --e4-rates 418000,836000,1254000 \
    --out benchmarks/results/2026-11-15-kvasm-standard
```

- **`kvasm`** is the build of `5735d7a` (ADR 0011), image
  `kvasm:adr0011`.
- **`kvasm-control`** is `kvasm:0.1.0` (`3728b25d…`), the image the
  2026-09-27 suite measured, built from a0264f7.
- **E4** ran at that session's offered loads.
- **Recorded:** revision `0e48073`, a clean tree. 272 runs, all valid. Outside
  the VM, a median 5.6 of 32 CPUs were busy (max 13.7).

This measures the change, not the newest code. Five more commits landed
on `main` while this suite ran (`8111e66`…`850cfd9`). They are another
session's request-path work: AES-NI key hash, protobuf fast path, the
16 KiB fix. [2026-11-15-kvasm-head](../2026-11-15-kvasm-head/README.md)
measures that code the same way.

## Result: writes up ~3×, reads unchanged

These are new ÷ control ratios, paired by block (6 blocks; the sign test
reaches p = 0.031 only when all 6 agree). Full tables: `REPORT.md` and
`transfer.md`.

| Cell | New | Control | New ÷ control |
|---|---|---|---|
| E2, 100% SET, 4 cores | **4.00M/s** | 1.34M/s | **2.99×** [2.32–3.41], 6/6 |
| E2, 50% GET, 4 cores | **3.73M/s** | 2.18M/s | **1.71×** [1.41–1.79], 6/6 |
| E2, 90% GET | 4.02M/s | 3.85M/s | 1.05× (n.s.) |
| E2, 90% GET, Zipf | 4.18M/s | 4.08M/s | 1.02× (n.s.) |
| E1, GET, 1 / 2 / 4 / 8 cores | 0.92 / 2.30 / 4.47 / 5.27M | 0.94 / 2.22 / 4.58 / 5.33M | 0.98 / 1.04 / 0.98 / 0.99 (all n.s.) |
| S1, GET, 4 cores (10 blocks) | 4.76M/s | 4.80M/s | 0.99× (n.s.) |
| N1, GET, 1 core | 909k/s, 1,106 ns | 901k/s, 1,117 ns | 1.01× (n.s.) |
| E3 (256×1), E5 / N3 (no-op), E6 (1 KiB, 16 KiB), E8 | — | — | 0.95–1.03 (all n.s.) |

- **Writes:** the A/B and solo results of
  [write-batching](../2026-11-15-write-batching/README.md) hold in the
  suite's own placement: 2.61× solo, 2.76× A/B, and 2.99× here. That was
  0.2.0 against ADR 0011; here the old build is the 2026-09-27 image.
- **Reads:** no cell moved. E7's 3-repetition cells and E4's latencies vary
  both ways, and none is significant. With 3 repetitions none could be.

## Against the other stores: estimates carried over from 2026-09-27

The host moved, so this session's absolute numbers cannot meet the other
stores' 2026-09-27 numbers. The control is the same image on both days,
which lets a ratio carry over:

new ÷ X ≈ (new ÷ control, paired today) × (control ÷ X, paired on 2026-09-27)

This assumes only that the new build's gain over the old one is the same on
both days' hosts. The brackets multiply the extremes of both factors'
block ratios (`transfer.py` → `transfer.md`).

| Cell | ÷ Redis | ÷ Valkey | ÷ Dragonfly | ÷ Garnet | ÷ Memcached |
|---|---|---|---|---|---|
| **100% SET, 4 cores** | **2.59×** [1.98–3.76] | **3.76×** [2.87–4.68] | **2.69×** [2.06–3.25] | **1.78×** [1.34–2.23] | **4.34×** [3.31–5.05] |
| **50% GET, 4 cores** | **2.45×** [1.97–2.90] | **2.89×** [2.36–3.09] | **2.79×** [2.27–2.97] | **1.69×** [1.36–1.88] | **2.75×** [2.23–2.96] |
| 100% GET, 4 cores (S1) | 3.05× | 2.74× | 3.65× | 2.27× | 2.92× |
| 100% GET, 1 core (N1) | 1.27× | 1.25× | — | — | — |
| 16 KiB values (E6) | 1.02× | 1.72× | 1.06× | 0.48× | 0.47× |

- **On 2026-09-27 the two write lines read** 0.87× / 1.26× / 0.90× /
  0.60× / 1.45× (100% SET) and 1.43× / 1.69× / 1.63× / 0.99× / 1.60×
  (50% GET).
- **This is the second line of evidence for writes ahead of every store.**
  The other is W1
  ([2026-11-15-writes-w1](../2026-11-15-writes-w1/README.md)), measured
  directly but on a loaded host: SET 2.79× Redis … 1.72× Garnet. Both agree
  within about 10%.
- **The read lines equal 2026-09-27's,** as they must when new ÷ control
  ≈ 1.
- **16 KiB values stay behind Garnet and Memcached** in this build. The
  regression was fixed afterwards, in `8111e66`.

## Interference

`vmwatch_report.py`, with this host's idle daemons declared via `--idle`
(see pass 1), flagged 167 of 272 runs (`vmwatch-verdict.txt`).
- **Cause:** another session built, tested and measured in `kvasm-dev`
  during most of E2–E7. Its five commits landed at 17:10–17:14.
- **Clean:** S1 (0 of 20 flagged) and most of E8, N1 and N3.
- **Most affected:** E2, with 46 of 48 runs flagged.

The pairing absorbs drift that both builds of a block share, but not a
burst that hits one run. For E2 the six SET blocks still agree (2.32–3.41).
They match the earlier clean A/B and solo results, so the write conclusion
stands. Smaller effects in E2–E7 should not be quoted from this session.

## Files

| File | What |
|---|---|
| `raw.jsonl`, `environment.json`, `REPORT.md`, `charts/`, `run.log` | The session, the environment (incl. `selection`) and analyze.py's report. Every table has "KvAsm ÷ KvAsm control" rows |
| `transfer.py` → `transfer.md` | new ÷ control per cell, the host's move (control today ÷ 2026-09-27), and the carried-over estimates against every store |
| `compare_sessions.py` → `compare.md` | Every cell against 2026-09-27 directly: unpaired, across sessions, distorted by the host's move. Kept for completeness |
| `vmwatch.sh`, `vmwatch.log`, `vmwatch_report.py` → `vmwatch-verdict.txt` | The in-VM watcher (`ffbafb7fae4a`, started for pass 1) and the verdict |

```sh
python vmwatch_report.py --log vmwatch.log --runs raw.jsonl --watch ffbafb7fae4a \
    --dev bae3fd5a0c43 --idle 577cf5134f8d,57e53854c733,a19fb32fe59d,buildx --offset 0
python transfer.py . ../2026-09-27-standard > transfer.md
```
