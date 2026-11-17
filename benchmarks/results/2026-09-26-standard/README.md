# Standard session under heavy outside load (supplementary)

This session ran the full standard profile: 711 runs, all valid, 03:23 to
08:30 on 2026-09-26, at revision `ba9e005`. Its server code is identical to
what the published session measures. **It is not the published result.**
The published result is
[`../2026-09-26-standard-monitored`](../2026-09-26-standard-monitored).
That choice was made before the comparisons of either session were
examined.

## Why it is supplementary

While it ran, other programs on the same PC used **14–23 of the 32 logical
CPUs** outside the container VM, measured by sampling the Windows host with
psutil at about 04:00:

- an optimization job with its worker processes;
- a data downloader, an Android emulator, a game, and two builds of a
  desktop app;
- a runaway `awk` stuck in an infinite loop. It was ended with the owner's
  permission, after this session.

The run itself showed the damage:

- **Identical runs differed by up to 3×.** In E0, Memcached at 8 cores
  measured 5.29M, 1.52M and 4.97M GET/s.
- **Throughput changed within single runs.** One Valkey window fell from
  1.46M to 0.44M GET/s.
- **Slow runs burned as much server CPU as fast ones** for a fraction of the
  work: the hypervisor took the vCPUs away while the guest still counted
  them as busy.

The per-run outside-load monitor did not exist yet, so this session's
report cannot show or adjust for that load. It was added after this session
for exactly that reason.

## What it is still good for

- **S1, which ran last, when the host had quietened.** Across its ten runs
  per system the between-run CV is **1.1–1.7%**. The instrument itself is
  stable. S1 ÷ E1 (the same cell, hours apart) is 1.26–1.87× for every
  system, which shows how much the loaded hours depressed E1.
- **The S1 medians give the best idea of an unloaded 4-core machine:**
  Garnet 4.20M, Valkey 3.52M, KvAsm 3.14M, Redis 3.00M, Memcached 2.80M,
  Dragonfly 2.53M GET/s.
- **E8, which ran just before S1, validates the client.** kvbench was faster
  than every standard tool on the same server: Redis 3.02M (kvbench) vs
  2.86M (redis-benchmark) vs 2.60M (memtier_benchmark), and KvAsm 3.19M
  (kvbench) vs 2.67M (h2load) on one key.
- **The paired comparisons stay internally consistent.** Load that is shared
  within a block cancels, but load that changes within a block adds noise
  and can tilt single cells.

## Files

- [`REPORT.md`](REPORT.md): the generated report, with the same analysis
  code as the published session.
- `raw.jsonl`: one record per run.
- `environment.json`: host, images and digests, calibration.
- `charts/`: charts generated with the report.
