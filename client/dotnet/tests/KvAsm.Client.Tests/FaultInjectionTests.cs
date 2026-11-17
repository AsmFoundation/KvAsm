using System.Diagnostics;
using System.Text;
using Grpc.Core;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>
/// Network interruptions between the client and the real server, injected by
/// a <see cref="FaultProxy"/> at exact byte counts: connections reset in the
/// middle of an upload or a download, paths that hang without breaking,
/// slow links, refused connections, deadlines. Each test checks the client's
/// outcome (a precise error, never a partial success) AND the server's state
/// through a direct client (nothing half-stored, the old value standing).
/// </summary>
/// <param name="server">The shared server.</param>
public sealed class FaultInjectionTests(KvAsmServer server)
{
    /// <summary>Cancellation for the current test (xUnit stops it on timeout/abort).</summary>
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    /// <summary>
    /// A connection reset after ~300 KB of a 2 MB upload fails the upload and
    /// stores nothing: the value uploaded before it stands, and the next upload
    /// over a healthy connection succeeds.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task UploadCutByAResetStoresNothing()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("cut-up");
        await direct.PutAsync(key, "old"u8.ToArray(), Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { AbortUpAfter = 300_000 } };
        using (KvAsmClient client = new(proxy.Address))
        {
            byte[] value = StreamingTests.RandomBytes(2_000_000, 1);
            RpcException error = await Assert.ThrowsAsync<RpcException>(() => client.UploadAsync(key, value, Ct));
            Assert.NotEqual(StatusCode.OK, error.StatusCode);
        }

        Assert.Equal("old"u8.ToArray(), (await direct.GetAsync(key, Ct))?.ToArray());
        proxy.Faults = new Faults();
        using (KvAsmClient client = new(proxy.Address))
        {
            byte[] value = StreamingTests.RandomBytes(2_000_000, 2);
            Assert.False((await client.UploadAsync(key, value, Ct)).Created);
            Assert.True((await direct.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
        }

        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A connection reset after ~200 KB of a 2 MB download is an error — never
    /// a short success — for both forms; the stream form has written only
    /// part of the value, which a returned result would never vouch for.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task DownloadCutByAResetFails()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("cut-down");
        byte[] value = StreamingTests.RandomBytes(2_000_000, 3);
        await direct.UploadAsync(key, value, Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { AbortDownAfter = 200_000 } };
        using (KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { ReadAttempts = 1 }))
        {
            await Assert.ThrowsAsync<RpcException>(() => client.DownloadAsync(key, cancellationToken: Ct));
        }

        using (KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { ReadAttempts = 1 }))
        {
            using var sink = new MemoryStream();
            await Assert.ThrowsAsync<RpcException>(() => client.DownloadToAsync(key, sink, cancellationToken: Ct));
            Assert.InRange(sink.Length, 0, 200_000);
            Assert.True(sink.ToArray().AsSpan().SequenceEqual(value.AsSpan(0, (int)sink.Length)));
        }

        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A download whose path hangs (the proxy stops forwarding and reading:
    /// TCP back-pressure, no error) fails with DEADLINE_EXCEEDED once no chunk
    /// arrived for TransferIdleTimeout — instead of hanging until the
    /// 10-minute transfer deadline.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task HungDownloadHitsTheIdleTimeout()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("hang-down");
        await direct.UploadAsync(key, StreamingTests.RandomBytes(8_000_000, 4), Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { StallDownAfter = 150_000 } };
        using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { TransferIdleTimeout = TimeSpan.FromSeconds(1), ReadAttempts = 1 });
        var clock = Stopwatch.StartNew();
        RpcException error = await Assert.ThrowsAsync<RpcException>(() => client.DownloadAsync(key, cancellationToken: Ct));
        Assert.Equal(StatusCode.DeadlineExceeded, error.StatusCode);
        Assert.Contains("TransferIdleTimeout", error.Status.Detail, StringComparison.Ordinal);
        Assert.InRange(clock.Elapsed, TimeSpan.FromSeconds(0.9), TimeSpan.FromSeconds(15));
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// An upload whose path hangs fails with DEADLINE_EXCEEDED after
    /// TransferIdleTimeout, and nothing is stored (the server discards the
    /// partial record when the connection goes).
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task HungUploadHitsTheIdleTimeout()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("hang-up");
        await direct.PutAsync(key, "old"u8.ToArray(), Ct);
        await using (var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { StallUpAfter = 300_000 } })
        {
            using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { TransferIdleTimeout = TimeSpan.FromSeconds(1) });
            var clock = Stopwatch.StartNew();
            RpcException error = await Assert.ThrowsAsync<RpcException>(
                () => client.UploadAsync(key, StreamingTests.RandomBytes(8_000_000, 5), Ct));
            Assert.Equal(StatusCode.DeadlineExceeded, error.StatusCode);
            Assert.InRange(clock.Elapsed, TimeSpan.FromSeconds(0.9), TimeSpan.FromSeconds(15));
        }

        Assert.Equal("old"u8.ToArray(), (await direct.GetAsync(key, Ct))?.ToArray());
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A slow but steady link (~1 MB/s) carries a 3 MB download that lasts
    /// far longer than the 1 s idle timeout: progress, not duration, is what
    /// the idle timeout measures.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task SlowLinkOutlastsTheIdleTimeout()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("slow");
        byte[] value = StreamingTests.RandomBytes(3_000_000, 6);
        await direct.UploadAsync(key, value, Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { ThrottleDownBytesPerSecond = 1_000_000 } };
        using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { TransferIdleTimeout = TimeSpan.FromSeconds(1) });
        var clock = Stopwatch.StartNew();
        Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
        Assert.True(clock.Elapsed > TimeSpan.FromSeconds(2), $"the link was not slow ({clock.Elapsed})");
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>The whole-transfer deadline still bounds a transfer that keeps moving slowly.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task TransferTimeoutBoundsASlowTransfer()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("deadline");
        await direct.UploadAsync(key, StreamingTests.RandomBytes(8_000_000, 7), Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { ThrottleDownBytesPerSecond = 1_000_000 } };
        using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { TransferTimeout = TimeSpan.FromSeconds(1), ReadAttempts = 1 });
        var clock = Stopwatch.StartNew();
        RpcException error = await Assert.ThrowsAsync<RpcException>(() => client.DownloadToAsync(key, Stream.Null, cancellationToken: Ct));
        Assert.Equal(StatusCode.DeadlineExceeded, error.StatusCode);
        Assert.InRange(clock.Elapsed, TimeSpan.FromSeconds(0.9), TimeSpan.FromSeconds(15));
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A connection that breaks before the first reply (reset as soon as the
    /// client starts talking) is retried for a Download — it is read-only and
    /// nothing was received yet — on a new connection, with the exact bytes.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    /// <remarks>
    /// The reset comes after the connection was established, which is the
    /// failure <see cref="KvAsmClientOptions.ReadAttempts"/> covers on every
    /// OS. (Until 2026-11-15 this test reset at accept instead; that only
    /// passed on Windows, where the reset surfaces after the connect — see
    /// <see cref="ConnectionResetAtAcceptNeverHangsOrCorruptsADownload"/>.)
    /// </remarks>
    [Fact]
    public async Task ConnectionResetBeforeTheFirstReplyIsRetriedForDownloads()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("retry");
        byte[] value = StreamingTests.RandomBytes(100_000, 8);
        await direct.UploadAsync(key, value, Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { ResetConnectionsOnFirstByte = 1 } };
        using KvAsmClient client = new(proxy.Address);
        Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
        Assert.True(proxy.Connections >= 2, $"expected a second connection, saw {proxy.Connections}");
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A connection reset at accept, before any byte moved, never hangs a
    /// Download and never yields wrong bytes: the result is the exact value
    /// or a prompt <c>UNAVAILABLE</c>.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    /// <remarks>
    /// <para>
    /// Both outcomes are correct, and which one happens depends on the OS.
    /// Linux usually reports such a reset to the pending connect itself, so
    /// gRPC counts a failed connection attempt and holds the channel in its
    /// reconnect backoff (1 s at first). The retries of
    /// <see cref="KvAsmClientOptions.ReadAttempts"/>, all within ~150 ms,
    /// then fail fast with <c>UNAVAILABLE</c>, as gRPC specifies for calls
    /// that are not wait-for-ready.
    /// </para>
    /// <para>
    /// Windows reports the reset only after the connect completed: the call
    /// fails as on a broken connection and the retry succeeds. Either way the
    /// call ends well before its deadline, which is what this test pins down.
    /// The deterministic retry path is
    /// <see cref="ConnectionResetBeforeTheFirstReplyIsRetriedForDownloads"/>.
    /// </para>
    /// </remarks>
    [Fact]
    public async Task ConnectionResetAtAcceptNeverHangsOrCorruptsADownload()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("refused");
        byte[] value = StreamingTests.RandomBytes(100_000, 9);
        await direct.UploadAsync(key, value, Ct);
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { RejectConnections = 1 } };
        using KvAsmClient client = new(proxy.Address);
        var clock = Stopwatch.StartNew();
        try
        {
            byte[]? got = await client.DownloadAsync(key, cancellationToken: Ct);
            Assert.True(got.AsSpan().SequenceEqual(value), "a retried download must return the exact value");
        }
        catch (RpcException error)
        {
            Assert.Equal(StatusCode.Unavailable, error.StatusCode);
        }

        // Fast either way: a fail-fast UNAVAILABLE or one quick retry, never
        // the 10-minute transfer deadline or the 30 s idle timeout.
        Assert.InRange(clock.Elapsed, TimeSpan.Zero, TimeSpan.FromSeconds(10));
        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// Resets at many points of an upload/download cycle: every attempt either
    /// succeeds exactly or fails, and the value on the server is always one of
    /// the complete versions — never a mix, never a prefix.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ResetsAnywhereNeverTearAValue()
    {
        using KvAsmClient direct = new(server.RequireAddress());
        byte[] key = Key("anywhere");
        byte[][] versions = [.. Enumerable.Range(0, 3).Select(i => StreamingTests.RandomBytes(700_000, 100 + i))];
        await direct.UploadAsync(key, versions[0], Ct);
        await using var proxy = new FaultProxy(server.RequireAddress());
        var random = new Random(9);
        for (int round = 0; round < 12; round++)
        {
            long cut = random.Next(100, 900_000);
            byte[] next = versions[(round + 1) % versions.Length];
            proxy.Faults = round % 2 == 0 ? new Faults { AbortUpAfter = cut } : new Faults { AbortDownAfter = cut };
            using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions { ReadAttempts = 1, UploadChunkBytes = 50_000 });
            try
            {
                await client.UploadAsync(key, next, Ct);
                Assert.True((await client.DownloadAsync(key, cancellationToken: Ct)).AsSpan().SequenceEqual(next));
            }
            catch (RpcException)
            {
                // the fault hit: the server's value must still be whole
            }

            byte[]? stored = await direct.DownloadAsync(key, cancellationToken: Ct);
            Assert.Contains(versions, v => stored.AsSpan().SequenceEqual(v));
        }

        Assert.True(await direct.DeleteAsync(key, Ct));
    }

    /// <summary>
    /// A refusal reaches an upload whose write is WAITING for flow-control
    /// credit. The server→client direction is paced to a trickle, so the
    /// client — sending at full speed with 4 MiB chunks, beyond the server's
    /// 1 MiB stream window — is blocked on credit when the refusal (and
    /// RST_STREAM(NO_ERROR)) finally arrives, and the server never credits a
    /// stream it answered: the answer must still come back promptly.
    /// </summary>
    /// <remarks>
    /// Pins the contract, not the 2026-09-30 hang itself: in this paced setup
    /// .NET releases the parked write on the reset even without the fix
    /// (verified against the pre-fix code). The hang needed a warm process
    /// (LimitsAreCheckedLocallyThenByTheServer after other StreamingTests,
    /// 1 MiB chunks): that sequence reproduces it, and the fix — a small
    /// first message and pending writes raced against the response — cured
    /// it (pre-fix code: hung 5 of 5 runs; fixed: passed 3 of 3).
    /// </remarks>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact(Timeout = 120_000)]   // a regression is a hang: fail instead of waiting forever
    public async Task RefusalReachesAnUploadWaitingForCredit()
    {
        CancellationToken ct = TestContext.Current.CancellationToken;   // referenced directly (xUnit1069)
        await using var proxy = new FaultProxy(server.RequireAddress()) { Faults = new Faults { ThrottleDownBytesPerSecond = 2_000 } };
        using KvAsmClient client = new(proxy.Address, new KvAsmClientOptions
        {
            ReadAttempts = 1,
            MaxUploadBytes = 128 * 1024 * 1024,
            UploadChunkBytes = KvAsmClientOptions.MaxUploadChunkBytes,
        });
        byte[] huge = new byte[(64 * 1024 * 1024) + 1];   // above the server's KVASM_MAX_UPLOAD_BYTES
        var watch = Stopwatch.StartNew();
        RpcException refused = await Assert.ThrowsAsync<RpcException>(() => client.UploadAsync(Key("credit"), huge, ct));
        Assert.Equal(StatusCode.InvalidArgument, refused.StatusCode);
        Assert.Contains("value too large", refused.Status.Detail, StringComparison.Ordinal);
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(20), $"the refusal took {watch.Elapsed}");
    }

    /// <summary>A key unique to this test run.</summary>
    /// <param name="name">A readable prefix.</param>
    /// <returns>The key bytes.</returns>
    private static byte[] Key(string name) => Encoding.UTF8.GetBytes($"{name}-{Guid.NewGuid():N}");
}
