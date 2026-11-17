using Grpc.Net.Client;
using Grpc.Net.Client.Configuration;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>
/// The transfer settings of <see cref="KvAsmClientOptions"/> and the channel
/// settings derived from them — no server needed: a wrong message limit would
/// fail every large download at runtime, a missing retry entry would silently
/// drop Download's retries.
/// </summary>
public sealed class TransferOptionsTests
{
    /// <summary>Out-of-range transfer settings are refused when the client is created.</summary>
    /// <param name="option">Which setting to break.</param>
    [Theory]
    [InlineData("MaxUploadBytes")]
    [InlineData("UploadChunkBytes0")]
    [InlineData("UploadChunkBytesHuge")]
    [InlineData("DownloadChunkBytesNegative")]
    [InlineData("DownloadChunkBytesHuge")]
    [InlineData("TransferTimeout")]
    [InlineData("TransferIdleTimeoutZero")]
    [InlineData("TransferIdleTimeoutHuge")]
    [InlineData("StreamReceiveWindowSmall")]
    [InlineData("StreamReceiveWindowHuge")]
    [InlineData("Connections0")]
    [InlineData("ConnectionsHuge")]
    public void InvalidTransferSettingsAreRefused(string option)
    {
        KvAsmClientOptions options = option switch
        {
            "MaxUploadBytes" => new() { MaxUploadBytes = -1 },
            "UploadChunkBytes0" => new() { UploadChunkBytes = 0 },
            "UploadChunkBytesHuge" => new() { UploadChunkBytes = (4 * 1024 * 1024) + 1 },
            "DownloadChunkBytesNegative" => new() { DownloadChunkBytes = -1 },
            "DownloadChunkBytesHuge" => new() { DownloadChunkBytes = (1024 * 1024) + 1 },
            "TransferTimeout" => new() { TransferTimeout = TimeSpan.Zero },
            "TransferIdleTimeoutZero" => new() { TransferIdleTimeout = TimeSpan.FromTicks(-5) },
            "StreamReceiveWindowSmall" => new() { StreamReceiveWindowBytes = 65534 },
            "StreamReceiveWindowHuge" => new() { StreamReceiveWindowBytes = (16 * 1024 * 1024) + 1 },
            "Connections0" => new() { Connections = 0 },
            "ConnectionsHuge" => new() { Connections = 65 },
            _ => new() { TransferIdleTimeout = TimeSpan.FromDays(30) },
        };
        Assert.Throws<ArgumentOutOfRangeException>(() => new KvAsmClient(new Uri("http://localhost:1"), options));

        // The channel settings refuse them too: a channel built from them
        // would fail later, farther from the mistake.
        Assert.Throws<ArgumentOutOfRangeException>(() => KvAsmClient.CreateChannelOptions(options));
    }

    /// <summary>The boundary values and the infinite timeouts are accepted.</summary>
    [Fact]
    public void BoundarySettingsAreAccepted()
    {
        var options = new KvAsmClientOptions
        {
            MaxUploadBytes = int.MaxValue,
            UploadChunkBytes = 4 * 1024 * 1024,
            DownloadChunkBytes = 1024 * 1024,
            TransferTimeout = Timeout.InfiniteTimeSpan,
            TransferIdleTimeout = Timeout.InfiniteTimeSpan,
            StreamReceiveWindowBytes = 16 * 1024 * 1024,
            Connections = 64,
        };
        using var client = new KvAsmClient(new Uri("http://localhost:1"), options);
        using var smallest = new KvAsmClient(
            new Uri("http://localhost:1"),
            new KvAsmClientOptions { MaxUploadBytes = 0, UploadChunkBytes = 1, DownloadChunkBytes = 0, StreamReceiveWindowBytes = 65535, Connections = 1 });
    }

    /// <summary>
    /// The client builds one lane (channel, connection) per configured
    /// connection — 4 by default, the measured choice — and a client given a
    /// channel has exactly that one, whatever the option says; nothing is
    /// connected by either constructor.
    /// </summary>
    /// <param name="connections">The configured connection count; 0 = the default options.</param>
    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(4)]
    [InlineData(64)]
    public void ClientBuildsOneLanePerConnection(int connections)
    {
        KvAsmClientOptions options = connections == 0 ? KvAsmClientOptions.Default : new() { Connections = connections };
        using (var owned = new KvAsmClient(new Uri("http://localhost:1"), options))
        {
            Assert.Equal(connections == 0 ? 4 : connections, owned.LaneCount);
        }

        using GrpcChannel channel = GrpcChannel.ForAddress("http://localhost:1");
        using var given = new KvAsmClient(channel, options);
        Assert.Equal(1, given.LaneCount);
    }

    /// <summary>
    /// The channel's HTTP/2 handler starts every call with the configured
    /// receive window — the setting that decides download speed — and the
    /// default stays the measured one.
    /// </summary>
    /// <param name="window">The configured window; 0 = the default options.</param>
    [Theory]
    [InlineData(0)]
    [InlineData(65535)]
    [InlineData(1024 * 1024)]
    [InlineData(16 * 1024 * 1024)]
    public void ChannelStartsCallsWithTheReceiveWindow(int window)
    {
        KvAsmClientOptions options = window == 0 ? KvAsmClientOptions.Default : new() { StreamReceiveWindowBytes = window };
        var handler = Assert.IsType<SocketsHttpHandler>(KvAsmClient.CreateChannelOptions(options).HttpHandler);
        Assert.Equal(options.StreamReceiveWindowBytes, handler.InitialHttp2StreamWindowSize);
        if (window == 0)
        {
            // The 2026-09-30 measurements chose it (benchmarks/results/2026-09-30-streaming).
            Assert.Equal(16 * 1024 * 1024, handler.InitialHttp2StreamWindowSize);
        }
    }

    /// <summary>
    /// Message limits fit every chunk: downloads carry up to 1 MiB whatever
    /// MaxValueBytes says, and an upload's first message a key next to a whole
    /// upload chunk.
    /// </summary>
    [Fact]
    public void MessageLimitsFitTransferChunks()
    {
        GrpcChannelOptions tiny = KvAsmClient.CreateChannelOptions(new KvAsmClientOptions { MaxValueBytes = 10 });
        Assert.True(tiny.MaxReceiveMessageSize >= (1024 * 1024) + 64);
        GrpcChannelOptions big = KvAsmClient.CreateChannelOptions(
            new KvAsmClientOptions { MaxValueBytes = 10, UploadChunkBytes = 4 * 1024 * 1024, MaxKeyBytes = 1024 });
        Assert.True(big.MaxSendMessageSize >= 1024 + (4 * 1024 * 1024) + 64);
    }

    /// <summary>Download is retried like Get; Upload, Put and Delete never are.</summary>
    [Fact]
    public void DownloadIsReadOnlyForRetries()
    {
        ServiceConfig config = KvAsmClient.CreateChannelOptions(KvAsmClientOptions.Default).ServiceConfig!;
        string[] retried = [.. config.MethodConfigs.SelectMany(m => m.Names).Select(n => $"{n.Service}/{n.Method}")];
        Assert.Contains("kvasm.v1.KvAsm/Download", retried);
        Assert.Contains("kvasm.v1.KvAsm/Get", retried);
        Assert.DoesNotContain("kvasm.v1.KvAsm/Upload", retried);
        Assert.DoesNotContain("kvasm.v1.KvAsm/Put", retried);
        Assert.DoesNotContain("kvasm.v1.KvAsm/Delete", retried);
        Assert.Null(KvAsmClient.CreateChannelOptions(new KvAsmClientOptions { ReadAttempts = 1 }).ServiceConfig);
    }
}
