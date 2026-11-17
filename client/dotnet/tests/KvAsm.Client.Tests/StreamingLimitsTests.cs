using System.Diagnostics;
using System.Text;
using DotNet.Testcontainers.Containers;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Net.Client;
using KvAsm.Client.V1;
using Xunit;
using GrpcKvAsm = global::KvAsm.Client.V1.KvAsm;

namespace KvAsm.Client.Tests;

/// <summary>
/// Upload limits against a dedicated server with small ones:
/// <c>KVASM_MAX_MEMORY_MB=16</c>, <c>KVASM_CONN_BUFFER_MB=8</c>,
/// <c>KVASM_MAX_UPLOAD_BYTES</c> = 8 MiB. Each limit must refuse with its
/// documented status and message, and the capacity must come back once the
/// holder is gone.
/// </summary>
/// <param name="shared">The shared server fixture (for the image and the skip decision).</param>
public sealed class StreamingLimitsTests(KvAsmServer shared)
{
    /// <summary>The dedicated server's KVASM_MAX_UPLOAD_BYTES.</summary>
    private const int MaxUpload = 8 * 1024 * 1024;

    /// <summary>Cancellation for the current test (xUnit stops it on timeout/abort).</summary>
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    /// <summary>
    /// The memory limit: one 8 MiB value fits, a second does not
    /// (RESOURCE_EXHAUSTED "store memory limit reached", refused as soon as its
    /// size is declared), and once the first is deleted the space comes back.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task MemoryLimitRefusesThenRecovers()
    {
        await using Dedicated server = await Dedicated.StartAsync(shared);
        using KvAsmClient client = new(server.Address, new KvAsmClientOptions { MaxUploadBytes = MaxUpload });
        byte[] a = Encoding.UTF8.GetBytes("limit-a"), b = Encoding.UTF8.GetBytes("limit-b");
        byte[] value = StreamingTests.RandomBytes(MaxUpload, 1);
        Assert.True((await client.UploadAsync(a, value, Ct)).Created);
        RpcException full = await Assert.ThrowsAsync<RpcException>(() => client.UploadAsync(b, value, Ct));
        Assert.Equal(StatusCode.ResourceExhausted, full.StatusCode);
        Assert.Contains("store memory limit reached", full.Status.Detail, StringComparison.Ordinal);
        Assert.Null(await client.DownloadAsync(b, cancellationToken: Ct));

        // Deleted records are freed after their grace period (at the latest by
        // the once-a-second housekeeping): retry for a few seconds.
        Assert.True(await client.DeleteAsync(a, Ct));
        var clock = Stopwatch.StartNew();
        while (true)
        {
            try
            {
                Assert.True((await client.UploadAsync(b, value, Ct)).Created);
                break;
            }
            catch (RpcException e) when (e.StatusCode == StatusCode.ResourceExhausted && clock.Elapsed < TimeSpan.FromSeconds(5))
            {
                await Task.Delay(100, Ct);
            }
        }

        Assert.True((await client.DownloadAsync(b, cancellationToken: Ct)).AsSpan().SequenceEqual(value));
    }

    /// <summary>
    /// The per-connection budget for uploads in progress: while one upload on
    /// a connection holds a 6 MiB reservation, a 3 MiB upload on the SAME
    /// connection is refused (RESOURCE_EXHAUSTED naming KVASM_CONN_BUFFER_MB),
    /// one on another connection is not; cancelling the holder frees the budget.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ConnectionBudgetCountsUploadsInProgress()
    {
        await using Dedicated server = await Dedicated.StartAsync(shared);
        // One channel = one HTTP/2 connection (well below its stream limit):
        // the raw holder and the client's uploads share it.
        using GrpcChannel channel = GrpcChannel.ForAddress(server.Address);
        var raw = new GrpcKvAsm.KvAsmClient(channel);
        using KvAsmClient sameConnection = new(channel, new KvAsmClientOptions { MaxUploadBytes = MaxUpload });
        using KvAsmClient otherConnection = new(server.Address, new KvAsmClientOptions { MaxUploadBytes = MaxUpload });
        byte[] small = StreamingTests.RandomBytes(3 * 1024 * 1024, 2);

        using var holderCancel = CancellationTokenSource.CreateLinkedTokenSource(Ct);
        AsyncClientStreamingCall<UploadRequest, UploadResponse> holder = raw.Upload(cancellationToken: holderCancel.Token);
        await holder.RequestStream.WriteAsync(
            new UploadRequest { Key = ByteString.CopyFromUtf8("budget-holder"), TotalSize = 6 * 1024 * 1024 }, Ct);

        // The holder's first message reaches the server asynchronously: retry
        // until the refusal shows (a success first would mean it had not yet).
        var clock = Stopwatch.StartNew();
        RpcException? refused = null;
        for (int i = 0; refused is null && clock.Elapsed < TimeSpan.FromSeconds(5); i++)
        {
            byte[] key = Encoding.UTF8.GetBytes($"budget-{i}");
            try
            {
                await sameConnection.UploadAsync(key, small, Ct);
                await sameConnection.DeleteAsync(key, Ct);
            }
            catch (RpcException e) when (e.StatusCode == StatusCode.ResourceExhausted)
            {
                refused = e;
            }
        }

        Assert.NotNull(refused);
        Assert.Contains("KVASM_CONN_BUFFER_MB", refused.Status.Detail, StringComparison.Ordinal);
        Assert.True((await otherConnection.UploadAsync("budget-other"u8.ToArray(), small, Ct)).Created);

        await holderCancel.CancelAsync();             // RST_STREAM: the reservation goes back
        holder.Dispose();
        clock.Restart();
        while (true)
        {
            try
            {
                Assert.True((await sameConnection.UploadAsync("budget-after"u8.ToArray(), small, Ct)).Created);
                break;
            }
            catch (RpcException e) when (e.StatusCode == StatusCode.ResourceExhausted && clock.Elapsed < TimeSpan.FromSeconds(5))
            {
                await Task.Delay(50, Ct);
            }
        }
    }

    /// <summary>
    /// The upload size limit: mirrored locally it throws before sending; a
    /// client configured above it hears the server's INVALID_ARGUMENT.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task UploadSizeLimitIsEnforcedBothSides()
    {
        await using Dedicated server = await Dedicated.StartAsync(shared);
        byte[] tooBig = new byte[MaxUpload + 1];
        using KvAsmClient mirrored = new(server.Address, new KvAsmClientOptions { MaxUploadBytes = MaxUpload });
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => mirrored.UploadAsync("size"u8.ToArray(), tooBig, Ct));
        using KvAsmClient generous = new(server.Address);
        RpcException error = await Assert.ThrowsAsync<RpcException>(() => generous.UploadAsync("size"u8.ToArray(), tooBig, Ct));
        Assert.Equal(StatusCode.InvalidArgument, error.StatusCode);
        Assert.Contains("value too large", error.Status.Detail, StringComparison.Ordinal);
    }

    /// <summary>A dedicated server container with the small limits, disposed with the test.</summary>
    private sealed class Dedicated : IAsyncDisposable
    {
        private readonly IContainer _container;

        /// <summary>Wraps a started container.</summary>
        /// <param name="container">The container.</param>
        /// <param name="address">Its h2c address.</param>
        private Dedicated(IContainer container, Uri address)
        {
            _container = container;
            Address = address;
        }

        /// <summary>The server's h2c address.</summary>
        public Uri Address { get; }

        /// <summary>Starts one (skipping the test when the shared server could not run).</summary>
        /// <param name="shared">The shared fixture: its skip decision applies here too.</param>
        /// <returns>The running server.</returns>
        public static async Task<Dedicated> StartAsync(KvAsmServer shared)
        {
            shared.RequireAddress();
            (IContainer container, Uri address) = await KvAsmServer.StartAsync(
                new Dictionary<string, string>
                {
                    ["KVASM_WORKERS"] = "2",
                    ["KVASM_MAX_MEMORY_MB"] = "16",
                    ["KVASM_CONN_BUFFER_MB"] = "8",
                    ["KVASM_MAX_UPLOAD_BYTES"] = MaxUpload.ToString(System.Globalization.CultureInfo.InvariantCulture),
                },
                snapshot: null,
                Ct);
            return new Dedicated(container, address);
        }

        /// <inheritdoc/>
        public ValueTask DisposeAsync() => _container.DisposeAsync();
    }
}
