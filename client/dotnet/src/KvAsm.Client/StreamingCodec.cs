using System.Buffers;
using System.Buffers.Binary;
using System.Diagnostics;
using Google.Protobuf;
using Grpc.Core;
using KvAsm.Client.V1;

namespace KvAsm.Client;

/// <summary>
/// Allocation-free wire codecs for Upload and Download: the same
/// <c>kvasm.v1.KvAsm</c> methods and bytes on the wire as the generated
/// client, with custom marshallers so that moving a chunk allocates nothing.
/// </summary>
/// <remarks>
/// <para>
/// Why: the generated messages cost an <see cref="UploadRequest"/> and a
/// <see cref="ByteString"/> wrapper per upload chunk, and per download chunk
/// a <see cref="DownloadResponse"/> plus a fresh copy of the chunk
/// (<see cref="ByteString"/> owns its bytes) — a 1 MiB download allocated
/// ~1.07 MB, all garbage (measured with client/dotnet/bench). Here an upload
/// reuses one <see cref="UploadFrame"/> per call, serialized straight into
/// gRPC's pooled write buffer, and a download parses each message into a
/// pooled <see cref="DownloadFrame"/> whose data sits in an
/// <see cref="ArrayPool{T}"/> buffer until the caller releases it.
/// </para>
/// <para>
/// The method names match the service's, so channel settings keyed by name
/// (the retry policy for Download) still apply.
/// </para>
/// </remarks>
internal static class StreamingCodec
{
    /// <summary>Upload with the allocation-free request marshaller.</summary>
    public static readonly Method<UploadFrame, UploadResponse> UploadMethod = new(
        MethodType.ClientStreaming,
        KvAsmClient.ServiceName,
        "Upload",
        Marshallers.Create<UploadFrame>(UploadFrame.Serialize, static _ => throw new NotSupportedException("Upload requests are never received.")),
        Marshallers.Create<UploadResponse>(SerializeMessage, static context => UploadResponse.Parser.ParseFrom(context.PayloadAsReadOnlySequence())));

    /// <summary>Download with the allocation-free response marshaller.</summary>
    public static readonly Method<DownloadRequest, DownloadFrame> DownloadMethod = new(
        MethodType.ServerStreaming,
        KvAsmClient.ServiceName,
        "Download",
        Marshallers.Create<DownloadRequest>(SerializeMessage, static context => DownloadRequest.Parser.ParseFrom(context.PayloadAsReadOnlySequence())),
        Marshallers.Create<DownloadFrame>(static (_, _) => throw new NotSupportedException("Download responses are never sent."), DownloadFrame.Parse));

    /// <summary>Serializes a generated message into gRPC's buffer writer (no intermediate array).</summary>
    /// <typeparam name="T">The message type.</typeparam>
    /// <param name="message">The message.</param>
    /// <param name="context">gRPC's serialization context.</param>
    private static void SerializeMessage<T>(T message, SerializationContext context)
        where T : IMessage
    {
        context.SetPayloadLength(message.CalculateSize());
        message.WriteTo(context.GetBufferWriter());
        context.Complete();
    }

    /// <summary>Bytes of a protobuf varint.</summary>
    /// <param name="value">The value.</param>
    /// <returns>1..10.</returns>
    internal static int VarintLength(ulong value)
    {
        int n = 1;
        while (value >= 0x80)
        {
            value >>= 7;
            n++;
        }

        return n;
    }

    /// <summary>Writes a protobuf varint.</summary>
    /// <param name="destination">Room for <see cref="VarintLength"/> bytes.</param>
    /// <param name="value">The value.</param>
    /// <returns>Bytes written.</returns>
    internal static int WriteVarint(Span<byte> destination, ulong value)
    {
        int n = 0;
        while (value >= 0x80)
        {
            destination[n++] = (byte)(value | 0x80);
            value >>= 7;
        }

        destination[n++] = (byte)value;
        return n;
    }
}

/// <summary>
/// One UploadRequest message, reused for every message of a call: the key and
/// total size go into the first, a chunk into each, the CRC-32C into the last.
/// </summary>
/// <remarks>
/// Not thread-safe, and owned by one upload: gRPC serializes a message before
/// its <c>WriteAsync</c> completes, so the frame (and the chunk it points to)
/// may be refilled once that write was awaited — never earlier.
/// </remarks>
internal sealed class UploadFrame
{
    private const byte TagKey = 0x0A;        // 1: bytes
    private const byte TagTotalSize = 0x10;  // 2: varint
    private const byte TagData = 0x1A;       // 3: bytes
    private const byte TagCrc32C = 0x25;     // 4: fixed32

    private ReadOnlyMemory<byte> _key;
    private long _totalSize = -1;
    private ReadOnlyMemory<byte> _data;
    private uint _crc32C;
    private bool _hasCrc32C;

    /// <summary>Makes this the stream's first message: key and total size (sent once, before any data).</summary>
    /// <param name="key">The wire key (not copied: must stay valid until the write completes).</param>
    /// <param name="totalSize">The value's exact length.</param>
    public void SetFirst(ReadOnlyMemory<byte> key, long totalSize)
    {
        _key = key;
        _totalSize = totalSize;
    }

    /// <summary>The chunk this message carries (not copied; empty = no data field).</summary>
    /// <param name="data">The bytes.</param>
    public void SetData(ReadOnlyMemory<byte> data) => _data = data;

    /// <summary>Makes this the last message: it carries the value's CRC-32C.</summary>
    /// <param name="crc32C">The checksum of the whole value.</param>
    public void SetCrc32C(uint crc32C)
    {
        _crc32C = crc32C;
        _hasCrc32C = true;
    }

    /// <summary>Empties the frame for the next message (after the previous write completed).</summary>
    public void Clear()
    {
        _key = default;
        _totalSize = -1;
        _data = default;
        _hasCrc32C = false;
    }

    /// <summary>The serialized size of the message as it stands.</summary>
    /// <returns>Bytes of the protobuf encoding.</returns>
    internal int CalculateSize()
    {
        int size = 0;
        if (!_key.IsEmpty)
        {
            size += 1 + StreamingCodec.VarintLength((ulong)_key.Length) + _key.Length;
        }

        if (_totalSize >= 0)
        {
            size += 1 + StreamingCodec.VarintLength((ulong)_totalSize);
        }

        if (!_data.IsEmpty)
        {
            size += 1 + StreamingCodec.VarintLength((ulong)_data.Length) + _data.Length;
        }

        if (_hasCrc32C)
        {
            size += 5;
        }

        return size;
    }

    /// <summary>
    /// gRPC's serializer for a frame: the fields in field-number order
    /// (key, total_size, data, crc32c — the order the server requires within
    /// the first message) written directly into the call's buffer.
    /// </summary>
    /// <param name="frame">The frame.</param>
    /// <param name="context">gRPC's serialization context.</param>
    internal static void Serialize(UploadFrame frame, SerializationContext context)
    {
        int size = frame.CalculateSize();
        context.SetPayloadLength(size);
        IBufferWriter<byte> writer = context.GetBufferWriter();
        Span<byte> span = writer.GetSpan(size);
        int n = 0;
        if (!frame._key.IsEmpty)
        {
            span[n++] = TagKey;
            n += StreamingCodec.WriteVarint(span[n..], (ulong)frame._key.Length);
            frame._key.Span.CopyTo(span[n..]);
            n += frame._key.Length;
        }

        if (frame._totalSize >= 0)
        {
            span[n++] = TagTotalSize;
            n += StreamingCodec.WriteVarint(span[n..], (ulong)frame._totalSize);
        }

        if (!frame._data.IsEmpty)
        {
            span[n++] = TagData;
            n += StreamingCodec.WriteVarint(span[n..], (ulong)frame._data.Length);
            frame._data.Span.CopyTo(span[n..]);
            n += frame._data.Length;
        }

        if (frame._hasCrc32C)
        {
            span[n++] = TagCrc32C;
            BinaryPrimitives.WriteUInt32LittleEndian(span[n..], frame._crc32C);
            n += 4;
        }

        Debug.Assert(n == size, "CalculateSize and Serialize disagree");
        writer.Advance(size);
        context.Complete();
    }
}

/// <summary>
/// One DownloadResponse message, parsed without allocating: the data lives in
/// a buffer rented from <see cref="ArrayPool{T}.Shared"/>, and the frame
/// itself comes from a small pool. The consumer MUST call
/// <see cref="Release"/> once it is done with the frame (and its data).
/// </summary>
/// <remarks>
/// <para>
/// Parsing follows protobuf rules: fields in any order, unknown fields
/// skipped, a repeated scalar or bytes field replaced by the later one. The
/// Download contract itself (total_size first, crc32c last, ...) is checked by
/// the caller, as before.
/// </para>
/// <para>
/// A frame that is never released is simply collected (its buffer is lost to
/// the pool, which tolerates that); a released frame must not be touched
/// again — it may already carry another call's message.
/// </para>
/// </remarks>
internal sealed class DownloadFrame
{
    private const byte TagTotalSize = 0x08;  // 1: varint
    private const byte TagData = 0x12;       // 2: bytes
    private const byte TagCrc32C = 0x1D;     // 3: fixed32

    /// <summary>Free frames; a slot is null when empty (Interlocked exchanges keep it thread-safe).</summary>
    private static readonly DownloadFrame?[] s_pool = new DownloadFrame?[Math.Clamp(Environment.ProcessorCount * 2, 8, 64)];

    private byte[]? _buffer;
    private int _length;

    /// <summary>
    /// 1 while the frame is in the pool (or not yet handed out), 0 while a
    /// caller owns it: a second <see cref="Release"/> must fail loudly,
    /// because it would return the data buffer to the shared ArrayPool twice
    /// — and the pool, shared with the HTTP/2 stack, would then hand one
    /// array to two owners.
    /// </summary>
    private int _free = 1;

    /// <summary>True when the message carried total_size.</summary>
    public bool HasTotalSize { get; private set; }

    /// <summary>The value's total size (first message only).</summary>
    public ulong TotalSize { get; private set; }

    /// <summary>True when the message carried crc32c (it must be the last).</summary>
    public bool HasCrc32C { get; private set; }

    /// <summary>The range's CRC-32C (last message only).</summary>
    public uint Crc32C { get; private set; }

    /// <summary>The chunk (empty when the message had none); valid until <see cref="Release"/>.</summary>
    public ReadOnlyMemory<byte> Data => _buffer is null ? ReadOnlyMemory<byte>.Empty : _buffer.AsMemory(0, _length);

    /// <summary>
    /// gRPC's deserializer: parses one DownloadResponse from the payload,
    /// which is only valid during this call — hence the copy of the data into
    /// a pooled buffer.
    /// </summary>
    /// <param name="context">gRPC's deserialization context.</param>
    /// <returns>A frame from the pool, holding the message.</returns>
    /// <exception cref="InvalidDataException">The payload is not a well-formed protobuf message (gRPC reports the call as failed).</exception>
    internal static DownloadFrame Parse(DeserializationContext context)
    {
        var reader = new SequenceReader<byte>(context.PayloadAsReadOnlySequence());
        DownloadFrame frame = Rent();
        while (!reader.End)
        {
            ulong tag = ReadVarint(ref reader);
            switch (tag)
            {
                case TagTotalSize:
                    frame.TotalSize = ReadVarint(ref reader);
                    frame.HasTotalSize = true;
                    break;
                case TagData:
                    ulong length = ReadVarint(ref reader);
                    if (length > (ulong)reader.Remaining)
                    {
                        throw Malformed("a data field longer than its message");
                    }

                    frame.SetData(ref reader, (int)length);
                    break;
                case TagCrc32C:
                    if (!reader.TryReadLittleEndian(out int crc))
                    {
                        throw Malformed("a truncated crc32c");
                    }

                    frame.Crc32C = (uint)crc;
                    frame.HasCrc32C = true;
                    break;
                default:
                    Skip(ref reader, tag);
                    break;
            }
        }

        return frame;
    }

    /// <summary>
    /// Returns the data buffer to <see cref="ArrayPool{T}.Shared"/> and the
    /// frame to its pool. Call exactly once, after the last use of the frame.
    /// </summary>
    /// <exception cref="InvalidOperationException">The frame was already released (a caller bug that would corrupt the shared ArrayPool).</exception>
    public void Release()
    {
        if (Interlocked.Exchange(ref _free, 1) != 0)
        {
            throw new InvalidOperationException("DownloadFrame released twice.");
        }

        if (_buffer is not null)
        {
            ArrayPool<byte>.Shared.Return(_buffer);
            _buffer = null;
        }

        _length = 0;
        HasTotalSize = false;
        TotalSize = 0;
        HasCrc32C = false;
        Crc32C = 0;
        for (int i = 0; i < s_pool.Length; i++)
        {
            if (Interlocked.CompareExchange(ref s_pool[i], this, null) is null)
            {
                return;
            }
        }

        // Pool full: the frame is simply collected.
    }

    /// <summary>A free frame from the pool, or a new one when the pool is empty.</summary>
    /// <returns>An empty frame.</returns>
    private static DownloadFrame Rent()
    {
        DownloadFrame? frame = null;
        for (int i = 0; i < s_pool.Length && frame is null; i++)
        {
            frame = Interlocked.Exchange(ref s_pool[i], null);
        }

        frame ??= new DownloadFrame();
        frame._free = 0;                // owned by the caller until Release
        return frame;
    }

    /// <summary>Copies a data field out of the payload into a pooled buffer (replacing an earlier one).</summary>
    /// <param name="reader">Positioned at the field's bytes; advanced past them.</param>
    /// <param name="length">The field's length (already checked against what remains).</param>
    private void SetData(ref SequenceReader<byte> reader, int length)
    {
        if (_buffer is null || _buffer.Length < length)
        {
            if (_buffer is not null)
            {
                ArrayPool<byte>.Shared.Return(_buffer);
            }

            _buffer = length == 0 ? null : ArrayPool<byte>.Shared.Rent(length);
        }

        if (length > 0)
        {
            reader.TryCopyTo(_buffer.AsSpan(0, length));
            reader.Advance(length);
        }

        _length = length;
    }

    /// <summary>Reads a protobuf varint.</summary>
    /// <param name="reader">The payload reader, advanced past the varint.</param>
    /// <returns>The value.</returns>
    /// <exception cref="InvalidDataException">Truncated or longer than 10 bytes.</exception>
    private static ulong ReadVarint(ref SequenceReader<byte> reader)
    {
        ulong value = 0;
        for (int shift = 0; shift < 70; shift += 7)
        {
            if (!reader.TryRead(out byte b))
            {
                throw Malformed("a truncated varint");
            }

            value |= (ulong)(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
            {
                return value;
            }
        }

        throw Malformed("a varint longer than 10 bytes");
    }

    /// <summary>Skips an unknown field (forward compatibility, as protobuf parsers do).</summary>
    /// <param name="reader">Positioned after the tag.</param>
    /// <param name="tag">The field's tag (its low 3 bits are the wire type).</param>
    /// <exception cref="InvalidDataException">A truncated field or an unsupported wire type.</exception>
    private static void Skip(ref SequenceReader<byte> reader, ulong tag)
    {
        long skip;
        switch (tag & 7)
        {
            case 0:
                ReadVarint(ref reader);
                return;
            case 1:
                skip = 8;
                break;
            case 2:
                ulong length = ReadVarint(ref reader);
                skip = length > (ulong)reader.Remaining ? -1 : (long)length;
                break;
            case 5:
                skip = 4;
                break;
            default:
                throw Malformed($"wire type {tag & 7}");
        }

        if (skip < 0 || skip > reader.Remaining)
        {
            throw Malformed("a truncated field");
        }

        reader.Advance(skip);
    }

    /// <summary>The exception for a malformed payload.</summary>
    /// <param name="what">What was wrong.</param>
    /// <returns>The exception to throw.</returns>
    private static InvalidDataException Malformed(string what) => new($"Malformed DownloadResponse: {what}.");
}
