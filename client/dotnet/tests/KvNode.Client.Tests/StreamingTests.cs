using System.Security.Cryptography;
using System.Text;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Net.Client;
using KvNode.Client.V1;
using Xunit;
using GrpcKvNode = global::KvNode.Client.V1.KvNode;

namespace KvNode.Client.Tests;

/// <summary>
/// End-to-end tests of <see cref="KvNodeClient"/>'s Upload/Download against the
/// real server container: sizes, chunkings, streams, ranges, statuses, one
/// version per download, concurrency — and, through the raw generated client,
/// the server's refusals of request streams the public API never produces.
/// </summary>
/// <param name="server">The shared server (defaults: 1 MiB values, 64 MiB uploads).</param>
public sealed class StreamingTests(KvNodeServer server)
{
    /// <summary>The server's default KVNODE_MAX_VALUE_BYTES.</summary>
    private const int ServerMaxValue = 1024 * 1024;

    /// <summary>Cancellation for the current test (xUnit stops it on timeout/abort).</summary>
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    /// <summary>
    /// Values around every boundary (empty, one byte, one chunk ± 1, the unary
    /// limit ± 1, several MiB) round-trip exactly through Upload and both
    /// Download forms, with exact created/replaced flags and checksums; Get
    /// serves those within its limit and refuses the rest with a hint.
    /// </summary>
    /// <param name="size">Value size.</param>
    /// <returns>A task that completes when the test finishes.</returns>
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(65535)]
    [InlineData(65536)]
    [InlineData(65537)]
    [InlineData(ServerMaxValue)]
    [InlineData(ServerMaxValue + 1)]
    [InlineData((5 * 1024 * 1024) + 3)]
    public async Task ValuesRoundTripExactly(int size)
    {
        using KvNodeClient client = NewClient();
        byte[] key = Key("rt");
        byte[] value = RandomBytes(size, size);
        UploadResult up = await client.UploadAsync(key, value, Ct);
        Assert.Equal(new UploadResult(true, size, Crc32C.Compute(value)), up);

        byte[]? read = await client.DownloadAsync(key, cancellationToken: Ct);
        Assert.NotNull(read);
        Assert.True(read.AsSpan().SequenceEqual(value));

        using var sink = new MemoryStream();
        DownloadResult? result = await client.DownloadToAsync(key, sink, cancellationToken: Ct);
        Assert.Equal(new DownloadResult(size, 0, size, Crc32C.Compute(value)), result);
        Assert.True(sink.ToArray().AsSpan().SequenceEqual(value));

        if (size <= ServerMaxValue)
        {
            Assert.True((await client.GetAsync(key, Ct))?.Span.SequenceEqual(value));
        }
        else
        {
            var error = await Assert.ThrowsAsync<RpcException>(() => client.GetAsync(key, Ct));
            Assert.Equal(StatusCode.ResourceExhausted, error.StatusCode);
            Assert.Contains("read it with Download", error.Status.Detail, StringComparison.Ordinal);
        }

        byte[] replacement = RandomBytes(size, size + 1);
        Assert.False((await client.UploadAsync(key, replacement, Ct)).Created);
        Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(replacement));
        Assert.True(await client.DeleteAsync(key, Ct));
        Assert.Null(await client.DownloadAsync(key, cancellationToken: Ct));
    }

    /// <summary>Chunk sizes on both sides change the messages, never the bytes.</summary>
    /// <param name="uploadChunk">Upload chunk size.</param>
    /// <param name="downloadChunk">Download chunk size (0 = server default).</param>
    /// <param name="size">Value size.</param>
    /// <returns>A task that completes when the test finishes.</returns>
    [Theory]
    [InlineData(1, 1, 2000)]
    [InlineData(7, 1000, 100_003)]
    [InlineData(65536, 0, 3_000_001)]
    [InlineData(4 * 1024 * 1024, 1024 * 1024, 9_000_000)]
    [InlineData(333_333, 77_777, 4_000_000)]
    public async Task ChunkSizesDoNotChangeTheBytes(int uploadChunk, int downloadChunk, int size)
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { UploadChunkBytes = uploadChunk, DownloadChunkBytes = downloadChunk });
        byte[] key = Key("chunks");
        byte[] value = RandomBytes(size, uploadChunk ^ downloadChunk);
        Assert.Equal(Crc32C.Compute(value), (await client.UploadAsync(key, value, Ct)).Crc32C);
        Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
        Assert.True(await client.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// Stream sources: a seekable stream uploads from its position to its end;
    /// a non-seekable one needs its length; a source shorter than announced
    /// throws EndOfStreamException and stores nothing — the old value stands.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task StreamSourcesUploadExactlyTheAnnouncedBytes()
    {
        using KvNodeClient client = NewClient();
        byte[] key = Key("streams");
        byte[] value = RandomBytes(1_500_000, 5);
        using (var seekable = new MemoryStream(value))
        {
            seekable.Position = 500_000;
            UploadResult up = await client.UploadAsync(key, seekable, Ct);
            Assert.Equal(1_000_000, up.Length);
            Assert.Equal(value.Length, seekable.Position);
            Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value.AsSpan(500_000)));
        }

        using (var forwardOnly = new ForwardOnlyStream(value))
        {
            await Assert.ThrowsAsync<ArgumentException>(() => client.UploadAsync(key, forwardOnly, Ct));
            UploadResult up = await client.UploadAsync(key, forwardOnly, value.Length, Ct);
            Assert.False(up.Created);
            Assert.Equal(Crc32C.Compute(value), up.Crc32C);
        }

        using (var tooShort = new ForwardOnlyStream(value.AsSpan(0, 1000).ToArray()))
        {
            await Assert.ThrowsAsync<EndOfStreamException>(() => client.UploadAsync(key, tooShort, 5000, Ct));
        }

        Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
        Assert.True(await client.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// Ranges: to the end, exact, cut at the end, empty at the end, zero
    /// length — and OUT_OF_RANGE one byte past the end. Every result reports
    /// the WHOLE value's size and the checksum of exactly the range.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task RangesFollowTheContract()
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { DownloadChunkBytes = 4096 });
        byte[] key = Key("ranges");
        const int size = 300_007;
        byte[] value = RandomBytes(size, 9);
        await client.UploadAsync(key, value, Ct);
        (long Offset, long? Length)[] ranges =
        [
            (0, null), (1, null), (size - 1, null), (size, null), (0, 0), (size, 0), (size, 10),
            (0, 1), (size - 1, 1), (7, size - 7), (7, size), (7, long.MaxValue), (100, 65536), (4095, 4097),
        ];
        foreach ((long offset, long? length) in ranges)
        {
            int end = (int)(length is { } l && l < size - offset ? offset + l : size);
            ReadOnlyMemory<byte> want = value.AsMemory((int)offset, end - (int)offset);
            byte[]? got = await client.DownloadAsync(key, offset, length, Ct);
            Assert.True(got.AsSpan().SequenceEqual(want.Span), $"range {offset}+{length}");
            using var sink = new MemoryStream();
            DownloadResult? result = await client.DownloadToAsync(key, sink, offset, length, Ct);
            Assert.Equal(new DownloadResult(size, offset, want.Length, Crc32C.Compute(want.Span)), result);
            Assert.True(sink.ToArray().AsSpan().SequenceEqual(want.Span));
        }

        var error = await Assert.ThrowsAsync<RpcException>(() => client.DownloadAsync(key, size + 1, null, Ct));
        Assert.Equal(StatusCode.OutOfRange, error.StatusCode);
        Assert.Contains("offset is beyond the end of the value", error.Status.Detail, StringComparison.Ordinal);
        Assert.True(await client.DeleteAsync(key, Ct));
    }

    /// <summary>A missing key is null for both forms, and nothing is written to the destination.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task MissingKeysAreNull()
    {
        using KvNodeClient client = NewClient();
        byte[] key = Key("missing");
        Assert.Null(await client.DownloadAsync(key, cancellationToken: Ct));
        using var sink = new MemoryStream();
        Assert.Null(await client.DownloadToAsync(key, sink, cancellationToken: Ct));
        Assert.Equal(0, sink.Length);
    }

    /// <summary>An empty value is found-and-empty through every call, never confused with missing.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task EmptyValueIsNotMissing()
    {
        using KvNodeClient client = NewClient();
        byte[] key = Key("empty");
        Assert.Equal(new UploadResult(true, 0, 0), await client.UploadAsync(key, ReadOnlyMemory<byte>.Empty, Ct));
        byte[]? read = await client.DownloadAsync(key, cancellationToken: Ct);
        Assert.NotNull(read);
        Assert.Empty(read);
        Assert.True((await client.GetAsync(key, Ct))?.IsEmpty);
        Assert.Equal(new DownloadResult(0, 0, 0, 0), await client.DownloadToAsync(key, Stream.Null, cancellationToken: Ct));
    }

    /// <summary>
    /// Local limits throw before any traffic; a client configured above the
    /// server hears the server's INVALID_ARGUMENT — answered as soon as the
    /// first message declares the size, not after 64 MiB were sent.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task LimitsAreCheckedLocallyThenByTheServer()
    {
        using KvNodeClient small = NewClient(new KvNodeClientOptions { MaxUploadBytes = 100 });
        byte[] key = Key("limits");
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => small.UploadAsync(key, new byte[101], Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => small.UploadAsync(key, Stream.Null, -1, Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => small.DownloadAsync(key, -1, null, Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => small.DownloadAsync(key, 0, -1, Ct));
        await Assert.ThrowsAsync<ArgumentException>(() => small.UploadAsync(ReadOnlyMemory<byte>.Empty, new byte[1], Ct));

        // In-memory downloads are bounded by MaxUploadBytes; streaming ones are not.
        using KvNodeClient client = NewClient(new KvNodeClientOptions { MaxUploadBytes = 128 * 1024 * 1024 });
        await client.UploadAsync(key, new byte[1000], Ct);
        var tooBig = await Assert.ThrowsAsync<RpcException>(() => small.DownloadAsync(key, cancellationToken: Ct));
        Assert.Equal(StatusCode.ResourceExhausted, tooBig.StatusCode);
        Assert.Equal(1000, (await small.DownloadToAsync(key, Stream.Null, cancellationToken: Ct))?.Length);

        byte[] huge = new byte[(64 * 1024 * 1024) + 1];
        DateTime start = DateTime.UtcNow;
        var refused = await Assert.ThrowsAsync<RpcException>(() => client.UploadAsync(key, huge, Ct));
        Assert.Equal(StatusCode.InvalidArgument, refused.StatusCode);
        Assert.Contains("value too large", refused.Status.Detail, StringComparison.Ordinal);
        Assert.True(DateTime.UtcNow - start < TimeSpan.FromSeconds(10), "the refusal should come before the body");
        Assert.Equal(1000, (await client.DownloadAsync(key, cancellationToken: Ct))?.Length);   // untouched
        Assert.True(await client.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// Request streams the public API never sends, through the generated
    /// client — another HTTP/2 and gRPC stack than the server's own tests use —
    /// each refused with its documented status, storing nothing.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ServerRefusesBrokenRequestStreams()
    {
        using GrpcChannel channel = GrpcChannel.ForAddress(server.RequireAddress());
        var raw = new GrpcKvNode.KvNodeClient(channel);
        using KvNodeClient client = NewClient();
        byte[] keyBytes = Key("raw");
        ByteString key = ByteString.CopyFrom(keyBytes);
        await client.PutAsync(keyBytes, "old"u8.ToArray(), Ct);

        async Task<RpcException> Refused(params UploadRequest[] messages)
        {
            using AsyncClientStreamingCall<UploadRequest, UploadResponse> call = raw.Upload(cancellationToken: Ct);
            try
            {
                foreach (UploadRequest m in messages)
                {
                    await call.RequestStream.WriteAsync(m, Ct);
                }

                await call.RequestStream.CompleteAsync();
            }
            catch (Exception e) when (e is RpcException or InvalidOperationException)
            {
                // answered early: the status is in the response
            }

            return await Assert.ThrowsAsync<RpcException>(() => call.ResponseAsync);
        }

        RpcException e1 = await Refused(new UploadRequest { Key = key });
        Assert.Equal((StatusCode.InvalidArgument, true), (e1.StatusCode, e1.Status.Detail.Contains("must carry key and total_size", StringComparison.Ordinal)));
        RpcException e2 = await Refused(new UploadRequest { Key = key, TotalSize = 3, Data = ByteString.CopyFromUtf8("abc"), Crc32C = 12345 });
        Assert.Equal((StatusCode.DataLoss, true), (e2.StatusCode, e2.Status.Detail.Contains("crc32c mismatch", StringComparison.Ordinal)));
        RpcException e3 = await Refused(new UploadRequest { Key = key, TotalSize = 2 }, new UploadRequest { Data = ByteString.CopyFromUtf8("abc") });
        Assert.Equal((StatusCode.InvalidArgument, true), (e3.StatusCode, e3.Status.Detail.Contains("more data than total_size", StringComparison.Ordinal)));
        RpcException e4 = await Refused(new UploadRequest { Key = key, TotalSize = 5 }, new UploadRequest { Data = ByteString.CopyFromUtf8("ab") });
        Assert.Equal((StatusCode.InvalidArgument, true), (e4.StatusCode, e4.Status.Detail.Contains("ended before total_size", StringComparison.Ordinal)));
        RpcException e5 = await Refused(new UploadRequest { Key = key, TotalSize = 1 }, new UploadRequest { Key = key, Data = ByteString.CopyFromUtf8("a") });
        Assert.Equal((StatusCode.InvalidArgument, true), (e5.StatusCode, e5.Status.Detail.Contains("only allowed in the first message", StringComparison.Ordinal)));
        RpcException e6 = await Refused(new UploadRequest { Key = key, TotalSize = 1, Crc32C = 1 }, new UploadRequest { Data = ByteString.CopyFromUtf8("a"), Crc32C = 1 });
        Assert.Equal((StatusCode.InvalidArgument, true), (e6.StatusCode, e6.Status.Detail.Contains("crc32c sent more than once", StringComparison.Ordinal)));
        Assert.Equal("old", await client.GetStringAsync(Encoding.UTF8.GetString(keyBytes), Ct));

        // A well-formed raw stream without a checksum is stored, and the reply
        // still carries the server's own CRC for end-to-end verification.
        using (AsyncClientStreamingCall<UploadRequest, UploadResponse> ok = raw.Upload(cancellationToken: Ct))
        {
            await ok.RequestStream.WriteAsync(new UploadRequest { Key = key, TotalSize = 3, Data = ByteString.CopyFromUtf8("ne") }, Ct);
            await ok.RequestStream.WriteAsync(new UploadRequest(), Ct);                                  // an empty message
            await ok.RequestStream.WriteAsync(new UploadRequest { Data = ByteString.CopyFromUtf8("w") }, Ct);
            await ok.RequestStream.CompleteAsync();
            UploadResponse reply = await ok.ResponseAsync;
            Assert.False(reply.Created);
            Assert.Equal(Crc32C.Compute("new"u8), reply.Crc32C);
        }

        Assert.Equal("new", await client.GetStringAsync(Encoding.UTF8.GetString(keyBytes), Ct));
        Assert.True(await client.DeleteAsync(keyBytes, Ct));
    }

    /// <summary>
    /// Cancelling an upload mid-way (its source slowed down) stores nothing:
    /// the old value stands, and the client is fine afterwards.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task CancelledUploadStoresNothing()
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { UploadChunkBytes = 16 * 1024 });
        byte[] key = Key("cancel");
        await client.PutAsync(key, "old"u8.ToArray(), Ct);
        byte[] value = RandomBytes(2_000_000, 11);
        using var cancel = CancellationTokenSource.CreateLinkedTokenSource(Ct);
        cancel.CancelAfter(TimeSpan.FromMilliseconds(300));
        using var slow = new SlowStream(new MemoryStream(value), TimeSpan.FromMilliseconds(20));
        // CANCELLED whether the cancellation lands in gRPC or in the slow
        // source's own read — one outcome, as for unary calls.
        RpcException error = await Assert.ThrowsAsync<RpcException>(() => client.UploadAsync(key, slow, value.Length, cancel.Token));
        Assert.Equal(StatusCode.Cancelled, error.StatusCode);
        Assert.True(slow.Position > 0 && slow.Position < value.Length, "the upload was cut in the middle");
        Assert.Equal("old"u8.ToArray(), (await client.GetAsync(key, Ct))?.ToArray());
        Assert.True((await client.UploadAsync(key, value, Ct)) is { Created: false });
        Assert.True(await client.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// One download reads one version: a download written into a slow
    /// destination keeps returning the value it started on while another
    /// client replaces the key by Upload and by Put and then deletes it.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task DownloadReadsOneVersion()
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { DownloadChunkBytes = 64 * 1024 });
        using KvNodeClient writer = NewClient();
        byte[] key = Key("version");
        byte[] v1 = RandomBytes(4 * 1024 * 1024, 21);
        await client.UploadAsync(key, v1, Ct);
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var slow = new SlowStream(new MemoryStream(), TimeSpan.FromMilliseconds(3), onFirstWrite: () => started.TrySetResult());
        Task<DownloadResult?> download = client.DownloadToAsync(key, slow, cancellationToken: Ct);
        await started.Task.WaitAsync(Ct);
        await writer.UploadAsync(key, RandomBytes(3 * 1024 * 1024, 22), Ct);
        await writer.PutAsync(key, "v3"u8.ToArray(), Ct);
        Assert.True(await writer.DeleteAsync(key, Ct));
        DownloadResult? result = await download;
        Assert.Equal(new DownloadResult(v1.Length, 0, v1.Length, Crc32C.Compute(v1)), result);
        Assert.True(((MemoryStream)slow.Inner).ToArray().AsSpan().SequenceEqual(v1));
        Assert.Null(await client.DownloadAsync(key, cancellationToken: Ct));
    }

    /// <summary>Sixteen concurrent upload/range-download cycles share one client and stay exact.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ConcurrentTransfersOverOneClient()
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { UploadChunkBytes = 100_000 });
        string prefix = Unique("conc");
        await Task.WhenAll(Enumerable.Range(0, 16).Select(async task =>
        {
            var random = new Random(task);
            for (int i = 0; i < 6; i++)
            {
                byte[] key = Encoding.UTF8.GetBytes($"{prefix}-{task}-{i}");
                byte[] value = RandomBytes(random.Next(0, 2_000_000), (task * 31) + i);
                Assert.True((await client.UploadAsync(key, value, Ct)).Created);
                long offset = random.Next(0, value.Length + 1);
                long length = random.Next(0, value.Length + 2);
                int end = (int)Math.Min(value.Length, offset + length);
                byte[]? got = await client.DownloadAsync(key, offset, length, Ct);
                Assert.True(got.AsSpan().SequenceEqual(value.AsSpan((int)offset, end - (int)offset)));
                Assert.True(await client.DeleteAsync(key, Ct));
            }
        }));
    }

    /// <summary>Creates a client for the shared server (skips the test when none is running).</summary>
    /// <param name="options">Client options, or null for defaults.</param>
    /// <returns>A new client; the caller disposes it.</returns>
    private KvNodeClient NewClient(KvNodeClientOptions? options = null) => new(server.RequireAddress(), options);

    /// <summary>A key unique to this test run.</summary>
    /// <param name="name">A readable prefix.</param>
    /// <returns>The key bytes.</returns>
    private static byte[] Key(string name) => Encoding.UTF8.GetBytes(Unique(name));

    /// <summary>A unique text key.</summary>
    /// <param name="name">A readable prefix.</param>
    /// <returns>The key text.</returns>
    private static string Unique(string name) => $"{name}-{Guid.NewGuid():N}";

    /// <summary>Deterministic pseudo-random bytes (a failing size reproduces).</summary>
    /// <param name="size">Byte count.</param>
    /// <param name="seed">Seed.</param>
    /// <returns>The bytes.</returns>
    internal static byte[] RandomBytes(int size, int seed)
    {
        byte[] bytes = new byte[size];
        new Random(seed).NextBytes(bytes);
        return bytes;
    }
}

/// <summary>A read-only stream that cannot seek (like a network or pipe source).</summary>
/// <param name="data">The bytes it yields.</param>
internal sealed class ForwardOnlyStream(byte[] data) : Stream
{
    private int _position;

    /// <inheritdoc/>
    public override bool CanRead => true;

    /// <inheritdoc/>
    public override bool CanSeek => false;

    /// <inheritdoc/>
    public override bool CanWrite => false;

    /// <inheritdoc/>
    public override long Length => throw new NotSupportedException();

    /// <inheritdoc/>
    public override long Position
    {
        get => throw new NotSupportedException();
        set => throw new NotSupportedException();
    }

    /// <inheritdoc/>
    public override int Read(byte[] buffer, int offset, int count)
    {
        // Short reads on purpose (at most 1000 bytes): callers must loop.
        int n = Math.Min(Math.Min(count, 1000), data.Length - _position);
        data.AsSpan(_position, n).CopyTo(buffer.AsSpan(offset, n));
        _position += n;
        return n;
    }

    /// <inheritdoc/>
    public override void Flush()
    {
    }

    /// <inheritdoc/>
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

    /// <inheritdoc/>
    public override void SetLength(long value) => throw new NotSupportedException();

    /// <inheritdoc/>
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}

/// <summary>Wraps a stream and delays every read and write (a slow disk or consumer).</summary>
/// <param name="inner">The wrapped stream (disposed with this one).</param>
/// <param name="delay">Delay per operation.</param>
/// <param name="onFirstWrite">Called once, before the first write.</param>
internal sealed class SlowStream(Stream inner, TimeSpan delay, Action? onFirstWrite = null) : Stream
{
    private int _writes;

    /// <summary>The wrapped stream.</summary>
    public Stream Inner => inner;

    /// <inheritdoc/>
    public override bool CanRead => inner.CanRead;

    /// <inheritdoc/>
    public override bool CanSeek => false;

    /// <inheritdoc/>
    public override bool CanWrite => inner.CanWrite;

    /// <inheritdoc/>
    public override long Length => inner.Length;

    /// <inheritdoc/>
    public override long Position
    {
        get => inner.Position;
        set => throw new NotSupportedException();
    }

    /// <inheritdoc/>
    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        await Task.Delay(delay, cancellationToken);
        return await inner.ReadAsync(buffer, cancellationToken);
    }

    /// <inheritdoc/>
    public override async ValueTask WriteAsync(ReadOnlyMemory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (Interlocked.Increment(ref _writes) == 1)
        {
            onFirstWrite?.Invoke();
        }

        await Task.Delay(delay, cancellationToken);
        await inner.WriteAsync(buffer, cancellationToken);
    }

    /// <inheritdoc/>
    public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException("async only");

    /// <inheritdoc/>
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException("async only");

    /// <inheritdoc/>
    public override void Flush() => inner.Flush();

    /// <inheritdoc/>
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

    /// <inheritdoc/>
    public override void SetLength(long value) => throw new NotSupportedException();

    /// <inheritdoc/>
    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            inner.Dispose();
        }

        base.Dispose(disposing);
    }
}
