using System.Text;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>
/// <see cref="Crc32C"/> against published vectors and an independent bitwise
/// implementation: the server checks every upload against the checksum this
/// class computes, so a wrong CRC here would make every upload fail with
/// DATA_LOSS (or, worse, agree with a wrong server).
/// </summary>
public sealed class Crc32CTests
{
    /// <summary>Published CRC-32C vectors (RFC 3720 B.4 and the common check values).</summary>
    /// <param name="hex">The input as hex, or "text:" + ASCII text.</param>
    /// <param name="expected">The checksum.</param>
    [Theory]
    [InlineData("", 0x00000000u)]
    [InlineData("text:a", 0xC1D04330u)]
    [InlineData("text:123456789", 0xE3069283u)]
    [InlineData("text:The quick brown fox jumps over the lazy dog", 0x22620404u)]
    [InlineData("0000000000000000000000000000000000000000000000000000000000000000", 0x8A9136AAu)]
    [InlineData("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", 0x62A8AB43u)]
    [InlineData("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F", 0x46DD794Eu)]
    [InlineData("1F1E1D1C1B1A191817161514131211100F0E0D0C0B0A09080706050403020100", 0x113FDB5Cu)]
    public void MatchesPublishedVectors(string hex, uint expected)
    {
        byte[] data = hex.StartsWith("text:", StringComparison.Ordinal)
            ? Encoding.ASCII.GetBytes(hex["text:".Length..])
            : Convert.FromHexString(hex);
        Assert.Equal(expected, Crc32C.Compute(data));
        Assert.Equal(expected, Reference(0, data));
    }

    /// <summary>Continuing a checksum at every split point equals one pass over everything.</summary>
    [Fact]
    public void ChainsAtEverySplit()
    {
        byte[] data = new byte[300];
        new Random(42).NextBytes(data);
        uint whole = Crc32C.Compute(data);
        for (int split = 0; split <= data.Length; split++)
        {
            Assert.Equal(whole, Crc32C.Append(Crc32C.Compute(data.AsSpan(0, split)), data.AsSpan(split)));
        }
    }

    /// <summary>
    /// Random lengths at random (unaligned) offsets, and random chaining,
    /// against the bitwise reference: the 8-byte fast path and the byte tail
    /// must agree for every alignment and length.
    /// </summary>
    [Fact]
    public void AgreesWithBitwiseReference()
    {
        var random = new Random(7);
        byte[] buffer = new byte[5000];
        random.NextBytes(buffer);
        for (int i = 0; i < 2000; i++)
        {
            int offset = random.Next(0, 64);
            int length = random.Next(0, buffer.Length - offset);
            ReadOnlySpan<byte> data = buffer.AsSpan(offset, length);
            uint seed = random.Next(4) == 0 ? 0 : (uint)random.Next();
            Assert.Equal(Reference(seed, data), Crc32C.Append(seed, data));
        }
    }

    /// <summary>A large buffer (the SIMD-size path) matches the reference.</summary>
    [Fact]
    public void LargeBufferMatchesReference()
    {
        byte[] data = new byte[(1 << 20) + 13];
        new Random(3).NextBytes(data);
        Assert.Equal(Reference(0, data), Crc32C.Compute(data));
    }

    /// <summary>
    /// Each implementation forced in turn — the CRC32-instruction chain, the
    /// 128-bit and the 256-bit folding paths (where this CPU and runtime have
    /// them) — against the bitwise reference, for every length from 0 to
    /// 1,200 at four alignments (crossing the 256-byte hand-over, one and
    /// several 128-byte blocks, the 16-byte loop and every tail length) and
    /// with a random checksum to continue: the dispatch in
    /// <see cref="Crc32C.Append(uint, ReadOnlySpan{byte})"/> may pick any of
    /// them, so each must be exact on its own.
    /// </summary>
    [Fact]
    public void EveryImplementationAgreesWithReference()
    {
        var random = new Random(11);
        byte[] buffer = new byte[1200 + 3];
        random.NextBytes(buffer);
        var paths = new List<(string Name, Func<uint, byte[], int, int, uint> Run)>
        {
            ("chain", (seed, b, o, n) => Crc32C.Chain(seed, b.AsSpan(o, n))),
        };
        if (Crc32C.Fold128Supported)
        {
            paths.Add(("fold128", (seed, b, o, n) => Crc32C.Fold128(seed, b.AsSpan(o, n))));
        }

        if (Crc32C.Fold256Supported)
        {
            paths.Add(("fold256", (seed, b, o, n) => Crc32C.Fold256(seed, b.AsSpan(o, n))));
        }

        foreach ((string name, Func<uint, byte[], int, int, uint> run) in paths)
        {
            // The folding paths need FoldMin bytes (callers guarantee it).
            int min = name == "chain" ? 0 : Crc32C.FoldMin;
            for (int length = min; length <= 1200; length++)
            {
                int offset = length % 4;
                uint seed = (uint)random.Next();
                Assert.True(
                    Reference(seed, buffer.AsSpan(offset, length)) == run(seed, buffer, offset, length),
                    $"{name}: length {length}, offset {offset}");
            }
        }
    }

    /// <summary>
    /// All-zero and all-one buffers through the folding paths: a wrong fold
    /// constant cannot hide behind random data (every multiply matters).
    /// </summary>
    /// <param name="fill">The byte every position holds.</param>
    [Theory]
    [InlineData(0x00)]
    [InlineData(0xFF)]
    public void UniformBuffersMatchReference(byte fill)
    {
        byte[] data = new byte[4096 + 7];
        Array.Fill(data, fill);
        foreach (uint seed in new uint[] { 0, 0xFFFFFFFF, 0x12345678 })
        {
            uint want = Reference(seed, data);
            Assert.Equal(want, Crc32C.Append(seed, data));
            Assert.Equal(want, Crc32C.Chain(seed, data));
            if (Crc32C.Fold128Supported)
            {
                Assert.Equal(want, Crc32C.Fold128(seed, data));
            }

            if (Crc32C.Fold256Supported)
            {
                Assert.Equal(want, Crc32C.Fold256(seed, data));
            }
        }
    }

    /// <summary>
    /// CRC-32C one bit at a time, zlib-style — shares nothing with the
    /// implementation under test beyond the polynomial.
    /// </summary>
    /// <param name="crc">Checksum to continue (0 to start).</param>
    /// <param name="data">Bytes.</param>
    /// <returns>The continued checksum.</returns>
    internal static uint Reference(uint crc, ReadOnlySpan<byte> data)
    {
        crc = ~crc;
        foreach (byte b in data)
        {
            crc ^= b;
            for (int k = 0; k < 8; k++)
            {
                crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
            }
        }

        return ~crc;
    }
}
