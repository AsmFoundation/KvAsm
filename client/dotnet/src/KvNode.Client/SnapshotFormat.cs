using System.Buffers.Binary;

namespace KvNode.Client;

/// <summary>
/// Byte layout of a KvNode snapshot file (little-endian), shared by
/// <see cref="SnapshotWriter"/> and <see cref="SnapshotFile"/>. It must match
/// the server's loader (server/src/snapshot.asm) exactly:
/// <code>
///   0   "KVNSNAP1"            magic
///   8   u32 version = 1
///   12  u32 flags   = 0
///   16  u64 record count
///   24  u64 reserved = 0
///   32  records: { u32 key_len, u32 value_len, key, value } x count
///   end "KVNSEND1" + u64 checksum = SipHash-2-4-128 (16 zero key bytes),
///       low word, over every byte before the trailer
/// </code>
/// </summary>
internal static class SnapshotFormat
{
    /// <summary>Format version written into the header.</summary>
    public const uint Version = 1;

    /// <summary>Size of the fixed header.</summary>
    public const int HeaderBytes = 32;

    /// <summary>Size of each record's length prefix (key_len + value_len).</summary>
    public const int RecordHeaderBytes = 8;

    /// <summary>Size of the trailer (magic + checksum).</summary>
    public const int TrailerBytes = 16;

    /// <summary>Header magic.</summary>
    public static ReadOnlySpan<byte> Magic => "KVNSNAP1"u8;

    /// <summary>Trailer magic.</summary>
    public static ReadOnlySpan<byte> TrailerMagic => "KVNSEND1"u8;

    /// <summary>Writes the 32-byte header.</summary>
    /// <param name="destination">At least <see cref="HeaderBytes"/> bytes.</param>
    /// <param name="recordCount">Number of records that follow.</param>
    public static void WriteHeader(Span<byte> destination, long recordCount)
    {
        Magic.CopyTo(destination);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[8..], Version);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[12..], 0);
        BinaryPrimitives.WriteInt64LittleEndian(destination[16..], recordCount);
        BinaryPrimitives.WriteInt64LittleEndian(destination[24..], 0);
    }

    /// <summary>Writes one record's length prefix.</summary>
    /// <param name="destination">At least <see cref="RecordHeaderBytes"/> bytes.</param>
    /// <param name="keyLength">Key length in bytes (as stored, after compaction).</param>
    /// <param name="valueLength">Value length in bytes.</param>
    public static void WriteRecordHeader(Span<byte> destination, int keyLength, int valueLength)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination, (uint)keyLength);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[4..], (uint)valueLength);
    }

    /// <summary>Writes the 16-byte trailer.</summary>
    /// <param name="destination">At least <see cref="TrailerBytes"/> bytes.</param>
    /// <param name="checksum">Low word of SipHash-2-4-128 over all preceding bytes.</param>
    public static void WriteTrailer(Span<byte> destination, ulong checksum)
    {
        TrailerMagic.CopyTo(destination);
        BinaryPrimitives.WriteUInt64LittleEndian(destination[8..], checksum);
    }

    /// <summary>Validates a key and applies the <see cref="KeyCompaction"/> contract.</summary>
    /// <param name="key">The application key.</param>
    /// <param name="options">Writer options (compaction flag, key limit).</param>
    /// <returns>The key as it must be stored: the key itself or a new compacted array.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The stored key would exceed <see cref="SnapshotWriterOptions.MaxKeyBytes"/>.</exception>
    public static ReadOnlyMemory<byte> WireKey(ReadOnlyMemory<byte> key, SnapshotWriterOptions options) =>
        Compacts(key.Length, options) ? KeyCompaction.Encode(key.Span) : key;

    /// <summary>Span twin of <see cref="WireKey(ReadOnlyMemory{byte}, SnapshotWriterOptions)"/>: no copy unless compacting.</summary>
    /// <param name="key">The application key.</param>
    /// <param name="options">Writer options (compaction flag, key limit).</param>
    /// <returns>The caller's span, or a new compacted array.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The stored key would exceed <see cref="SnapshotWriterOptions.MaxKeyBytes"/>.</exception>
    public static ReadOnlySpan<byte> WireKey(ReadOnlySpan<byte> key, SnapshotWriterOptions options) =>
        Compacts(key.Length, options) ? KeyCompaction.Encode(key) : key;

    /// <summary>Validates a key length and decides whether the key gets compacted.</summary>
    /// <param name="length">Key length in bytes.</param>
    /// <param name="options">Writer options.</param>
    /// <returns><see langword="true"/> when the stored key is the compacted form.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">An uncompacted key exceeds the limit.</exception>
    private static bool Compacts(int length, SnapshotWriterOptions options)
    {
        if (length == 0)
        {
            throw new ArgumentException("A key must contain at least one byte.", "key");
        }

        if (options.CompactLongKeys && length > KeyCompaction.MaxRawKeyBytes)
        {
            return true;
        }

        if (length > options.MaxKeyBytes)
        {
            throw new ArgumentOutOfRangeException(
                "key", length, $"Keys are limited to {options.MaxKeyBytes} bytes (SnapshotWriterOptions.MaxKeyBytes).");
        }

        return false;
    }
}
