# KvAsm protocol and client contract

Everything a KvAsm client needs, in any language. The normative contract is
[`proto/kvasm/v1/kvasm.proto`](../proto/kvasm/v1/kvasm.proto); this page
adds the parts protobuf cannot express. The .NET client
([`client/dotnet`](../client/dotnet)) is the reference implementation, and its
tests pin every rule below.

## Transport

- gRPC over **HTTP/2 cleartext** (h2c, "prior knowledge", no HTTP/1.1
  upgrade) on port 50051 by default. Put TLS in front with a mesh sidecar or a
  TLS-terminating proxy when traffic crosses an untrusted network.
- Get, Put, Delete, Download and the health check take exactly one request
  message. Upload is client streaming (any number of request messages);
  Download is server streaming (see [Streaming](#streaming-upload-and-download)).
- Messages must be uncompressed. A request with the compressed flag set is
  refused. With a `grpc-encoding` other than `identity` the answer is
  `UNIMPLEMENTED` plus `grpc-accept-encoding: identity`. Without one, the
  sender contradicts itself and the answer is `INTERNAL`. Upload applies
  this to every message of its stream.
- Requests that are not gRPC get plain HTTP answers with a short text body:
  a non-POST gets `405` with `allow: POST`, and a missing or foreign
  `content-type` gets `415` with `accept: application/grpc`.

## Methods

| Method | Request | Response | Notes |
|---|---|---|---|
| `/kvasm.v1.KvAsm/Get` | `key` | `found`, `value` | A missing key is `OK` with `found = false`. An empty stored value is `found = true` and an empty `value`. |
| `/kvasm.v1.KvAsm/Put` | `key`, `value` | `created` | Atomic replace: readers see the old or the new value, never a mix. `created = false` means an existing value was replaced. |
| `/kvasm.v1.KvAsm/Delete` | `key` | `deleted` | `deleted = false` means the key was absent. |
| `/kvasm.v1.KvAsm/Upload` | stream of `key`, `total_size`, `data`, `crc32c` | `created`, `crc32c` | Values up to `KVASM_MAX_UPLOAD_BYTES`, sent in chunks; stored atomically at the end, or not at all. |
| `/kvasm.v1.KvAsm/Download` | `key`, `offset`, `length`, `chunk_size` | stream of `total_size`, `data`, `crc32c` | A value or a byte range of it, in chunks; one version per call. A missing key is `NOT_FOUND`. |
| `/grpc.health.v1.Health/Check` | `service` | `status` | `""` or `kvasm.v1.KvAsm` gets `SERVING`, or `NOT_SERVING` once the server drains for shutdown. Other services get `NOT_FOUND`. `Watch` is `UNIMPLEMENTED`. |

Get refuses a value larger than `KVASM_MAX_VALUE_BYTES` (which only Upload
can store when `KVASM_MAX_UPLOAD_BYTES` is larger) with `RESOURCE_EXHAUSTED`
and the message "read it with Download": one unary message holding the whole
value is exactly what the streaming calls exist to avoid.

## Status codes

| Code | When |
|---|---|
| `INVALID_ARGUMENT` (3) | empty key, key over `KVASM_MAX_KEY_BYTES`, value over `KVASM_MAX_VALUE_BYTES` (Put) or `KVASM_MAX_UPLOAD_BYTES` (Upload), malformed protobuf; an Upload [rule](#upload) broken |
| `NOT_FOUND` (5) | Download of a missing key; health check for an unknown service |
| `RESOURCE_EXHAUSTED` (8) | store at `KVASM_CAPACITY` keys or at `KVASM_MAX_MEMORY_MB`; request message larger than key + value limits allow; Get of a value above `KVASM_MAX_VALUE_BYTES`; the connection's uploads in progress would exceed `KVASM_CONN_BUFFER_MB` |
| `OUT_OF_RANGE` (11) | Download `offset` past the end of the value |
| `UNIMPLEMENTED` (12) | unknown method or service; unsupported compression codec |
| `INTERNAL` (13) | framing violations (no message, two messages, bad length prefix, compressed flag without codec, a message flag other than 0/1, an Upload stream that ends inside a message) |
| `DATA_LOSS` (15) | Upload whose `crc32c` does not match the bytes received — nothing was stored |

**Out of memory while answering.** Replies are framed straight into the
connection's output buffer. If the server cannot grow it, or cannot buffer
a reply that must wait for flow-control window, it closes the connection,
and the client sees `UNAVAILABLE`. The call itself may already have been
applied: a Put or Delete completes in the store before its reply is
framed.

**Retries.** Get, Download and health checks are safe to retry on
`UNAVAILABLE`. A retried Put, Delete or Upload is *applied* correctly, but
its flag can be wrong. For example, a retry after a lost reply reports
`created = false` for a Put that did create the key. The .NET client
therefore retries only read-only calls — and a Download only until its first
response message arrives: a transfer broken mid-way is reported, not silently
restarted, because the value may have changed in between (resume with
`offset` only when the application knows it did not).

## Streaming: Upload and Download

For values larger than one comfortable message — up to
`KVASM_MAX_UPLOAD_BYTES` (default 64 MiB, at most 2³¹ − 1 and never above
`KVASM_CONN_BUFFER_MB`) — and for byte ranges of any value. Put/Get stay the
fast path for small values; the two families interoperate (Upload then Get,
Put then Download).

**Checksum.** CRC-32C (Castagnoli, reflected polynomial `0x82F63B78`, the
iSCSI and SSE4.2 one) with the usual pre- and post-inversion:
`crc32c("123456789") = 0xE3069283`, and the checksum of nothing is 0. It
travels as protobuf `fixed32` (4 bytes, little-endian).

### Upload

The request stream's messages (`UploadRequest`) obey these rules; a broken
rule is refused at once with `INVALID_ARGUMENT` unless noted, and nothing is
stored:

1. The **first** message carries `key` and `total_size`, the value's exact
   length. Inside the first message a repeated field replaces the earlier one
   (protobuf's last-wins). A first message without both is refused.
2. `key` and `total_size` come **before any data** and never in a later
   message.
3. The `data` fields of all messages, in order, are **exactly** `total_size`
   bytes. The byte after `total_size` is refused as it arrives; fewer bytes
   when the stream ends are refused then.
4. `crc32c` is optional, **at most once**, in any message. A mismatch is
   `DATA_LOSS`.
5. The request stream must end **between messages** (`INTERNAL` otherwise).
6. Messages and `data` fields may be empty; unknown fields, and known field
   numbers with another wire type, are skipped. Chunk sizes are free — 64 KiB
   to 1 MiB is efficient.

As soon as `key` and `total_size` are known, the server **reserves** the
final record: `total_size` above `KVASM_MAX_UPLOAD_BYTES` is refused
(`INVALID_ARGUMENT`), and so is a reservation the memory limit or the
connection's budget cannot take (`RESOURCE_EXHAUSTED`; each connection may
hold at most `KVASM_CONN_BUFFER_MB` of declared sizes in uploads in
progress). Every data byte is then copied **once**, straight from the
received frame into that record, and checksummed in the same pass. At the
end of the stream the server checks the size and the checksum and
**publishes** the record with one pointer store: readers see the old value
or the new one, never a part. `UploadResponse` reports `created` and the
CRC-32C of what was stored, so a client that sent no checksum can still
verify end to end.

**Refusals are immediate.** The server answers a broken rule the moment it
sees it — a Trailers-Only error while the client may still be sending — and
then resets the rest of the request stream with `RST_STREAM(NO_ERROR)`
(RFC 9113 §8.1), so the client stops. Clients must therefore take the
*response* status as the answer when a write fails after the server replied.
A cancelled call (`RST_STREAM` from the client), a broken connection and the
idle timeout all discard the unpublished record and give its reservation
back.

### Download

`DownloadRequest` names the key and the range: `offset` (default 0),
`length` (absent: to the end; a length reaching past the end is cut at the
end), and `chunk_size` (0: 64 KiB; above 1 MiB: 1 MiB). An `offset` past the
end is `OUT_OF_RANGE`; `offset` equal to the size is an empty range, not an
error.

The response is `k = max(1, ⌈range / chunk⌉)` messages: the **first** carries
`total_size` (of the whole value, not of the range), **every** message a
`data` chunk of exactly `chunk_size` bytes except the last (omitted when the
range is empty), and the **last** carries `crc32c` of exactly the range —
computed by the server as it sent the bytes; its presence also marks the end.
A client must check all three: a stream that ends without `crc32c`, carries
more data than the range, or fails the checksum is not a value.

**One version per call.** The server pins the record for the whole call, so
a value replaced or deleted meanwhile is still read whole as it was when the
call started. The pinned record stays in memory (and counted against
`KVASM_MAX_MEMORY_MB`) until the call ends. The response body is generated
frame by frame as the client's flow-control window opens: a slow reader
slows the call, it never grows the server's buffers. A reader that stops
entirely for `KVASM_IDLE_TIMEOUT_SEC` is disconnected, which releases the
pin — also when it stopped reading its socket altogether.

### Conformance vectors

Request and response bodies (gRPC message prefix included, hex, spaces only
for reading) for key `k` (`6b`) and the value `abc` (`crc32c = 0x364B3FB7`,
`b73f4b36` little-endian), on a server where `k` does not exist yet. The
end-to-end suite sends these exact bytes and requires these exact answers.

| Call | Request body | Response body |
|---|---|---|
| Upload `abc` with crc32c | `00 0000000f 0a016b 1003 1a03616263 25b73f4b36` | `00 00000007 0801 15b73f4b36` (created) |
| the same again | same | `00 00000005 15b73f4b36` (replaced: `created` omitted) |
| Download, `chunk_size = 2` | `00 00000005 0a016b 2002` | `00 00000006 0803 12026162` `00 00000008 120163 1db73f4b36` |
| Download, `offset = 3` | `00 00000005 0a016b 1003` | `00 00000007 0803 1d00000000` (empty range) |

## Long keys: client-side compaction

Keys longer than **64 bytes** are sent as the **65 bytes** `0xFF ‖ SHA-512(key)`.
Keys of 64 bytes or less are sent unchanged.

- *Why:* bounded memory and compare cost per key on the server, without
  trusting clients with bucket placement. The server always hashes keys with
  its own secret key (AES-128 or SipHash-2-4, ADR 0004), so clients cannot
  engineer collisions.
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

## Snapshot file format (`KVASNAP1`)

A snapshot is the bulk data source a server loads at startup
(`KVASM_SNAPSHOT=/path`). All integers are little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `KVASNAP1` |
| 8 | 4 | version = 1 |
| 12 | 4 | flags = 0 |
| 16 | 8 | record count |
| 24 | 8 | reserved = 0 |
| 32 | … | records: `u32 key_len`, `u32 value_len`, key bytes, value bytes |
| end − 16 | 8 | trailer magic `KVASEND1` |
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
4b5641534e415031 01000000 00000000 0300000000000000 0000000000000000
06000000 00000000 736e61702d30
06000000 01000000 736e61702d31 83
06000000 02000000 736e61702d32 060d
4b564153454e4431 0a633ad6a4e06994
```

The server-side tool (`server/tests/integration/mksnap`) and the .NET writer
both produce these 93 bytes. A new writer should reproduce them before it is
trusted.
