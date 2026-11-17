using System.Buffers.Binary;
using System.Numerics;
using System.Runtime.CompilerServices;

namespace KvAsm.Client;

/// <summary>
/// Incremental SipHash-2-4 with a 128-bit output: the checksum function of
/// KvAsm snapshot files.
/// </summary>
/// <remarks>
/// <para>
/// Bit-for-bit identical to the reference implementation
/// (github.com/veorq/SipHash with <c>outlen = 16</c>) and to the server's
/// assembly <c>kv_siphash128</c>; the unit tests check all 64 reference vectors
/// and prove that arbitrary chunking of the input does not change the result.
/// </para>
/// <para>
/// Snapshots use a fixed all-zero key, so the checksum detects corruption
/// (truncation, bit rot, the wrong file), not deliberate tampering: anyone can
/// recompute it. Protect snapshot files with file-system permissions or
/// signatures when tampering is a concern.
/// </para>
/// <para>Not thread-safe, and single-use: <see cref="Finish"/> may be called once.</para>
/// </remarks>
internal sealed class SipHash128
{
    private ulong _v0;
    private ulong _v1;
    private ulong _v2;
    private ulong _v3;

    /// <summary>Pending input bytes (fewer than 8), little-endian, first byte lowest.</summary>
    private ulong _tail;

    /// <summary>Number of valid bytes in <see cref="_tail"/> (0..7).</summary>
    private int _tailLength;

    /// <summary>Total bytes appended; only its low 8 bits enter the final block.</summary>
    private ulong _length;

    private bool _finished;

    /// <summary>Starts a hash with the given 128-bit key.</summary>
    /// <param name="key0">Key bytes 0..7 as a little-endian integer.</param>
    /// <param name="key1">Key bytes 8..15 as a little-endian integer.</param>
    public SipHash128(ulong key0, ulong key1)
    {
        _v0 = 0x736f6d6570736575UL ^ key0;
        _v1 = 0x646f72616e646f6dUL ^ key1 ^ 0xee; // 0xee: the 128-bit output variant
        _v2 = 0x6c7967656e657261UL ^ key0;
        _v3 = 0x7465646279746573UL ^ key1;
    }

    /// <summary>Feeds more input; any split of the input gives the same result.</summary>
    /// <param name="data">The next input bytes (may be empty).</param>
    /// <exception cref="InvalidOperationException"><see cref="Finish"/> was already called.</exception>
    public void Append(ReadOnlySpan<byte> data)
    {
        if (_finished)
        {
            throw new InvalidOperationException("The hash was already finished.");
        }

        _length += (ulong)data.Length;

        // Complete a block left partial by the previous call before taking
        // whole 8-byte blocks straight from the input.
        if (_tailLength > 0)
        {
            while (_tailLength < 8 && !data.IsEmpty)
            {
                _tail |= (ulong)data[0] << (8 * _tailLength);
                _tailLength++;
                data = data[1..];
            }

            if (_tailLength < 8)
            {
                return;
            }

            Compress(_tail);
            _tail = 0;
            _tailLength = 0;
        }

        while (data.Length >= 8)
        {
            Compress(BinaryPrimitives.ReadUInt64LittleEndian(data));
            data = data[8..];
        }

        for (int i = 0; i < data.Length; i++)
        {
            _tail |= (ulong)data[i] << (8 * i);
        }

        _tailLength = data.Length;
    }

    /// <summary>Completes the hash.</summary>
    /// <returns>
    /// The 128-bit result as two little-endian halves: <c>Low</c> is output
    /// bytes 0..7 (what the server calls h1, and the value stored in snapshot
    /// trailers), <c>High</c> is bytes 8..15.
    /// </returns>
    /// <exception cref="InvalidOperationException">Called a second time.</exception>
    public (ulong Low, ulong High) Finish()
    {
        if (_finished)
        {
            throw new InvalidOperationException("The hash was already finished.");
        }

        _finished = true;
        ulong last = (_length << 56) | _tail;
        _v3 ^= last;
        Round();
        Round();
        _v0 ^= last;

        _v2 ^= 0xee;
        Round();
        Round();
        Round();
        Round();
        ulong low = _v0 ^ _v1 ^ _v2 ^ _v3;

        _v1 ^= 0xdd;
        Round();
        Round();
        Round();
        Round();
        ulong high = _v0 ^ _v1 ^ _v2 ^ _v3;
        return (low, high);
    }

    /// <summary>Hashes a complete input in one call.</summary>
    /// <param name="data">The input.</param>
    /// <param name="key0">Key bytes 0..7, little-endian (0 for snapshot checksums).</param>
    /// <param name="key1">Key bytes 8..15, little-endian (0 for snapshot checksums).</param>
    /// <returns>The 128-bit result; see <see cref="Finish"/>.</returns>
    public static (ulong Low, ulong High) Hash(ReadOnlySpan<byte> data, ulong key0 = 0, ulong key1 = 0)
    {
        var hash = new SipHash128(key0, key1);
        hash.Append(data);
        return hash.Finish();
    }

    /// <summary>Absorbs one 8-byte block (2 compression rounds).</summary>
    /// <param name="block">The block as a little-endian integer.</param>
    private void Compress(ulong block)
    {
        _v3 ^= block;
        Round();
        Round();
        _v0 ^= block;
    }

    /// <summary>One SipRound (the ARX permutation of the four state words).</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void Round()
    {
        _v0 += _v1;
        _v1 = BitOperations.RotateLeft(_v1, 13);
        _v1 ^= _v0;
        _v0 = BitOperations.RotateLeft(_v0, 32);
        _v2 += _v3;
        _v3 = BitOperations.RotateLeft(_v3, 16);
        _v3 ^= _v2;
        _v0 += _v3;
        _v3 = BitOperations.RotateLeft(_v3, 21);
        _v3 ^= _v0;
        _v2 += _v1;
        _v1 = BitOperations.RotateLeft(_v1, 17);
        _v1 ^= _v2;
        _v2 = BitOperations.RotateLeft(_v2, 32);
    }
}
