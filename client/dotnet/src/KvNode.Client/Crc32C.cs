using System.Buffers.Binary;
using System.Numerics;

namespace KvNode.Client;

/// <summary>
/// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) — the checksum the
/// KvNode server computes over every value it receives through Upload and
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
/// Uses the CPU's CRC32 instruction (SSE4.2 on x64, the CRC extension on
/// Arm64) through <see cref="BitOperations.Crc32C(uint, ulong)"/>, which the
/// runtime implements in software where the instruction is missing: always
/// correct, fast where the hardware allows (gigabytes per second).
/// </para>
/// </remarks>
public static class Crc32C
{
    /// <summary>Checksum of a whole buffer.</summary>
    /// <param name="data">The bytes.</param>
    /// <returns>The CRC-32C of <paramref name="data"/> (0 for an empty span).</returns>
    public static uint Compute(ReadOnlySpan<byte> data) => Append(0, data);

    /// <summary>Continues a checksum with more bytes.</summary>
    /// <param name="crc">0 to start, else the result of a previous call for the preceding bytes.</param>
    /// <param name="data">The next bytes.</param>
    /// <returns>The checksum of everything so far.</returns>
    public static uint Append(uint crc, ReadOnlySpan<byte> data)
    {
        // The instruction works on the raw register value; the standard CRC
        // inverts it on entry and exit, which is what makes chaining work.
        uint state = ~crc;
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

        return ~state;
    }
}
