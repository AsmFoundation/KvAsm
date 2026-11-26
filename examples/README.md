# Example clients

Programs that talk to KvAsm over plain gRPC, through stubs generated from
[`proto/kvasm/v1/kvasm.proto`](../proto/kvasm/v1/kvasm.proto), with no KvAsm
library. Each one makes every call once and checks every answer:

1. Put, Get and Delete of a short key, then a Get that must miss.
2. An Upload of 1 MiB in 64 KiB chunks under a key over 64 bytes, with the
   value's CRC-32C.
3. A Download of bytes 1000..1999 of it in 256-byte chunks, checked against
   the CRC-32C the server sends, then a Delete.

CI runs both against the server image on every push and pull request
([ci.yml](../.github/workflows/ci.yml)).

| Folder | Language | gRPC library | Stubs |
|---|---|---|---|
| [`python/`](python/kvasm_example.py) | Python 3.10 or later | grpcio 1.85.4 | Generated on the first run into `python/gen/` (ignored by git) by grpcio-tools, which brings its own protoc |
| [`go/`](go/main.go) | Go 1.26 or later | grpc-go 1.85.0 | Committed in [`go/kvasmpb/`](go/kvasmpb). `go generate` remakes them ([generate.go](go/generate.go)), and CI fails when they no longer match kvasm.proto |

## Run them

Start a server:

```sh
docker run --rm -p 50051:50051 ghcr.io/asmfoundation/kvasm:0.3.0
```

Then, from this folder:

```sh
cd python && pip install -r requirements.txt && python kvasm_example.py   # or: python kvasm_example.py host:port
cd go && go run .                                                         # or: go run . host:port
```

Both print the same answers. Python writes `True` and `b'Ada'` where Go
writes `true` and `"Ada"`:

```text
Put created: True
Get found: True
Get value: b'Ada'
Delete deleted: True
Get after Delete found: False
Long key sent as: 65
Upload crc32c: 3494318171
Download crc32c: 2193816589
Download bytes: 1000 bytes
Download total_size: 1048576
Delete big deleted: True
```

The exit code says how it went:

- **0:** every answer matched the contract.
- **1:** one did not. Each difference is printed as `FAILED`, and a refused
  call as `FAILED` with its status, for example
  `FAILED DATA_LOSS: crc32c mismatch: nothing was stored` (Python) or
  `FAILED DataLoss: …` (Go).
- **2:** no server answered within 10 seconds.

## What they show of the contract

[docs/PROTOCOL.md](../docs/PROTOCOL.md) has rules that every client must
follow, and the examples follow them:

- **Long keys.** A key over 64 bytes is sent as the 65 bytes `0xFF`
  followed by its SHA-512 (`wire_key` in Python, `wireKey` in Go). This is
  the .NET client's `KeyCompaction`, so a long key one client writes is
  found by the others. A client that sends long keys raw stores them under
  different keys.
- **Checksums are CRC-32C (Castagnoli),** not the CRC-32 of zlib and
  `hash/crc32`'s IEEE table.
  - The server checks an Upload's `crc32c` before it stores anything. A
    mismatch is `DATA_LOSS`, and nothing is stored.
  - A Download is a value only if its last message carries a `crc32c` that
    matches the bytes received.
- **A refused Upload is answered by its status.** The server may answer
  while the client is still sending. grpcio raises the status, and in Go a
  `Send` that returns `io.EOF` is followed by `CloseAndRecv`, which returns
  it.

## What they leave out

The [.NET client](../client/dotnet/README.md) also does these things, and
the two client languages planned in the [README](../README.md#status) will
too:

- It retries read-only calls on `UNAVAILABLE`. A Download is retried only
  until its first message arrives.
- It checks key and value limits before sending anything.
- It writes snapshot files.
- It spreads calls over several connections, and sizes the receive window
  for large transfers.

PROTOCOL.md describes each of these.
