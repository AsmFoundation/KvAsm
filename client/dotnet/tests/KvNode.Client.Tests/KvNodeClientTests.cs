using System.Security.Cryptography;
using System.Text;
using Grpc.Core;
using Xunit;

namespace KvNode.Client.Tests;

/// <summary>End-to-end tests of <see cref="KvNodeClient"/> against the real server container.</summary>
/// <param name="server">The shared server.</param>
public sealed class KvNodeClientTests(KvNodeServer server)
{
    /// <summary>Cancellation for the current test (xUnit stops it on timeout/abort).</summary>
    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    /// <summary>Put/Get/Delete with exact created/replaced/deleted flags.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task PutGetDeleteReportExactFlags()
    {
        using KvNodeClient client = NewClient();
        string key = Unique("crud");
        Assert.Null(await client.GetStringAsync(key, Ct));
        Assert.True(await client.PutAsync(key, "first", Ct));
        Assert.Equal("first", await client.GetStringAsync(key, Ct));
        Assert.False(await client.PutAsync(key, "second", Ct));
        Assert.Equal("second", await client.GetStringAsync(key, Ct));
        Assert.True(await client.DeleteAsync(key, Ct));
        Assert.False(await client.DeleteAsync(key, Ct));
        Assert.Null(await client.GetStringAsync(key, Ct));
    }

    /// <summary>Binary keys/values (NULs, 0xFF) and a maximum-size value round-trip exactly.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task BinaryDataRoundTripsExactly()
    {
        using KvNodeClient client = NewClient();
        byte[] key = [0, 0xFF, 0, 1, .. Encoding.ASCII.GetBytes(Unique("bin"))];
        byte[] value = RandomNumberGenerator.GetBytes(KvNodeClientOptions.Default.MaxValueBytes);
        Assert.True(await client.PutAsync(key, value, Ct));
        ReadOnlyMemory<byte>? read = await client.GetAsync(key, Ct);
        Assert.True(read.HasValue);
        Assert.True(read.Value.Span.SequenceEqual(value));
        Assert.Null(await client.GetAsync(key.AsMemory(0, key.Length - 1), Ct)); // a prefix is another key
    }

    /// <summary>An empty value is stored and returned as empty — not confused with a missing key.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task EmptyValueIsNotMissing()
    {
        using KvNodeClient client = NewClient();
        byte[] key = Encoding.UTF8.GetBytes(Unique("empty"));
        Assert.True(await client.PutAsync(key, ReadOnlyMemory<byte>.Empty, Ct));
        ReadOnlyMemory<byte>? read = await client.GetAsync(key, Ct);
        Assert.True(read.HasValue);
        Assert.True(read.Value.IsEmpty);
    }

    /// <summary>
    /// A long key is compacted client-side and found again; a raw-mode client
    /// sees it stored under exactly 0xFF ‖ SHA-512(key).
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task LongKeysAreCompactedPerContract()
    {
        using KvNodeClient client = NewClient();
        using KvNodeClient raw = NewClient(new KvNodeClientOptions { CompactLongKeys = false });
        byte[] longKey = Encoding.UTF8.GetBytes(Unique("long") + new string('x', 5000));
        Assert.True(await client.PutAsync(longKey, "v"u8.ToArray(), Ct));
        Assert.Equal("v"u8.ToArray(), (await client.GetAsync(longKey, Ct))?.ToArray());
        Assert.Equal("v"u8.ToArray(), (await raw.GetAsync(KeyCompaction.Encode(longKey), Ct))?.ToArray());
        Assert.True(await client.DeleteAsync(longKey, Ct));
    }

    /// <summary>Input the client can judge locally never reaches the network.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task LocalLimitsThrowBeforeSending()
    {
        using KvNodeClient client = NewClient(new KvNodeClientOptions { CompactLongKeys = false, MaxKeyBytes = 100, MaxValueBytes = 10 });
        await Assert.ThrowsAsync<ArgumentException>(() => client.GetAsync(ReadOnlyMemory<byte>.Empty, Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => client.GetAsync(new byte[101], Ct));
        await Assert.ThrowsAsync<ArgumentOutOfRangeException>(() => client.PutAsync("k"u8.ToArray(), new byte[11], Ct));
    }

    /// <summary>Limits enforced by the server surface as INVALID_ARGUMENT.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ServerLimitsSurfaceAsInvalidArgument()
    {
        // The client is configured above the server's 1 MiB default, so the
        // server is the one to refuse.
        using KvNodeClient client = NewClient(new KvNodeClientOptions { MaxValueBytes = 2 * 1024 * 1024 });
        var error = await Assert.ThrowsAsync<RpcException>(
            () => client.PutAsync(Encoding.UTF8.GetBytes(Unique("big")), new byte[(1024 * 1024) + 1], Ct));
        Assert.Equal(StatusCode.InvalidArgument, error.StatusCode);
        Assert.Contains("value too large", error.Status.Detail, StringComparison.Ordinal);
    }

    /// <summary>The standard gRPC health service reports SERVING.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task HealthReportsServing()
    {
        using KvNodeClient client = NewClient();
        Assert.True(await client.IsServingAsync(Ct));
    }

    /// <summary>
    /// 64 concurrent tasks share one client (HTTP/2 multiplexing): every
    /// private key sequence keeps exact flags, and readers of a shared key
    /// only ever see complete values.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ConcurrentCallsOverOneClientStayConsistent()
    {
        using KvNodeClient client = NewClient();
        string shared = Unique("shared");
        await client.PutAsync(shared, new string('a', 1000), Ct);

        await Task.WhenAll(Enumerable.Range(0, 64).Select(async task =>
        {
            for (int i = 0; i < 100; i++)
            {
                string key = $"{shared}-{task}-{i}";
                Assert.True(await client.PutAsync(key, key, Ct));
                Assert.Equal(key, await client.GetStringAsync(key, Ct));
                Assert.True(await client.DeleteAsync(key, Ct));

                // A shared key flips between two uniform values; a torn read
                // would mix letters.
                char letter = (task + i) % 2 == 0 ? 'a' : 'b';
                await client.PutAsync(shared, new string(letter, 1000), Ct);
                string? seen = await client.GetStringAsync(shared, Ct);
                Assert.NotNull(seen);
                Assert.True(seen.Length == 1000 && (seen.All(c => c == 'a') || seen.All(c => c == 'b')));
            }
        }));
    }

    /// <summary>A cancelled token cancels the call with StatusCode.Cancelled.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task CancellationIsObserved()
    {
        using KvNodeClient client = NewClient();
        using var cancelled = new CancellationTokenSource();
        await cancelled.CancelAsync();
        var error = await Assert.ThrowsAsync<RpcException>(() => client.GetStringAsync(Unique("cancel"), cancelled.Token));
        Assert.Equal(StatusCode.Cancelled, error.StatusCode);
    }

    /// <summary>Creates a client for the shared server (skips the test when none is running).</summary>
    /// <param name="options">Client options, or null for defaults.</param>
    /// <returns>A new client; the caller disposes it.</returns>
    private KvNodeClient NewClient(KvNodeClientOptions? options = null) => new(server.RequireAddress(), options);

    /// <summary>A key unique to this test run, so tests never interfere.</summary>
    /// <param name="name">A readable prefix.</param>
    /// <returns>The unique key text.</returns>
    private static string Unique(string name) => $"{name}-{Guid.NewGuid():N}";
}
