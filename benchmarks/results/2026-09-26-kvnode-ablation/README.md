# KvNode ablation, 2026-09-26

This measures which of three server changes produced KvNode's throughput
gain, with each change isolated. It is the evidence behind
[ADR 0005](../../../docs/adr/0005-batched-get-prefetch-and-worker-pool.md).

## Variants

Each variant adds one change to the previous one:

| Variant | Build |
|---|---|
| `s0-base` | KvNode 0.1.0 as committed in `d95793d` |
| `s1-cache` | + one-entry stream cache (`stream_lookup`) |
| `s2-pool` | + per-worker pool allocator (`pool.asm`, `nghttp2_mem`) |
| `s3-batch` | + batched GETs with two-stage prefetch (`kv_grpc_run_batch`) = commit `ba9e005` |

The intermediate variants were built from the working tree at each stage
of the change and were not committed separately. Reproducing `s1` and `s2`
means reverting the later changes by hand.

## Procedure ([`ablation.sh`](ablation.sh))

- The server runs as a plain process in the `kvnode-dev` container, pinned
  with `taskset` to vCPU 0 (1 core) or 0–3 (4 cores), with
  `KVNODE_WORKERS` = cores. kvbench runs on vCPUs 16–31 with 16 threads.
- Workload: 100% GET, 1,000,000 preloaded keys, 100-byte values, 64
  connections × 16 in flight, 2 s warm-up, 8 s measured. Every value is
  validated; all 104 runs were valid.
- Every repetition runs all four variants back to back, each with a fresh
  server process.
- **[`fixed-order.txt`](fixed-order.txt)**: 5 repetitions, always in the
  order s0, s1, s2, s3. This is a flawed design: a variant's position in
  the sequence is confounded with the variant itself.
- **[`rotated.txt`](rotated.txt)** (the reference): 8 repetitions with a
  Latin-square rotation. Repetition *r* starts at variant *r* mod 4, so
  every variant runs in every position equally often.

## Results

`python summarize.py rotated.txt fixed-order.txt`:

```
== rotated.txt
cores variant    n   median     min     max     CV   paired median [range]      faster
    1 s0-base    8   0.361M  0.233M  0.418M  15.8%   1.000 [1.000–1.000]
    1 s1-cache   8   0.348M  0.297M  0.413M  12.3%   0.991 [0.828–1.332]        3/8
    1 s2-pool    8   0.384M  0.308M  0.482M  14.4%   1.156 [0.843–1.459]        6/8
    1 s3-batch   8   0.459M  0.339M  0.667M  19.3%   1.349 [1.073–1.808]        8/8
    4 s0-base    8   1.837M  1.020M  2.048M  19.6%   1.000 [1.000–1.000]
    4 s1-cache   8   1.428M  0.663M  1.985M  29.2%   0.983 [0.361–1.151]        4/8
    4 s2-pool    8   1.973M  1.118M  2.251M  23.4%   1.095 [0.608–1.536]        6/8
    4 s3-batch   8   2.126M  1.616M  2.609M  15.0%   1.282 [1.073–1.585]        8/8
== fixed-order.txt
cores variant    n   median     min     max     CV   paired median [range]      faster
    1 s0-base    5   0.463M  0.421M  0.498M   5.9%   1.000 [1.000–1.000]
    1 s1-cache   5   0.444M  0.282M  0.465M  16.4%   0.987 [0.669–1.005]        1/5
    1 s2-pool    5   0.523M  0.372M  0.543M  14.8%   1.091 [0.882–1.150]        3/5
    1 s3-batch   5   0.640M  0.467M  0.684M  12.7%   1.352 [1.108–1.477]        5/5
    4 s0-base    5   1.633M  1.278M  1.730M   9.9%   1.000 [1.000–1.000]
    4 s1-cache   5   1.536M  1.187M  1.599M  10.3%   0.972 [0.841–0.989]        0/5
    4 s2-pool    5   1.759M  1.508M  1.837M   7.9%   1.125 [0.913–1.180]        4/5
    4 s3-batch   5   2.045M  1.963M  2.095M   2.3%   1.263 [1.135–1.601]        5/5
```

"Paired" means each variant divided by the baseline of the same
repetition. "faster" counts the repetitions in which the variant beat that
baseline.

## Reading

- **Batching with prefetch (s2 → s3) is the one clear win:** +35% at 1
  core and +28% at 4 cores (rotated paired medians). It was faster than the
  baseline in 8 of 8 rotated repetitions at both budgets: exact sign test
  p = 2/256 = 0.008. The fixed-order run agrees (+35%, +26%, 5 of 5).
- **The pool (s1 → s2) is probably positive but not conclusive:** paired
  medians +16% (1 core) and +10% (4 cores), faster in 6 of 8 (sign test
  p = 0.29).
- **The stream cache (s0 → s1) shows no benefit:** paired medians 0.99
  and 0.98. The fixed-order run had it slower in all 5 repetitions at 4
  cores (median 0.97, p = 0.06), but it always ran second there, so
  position cannot be ruled out. It is neutral at best.
- **2,000,000 GET/s at 4 cores:** s3's medians are 2.13M (rotated) and
  2.05M (fixed order). Its lowest rotated run was 1.62M, which shows how
  much this host moves between runs.

## What this run taught about the testbed

- **Between-run noise is large:** CV 12–29% per cell in the rotated run,
  2–16% in the fixed-order run an hour earlier. The host is WSL2 on a
  hybrid P/E-core i9-13900K; Hyper-V may place the pinned vCPUs on either
  kind of core.
- **Levels drift between sessions:** the same s3 build had a median of
  640k GET/s at 1 core in the first session and 459k in the second (−28%).
- **Ratios within a repetition stay consistent** even so: the batching
  change was faster in every one of the 26 repetitions across both runs
  and both budgets.

These three observations are why the benchmark suite uses blocked
randomization with paired statistics ([METHODOLOGY.md](../../METHODOLOGY.md)
§6–§8) instead of comparing medians of unpaired runs.
