using System.Security.Cryptography;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>Pins the long-key contract every KvAsm client must implement identically.</summary>
public sealed class KeyCompactionTests
{
    /// <summary>Keys up to 64 bytes are sent unchanged (as a copy).</summary>
    /// <param name="length">Key length.</param>
    [Theory]
    [InlineData(1)]
    [InlineData(20)]
    [InlineData(KeyCompaction.MaxRawKeyBytes)]
    public void ShortKeysAreUnchanged(int length)
    {
        byte[] key = RandomNumberGenerator.GetBytes(length);
        byte[] encoded = KeyCompaction.Encode(key);
        Assert.Equal(key, encoded);
        Assert.NotSame(key, encoded);
    }

    /// <summary>Longer keys become 0xFF followed by their SHA-512 digest.</summary>
    /// <param name="length">Key length.</param>
    [Theory]
    [InlineData(KeyCompaction.MaxRawKeyBytes + 1)]
    [InlineData(1000)]
    [InlineData(1 << 20)]
    public void LongKeysBecomeMarkerPlusSha512(int length)
    {
        byte[] key = RandomNumberGenerator.GetBytes(length);
        byte[] encoded = KeyCompaction.Encode(key);
        Assert.Equal(KeyCompaction.CompactedKeyBytes, encoded.Length);
        Assert.Equal(KeyCompaction.Marker, encoded[0]);
        Assert.Equal(SHA512.HashData(key), encoded[1..]);
        Assert.True(KeyCompaction.IsCompacted(encoded));
    }

    /// <summary>
    /// The two key spaces are disjoint by length: no raw key is 65 bytes, and
    /// every compacted key is. This is what makes the scheme collision-safe.
    /// </summary>
    [Fact]
    public void RawAndCompactedKeysNeverShareALength()
    {
        for (int length = 1; length <= 200; length++)
        {
            int encodedLength = KeyCompaction.Encode(new byte[length]).Length;
            Assert.True(length <= KeyCompaction.MaxRawKeyBytes
                ? encodedLength == length && encodedLength < KeyCompaction.CompactedKeyBytes
                : encodedLength == KeyCompaction.CompactedKeyBytes);
        }
    }

    /// <summary>Compaction can be turned off (raw mode) for callers that need exact keys.</summary>
    [Fact]
    public void CompactionCanBeDisabled()
    {
        byte[] key = RandomNumberGenerator.GetBytes(300);
        Assert.Equal(key, KeyCompaction.Encode(key, compactLongKeys: false));
    }

    /// <summary>An empty key is invalid everywhere in KvAsm.</summary>
    [Fact]
    public void EmptyKeyIsRejected() =>
        Assert.Throws<ArgumentException>(() => KeyCompaction.Encode(ReadOnlySpan<byte>.Empty));

    /// <summary>Only 65-byte keys starting with the marker count as compacted.</summary>
    [Fact]
    public void IsCompactedRecognizesOnlyTheCompactedForm()
    {
        byte[] almost = new byte[KeyCompaction.CompactedKeyBytes];
        Assert.False(KeyCompaction.IsCompacted(almost));             // wrong marker
        almost[0] = KeyCompaction.Marker;
        Assert.True(KeyCompaction.IsCompacted(almost));
        Assert.False(KeyCompaction.IsCompacted(almost.AsSpan(1)));   // wrong length
    }
}
