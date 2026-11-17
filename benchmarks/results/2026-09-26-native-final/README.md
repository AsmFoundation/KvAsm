# Native HTTP/2 engine, final — N1-N3, 2026-09-26

The measurement behind the published 1-core claim: KvAsm's native HTTP/2
engine ([ADR 0007](../../../docs/adr/0007-native-http2-engine.md)) against
nghttp2, Redis and Valkey. The full report is in [REPORT.md](REPORT.md);
the raw records are in `raw.jsonl`.

**What was measured.** The `kvasm:0.1.0` image of the commit that adds the
engine, with everything in it:

- the header-block cache and O(1) stream-map removal (as in
  [2026-09-26-native-cache](../2026-09-26-native-cache/README.md));
- zero-copy request bodies, and the inline head copy;
- huge pages for the store's table (prefaulted).

The Docker build that produced the image ran every test, including h2spec
146/146 and the fuzzer. Each KvAsm system names its engine explicitly
(`KVASM_H2_ENGINE=native` or `nghttp2`), so the result does not depend on
the default (which is `native`).

**Host:** outside load was a median of 7.0, 8.0 and 6.6 logical CPUs for
N1, N2 and N3, with one spike block that hit every system at once; the
paired ratios absorb it. 72 of 72 runs were valid.

| Experiment | native | ÷ Redis | ÷ Valkey | ÷ nghttp2 |
|---|---|---|---|---|
| N1: 1 core, 100% GET, 1M keys, 100 B, 64×16 | **1.51M GET/s** | **1.19×** [1.10–1.35] | **1.22×** [1.14–1.34] | 2.03× |
| N1: server CPU per GET | 665 ns | 0.84× (788 ns) | 0.82× (813 ns) | 0.49× |
| N1: p50 / p99 latency | 586 / 1630 µs | 0.79× / 0.98× | 0.77× / 0.96× | 0.46× / 0.60× |
| N2: 2 cores | 2.56M | — | — | 1.96× |
| N2: 4 cores | 5.45M | — | — | 2.07× |
| N3: no-op round trip, 1 core | 1.93M/s | 1.00× (tie) | 1.07× | 2.40× |

Ratios are medians of the six within-block ratios, with the per-block range
in brackets. Every ratio shown to two decimals without "(tie)" had all six
blocks on the same side: the exact sign test gives p = 0.031, the smallest
possible with six blocks. The ties (N3 against Redis, N1's p99) are not
significant. N2 here is noisier than in the quieter
[2026-09-26-native-cache](../2026-09-26-native-cache/README.md) session,
where 4 cores reached 7.80M GET/s (3.06× nghttp2).

**Reading.**

- On one core KvAsm now serves more GETs than Redis and Valkey, with less
  CPU per request and lower median latency.
- For a pure no-op, a gRPC health check (HTTP/2 frames, HPACK, protobuf)
  now costs the same as a RESP `PING`.
- The remaining time per request is mostly kernel: one `read` and one
  `sendto` per 16 requests over loopback TCP, the same work every server
  here does.
