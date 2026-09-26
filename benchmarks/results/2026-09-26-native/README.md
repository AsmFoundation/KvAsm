# Native HTTP/2 engine, first version — N1-N3, 2026-09-26

The first measurement of KvNode's own HTTP/2 engine (`server/src/h2n.asm`,
[ADR 0007](../../../docs/adr/0007-native-http2-engine.md)) against nghttp2,
Redis and Valkey. The full report is in [REPORT.md](REPORT.md); the raw
records are in `raw.jsonl`.

**What was measured.** The `kvnode:0.1.0` image built from the working tree
before h2n.asm was committed. The engine was complete (h2spec 146/146, the
full itest suite), but the header-block cache, O(1) stream-map removal,
zero-copy request bodies and the table prefault were not in it yet. Those
arrived in the next two runs:
[2026-09-26-native-cache](../2026-09-26-native-cache/README.md) and
[2026-09-26-native-final](../2026-09-26-native-final/README.md). This run is
kept as the record of the engine's first version, not as its current
performance.

**The host was heavily loaded.** During N1, other programs kept a median
**27.2 of 32 logical CPUs** busy outside the container VM (24 of 24 runs
above the 2-CPU "busy" mark). N2 and N3 saw 6.1 and 5.1. Absolute numbers
are therefore far below the published quiet-host levels: Redis did 319k GET/s
on 1 core here, against 1.45M in E1. The paired, blocked design still makes
the within-block ratios valid, but see the caveat below.

| Experiment | Result (native ÷ X, paired over 6 blocks, sign test) |
|---|---|
| N1, 1 core, GET | 369k GET/s. 1.79× nghttp2, 1.16× Redis, 1.18× Valkey (all p = 0.031) |
| N2, scaling | 2.16× nghttp2 at 2 cores, 2.66× at 4 cores (7.29M GET/s) |
| N3, no-op floor | 1.68M/s. 2.00× nghttp2, **0.82× Redis** (p = 0.031), 0.89× Valkey (not significant) |

**Caveat on N1's ratio against Redis and Valkey.** With the host's memory
system saturated, every cache miss costs more. KvNode overlaps its misses
(batched, prefetched GETs), while Redis and Valkey take theirs one after
another, so heavy outside load favors KvNode's GET ratio. The quieter runs
that followed are the ones to quote.
