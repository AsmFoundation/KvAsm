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

### Options

```csharp
var options = new KvNodeClientOptions
{
    MaxKeyBytes = 1024,             // mirror KVNODE_MAX_KEY_BYTES
    MaxValueBytes = 4 * 1024 * 1024,// mirror KVNODE_MAX_VALUE_BYTES
    Timeout = TimeSpan.FromSeconds(2),
    ReadAttempts = 3,               // Get/health retried on UNAVAILABLE; Put/Delete never
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
- Server and transport errors are `Grpc.Core.RpcException`. The status codes
  are listed in [docs/PROTOCOL.md](../../docs/PROTOCOL.md#status-codes); for
  example, `RESOURCE_EXHAUSTED` means the store is full.

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

- Unit tests: all 64 SipHash reference vectors, key compaction, and the
  snapshot writer. Its output must match the server tool's file byte for
  byte.
- Integration tests: Testcontainers starts the real image. They run
  without Docker too, but are then skipped. Set `KVNODE_REQUIRE_DOCKER=1`
  to fail instead (use it in CI), and `KVNODE_IMAGE` to test another tag.
