using System.Security.Cryptography;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>Byte-level and behavioral tests of <see cref="SnapshotWriter"/> and <see cref="SnapshotFile"/>.</summary>
public sealed class SnapshotWriterTests : IDisposable
{
    /// <summary>
    /// The 3-record snapshot written by the server repository's C tool
    /// (server/tests/integration/mksnap, checksum by the server's own assembly
    /// SipHash), dataset from snapgen.h. Byte equality here proves the C#
    /// writer and the server agree on every header, record and checksum byte.
    /// </summary>
    private const string GoldenThreeRecords =
        "4b5641534e415031" + "01000000" + "00000000" + "0300000000000000" + "0000000000000000" +
        "06000000" + "00000000" + "736e61702d30" +
        "06000000" + "01000000" + "736e61702d31" + "83" +
        "06000000" + "02000000" + "736e61702d32" + "060d" +
        "4b564153454e4431" + "0a633ad6a4e06994";

    /// <summary>Scratch directory for file-based tests, removed on dispose.</summary>
    private readonly string _directory = Directory.CreateTempSubdirectory("kvasm-snapshot-tests-").FullName;

    /// <summary>Deletes the scratch directory.</summary>
    public void Dispose() => Directory.Delete(_directory, recursive: true);

    /// <summary>The C# writer reproduces the server tool's file byte for byte.</summary>
    [Fact]
    public void MatchesTheServersGoldenFile()
    {
        using var output = new MemoryStream();
        using (var writer = new SnapshotWriter(output, 3, leaveOpen: true))
        {
            for (int i = 0; i < 3; i++)
            {
                (byte[] key, byte[] value) = SnapGen(i);
                writer.Write(key, value);
            }

            writer.Complete();
        }

        Assert.Equal(GoldenThreeRecords, Convert.ToHexString(output.ToArray()).ToLowerInvariant());
    }

    /// <summary>
    /// Random records (empty values, 1 MiB values, long keys, duplicates) round
    /// trip through the writer and an independent, fully validating parser.
    /// </summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task RoundTripsThroughAnIndependentParser()
    {
        var random = new Random(42);
        var records = new List<(byte[] Key, byte[] Value)>();
        for (int i = 0; i < 500; i++)
        {
            byte[] key = new byte[i % 50 == 0 ? 200 : random.Next(1, 65)];
            random.NextBytes(key);
            byte[] value = new byte[i == 7 ? 1024 * 1024 : random.Next(0, 300)];
            random.NextBytes(value);
            records.Add((key, value));
        }

        records.Add(records[3]); // a duplicate: the server keeps the later one

        using var output = new MemoryStream();
        await using (var writer = new SnapshotWriter(output, records.Count, leaveOpen: true))
        {
            foreach ((byte[] key, byte[] value) in records)
            {
                await writer.WriteAsync(key, value, TestContext.Current.CancellationToken);
            }

            await writer.CompleteAsync(TestContext.Current.CancellationToken);
        }

        List<(byte[] Key, byte[] Value)> parsed = SnapshotParser.Parse(output.ToArray());
        Assert.Equal(records.Count, parsed.Count);
        for (int i = 0; i < records.Count; i++)
        {
            Assert.Equal(KeyCompaction.Encode(records[i].Key), parsed[i].Key);
            Assert.Equal(records[i].Value, parsed[i].Value);
        }
    }

    /// <summary>The atomic file writer produces exactly the streaming writer's bytes.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task SnapshotFileEqualsStreamingOutput()
    {
        var records = Enumerable.Range(0, 2000).Select(SnapGen).ToList();
        using var streamed = new MemoryStream();
        using (var writer = new SnapshotWriter(streamed, records.Count, leaveOpen: true))
        {
            foreach ((byte[] key, byte[] value) in records)
            {
                writer.Write(key, value);
            }

            writer.Complete();
        }

        string path = Path.Combine(_directory, "seed.kvs");
        long count = await SnapshotFile.WriteAsync(
            path, records.Select(r => KeyValuePair.Create<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>(r.Key, r.Value)),
            cancellationToken: TestContext.Current.CancellationToken);

        Assert.Equal(records.Count, count);
        Assert.Equal(streamed.ToArray(), await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
        Assert.Single(Directory.GetFiles(_directory)); // no temporary file left behind
    }

    /// <summary>A failing export leaves the previous snapshot untouched and cleans up.</summary>
    /// <returns>A task that completes when the test finishes.</returns>
    [Fact]
    public async Task FailedExportLeavesTheTargetUntouched()
    {
        string path = Path.Combine(_directory, "seed.kvs");
        await File.WriteAllBytesAsync(path, [1, 2, 3], TestContext.Current.CancellationToken);

        await Assert.ThrowsAsync<InvalidOperationException>(() => SnapshotFile.WriteAsync(
            path, FailingRecords(), cancellationToken: TestContext.Current.CancellationToken));

        Assert.Equal([1, 2, 3], await File.ReadAllBytesAsync(path, TestContext.Current.CancellationToken));
        Assert.Single(Directory.GetFiles(_directory));
    }

    /// <summary>Completing early is an error: a short file must never look valid.</summary>
    [Fact]
    public void CompleteRequiresTheDeclaredCount()
    {
        using var writer = new SnapshotWriter(new MemoryStream(), 2);
        writer.Write("a"u8, "1"u8);
        Assert.Throws<InvalidOperationException>(writer.Complete);
    }

    /// <summary>Writing more records than declared is rejected before any byte is written.</summary>
    [Fact]
    public void WritingBeyondTheDeclaredCountThrows()
    {
        using var writer = new SnapshotWriter(new MemoryStream(), 1);
        writer.Write("a"u8, "1"u8);
        Assert.Throws<InvalidOperationException>(() => writer.Write("b"u8, "2"u8));
        writer.Complete(); // the rejected record left no partial bytes
    }

    /// <summary>Limits are enforced at export time, mirroring the server's startup check.</summary>
    [Fact]
    public void LimitsAreEnforcedAtExportTime()
    {
        var options = new SnapshotWriterOptions { MaxKeyBytes = 100, MaxValueBytes = 10, CompactLongKeys = false };
        using var writer = new SnapshotWriter(new MemoryStream(), 1, options);
        Assert.Throws<ArgumentOutOfRangeException>(() => writer.Write("k"u8, new byte[11]));
        Assert.Throws<ArgumentOutOfRangeException>(() => writer.Write(new byte[101], "v"u8));
        Assert.Throws<ArgumentException>(() => writer.Write(ReadOnlySpan<byte>.Empty, "v"u8));
        writer.Write(new byte[100], new byte[10]);
        writer.Complete();
    }

    /// <summary>Disposing without Complete leaves no trailer, so the server would refuse the file.</summary>
    [Fact]
    public void DisposeWithoutCompleteLeavesAnInvalidFile()
    {
        using var output = new MemoryStream();
        using (var writer = new SnapshotWriter(output, 1, leaveOpen: true))
        {
            writer.Write("k"u8, "v"u8);
        }

        Assert.Throws<InvalidDataException>(() => SnapshotParser.Parse(output.ToArray()));
    }

    /// <summary>Long keys are compacted in snapshots exactly as the client compacts them.</summary>
    [Fact]
    public void LongKeysAreCompactedInSnapshots()
    {
        byte[] longKey = RandomNumberGenerator.GetBytes(5000);
        using var output = new MemoryStream();
        using (var writer = new SnapshotWriter(output, 1, leaveOpen: true))
        {
            writer.Write(longKey, "v"u8);
            writer.Complete();
        }

        (byte[] key, _) = Assert.Single(SnapshotParser.Parse(output.ToArray()));
        Assert.Equal(KeyCompaction.Encode(longKey), key);
    }

    /// <summary>Record i of the dataset in server/tests/integration/snapgen.h.</summary>
    /// <param name="i">Record index.</param>
    /// <returns>Key "snap-i" and (i mod 301) value bytes, byte j = (byte)(i*131 + j*7).</returns>
    internal static (byte[] Key, byte[] Value) SnapGen(int i)
    {
        byte[] value = new byte[i % 301];
        for (int j = 0; j < value.Length; j++)
        {
            value[j] = (byte)((i * 131) + (j * 7));
        }

        return (System.Text.Encoding.ASCII.GetBytes($"snap-{i}"), value);
    }

    /// <summary>Yields two records, then fails like a broken data source.</summary>
    /// <returns>The failing sequence.</returns>
    private static async IAsyncEnumerable<KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>> FailingRecords()
    {
        yield return KeyValuePair.Create<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>(new byte[] { 1 }, new byte[] { 2 });
        yield return KeyValuePair.Create<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>(new byte[] { 3 }, new byte[] { 4 });
        await Task.Yield();
        throw new InvalidOperationException("source failed");
    }
}
