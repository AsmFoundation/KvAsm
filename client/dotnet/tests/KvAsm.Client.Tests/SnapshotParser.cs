using System.Buffers.Binary;

namespace KvAsm.Client.Tests;

/// <summary>
/// Test-side snapshot reader applying the server loader's rules
/// (server/src/snapshot.asm) in the same order: size, magic, version/flags,
/// trailer, checksum — all before any record is parsed — then bounds-checked
/// records that must consume the data exactly.
/// </summary>
/// <remarks>
/// Deliberately written without the production <see cref="SnapshotFormat"/>
/// helpers, so a layout bug in the writer cannot hide behind the same bug in
/// the reader.
/// </remarks>
internal static class SnapshotParser
{
    /// <summary>Parses and fully validates a snapshot image.</summary>
    /// <param name="file">The complete file contents.</param>
    /// <returns>The records in file order (duplicates preserved).</returns>
    /// <exception cref="InvalidDataException">Any rule is violated; the message names the rule.</exception>
    public static List<(byte[] Key, byte[] Value)> Parse(ReadOnlySpan<byte> file)
    {
        if (file.Length < 32 + 16)
        {
            throw new InvalidDataException("file too small");
        }

        if (!file[..8].SequenceEqual("KVASNAP1"u8))
        {
            throw new InvalidDataException("bad header magic");
        }

        if (BinaryPrimitives.ReadUInt32LittleEndian(file[8..]) != 1 || BinaryPrimitives.ReadUInt32LittleEndian(file[12..]) != 0)
        {
            throw new InvalidDataException("unsupported version or flags");
        }

        ReadOnlySpan<byte> trailer = file[^16..];
        if (!trailer[..8].SequenceEqual("KVASEND1"u8))
        {
            throw new InvalidDataException("bad trailer magic");
        }

        ReadOnlySpan<byte> body = file[..^16];
        if (SipHash128.Hash(body).Low != BinaryPrimitives.ReadUInt64LittleEndian(trailer[8..]))
        {
            throw new InvalidDataException("checksum mismatch");
        }

        long count = BinaryPrimitives.ReadInt64LittleEndian(file[16..]);
        var records = new List<(byte[], byte[])>();
        int offset = 32;
        for (long i = 0; i < count; i++)
        {
            if (body.Length - offset < 8)
            {
                throw new InvalidDataException($"record {i} truncated");
            }

            uint keyLength = BinaryPrimitives.ReadUInt32LittleEndian(body[offset..]);
            uint valueLength = BinaryPrimitives.ReadUInt32LittleEndian(body[(offset + 4)..]);
            offset += 8;
            if (keyLength == 0 || (ulong)keyLength + valueLength > (ulong)(body.Length - offset))
            {
                throw new InvalidDataException($"record {i} truncated or has an empty key");
            }

            records.Add((body.Slice(offset, (int)keyLength).ToArray(), body.Slice(offset + (int)keyLength, (int)valueLength).ToArray()));
            offset += (int)(keyLength + valueLength);
        }

        if (offset != body.Length)
        {
            throw new InvalidDataException("unexpected bytes after the last record");
        }

        return records;
    }
}
