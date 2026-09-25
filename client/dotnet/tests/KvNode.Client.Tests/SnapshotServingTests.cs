using System.Security.Cryptography;
using DotNet.Testcontainers.Containers;
using Xunit;

namespace KvNode.Client.Tests;

/// <summary>
/// The full data path of the "snapshot source": a C#-written snapshot, loaded
/// by a fresh server container at startup, served through the C# client.
/// </summary>
/// <param name="server">The shared server (only used to skip when Docker is unavailable).</param>
public sealed class SnapshotServingTests(KvNodeServer server)
{
    /// <summary>
    /// 5000 records — snapgen dataset, long keys, a duplicate — written with
    /// <see cref="SnapshotFile"/>, are all served exactly, with the duplicate
    /// resolved to its last value.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task ServerServesEveryRecordOfAClientWrittenSnapshot()
    {
        server.RequireAddress();
        CancellationToken ct = TestContext.Current.CancellationToken;

        var records = Enumerable.Range(0, 5000).Select(SnapshotWriterTests.SnapGen).ToList();
        byte[] longKey = RandomNumberGenerator.GetBytes(3000);
        records.Add((longKey, "long-key-value"u8.ToArray()));
        records.Add((records[10].Key, "latest"u8.ToArray())); // duplicate: the last one wins

        string path = Path.Combine(Path.GetTempPath(), $"kvnode-{Guid.NewGuid():N}.kvs");
        try
        {
            long written = await SnapshotFile.WriteAsync(
                path, records.Select(r => KeyValuePair.Create<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>(r.Key, r.Value)),
                cancellationToken: ct);
            Assert.Equal(records.Count, written);

            (IContainer container, Uri address) = await KvNodeServer.StartAsync(
                new Dictionary<string, string> { ["KVNODE_WORKERS"] = "1" }, await File.ReadAllBytesAsync(path, ct), ct);
            await using (container)
            {
                (string stdout, string stderr) = await container.GetLogsAsync(ct: ct);
                Assert.Contains($"msg=\"snapshot loaded\" records={records.Count}", stdout + stderr, StringComparison.Ordinal);

                using var client = new KvNodeClient(address);
                for (int i = 0; i < 5000; i++)
                {
                    byte[] expected = i == 10 ? "latest"u8.ToArray() : records[i].Value;
                    ReadOnlyMemory<byte>? value = await client.GetAsync(records[i].Key, ct);
                    Assert.True(value.HasValue, $"record {i} missing");
                    Assert.True(value.Value.Span.SequenceEqual(expected), $"record {i} differs");
                }

                Assert.Equal("long-key-value"u8.ToArray(), (await client.GetAsync(longKey, ct))?.ToArray());
            }
        }
        finally
        {
            File.Delete(path);
        }
    }
}
