using System.Buffers;
using Google.Protobuf;
using Grpc.Core;
using KvAsm.Client.V1;
using Xunit;

namespace KvAsm.Client.Tests;

/// <summary>
/// The allocation-free streaming codecs (<see cref="StreamingCodec"/>) against
/// the generated protobuf messages: an <see cref="UploadFrame"/> must
/// serialize to exactly the bytes of the equivalent <see cref="UploadRequest"/>
/// (the server's decoder sees no difference), and <see cref="DownloadFrame"/>
/// must read every <see cref="DownloadResponse"/> the generated code writes —
/// split across buffer segments anywhere, with unknown fields — and refuse
/// malformed payloads instead of misreading them.
/// </summary>
public sealed class StreamingCodecTests
{
    /// <summary>Every message shape an upload sends serializes byte-identically to the generated message.</summary>
    /// <param name="keyLength">Key bytes (0 = not the first message).</param>
    /// <param name="totalSize">total_size (-1 = absent).</param>
    /// <param name="dataLength">Chunk bytes (0 = no data field).</param>
    /// <param name="withCrc">Whether the message carries crc32c (the last one).</param>
    [Theory]
    [InlineData(16, 1048576, 65536, false)]  // first of many
    [InlineData(0, -1, 65536, false)]        // middle
    [InlineData(0, -1, 1000, true)]          // last
    [InlineData(40, 1000, 1000, true)]       // the only message
    [InlineData(5, 0, 0, true)]              // the empty value: no data field
    [InlineData(300, 200, 200, true)]        // a key long enough for a 2-byte length varint
    [InlineData(0, -1, 3_000_000, false)]    // a 3-byte length varint
    public void UploadFrameSerializesLikeTheGeneratedMessage(int keyLength, long totalSize, int dataLength, bool withCrc)
    {
        var random = new Random(keyLength + dataLength);
        byte[] key = new byte[keyLength];
        byte[] data = new byte[dataLength];
        random.NextBytes(key);
        random.NextBytes(data);
        uint crc = (uint)random.Next();

        var frame = new UploadFrame();
        var expected = new UploadRequest();
        if (keyLength > 0)
        {
            frame.SetFirst(key, totalSize);
            expected.Key = ByteString.CopyFrom(key);
            expected.TotalSize = (ulong)totalSize;
        }

        frame.SetData(data);
        if (dataLength > 0)
        {
            expected.Data = ByteString.CopyFrom(data);
        }

        if (withCrc)
        {
            frame.SetCrc32C(crc);
            expected.Crc32C = crc;
        }

        var context = new CapturingSerializationContext();
        UploadFrame.Serialize(frame, context);
        Assert.True(context.Completed);
        Assert.Equal(context.DeclaredLength, context.Written.Length);
        Assert.Equal(expected.ToByteArray(), context.Written.ToArray());

        // Cleared, the same frame is an empty message (then refilled, a middle one).
        frame.Clear();
        Assert.Equal(0, frame.CalculateSize());
    }

    /// <summary>
    /// Random DownloadResponse messages, serialized by the generated code and
    /// cut into buffer segments at random points, parse to the same fields.
    /// </summary>
    [Fact]
    public void DownloadFrameParsesGeneratedMessagesAcrossSegments()
    {
        var random = new Random(5);
        for (int i = 0; i < 500; i++)
        {
            var message = new DownloadResponse();
            if (random.Next(2) == 0)
            {
                message.TotalSize = (ulong)random.NextInt64(0, long.MaxValue);
            }

            byte[] data = new byte[random.Next(3) == 0 ? 0 : random.Next(1, 70_000)];
            random.NextBytes(data);
            message.Data = ByteString.CopyFrom(data);
            if (random.Next(2) == 0)
            {
                message.Crc32C = (uint)random.Next();
            }

            byte[] wire = message.ToByteArray();
            DownloadFrame frame = DownloadFrame.Parse(new SequenceDeserializationContext(wire, random));
            try
            {
                Assert.Equal(message.HasTotalSize, frame.HasTotalSize);
                Assert.Equal(message.TotalSize, frame.TotalSize);
                Assert.Equal(message.HasCrc32C, frame.HasCrc32C);
                Assert.Equal(message.Crc32C, frame.Crc32C);
                Assert.True(frame.Data.Span.SequenceEqual(data), $"message {i}: data differs");
            }
            finally
            {
                frame.Release();
            }
        }
    }

    /// <summary>Unknown fields of every wire type are skipped, as protobuf parsers do (forward compatibility).</summary>
    [Fact]
    public void DownloadFrameSkipsUnknownFields()
    {
        byte[] known = new DownloadResponse { TotalSize = 7, Data = ByteString.CopyFromUtf8("abc"), Crc32C = 9 }.ToByteArray();
        byte[] unknown =
        [
            0x20, 0x96, 0x01,                               // field 4, varint 150
            0x29, 1, 2, 3, 4, 5, 6, 7, 8,                   // field 5, fixed64
            0x32, 0x02, 0xAA, 0xBB,                         // field 6, 2 bytes
            0x3D, 1, 2, 3, 4,                               // field 7, fixed32
        ];
        DownloadFrame frame = DownloadFrame.Parse(new SequenceDeserializationContext([.. unknown, .. known, .. unknown], new Random(1)));
        Assert.True(frame.HasTotalSize);
        Assert.Equal(7ul, frame.TotalSize);
        Assert.Equal("abc", System.Text.Encoding.ASCII.GetString(frame.Data.Span));
        Assert.Equal(9u, frame.Crc32C);
        frame.Release();
    }

    /// <summary>Malformed payloads throw (gRPC then fails the call) instead of yielding a wrong frame.</summary>
    /// <param name="hex">The payload.</param>
    [Theory]
    [InlineData("08")]                    // total_size varint missing
    [InlineData("08FFFFFFFFFFFFFFFFFFFF01")] // varint longer than 10 bytes
    [InlineData("1205616263")]            // data claims 5 bytes, 3 present
    [InlineData("1D0102")]                // crc32c truncated
    [InlineData("1B")]                    // wire type 3 (group start): unsupported
    [InlineData("2A05AABB")]              // unknown length-delimited field truncated
    [InlineData("2901020304")]            // unknown fixed64 truncated
    public void DownloadFrameRefusesMalformedPayloads(string hex)
    {
        Assert.Throws<InvalidDataException>(() => DownloadFrame.Parse(new SequenceDeserializationContext(Convert.FromHexString(hex), new Random(2))));
    }

    /// <summary>A released frame comes back from the pool empty: nothing of the previous message leaks into the next.</summary>
    [Fact]
    public void ReleasedFramesAreReusedEmpty()
    {
        byte[] full = new DownloadResponse { TotalSize = 5, Data = ByteString.CopyFromUtf8("hello"), Crc32C = 3 }.ToByteArray();
        byte[] bare = new DownloadResponse { Data = ByteString.CopyFromUtf8("x") }.ToByteArray();
        for (int i = 0; i < 100; i++)
        {
            DownloadFrame a = DownloadFrame.Parse(new SequenceDeserializationContext(full, new Random(i)));
            a.Release();
            DownloadFrame b = DownloadFrame.Parse(new SequenceDeserializationContext(bare, new Random(i)));
            Assert.False(b.HasTotalSize);
            Assert.False(b.HasCrc32C);
            Assert.Equal("x", System.Text.Encoding.ASCII.GetString(b.Data.Span));
            b.Release();
        }
    }

    /// <summary>A serialization context that records what a serializer wrote through the buffer-writer API.</summary>
    private sealed class CapturingSerializationContext : SerializationContext
    {
        private readonly ArrayBufferWriter<byte> _writer = new();

        /// <summary>The length passed to <see cref="SetPayloadLength"/>.</summary>
        public int DeclaredLength { get; private set; } = -1;

        /// <summary>True once <see cref="Complete()"/> was called.</summary>
        public bool Completed { get; private set; }

        /// <summary>The bytes written.</summary>
        public ReadOnlyMemory<byte> Written => _writer.WrittenMemory;

        /// <inheritdoc/>
        public override void SetPayloadLength(int payloadLength) => DeclaredLength = payloadLength;

        /// <inheritdoc/>
        public override IBufferWriter<byte> GetBufferWriter() => _writer;

        /// <inheritdoc/>
        public override void Complete() => Completed = true;

        /// <inheritdoc/>
        public override void Complete(byte[] payload) => throw new NotSupportedException("the codec must use the buffer writer");
    }

    /// <summary>A deserialization context whose payload is split into random segments.</summary>
    private sealed class SequenceDeserializationContext : DeserializationContext
    {
        private readonly ReadOnlySequence<byte> _payload;

        /// <summary>Cuts the payload into 1..5 segments at random points.</summary>
        /// <param name="payload">The message bytes.</param>
        /// <param name="random">Where the cuts fall.</param>
        public SequenceDeserializationContext(byte[] payload, Random random)
        {
            if (payload.Length == 0)
            {
                // A message with every field at its default is zero bytes long.
                _payload = ReadOnlySequence<byte>.Empty;
                return;
            }

            // A payload of n bytes has only n - 1 inner cut points: never ask
            // for more pieces than that (the loop below would never end).
            int pieces = Math.Min(random.Next(1, 6), Math.Max(payload.Length, 1));
            var cuts = new SortedSet<int> { 0, payload.Length };
            while (cuts.Count < pieces + 1 && payload.Length > 1)
            {
                cuts.Add(random.Next(1, payload.Length));
            }

            int[] bounds = [.. cuts];
            Segment first = new(payload.AsMemory(bounds[0], bounds[1] - bounds[0]), 0);
            Segment last = first;
            for (int i = 1; i + 1 < bounds.Length; i++)
            {
                last = last.Append(payload.AsMemory(bounds[i], bounds[i + 1] - bounds[i]));
            }

            _payload = new ReadOnlySequence<byte>(first, 0, last, last.Memory.Length);
        }

        /// <inheritdoc/>
        public override int PayloadLength => (int)_payload.Length;

        /// <inheritdoc/>
        public override byte[] PayloadAsNewBuffer() => _payload.ToArray();

        /// <inheritdoc/>
        public override ReadOnlySequence<byte> PayloadAsReadOnlySequence() => _payload;
    }

    /// <summary>A linked buffer segment for building multi-segment sequences.</summary>
    private sealed class Segment : ReadOnlySequenceSegment<byte>
    {
        /// <summary>Creates a segment.</summary>
        /// <param name="memory">Its bytes.</param>
        /// <param name="runningIndex">Bytes in the segments before it.</param>
        public Segment(ReadOnlyMemory<byte> memory, long runningIndex)
        {
            Memory = memory;
            RunningIndex = runningIndex;
        }

        /// <summary>Links a new segment after this one.</summary>
        /// <param name="memory">Its bytes.</param>
        /// <returns>The new segment.</returns>
        public Segment Append(ReadOnlyMemory<byte> memory)
        {
            var next = new Segment(memory, RunningIndex + Memory.Length);
            Next = next;
            return next;
        }
    }
}
