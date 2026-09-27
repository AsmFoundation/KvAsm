# KvNode protocol and client contract

Everything a KvNode client needs, in any language. The normative contract is
[`proto/kvnode/v1/kvnode.proto`](../proto/kvnode/v1/kvnode.proto); this page
adds the parts protobuf cannot express. The .NET client
([`client/dotnet`](../client/dotnet)) is the reference implementation, and its
tests pin every rule below.

## Transport

- gRPC over **HTTP/2 cleartext** (h2c, "prior knowledge", no HTTP/1.1
  upgrade) on port 50051 by default. Put TLS in front with a mesh sidecar or a
  TLS-terminating proxy when traffic crosses an untrusted network.
- Unary calls only: exactly one request message per call.
- Messages must be uncompressed. A request with the compressed flag set is
  refused. With a `grpc-encoding` other than `identity` the answer is
  `UNIMPLEMENTED` plus `grpc-accept-encoding: identity`. Without one, the
  sender contradicts itself and the answer is `INTERNAL`.
- Requests that are not gRPC get plain HTTP answers with a short text body:
  a non-POST gets `405` with `allow: POST`, and a missing or foreign
  `content-type` gets `415` with `accept: application/grpc`.

## Methods

| Method | Request | Response | Notes |
|---|---|---|---|
| `/kvnode.v1.KvNode/Get` | `key` | `found`, `value` | A missing key is `OK` with `found = false`. An empty stored value is `found = true` and an empty `value`. |
| `/kvnode.v1.KvNode/Put` | `key`, `value` | `created` | Atomic replace: readers see the old or the new value, never a mix. `created = false` means an existing value was replaced. |
| `/kvnode.v1.KvNode/Delete` | `key` | `deleted` | `deleted = false` means the key was absent. |
| `/grpc.health.v1.Health/Check` | `service` | `status` | `""` or `kvnode.v1.KvNode` gets `SERVING`, or `NOT_SERVING` once the server drains for shutdown. Other services get `NOT_FOUND`. `Watch` is `UNIMPLEMENTED`. |

## Status codes

| Code | When |
|---|---|
| `INVALID_ARGUMENT` (3) | empty key, key over `KVNODE_MAX_KEY_BYTES`, value over `KVNODE_MAX_VALUE_BYTES`, malformed protobuf |
| `NOT_FOUND` (5) | health check for an unknown service |
| `RESOURCE_EXHAUSTED` (8) | store at `KVNODE_CAPACITY` keys or at `KVNODE_MAX_MEMORY_MB`; request message larger than key + value limits allow |
| `UNIMPLEMENTED` (12) | unknown method or service; unsupported compression codec |
| `INTERNAL` (13) | framing violations (no message, two messages, bad length prefix, compressed flag without codec) |

**Out of memory while answering.** Replies are framed straight into the
connection's output buffer. If the server cannot grow it, or cannot buffer
a reply that must wait for flow-control window, it closes the connection,
and the client sees `UNAVAILABLE`. The call itself may already have been
applied: a Put or Delete completes in the store before its reply is
framed.

**Retries.** Get and health checks are safe to retry on `UNAVAILABLE`. A
retried Put or Delete is *applied* correctly, but its flag can be wrong. For
example, a retry after a lost reply reports `created = false` for a Put that
did create the key. The .NET client therefore retries only read-only calls.

## Long keys: client-side compaction

Keys longer than **64 bytes** are sent as the **65 bytes** `0xFF ‖ SHA-512(key)`.
Keys of 64 bytes or less are sent unchanged.

- *Why:* bounded memory and compare cost per key on the server, without
  trusting clients with bucket placement. The server always hashes keys with
  its own secret SipHash key, so clients cannot engineer collisions.
- *Collision safety:* raw keys are at most 64 bytes and compacted keys are
  exactly 65 bytes, so the two spaces are disjoint by length. Two long keys
  collide only if SHA-512 does (about 2⁻²⁵⁶ per pair).
- *Consistency:* every client of one server **must** apply the same rule, or
  one logical long key is stored under two different keys. It is on by
  default in the official clients. Raw mode exists for callers that must
  control exact key bytes. A raw 65-byte key starting with `0xFF` shares the
  compacted key space.
- *Snapshots* apply the same rule (see below), so data loaded from a
  snapshot is found by the clients.

## Snapshot file format (`KVNSNAP1`)

A snapshot is the bulk data source a server loads at startup
(`KVNODE_SNAPSHOT=/path`). All integers are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `KVNSNAP1` |
| 8 | 4 | version = 1 |
| 12 | 4 | flags = 0 |
| 16 | 8 | record count |
| 24 | 8 | reserved = 0 |
| 32 | … | records: `u32 key_len`, `u32 value_len`, key bytes, value bytes |
| end − 16 | 8 | trailer magic `KVNSEND1` |
| end − 8 | 8 | checksum: SipHash-2-4-128 with a 16-byte all-zero key, **low** 64-bit word, over every byte before the trailer |

The server verifies the file before storing anything, in this order: size,
magic, version and flags, trailer, and checksum. Only then does it parse the
records, each bounds-checked against the data and the key/value limits; the
record count must consume the data exactly. Any violation stops startup with
the precise reason in the log. Duplicate keys are allowed, and the last one
wins.

The checksum detects corruption: truncation, bit rot, or the wrong file. It
is not a MAC, because the key is public. Protect snapshot files with
file-system permissions when tampering is a concern.

Writers must make replacement atomic: write a temporary file in the same
directory, flush it to disk, then rename it over the target. A crashed export
must never leave a partial file where the server looks. The .NET
`SnapshotFile.WriteAsync` does exactly this.

**Conformance vector.** The 3-record file for keys `snap-0..2`, where record
*i* has `i mod 301` value bytes and byte *j* is `(i·131 + j·7) mod 256`, is:

```
4b564e534e415031 01000000 00000000 0300000000000000 0000000000000000
06000000 00000000 736e61702d30
06000000 01000000 736e61702d31 83
06000000 02000000 736e61702d32 060d
4b564e53454e4431 2d587aca1228a35d
```

The server-side tool (`server/tests/integration/mksnap`) and the .NET writer
both produce these 93 bytes. A new writer should reproduce them before it is
trusted.
