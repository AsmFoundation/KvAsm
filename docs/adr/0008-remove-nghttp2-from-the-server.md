# ADR 0008: Remove nghttp2 from the server

- **Status:** accepted
- **Date:** 2026-09-26
- **Supersedes:** [ADR 0002](0002-http2-library-nghttp2.md) (HTTP/2 via
  nghttp2)
- **Amends:** [ADR 0007](0007-native-http2-engine.md): the nghttp2
  alternative and `KVASM_H2_ENGINE` are gone

## Context

ADR 0007 made KvAsm's own HTTP/2 engine (`h2n.asm`, with the HPACK decoder
`hpack.asm`) the default. It kept nghttp2 1.70 one setting away, as a
fallback and as a reference. The direction since: "So we are discarding
nghttp fully."

The native engine no longer needs a fallback:

- **Conformance:** h2spec 2.6.0 **146/146** with an empty baseline. nghttp2
  1.70 passed 138 (it no longer tracks closed streams or validates
  PRIORITY).
- **HPACK:** `test_hpack` decodes 30,000 random header lists from nghttp2's
  deflater (194,660 fields) identically, plus the RFC 7541 vectors.
- **Hostile input:** `fuzz_h2` runs in every build, with liveness and memory
  bounds, and the full suite must still pass on the fuzzed server.
- **Interop:** grpc-dotnet (an HTTP/2 stack that shares nothing with
  nghttp2 or KvAsm) passes 190/190.
- **Speed:** 2.03× nghttp2 on one core, 1.96-2.07× at 2 and 4 cores
  (`benchmarks/results/2026-09-26-native-final`).

What keeping nghttp2 in the server still cost:

- **Two code paths.** They ran through `worker.asm` (read, flush, linger,
  destroy, idle timeout, drain) and `grpc.asm` (five callbacks, a stream-id
  cache, a data provider and three submission helpers).
- **A hand-mirrored ABI.** `nghttp2.inc` held constants and struct offsets,
  checked by `test_nghttp2_abi`, plus nghttp2's 40-byte `nghttp2_nv` layout
  in the static response tables.
- **Unused state and settings.** Connection fields the native engine never
  used, a shared callback table, and a configuration setting.
- **A doubled test matrix,** with a second h2spec baseline.
- **A larger binary and a CVE surface:** 162 KB more code. ADR 0002
  already had to pin nghttp2 ≥ 1.68.1 for CVE-2026-27135.

## Decision

The server links no HTTP/2 library. `h2n.asm` and `hpack.asm` are its only
HTTP/2 implementation.

**Removed from the server:**

- **`grpc.asm`'s nghttp2 layer:**
  - the callbacks: `on_begin_headers`, `on_header`, `on_data_chunk_recv`,
    `on_frame_recv`, `on_stream_close`;
  - the stream-id cache (`Conn.cur_id`, `Conn.cur_stream`) and the data
    provider (`resp_read`);
  - the helpers `respond_message`, `respond_error`, `respond_varint` and
    `kv_grpc_callbacks_new`.
  
  `kv_grpc_dispatch` is now the dispatcher itself. It answers only through
  `kv_h2n_reply_buf`, `kv_h2n_reply_static` and `kv_h2n_reply_headers`, and
  a GET only through `kv_h2n_reply_parts` (no allocation, no copy of the
  value beyond the one into the DATA frame).
- **`worker.asm`'s session paths:**
  - `mem_recv2` and `mem_send2` (with the copy and grow loop of the send
    path);
  - `want_read` and `want_write`, `session_del`, `terminate_session`,
    `submit_goaway`;
  - the `CONN_NATIVE` branches.
  
  Every connection reads through `kv_h2n_read` and flushes through
  `kv_h2n_pump`. Idle timeout and drain both send GOAWAY through
  `kv_h2n_goaway`.
- **Startup and configuration:** `main.asm`'s callback table and its
  `h2_engine=` log field, and `config.asm`'s `KVASM_H2_ENGINE` choice.
- **Structure fields:** `Config.h2_engine`, `Worker.callbacks`,
  `Worker.mem`, `Conn.session`, `CONN_NATIVE`.
- **Files:** `server/include/nghttp2.inc`, `server/tests/test_nghttp2_abi.c`
  and the second h2spec baseline. The one baseline left,
  `h2spec-known-failures.txt`, is empty.

**Replaced:** the static header tables (405/415 and Trailers-Only gRPC
errors) are arrays of `HdrField` (`server.inc`: name, value and their
lengths, 32 bytes). The `NV` macro builds them with `istruc`, so a layout
change cannot desynchronize them from `h2n.asm`'s encoder.

**Kept, deliberately outside the server:** nghttp2 stays in the toolchain
image for three independent tools:

- `itest_grpc` drives the end-to-end suite through nghttp2's client API;
- `test_hpack` uses nghttp2's deflater and inflater as the differential
  oracle;
- `kvbench` is the gRPC benchmark client. It is the same client as every
  published result, so sessions stay comparable.

A server checked only by its own code verifies only its own understanding
of the protocol. These tools, h2spec and grpc-dotnet are the outside view.
None of them faces untrusted peers.

## Consequences

- **No fallback engine.** A protocol bug in `h2n.asm` cannot be sidestepped
  by configuration. It must be fixed. Mitigations run in every build: h2spec
  146/146 with an empty baseline, the fuzzer, and the differential HPACK
  test. The .NET suite runs against every image.
- **Smaller binary:** 1,331,448 → 1,125,504 bytes unstripped (−15.5%), and
  text 1,014,804 → 852,272 bytes (−162,532). KvAsm's own code is 49 KB of
  text; the rest is static glibc. The image's binary is 1,105,104 bytes.
- **Less state per connection:** the connection record went from 872 to
  848 bytes, and there is no session object or callback table.
- **One code path:** every connection takes the same branch-free route
  through `conn_event` and `conn_flush`.
- **Operations:** `KVASM_H2_ENGINE` is ignored like any unknown variable.
  It lived for less than a day and was never released. The startup log no
  longer prints `h2_engine=`.
- **Security:** nghttp2 advisories no longer concern the server. They still
  concern the test clients, which only ever talk to the server under test.
- **Tests:** `make itest` runs each phase once (the former n-phases are
  phases 1-7) and takes about half as long.
- **Benchmarks:** the `kvasm-native` and `kvasm-nghttp2` variants are
  gone. N1 and N3 now run KvAsm against Redis and Valkey on one core, as a
  quick re-check of the headline. N2 (engine against engine) is retired.
  `analyze.py` still renders archived sessions exactly as they were
  published.

## Verification (this revision)

- `make test`: every native test passed.
- `make itest`: all 7 phases passed.
  - h2spec 146/146, 0 skipped.
  - 3,000 fuzz connections, with resident memory going from 2,796 to
    2,884 KiB.
  - The suite passed on the fuzzed server, which then exited 0.
- `docker build --target runtime`: every suite again inside the build.
  Passed.
- grpc-dotnet: 190/190 on net8.0 and net10.0 against the new image, with
  `KVASM_REQUIRE_DOCKER=1` so that nothing can skip.
