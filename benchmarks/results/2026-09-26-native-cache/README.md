# Native HTTP/2 engine with the header-block cache — N1-N3, 2026-09-26

The second measurement of KvAsm's native HTTP/2 engine
([ADR 0007](../../../docs/adr/0007-native-http2-engine.md)). The full
report is in [REPORT.md](REPORT.md); the raw records are in `raw.jsonl`.

**What was measured.** The `kvasm:0.1.0` image built from the working tree
with the engine's first version plus two changes that a profile pointed
to:

- **A per-connection header-block cache.** HPACK blocks that are
  byte-identical under an unchanged dynamic table replay the effects of
  their previous decode. nghttp2-based clients such as kvbench never index
  `:path`, so before this every request Huffman-decoded its path: about 23%
  of user time.
- **O(1) stream-map removal:** a displaced-entry counter lets removal skip
  the backward-shift scan.

It did not yet have zero-copy request bodies, the inline head copy or the
table prefault; see [2026-09-26-native-final](../2026-09-26-native-final/README.md).
`native` was already the default engine; the systems here name their
engine explicitly anyway.

**Host:** outside load was a median of 5.0, 7.7 and 5.0 logical CPUs for
N1, N2 and N3, close to the published standard session (2-7). Absolute
levels match it: KvAsm with nghttp2 did 785k GET/s on 1 core here, against
813k in E1.

| Experiment | Result (native ÷ X, paired over 6 blocks, sign test) |
|---|---|
| N1, 1 core, GET | **1.61M GET/s. 1.14× Redis** (1.42M), **1.18× Valkey** (1.36M), 2.05× nghttp2 (785k); all p = 0.031. Server CPU per GET: 622 ns vs Redis 708. p50 0.85× Redis's, p99 equal |
| N2, scaling | 3.16M at 2 cores (2.29× nghttp2), **7.80M at 4 cores** (3.06× nghttp2) |
| N3, no-op floor | 2.00M/s: 0.99× Redis (not significant, 501 vs 495 ns of CPU per op), 1.07× Valkey (p = 0.031), 2.50× nghttp2 |

Every one of the 72 runs was valid. The weakest N1 block against Redis was
a near-tie (1.00×): all six blocks favored native, but the margin is thin
in some of them. That is why the final run adds the remaining per-request
savings.
