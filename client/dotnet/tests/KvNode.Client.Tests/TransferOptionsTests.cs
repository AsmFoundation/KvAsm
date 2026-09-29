using Grpc.Net.Client;
using Grpc.Net.Client.Configuration;
using Xunit;

namespace KvNode.Client.Tests;

/// <summary>
/// The transfer settings of <see cref="KvNodeClientOptions"/> and the channel
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
    public void InvalidTransferSettingsAreRefused(string option)
    {
        KvNodeClientOptions options = option switch
        {
            "MaxUploadBytes" => new() { MaxUploadBytes = -1 },
            "UploadChunkBytes0" => new() { UploadChunkBytes = 0 },
            "UploadChunkBytesHuge" => new() { UploadChunkBytes = (4 * 1024 * 1024) + 1 },
            "DownloadChunkBytesNegative" => new() { DownloadChunkBytes = -1 },
            "DownloadChunkBytesHuge" => new() { DownloadChunkBytes = (1024 * 1024) + 1 },
            "TransferTimeout" => new() { TransferTimeout = TimeSpan.Zero },
            "TransferIdleTimeoutZero" => new() { TransferIdleTimeout = TimeSpan.FromTicks(-5) },
            _ => new() { TransferIdleTimeout = TimeSpan.FromDays(30) },
        };
        Assert.Throws<ArgumentOutOfRangeException>(() => new KvNodeClient(new Uri("http://localhost:1"), options));
    }

    /// <summary>The boundary values and the infinite timeouts are accepted.</summary>
    [Fact]
    public void BoundarySettingsAreAccepted()
    {
        var options = new KvNodeClientOptions
        {
            MaxUploadBytes = int.MaxValue,
            UploadChunkBytes = 4 * 1024 * 1024,
            DownloadChunkBytes = 1024 * 1024,
            TransferTimeout = Timeout.InfiniteTimeSpan,
            TransferIdleTimeout = Timeout.InfiniteTimeSpan,
        };
        using var client = new KvNodeClient(new Uri("http://localhost:1"), options);
        using var smallest = new KvNodeClient(
            new Uri("http://localhost:1"), new KvNodeClientOptions { MaxUploadBytes = 0, UploadChunkBytes = 1, DownloadChunkBytes = 0 });
    }

    /// <summary>
    /// Message limits fit every chunk: downloads carry up to 1 MiB whatever
    /// MaxValueBytes says, and an upload's first message a key next to a whole
    /// upload chunk.
    /// </summary>
    [Fact]
    public void MessageLimitsFitTransferChunks()
    {
        GrpcChannelOptions tiny = KvNodeClient.CreateChannelOptions(new KvNodeClientOptions { MaxValueBytes = 10 });
        Assert.True(tiny.MaxReceiveMessageSize >= (1024 * 1024) + 64);
        GrpcChannelOptions big = KvNodeClient.CreateChannelOptions(
            new KvNodeClientOptions { MaxValueBytes = 10, UploadChunkBytes = 4 * 1024 * 1024, MaxKeyBytes = 1024 });
        Assert.True(big.MaxSendMessageSize >= 1024 + (4 * 1024 * 1024) + 64);
    }

    /// <summary>Download is retried like Get; Upload, Put and Delete never are.</summary>
    [Fact]
    public void DownloadIsReadOnlyForRetries()
    {
        ServiceConfig config = KvNodeClient.CreateChannelOptions(KvNodeClientOptions.Default).ServiceConfig!;
        string[] retried = [.. config.MethodConfigs.SelectMany(m => m.Names).Select(n => $"{n.Service}/{n.Method}")];
        Assert.Contains("kvnode.v1.KvNode/Download", retried);
        Assert.Contains("kvnode.v1.KvNode/Get", retried);
        Assert.DoesNotContain("kvnode.v1.KvNode/Upload", retried);
        Assert.DoesNotContain("kvnode.v1.KvNode/Put", retried);
        Assert.DoesNotContain("kvnode.v1.KvNode/Delete", retried);
        Assert.Null(KvNodeClient.CreateChannelOptions(new KvNodeClientOptions { ReadAttempts = 1 }).ServiceConfig);
    }
}
