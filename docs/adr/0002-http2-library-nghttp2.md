# ADR 0002: HTTP/2 via nghttp2 1.70.0, statically linked

- **Status:** superseded by
  [ADR 0008](0008-remove-nghttp2-from-the-server.md), after being amended
  by [ADR 0007](0007-native-http2-engine.md). KvNode's own engine
  (`h2n.asm`) became the default, and nghttp2 was then removed from the
  server. It remains only as the independent client and HPACK oracle in the
  tests and in kvbench, built by the same pinned, verified Dockerfile step.
  The record below is kept as history: why nghttp2 was chosen, and what
  its versions did under h2spec.
- **Date:** 2026-09-26

## Context

KvNode serves gRPC, which requires HTTP/2: framing, HPACK, flow control,
stream state machines, and a long list of protocol-abuse defenses. There
is no production HTTP/2 implementation in assembly. Writing one would put
the riskiest, most attack-exposed code in the least reviewable language.

## Decision

Use **nghttp2** through its sans-I/O API (`nghttp2_session_mem_recv2`,
`nghttp2_session_mem_send2`). The server owns the sockets and the event loop;
nghttp2 only transforms bytes. Build it as a static, position-independent
library (C99, no C++ toolchain) and link it into a static-PIE server. The
Dockerfile pins the version *and* the SHA-256 of the tarball.

Why nghttp2: it is the HTTP/2 engine of curl, Apache httpd, and many
others. It is maintained by the author of RFC 7540's reference
implementation. Its callback API maps cleanly onto an assembly event loop,
and it enforces protocol rules (stream limits, flow control, header
validation, CONTINUATION and rapid-reset flood defenses) before any KvNode
code runs.

## Version: why 1.70.0 and not an older release

h2spec (v2.6.0, 146 cases), measured runs:

| Server | nghttp2 | Passed | Skipped | Failed |
|---|---|---|---|---|
| nghttpd (nghttp2's reference server) | 1.64.0 | 145 | 0 | 1 |
| assembly PoC server (bodyless responses) | 1.64.0 | 138 | 4 | 4 |
| assembly PoC server (bodyless responses) | 1.70.0 | 131 | 4 | 11 |
| KvNode before the 405/415 body fix | 1.70.0 | 131 | 4 | 11 |
| **KvNode** | **1.70.0** | **138** | **0** | **8** |

How the failures break down:

- **3 failures and 4 skips were server-side and are fixed.** h2spec's
  generic and stream-concurrency cases need a response with a body, and they
  hold the stream open by sending a zero flow-control window. KvNode's 405
  and 415 answers now carry a short text body, plus the headers RFC 9110
  asks for (`allow`, `accept`). With that, all 7 cases run and pass.
- **7 failures come with the library version.** The same set appears
  whenever 1.70.0 is linked, in the PoC and in KvNode, and never with
  1.64.0.
- **1 failure (5.1.1) is nghttp2 itself**, shared by every server above,
  nghttpd included.

The seven version-dependent failures are an upstream decision, not a KvNode
defect. **nghttp2 v1.65.0 (2025-03-02) removed RFC 7540 stream priorities**
(PR #2287) and deprecated closed-stream tracking (PR #2289). PRIORITY
frames are no longer validated, and frames on already-closed streams are
ignored rather than answered with `STREAM_CLOSED`. RFC 9113 itself
deprecated the RFC 7540 priority scheme. The remaining failure (5.1.1) is
shared by every nghttp2 release, nghttpd included.

Downgrading to regain those checkmarks is **not an option**:
**CVE-2026-27135** (high severity) is a remote assertion-failure denial of
service reachable through `nghttp2_session_terminate_session`. KvNode calls
that function for idle timeouts, and nghttp2 calls it internally on
connection errors. Upstream fixed it in 1.68.1, and every upstream release up
to 1.68.0 is affected. Distributions backport the fix; Debian's 1.64.0
*package* is patched. KvNode, however, builds from pinned upstream tarballs,
so for this build an older version means a vulnerable version.

The accepted failures are pinned in
`server/tests/integration/h2spec-known-failures.txt`. `make itest` (and
therefore the image build) fails on any *new* h2spec failure or skip.

## Consequences

- The server binary depends on no shared library, so the runtime image is
  `FROM scratch`: about 1.7 MB and no userland.
- nghttp2 constants and struct offsets are hand-mirrored in
  `server/include/nghttp2.inc`. `test_nghttp2_abi` compares every one of
  them against the real header of the linked version, so an upgrade that
  shifts a field fails the build instead of corrupting memory.
- Upgrading nghttp2 means changing the pinned version and SHA-256 and
  running `make test itest`. The h2spec baseline shows immediately whether
  protocol behavior changed.
