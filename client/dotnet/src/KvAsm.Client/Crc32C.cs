using System.Buffers.Binary;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;

namespace KvAsm.Client;

/// <summary>
/// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) — the checksum the
/// KvAsm server computes over every value it receives through Upload and
/// every range it sends through Download.
/// </summary>
/// <remarks>
/// <para>
/// zlib-style: the pre- and post-inversion happen inside, so the checksum of
/// nothing is 0, <c>Compute("123456789")</c> is <c>0xE3069283</c>, and a
/// checksum is continued by passing the previous result to
/// <see cref="Append(uint, ReadOnlySpan{byte})"/> — any split of the data
/// gives the same result as one call over all of it.
/// </para>
/// <para>
/// Three implementations with identical results, the first the CPU supports
/// chosen per call (the checks are JIT-time constants, so the others compile
/// away):
/// </para>
/// <list type="bullet">
/// <item><description>
/// 256-bit carry-less folding (x64 with VPCLMULQDQ and AVX2: Ice Lake, Zen 3
/// and later) for buffers of <see cref="FoldMin"/> bytes and more: eight
/// independent 128-bit lanes, ~6x the CRC32 instruction — the server's
/// algorithm (server/src/crc32c.asm), so client and server check at the same
/// speed.
/// </description></item>
/// <item><description>
/// 128-bit carry-less folding (x64 with PCLMULQDQ: every x86-64 CPU since
/// 2010) for the same buffer sizes, ~3x the instruction.
/// </description></item>
/// <item><description>
/// The CPU's CRC32 instruction (SSE4.2 on x64, the CRC extension on Arm64)
/// through <see cref="BitOperations.Crc32C(uint, ulong)"/>, which the
/// runtime implements in software where the instruction is missing: small
/// buffers everywhere, all buffers on Arm64 and older x64. One dependency
/// chain, ~14 GB/s at best — enough to become a client's bottleneck once
/// transfers run at several GB/s, hence the folding paths.
/// </description></item>
/// </list>
/// <para>
/// Folding, in short: CRC is linear, so a 128-bit block V may be replaced by
/// V * x^D mod P placed D bits later — two 64x32-bit carry-less multiplies
/// with precomputed constants — and XORed into the data there. At the end the
/// last 128-bit block is reduced by two CRC32 instructions from a zero
/// register. The constants are x^e mod P, bit-reflected into bits 32..63.
/// </para>
/// </remarks>
public static class Crc32C
{
    /// <summary>Below this many bytes the CRC32-instruction chain is used: folding's setup and finish cost what the chain spends on ~160 bytes.</summary>
    internal const int FoldMin = 256;

    // Fold constants per distance D (bits): the multiplier of a lane's low
    // qword (the earlier, high-order half: x^(D+63) mod P) and of its high
    // qword (x^(D-1) mod P), each bit-reflected into bits 32..63 — the values
    // server/src/crc32c.asm's FOLD_K computes at assembly time.
    private const ulong K1024H = 0x6577B24500000000;
    private const ulong K1024L = 0x7417153F00000000;
    private const ulong K512H = 0x1C19243B00000000;
    private const ulong K512L = 0x75BBA45B00000000;
    private const ulong K256H = 0x33CCBBBC00000000;
    private const ulong K256L = 0xA2158B3400000000;
    private const ulong K128H = 0x3743F7BD00000000;
    private const ulong K128L = 0x3171D43000000000;

    /// <summary>
    /// True when the 256-bit folding path runs on this CPU (and runtime:
    /// <c>Pclmulqdq.V256</c> exists from .NET 9, so a net8.0 build always
    /// takes the 128-bit path).
    /// </summary>
#if NET9_0_OR_GREATER
    internal static bool Fold256Supported => Pclmulqdq.V256.IsSupported && Avx2.IsSupported && Sse42.X64.IsSupported;
#else
    internal static bool Fold256Supported => false;
#endif

    /// <summary>True when the 128-bit folding path runs on this CPU.</summary>
    internal static bool Fold128Supported => Pclmulqdq.IsSupported && Sse41.X64.IsSupported && Sse42.X64.IsSupported;

    /// <summary>Checksum of a whole buffer.</summary>
    /// <param name="data">The bytes.</param>
    /// <returns>The CRC-32C of <paramref name="data"/> (0 for an empty span).</returns>
    public static uint Compute(ReadOnlySpan<byte> data) => Append(0, data);

    /// <summary>Continues a checksum with more bytes.</summary>
    /// <param name="crc">0 to start, else the result of a previous call for the preceding bytes.</param>
    /// <param name="data">The next bytes.</param>
    /// <returns>The checksum of everything so far.</returns>
    /// <remarks>Thread-safe and allocation-free: a pure function of its arguments.</remarks>
    public static uint Append(uint crc, ReadOnlySpan<byte> data)
    {
        if (data.Length >= FoldMin)
        {
            if (Fold256Supported)
            {
                return Fold256(crc, data);
            }

            if (Fold128Supported)
            {
                return Fold128(crc, data);
            }
        }

        return Chain(crc, data);
    }

    /// <summary>The CRC32-instruction path (any buffer, any CPU): see the class remarks.</summary>
    /// <param name="crc">Checksum to continue (0 to start).</param>
    /// <param name="data">The bytes.</param>
    /// <returns>The continued checksum.</returns>
    internal static uint Chain(uint crc, ReadOnlySpan<byte> data) => ~ChainRaw(~crc, data);

    /// <summary>
    /// The CRC register update without the pre/post inversion — the CRC32
    /// instruction's own semantics, which the folding paths' finish needs.
    /// </summary>
    /// <param name="state">The raw register.</param>
    /// <param name="data">The bytes.</param>
    /// <returns>The updated raw register.</returns>
    private static uint ChainRaw(uint state, ReadOnlySpan<byte> data)
    {
        while (data.Length >= sizeof(ulong))
        {
            // Little-endian read: the CRC consumes the lowest byte first, which
            // must be the byte that comes first in memory on any host.
            state = BitOperations.Crc32C(state, BinaryPrimitives.ReadUInt64LittleEndian(data));
            data = data[sizeof(ulong)..];
        }

        foreach (byte b in data)
        {
            state = BitOperations.Crc32C(state, b);
        }

        return state;
    }

#if NET9_0_OR_GREATER
    /// <summary>One 256-bit fold step: per 128-bit lane, acc.lo * k.lo ^ acc.hi * k.hi.</summary>
    /// <param name="acc">Two lanes.</param>
    /// <param name="k">The distance's constants, in both lanes.</param>
    /// <returns>The lanes moved forward by the distance (congruent mod P).</returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static Vector256<ulong> Fold(Vector256<ulong> acc, Vector256<ulong> k) =>
        Pclmulqdq.V256.CarrylessMultiply(acc, k, 0x00) ^ Pclmulqdq.V256.CarrylessMultiply(acc, k, 0x11);
#endif

    /// <summary>One 128-bit fold step (see the 256-bit overload).</summary>
    /// <param name="acc">One lane.</param>
    /// <param name="k">The distance's constants.</param>
    /// <returns>The lane moved forward by the distance.</returns>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static Vector128<ulong> Fold(Vector128<ulong> acc, Vector128<ulong> k) =>
        Pclmulqdq.CarrylessMultiply(acc, k, 0x00) ^ Pclmulqdq.CarrylessMultiply(acc, k, 0x11);

    /// <summary>
    /// The last folded block reduced to the register value, then the
    /// remaining tail (&lt; 16 bytes) through the chain, then the final
    /// inversion.
    /// </summary>
    /// <param name="block">Everything folded so far, as one 128-bit block.</param>
    /// <param name="tail">The bytes after it.</param>
    /// <returns>The checksum.</returns>
    /// <remarks>
    /// Every earlier bit of the message was folded into the block, so the CRC
    /// register over just its 16 bytes, from zero, is the register over all of
    /// them — no Barrett reduction needed.
    /// </remarks>
    private static uint Finish(Vector128<ulong> block, ReadOnlySpan<byte> tail)
    {
        uint state = BitOperations.Crc32C(0u, block.GetElement(0));
        state = BitOperations.Crc32C(state, block.GetElement(1));
        return ~ChainRaw(state, tail);
    }

    /// <summary>256-bit folding: four registers of two lanes, 128 bytes per step.</summary>
    /// <param name="crc">Checksum to continue (0 to start).</param>
    /// <param name="data">At least <see cref="FoldMin"/> bytes.</param>
    /// <returns>The continued checksum.</returns>
    /// <remarks>
    /// Call only when <see cref="Fold256Supported"/> (else the instructions
    /// fault); on a runtime without <c>Pclmulqdq.V256</c> (net8.0) it is the
    /// 128-bit path, so a caller never needs a runtime check of its own.
    /// </remarks>
    internal static uint Fold256(uint crc, ReadOnlySpan<byte> data)
    {
#if !NET9_0_OR_GREATER
        return Fold128(crc, data);
#else
        ref byte p = ref MemoryMarshal.GetReference(data);
        nuint n = (nuint)data.Length;
        Vector256<ulong> y0 = Vector256.LoadUnsafe(ref p).AsUInt64();
        Vector256<ulong> y1 = Vector256.LoadUnsafe(ref p, 32).AsUInt64();
        Vector256<ulong> y2 = Vector256.LoadUnsafe(ref p, 64).AsUInt64();
        Vector256<ulong> y3 = Vector256.LoadUnsafe(ref p, 96).AsUInt64();
        // The (inverted) register enters as the message's first 4 bytes.
        y0 ^= Vector256.CreateScalar((ulong)~crc);
        Vector256<ulong> k = Vector256.Create(K1024H, K1024L, K1024H, K1024L);
        nuint off = 128;
        while (n - off >= 128)
        {
            y0 = Fold(y0, k) ^ Vector256.LoadUnsafe(ref p, off).AsUInt64();
            y1 = Fold(y1, k) ^ Vector256.LoadUnsafe(ref p, off + 32).AsUInt64();
            y2 = Fold(y2, k) ^ Vector256.LoadUnsafe(ref p, off + 64).AsUInt64();
            y3 = Fold(y3, k) ^ Vector256.LoadUnsafe(ref p, off + 96).AsUInt64();
            off += 128;
        }

        // Lanes at byte offsets 0,16 | 32,48 | 64,80 | 96,112 of the last
        // block: y0 onto y2 and y1 onto y3 (64 bytes on), then onto y3's
        // lanes (32 on), then lane 96 onto lane 112 (16 on).
        k = Vector256.Create(K512H, K512L, K512H, K512L);
        y0 = Fold(y0, k) ^ y2;
        y1 = Fold(y1, k) ^ y3;
        y0 = Fold(y0, Vector256.Create(K256H, K256L, K256H, K256L)) ^ y1;
        Vector128<ulong> k128 = Vector128.Create(K128H, K128L);
        Vector128<ulong> x = Fold(y0.GetLower(), k128) ^ y0.GetUpper();
        while (n - off >= 16)
        {
            x = Fold(x, k128) ^ Vector128.LoadUnsafe(ref p, off).AsUInt64();
            off += 16;
        }

        return Finish(x, data[(int)off..]);
#endif
    }

    /// <summary>128-bit folding: eight lanes, 128 bytes per step (the 256-bit algorithm on SSE registers).</summary>
    /// <param name="crc">Checksum to continue (0 to start).</param>
    /// <param name="data">At least <see cref="FoldMin"/> bytes.</param>
    /// <returns>The continued checksum.</returns>
    /// <remarks>Call only when <see cref="Fold128Supported"/>.</remarks>
    internal static uint Fold128(uint crc, ReadOnlySpan<byte> data)
    {
        ref byte p = ref MemoryMarshal.GetReference(data);
        nuint n = (nuint)data.Length;
        Vector128<ulong> x0 = Vector128.LoadUnsafe(ref p).AsUInt64();
        Vector128<ulong> x1 = Vector128.LoadUnsafe(ref p, 16).AsUInt64();
        Vector128<ulong> x2 = Vector128.LoadUnsafe(ref p, 32).AsUInt64();
        Vector128<ulong> x3 = Vector128.LoadUnsafe(ref p, 48).AsUInt64();
        Vector128<ulong> x4 = Vector128.LoadUnsafe(ref p, 64).AsUInt64();
        Vector128<ulong> x5 = Vector128.LoadUnsafe(ref p, 80).AsUInt64();
        Vector128<ulong> x6 = Vector128.LoadUnsafe(ref p, 96).AsUInt64();
        Vector128<ulong> x7 = Vector128.LoadUnsafe(ref p, 112).AsUInt64();
        x0 ^= Vector128.CreateScalar((ulong)~crc);
        Vector128<ulong> k = Vector128.Create(K1024H, K1024L);
        nuint off = 128;
        while (n - off >= 128)
        {
            x0 = Fold(x0, k) ^ Vector128.LoadUnsafe(ref p, off).AsUInt64();
            x1 = Fold(x1, k) ^ Vector128.LoadUnsafe(ref p, off + 16).AsUInt64();
            x2 = Fold(x2, k) ^ Vector128.LoadUnsafe(ref p, off + 32).AsUInt64();
            x3 = Fold(x3, k) ^ Vector128.LoadUnsafe(ref p, off + 48).AsUInt64();
            x4 = Fold(x4, k) ^ Vector128.LoadUnsafe(ref p, off + 64).AsUInt64();
            x5 = Fold(x5, k) ^ Vector128.LoadUnsafe(ref p, off + 80).AsUInt64();
            x6 = Fold(x6, k) ^ Vector128.LoadUnsafe(ref p, off + 96).AsUInt64();
            x7 = Fold(x7, k) ^ Vector128.LoadUnsafe(ref p, off + 112).AsUInt64();
            off += 128;
        }

        // Lane i sits at byte 16*i of the last block: 0-3 onto 4-7, then onto
        // 6-7, then 6 onto 7 (the results kept in the low registers).
        k = Vector128.Create(K512H, K512L);
        x0 = Fold(x0, k) ^ x4;
        x1 = Fold(x1, k) ^ x5;
        x2 = Fold(x2, k) ^ x6;
        x3 = Fold(x3, k) ^ x7;
        k = Vector128.Create(K256H, K256L);
        x0 = Fold(x0, k) ^ x2;
        x1 = Fold(x1, k) ^ x3;
        k = Vector128.Create(K128H, K128L);
        x0 = Fold(x0, k) ^ x1;
        while (n - off >= 16)
        {
            x0 = Fold(x0, k) ^ Vector128.LoadUnsafe(ref p, off).AsUInt64();
            off += 16;
        }

        return Finish(x0, data[(int)off..]);
    }
}
