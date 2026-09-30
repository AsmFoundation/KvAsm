# KvNode.Client for .NET

The .NET client for [KvNode](../../README.md). Targets .NET 8 (LTS) and
.NET 10.

## Usage

```csharp
using KvNode.Client;

// One long-lived client per server: it multiplexes all calls over HTTP/2.
using var kv = new KvNodeClient(new Uri("http://kvnode:50051"));

await kv.PutAsync("greeting", "hello");                    // true = created
string? text = await kv.GetStringAsync("greeting");         // null when missing

byte[] key = [0x00, 0xFF, 0x10];
await kv.PutAsync(key, File.ReadAllBytes("blob.bin"));
ReadOnlyMemory<byte>? blob = await kv.GetAsync(key);        // empty ≠ missing
bool removed = await kv.DeleteAsync(key);

bool serving = await kv.IsServingAsync();                   // grpc.health.v1
```

### Large values: Upload and Download

Values above `KVNODE_MAX_VALUE_BYTES` (1 MiB by default), up to
`KVNODE_MAX_UPLOAD_BYTES` (64 MiB by default), move as streams of chunks,
each end checking the CRC-32C of the bytes:

```csharp
// From memory (sent without copying) or from a stream (one chunk in memory at a time).
UploadResult up = await kv.UploadAsync(key, bytes);                 // Created, Length, Crc32C
await using (FileStream file = File.OpenRead("video.mp4"))
{
    await kv.UploadAsync(key, file);                                // seekable: length from the stream
}

byte[]? all = await kv.DownloadAsync(key);                          // null when missing
byte[]? part = await kv.DownloadAsync(key, offset: 1_000, length: 500);
await using (FileStream copy = File.Create("copy.mp4"))
{
    DownloadResult? r = await kv.DownloadToAsync(key, copy);        // TotalSize, Offset, Length, Crc32C
}

uint mine = Crc32C.Compute(bytes);                                  // == up.Crc32C
```

- **Atomic:** readers see the old value until the upload completes, then
  the new one. A cancelled, refused or broken upload stores nothing.
- **One version per download:** a value replaced or deleted during a
  download is still read whole as it was when the download started. Two
  separate downloads (for example two ranges) may see different versions.
- **Never silently partial:** a download that breaks, ends early or fails
  its checksum throws; only a returned result vouches for the bytes (after
  a failure, a `DownloadToAsync` destination may hold a prefix).
- **Retries:** Download is retried on `UNAVAILABLE` until its first chunk
  arrives; Upload, like Put, never. A transfer broken mid-way is not
  resumed automatically — the value may have changed; resume with `offset`
  only when you know it did not.
- **Stalls:** a transfer that makes no progress for `TransferIdleTimeout`
  (30 s) fails with `DEADLINE_EXCEEDED`, however long it has run; the
  whole transfer is bounded by `TransferTimeout` (10 minutes).
- `GetAsync` of a value above the server's `KVNODE_MAX_VALUE_BYTES` fails
  with `RESOURCE_EXHAUSTED` ("read it with Download").
- **No garbage per chunk:** Upload refills one message object for the whole
  call and serializes it straight into gRPC's write buffer; Download parses
  every message into a pooled frame whose bytes sit in an `ArrayPool`
  buffer until they are checksummed and written. A 1 MiB download used to
  allocate ~1.07 MB (a message and a copy of every 64 KiB chunk); what is
  left per call is gRPC's own per-call state (benchmarks/results/
  2026-09-30-streaming).
- **Fast checksums:** `Crc32C` folds with carry-less multiplies — 256-bit
  (VPCLMULQDQ + AVX2, .NET 9+) or 128-bit (PCLMULQDQ) — for buffers of 256
  bytes and more, the server's own algorithm, and falls back to the CRC32
  instruction (x64 SSE4.2, Arm64 CRC) and then to the runtime's software
  path. All give identical results; measured 9% less client CPU per GB.

### Options

```csharp
var options = new KvNodeClientOptions
{
    MaxKeyBytes = 1024,             // mirror KVNODE_MAX_KEY_BYTES
    MaxValueBytes = 4 * 1024 * 1024,// mirror KVNODE_MAX_VALUE_BYTES
    MaxUploadBytes = 64 << 20,      // mirror KVNODE_MAX_UPLOAD_BYTES (also caps DownloadAsync in memory)
    Timeout = TimeSpan.FromSeconds(2),                  // unary calls
    TransferTimeout = TimeSpan.FromMinutes(10),         // a whole upload/download
    TransferIdleTimeout = TimeSpan.FromSeconds(30),     // no chunk moved for this long: fail
    UploadChunkBytes = 1 << 20,     // 1 byte..4 MiB per upload message (fewer messages: faster)
    DownloadChunkBytes = 0,         // 0 = server default (64 KiB); up to 1 MiB
    ReadAttempts = 3,               // Get/Download/health retried on UNAVAILABLE; Put/Delete/Upload never
    CompactLongKeys = true,         // the contract; keep it on in every client
};
using var kv = new KvNodeClient(new Uri("http://kvnode:50051"), options);
```

To bring your own channel (TLS, interceptors), start from
`KvNodeClient.CreateChannelOptions(options)` and pass the channel to the
`KvNodeClient(ChannelBase, options)` constructor. The client does not
dispose a channel it did not create.

### Errors

- Invalid input that can be checked locally throws before any network I/O:
  an empty key (`ArgumentException`), or a key or value over the configured
  limits (`ArgumentOutOfRangeException`).
- Server and transport errors are `Grpc.Core.RpcException` — every one,
  including a connection that breaks in the middle of an upload. The status
  codes are listed in [docs/PROTOCOL.md](../../docs/PROTOCOL.md#status-codes);
  for example, `RESOURCE_EXHAUSTED` means the store is full, `DATA_LOSS`
  that transferred bytes failed their checksum. Cancellation is `CANCELLED`
  for every call.
- Exceptions of your own streams (an upload source, a download
  destination) propagate unchanged; a source shorter than the announced
  length throws `EndOfStreamException` and nothing is stored.

### Long keys

Keys longer than 64 bytes are sent as `0xFF ‖ SHA-512(key)`
(`KeyCompaction`), which is the cross-language contract. Short keys are sent
as-is.

## Snapshots

Build a data file for `KVNODE_SNAPSHOT`:

```csharp
// Unknown count: streams to a temporary file, then atomically replaces the target.
long written = await SnapshotFile.WriteAsync("seed.kvs", ReadRecordsAsync());

// Known count: single pass to any stream.
await using var writer = new SnapshotWriter(stream, recordCount: 3);
await writer.WriteAsync("a"u8.ToArray(), "1"u8.ToArray());
...
await writer.CompleteAsync();   // without this, the file stays invalid by design
```

The writer enforces the server's limits at export time
(`SnapshotWriterOptions`), applies the long-key contract, and computes the
SipHash-2-4-128 checksum the server verifies.

## Tests

```sh
docker build --target runtime -t kvnode:0.1.0 ../..   # the image under test
dotnet test --solution KvNode.Client.slnx
```

- Unit tests: all 64 SipHash reference vectors, key compaction, the
  snapshot writer (its output must match the server tool's file byte for
  byte), CRC-32C (published vectors, every split, a bitwise reference,
  each implementation forced in turn at every length to 1,200), the
  streaming codecs (byte-identical to the generated messages, payloads cut
  into random buffer segments, unknown fields, malformed input, pool reuse)
  and the transfer options.
- Benchmark: `bench/KvNode.Client.Bench` measures Upload/Download
  throughput, client CPU, bytes allocated per transfer and GC counts
  against a running server (see its header; the A/B drivers are in
  benchmarks/results/2026-09-30-streaming).
- Integration tests: Testcontainers starts the real image. They run
  without Docker too, but are then skipped. Set `KVNODE_REQUIRE_DOCKER=1`
  to fail instead (use it in CI), and `KVNODE_IMAGE` to test another tag.
  Upload/Download are tested across sizes and chunkings, streams, ranges,
  refusals (also through the raw generated client), cancellation, one
  version per download, concurrency; through `FaultProxy`, a TCP proxy
  that resets, hangs, throttles or refuses connections at exact byte
  counts; and against a dedicated server with small memory and budget
  limits.
