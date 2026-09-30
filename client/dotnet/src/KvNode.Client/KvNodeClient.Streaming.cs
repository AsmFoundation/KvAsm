using System.Buffers;
using Google.Protobuf;
using Grpc.Core;
using KvNode.Client.V1;

namespace KvNode.Client;

/// <content>
/// Upload (client streaming) and Download (server streaming): values of any
/// size up to <see cref="KvNodeClientOptions.MaxUploadBytes"/>, and byte
/// ranges of them, moved in chunks with CRC-32C verification at both ends.
/// </content>
public sealed partial class KvNodeClient
{
    /// <summary>
    /// Stores a value held in memory under a key through a streaming upload,
    /// replacing any previous value atomically.
    /// </summary>
    /// <param name="key">The key (1+ bytes; long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="value">
    /// The value (may be empty). Sent without copying, chunk by chunk: do not
    /// modify it until the returned task completes.
    /// </param>
    /// <param name="cancellationToken">
    /// Cancels the upload. Nothing is stored unless the upload completed: the
    /// server publishes the value only after its last byte and checksum.
    /// </param>
    /// <returns>Whether the key was created, the stored length and its verified CRC-32C.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">
    /// The key exceeds <see cref="KvNodeClientOptions.MaxKeyBytes"/>, or the value
    /// exceeds <see cref="KvNodeClientOptions.MaxUploadBytes"/>.
    /// </exception>
    /// <exception cref="RpcException">
    /// The server refused the upload or the transport failed — nothing was
    /// stored then. <c>RESOURCE_EXHAUSTED</c>: memory or key capacity, or this
    /// connection's uploads in progress exceed the server's
    /// <c>KVNODE_CONN_BUFFER_MB</c>; <c>DATA_LOSS</c>: the bytes did not arrive
    /// intact; <c>DEADLINE_EXCEEDED</c>: <see cref="KvNodeClientOptions.TransferTimeout"/>
    /// or <see cref="KvNodeClientOptions.TransferIdleTimeout"/> ran out;
    /// <c>CANCELLED</c>: <paramref name="cancellationToken"/>.
    /// </exception>
    /// <remarks>
    /// <para>
    /// Unlike <see cref="PutAsync(ReadOnlyMemory{byte}, ReadOnlyMemory{byte}, CancellationToken)"/>,
    /// the value is never one message: it is sent in
    /// <see cref="KvNodeClientOptions.UploadChunkBytes"/> chunks, with its
    /// CRC-32C, and the server copies each chunk straight into the record it
    /// will publish. Readers see the old value until the upload completes,
    /// then the new one — never a part.
    /// </para>
    /// <para>Never retried automatically (like Put): see <see cref="KvNodeClientOptions.ReadAttempts"/>.</para>
    /// </remarks>
    public Task<UploadResult> UploadAsync(ReadOnlyMemory<byte> key, ReadOnlyMemory<byte> value, CancellationToken cancellationToken = default)
    {
        ByteString wireKey = WireKey(key);
        CheckUploadLength(value.Length, nameof(value));
        return UploadCoreAsync(wireKey, value.Length, new MemoryChunks(value), cancellationToken);
    }

    /// <summary>
    /// Stores exactly <paramref name="length"/> bytes read from a stream under a
    /// key through a streaming upload, replacing any previous value atomically.
    /// </summary>
    /// <param name="key">The key (1+ bytes; long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="source">
    /// Where the value is read from, from its current position; not disposed.
    /// Read one chunk at a time, so memory stays one chunk whatever the size.
    /// </param>
    /// <param name="length">The value's exact length: the server reserves it up front and needs every byte.</param>
    /// <param name="cancellationToken">Cancels the upload; nothing is stored then.</param>
    /// <returns>Whether the key was created, the stored length and its verified CRC-32C.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="source"/> is null.</exception>
    /// <exception cref="ArgumentException">The key is empty, or <paramref name="source"/> is not readable.</exception>
    /// <exception cref="ArgumentOutOfRangeException">
    /// The key is too long, or <paramref name="length"/> is negative or above
    /// <see cref="KvNodeClientOptions.MaxUploadBytes"/>.
    /// </exception>
    /// <exception cref="EndOfStreamException">
    /// <paramref name="source"/> ended before <paramref name="length"/> bytes;
    /// the upload is cancelled and nothing is stored.
    /// </exception>
    /// <exception cref="RpcException">The server refused the upload or the transport failed (see the memory overload).</exception>
    /// <remarks>
    /// Exceptions thrown by <paramref name="source"/> itself propagate as they
    /// are, after the upload is cancelled. Bytes after the first
    /// <paramref name="length"/> are left unread.
    /// </remarks>
    public Task<UploadResult> UploadAsync(ReadOnlyMemory<byte> key, Stream source, long length, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(source);
        ByteString wireKey = WireKey(key);
        if (!source.CanRead)
        {
            throw new ArgumentException("The stream must be readable.", nameof(source));
        }

        CheckUploadLength(length, nameof(length));
        return UploadCoreAsync(wireKey, length, new StreamChunks(source), cancellationToken);
    }

    /// <summary>
    /// Stores the rest of a seekable stream (from its position to its end)
    /// under a key through a streaming upload.
    /// </summary>
    /// <param name="key">The key (1+ bytes; long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="source">A readable, seekable stream; not disposed.</param>
    /// <param name="cancellationToken">Cancels the upload; nothing is stored then.</param>
    /// <returns>Whether the key was created, the stored length and its verified CRC-32C.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="source"/> is null.</exception>
    /// <exception cref="ArgumentException">
    /// The key is empty, or <paramref name="source"/> cannot seek (pass the
    /// length explicitly then) or is not readable.
    /// </exception>
    /// <exception cref="ArgumentOutOfRangeException">The key is too long, or the value above <see cref="KvNodeClientOptions.MaxUploadBytes"/>.</exception>
    /// <exception cref="EndOfStreamException">The stream shrank while being read; nothing is stored.</exception>
    /// <exception cref="RpcException">The server refused the upload or the transport failed.</exception>
    public Task<UploadResult> UploadAsync(ReadOnlyMemory<byte> key, Stream source, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (!source.CanSeek)
        {
            throw new ArgumentException(
                "The stream cannot report its length: pass it explicitly (UploadAsync(key, source, length, ...)).", nameof(source));
        }

        return UploadAsync(key, source, source.Length - source.Position, cancellationToken);
    }

    /// <summary>
    /// Reads a whole value, or a byte range of it, into memory through a
    /// streaming download.
    /// </summary>
    /// <param name="key">The key (long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="offset">First byte to read (0 = the start).</param>
    /// <param name="length">Bytes to read from <paramref name="offset"/>; null = to the end. Cut at the end of the value.</param>
    /// <param name="cancellationToken">Cancels the download.</param>
    /// <returns>
    /// The bytes (possibly empty), or <see langword="null"/> when the key does
    /// not exist. One array of exactly the range's size, allocated once the
    /// server has announced the value's size.
    /// </returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">
    /// The key is too long, or <paramref name="offset"/> / <paramref name="length"/> is negative.
    /// </exception>
    /// <exception cref="RpcException">
    /// <c>OUT_OF_RANGE</c>: <paramref name="offset"/> is past the end of the
    /// value; <c>RESOURCE_EXHAUSTED</c>: the range is larger than
    /// <see cref="KvNodeClientOptions.MaxUploadBytes"/> (read it with
    /// <see cref="DownloadToAsync"/> into a stream); <c>DATA_LOSS</c>: the
    /// bytes failed the CRC-32C check; <c>INTERNAL</c>: the response broke the
    /// protocol; <c>DEADLINE_EXCEEDED</c>: a transfer timeout ran out; other
    /// codes from the server or transport.
    /// </exception>
    /// <remarks>
    /// One download reads one version of the value (see <see cref="DownloadResult"/>).
    /// Retried on <c>UNAVAILABLE</c> only until the first chunk arrives
    /// (<see cref="KvNodeClientOptions.ReadAttempts"/>).
    /// </remarks>
    public async Task<byte[]?> DownloadAsync(ReadOnlyMemory<byte> key, long offset = 0, long? length = null, CancellationToken cancellationToken = default)
    {
        ByteString wireKey = WireKey(key);
        CheckRange(offset, length);
        var sink = new ArraySink(_options.MaxUploadBytes);
        DownloadResult? result = await DownloadCoreAsync(wireKey, offset, length, sink, cancellationToken).ConfigureAwait(false);
        return result is null ? null : sink.Array;
    }

    /// <summary>
    /// Writes a whole value, or a byte range of it, to a stream through a
    /// streaming download.
    /// </summary>
    /// <param name="key">The key (long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="destination">Where the bytes are written, chunk by chunk as they arrive; not disposed or flushed.</param>
    /// <param name="offset">First byte to read (0 = the start).</param>
    /// <param name="length">Bytes to read from <paramref name="offset"/>; null = to the end. Cut at the end of the value.</param>
    /// <param name="cancellationToken">Cancels the download.</param>
    /// <returns>
    /// The value's total size, the range read and its verified CRC-32C; or
    /// <see langword="null"/> when the key does not exist (nothing was written).
    /// </returns>
    /// <exception cref="ArgumentNullException"><paramref name="destination"/> is null.</exception>
    /// <exception cref="ArgumentException">The key is empty, or <paramref name="destination"/> is not writable.</exception>
    /// <exception cref="ArgumentOutOfRangeException">
    /// The key is too long, or <paramref name="offset"/> / <paramref name="length"/> is negative.
    /// </exception>
    /// <exception cref="RpcException">
    /// As <see cref="DownloadAsync"/> (without the in-memory size limit). After
    /// a failure <paramref name="destination"/> may hold part of the range:
    /// only a returned result vouches for the bytes.
    /// </exception>
    /// <remarks>
    /// Memory stays one chunk whatever the value's size: the server sends as
    /// the client's HTTP/2 flow control allows, so a slow
    /// <paramref name="destination"/> slows the transfer instead of growing
    /// buffers (and a destination that stops for longer than
    /// <see cref="KvNodeClientOptions.TransferIdleTimeout"/> ends it).
    /// Exceptions thrown by <paramref name="destination"/> propagate as they
    /// are, after the download is cancelled. A transfer broken mid-way is not
    /// resumed automatically; resuming with <paramref name="offset"/> is safe
    /// only when the application knows the value did not change meanwhile.
    /// </remarks>
    public async Task<DownloadResult?> DownloadToAsync(
        ReadOnlyMemory<byte> key, Stream destination, long offset = 0, long? length = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(destination);
        ByteString wireKey = WireKey(key);
        if (!destination.CanWrite)
        {
            throw new ArgumentException("The stream must be writable.", nameof(destination));
        }

        CheckRange(offset, length);
        return await DownloadCoreAsync(wireKey, offset, length, new StreamSink(destination), cancellationToken).ConfigureAwait(false);
    }

    /// <summary>The upload loop shared by every overload.</summary>
    /// <param name="wireKey">The validated wire key.</param>
    /// <param name="length">The value's exact length (validated).</param>
    /// <param name="chunks">Where the value's bytes come from.</param>
    /// <param name="cancellationToken">The caller's token.</param>
    /// <returns>The verified result.</returns>
    /// <exception cref="RpcException">The server refused, the transport failed, or a transfer timeout ran out.</exception>
    /// <exception cref="EndOfStreamException">The source ended early.</exception>
    /// <remarks>
    /// <para>
    /// The first message carries key, total_size and the first chunk (one
    /// message fewer); the last carries the CRC-32C, so the server verifies
    /// before it stores anything. Disposing the call on any failure resets the
    /// stream (RST_STREAM), and the server then discards the partial record.
    /// </para>
    /// <para>
    /// Allocation-free per chunk: one <see cref="UploadFrame"/> is refilled
    /// for every message and serialized straight into gRPC's write buffer
    /// (<see cref="StreamingCodec.UploadMethod"/>) — the chunk is copied once,
    /// by that serialization, and nothing else is created per message.
    /// </para>
    /// </remarks>
    private async Task<UploadResult> UploadCoreAsync(ByteString wireKey, long length, IChunkSource chunks, CancellationToken cancellationToken)
    {
        using var watch = new TransferWatch(_options.TransferIdleTimeout, cancellationToken);
        try
        {
            // The whole transfer stays on the lane (connection) it starts on.
            using AsyncClientStreamingCall<UploadFrame, UploadResponse> call = NextLane().Invoker.AsyncClientStreamingCall(
                StreamingCodec.UploadMethod, host: null, new CallOptions(deadline: TransferDeadline(), cancellationToken: watch.Token));
            uint crc = 0;
            long sent = 0;
            var frame = new UploadFrame();
            frame.SetFirst(wireKey.Memory, length);
            try
            {
                do
                {
                    // The first message stays small (FirstChunkBytes): the
                    // server decides every early refusal from its key and
                    // total_size, and a first message larger than the
                    // stream's flow-control window would leave the write
                    // waiting for credit the refusing server never grants.
                    int limit = sent == 0 ? Math.Min(_options.UploadChunkBytes, FirstChunkBytes) : _options.UploadChunkBytes;
                    int n = (int)Math.Min(limit, length - sent);
                    ReadOnlyMemory<byte> chunk = n == 0
                        ? ReadOnlyMemory<byte>.Empty
                        : await chunks.NextAsync(n, watch.Token).ConfigureAwait(false);
                    crc = Crc32C.Append(crc, chunk.Span);
                    sent += n;

                    // No copy: gRPC serializes the frame before WriteAsync
                    // completes, so the frame — and a pooled chunk — may be
                    // refilled right after the await.
                    frame.SetData(chunk);
                    if (sent == length)
                    {
                        frame.SetCrc32C(crc);
                    }

                    await SendAsync(call, frame, watch).ConfigureAwait(false);
                    frame.Clear();
                    watch.Progress();
                }
                while (sent < length);

                try
                {
                    await call.RequestStream.CompleteAsync().ConfigureAwait(false);
                }
                catch (Exception e) when (e is not OperationCanceledException)
                {
                    throw await UploadFailureAsync(call, e).ConfigureAwait(false);
                }
            }
            finally
            {
                chunks.Release();
            }

            UploadResponse response = await call.ResponseAsync.ConfigureAwait(false);
            if (response.Crc32C != crc)
            {
                // The server verified the checksum this client sent before it
                // stored anything, so disagreeing now means the reply itself is
                // not what the server meant: never report such an upload as fine.
                throw new RpcException(new Status(
                    StatusCode.DataLoss, $"The server reports crc32c 0x{response.Crc32C:x8} for an upload whose crc32c is 0x{crc:x8}."));
            }

            return new UploadResult(response.Created, length, response.Crc32C);
        }
        catch (Exception e) when (watch.Stalled && e is RpcException or OperationCanceledException)
        {
            throw watch.StallError();
        }
        catch (OperationCanceledException e) when (cancellationToken.IsCancellationRequested)
        {
            throw Cancelled(e);
        }
    }

    /// <summary>
    /// Data bytes of an upload's first message at most: small enough for any
    /// server's initial stream window (KvNode's is 1 MiB), so the message
    /// that carries key and total_size — the one every early refusal is
    /// decided on — never waits for flow-control credit.
    /// </summary>
    private const int FirstChunkBytes = 64 * 1024;

    /// <summary>Sends one upload message; a failed write surfaces as the call's real status.</summary>
    /// <param name="call">The upload call.</param>
    /// <param name="message">The message (serialized before the returned task completes).</param>
    /// <param name="watch">The transfer's watch (its token cancels the write).</param>
    /// <returns>A task that completes when gRPC accepted the message.</returns>
    /// <exception cref="RpcException">The server's refusal, or the transport's failure.</exception>
    /// <exception cref="OperationCanceledException">The watch's token was cancelled (translated by the caller).</exception>
    /// <remarks>
    /// A write that waits for HTTP/2 flow-control credit is raced against the
    /// call's response. A server that answers early (a refusal) sends its
    /// status and RST_STREAM(NO_ERROR) and never credits the stream again —
    /// and a .NET write already waiting for that credit was observed to stay
    /// parked even after the reset and after its token was cancelled
    /// (2026-09-30, with 1 MiB messages: a hang, not an error). Once the
    /// response is in, the answer is known: the write is abandoned (the
    /// call's disposal ends it; its eventual fault is observed so it is never
    /// reported as unobserved) and the server's status is thrown. Only a
    /// pending write pays for the race (one small task).
    /// </remarks>
    private static async Task SendAsync(AsyncClientStreamingCall<UploadFrame, UploadResponse> call, UploadFrame message, TransferWatch watch)
    {
        try
        {
            Task write = call.RequestStream.WriteAsync(message, watch.Token);
            if (!write.IsCompleted)
            {
                Task response = call.ResponseAsync;
                if (await Task.WhenAny(write, response).ConfigureAwait(false) == response && !write.IsCompleted)
                {
                    // The server answered while the write still waits: stop
                    // waiting. The abandoned write ends with the call; observe
                    // its fault so it is never reported as unobserved.
                    _ = write.ContinueWith(
                        static t => _ = t.Exception, CancellationToken.None,
                        TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);
                    throw new IOException("The server answered the upload before this message could be sent.");
                }
            }

            await write.ConfigureAwait(false);
        }
        catch (Exception e) when (e is not OperationCanceledException)
        {
            // The response is complete (or completes within ResponseGrace):
            // UploadFailureAsync returns the server's own status.
            throw await UploadFailureAsync(call, e).ConfigureAwait(false);
        }
    }

    /// <summary>How long a failed write waits for the call's own status before reporting the write's error.</summary>
    private static readonly TimeSpan ResponseGrace = TimeSpan.FromSeconds(2);

    /// <summary>The exception to surface when writing to an upload (or completing it) failed.</summary>
    /// <param name="call">The upload call.</param>
    /// <param name="writeError">What the write threw.</param>
    /// <returns>
    /// The call's own <see cref="RpcException"/> when it has one — the server's
    /// refusal, or grpc's report of the broken connection — else the write's
    /// failure as <c>UNAVAILABLE</c>.
    /// </returns>
    /// <remarks>
    /// <para>
    /// Two failures look alike from the write side. The server answers a
    /// refusal at once and resets the rest of the request (RST_STREAM
    /// NO_ERROR): the next write then fails with a generic "call is complete",
    /// while the real answer — RESOURCE_EXHAUSTED, "value too large", ... — is
    /// in the completed response. And a connection that breaks mid-upload
    /// makes the HTTP/2 layer throw a raw <see cref="IOException"/> ("The
    /// request was aborted"), which must not leak: every transport failure of
    /// this client is an <see cref="RpcException"/>.
    /// </para>
    /// <para>
    /// Either way the response completes promptly once the write failed, so
    /// it is awaited for a bounded moment (<see cref="ResponseGrace"/>) — never
    /// indefinitely, whatever the transport does.
    /// </para>
    /// </remarks>
    private static async Task<Exception> UploadFailureAsync(AsyncClientStreamingCall<UploadFrame, UploadResponse> call, Exception writeError)
    {
        Task<UploadResponse> response = call.ResponseAsync;
        try
        {
            await response.WaitAsync(ResponseGrace).ConfigureAwait(false);
        }
        catch (RpcException status)
        {
            return status;
        }
        catch (TimeoutException)
        {
            // no answer in time: report the write's own failure below
        }

        return writeError as RpcException ?? new RpcException(
            new Status(StatusCode.Unavailable, $"The upload's connection failed: {writeError.Message}", writeError));
    }

    /// <summary>The exception a transfer cancelled by its caller surfaces, as unary calls do.</summary>
    /// <param name="cause">The cancellation observed (kept as the status's debug exception).</param>
    /// <returns><c>CANCELLED</c>, like grpc's own cancelled calls.</returns>
    /// <remarks>
    /// Cancellation can land in grpc (which reports <c>CANCELLED</c>) or in the
    /// caller's own source or destination stream (which throws
    /// <see cref="OperationCanceledException"/>); one outcome for both keeps
    /// the contract of <see cref="GetAsync(ReadOnlyMemory{byte}, CancellationToken)"/>.
    /// </remarks>
    private static RpcException Cancelled(OperationCanceledException cause) =>
        new(new Status(StatusCode.Cancelled, "Call canceled by the client.", cause));

    /// <summary>The download loop shared by every overload: validates every message against the contract.</summary>
    /// <param name="wireKey">The validated wire key.</param>
    /// <param name="offset">Range start (validated, non-negative).</param>
    /// <param name="length">Range length, or null for "to the end".</param>
    /// <param name="sink">Where the bytes go.</param>
    /// <param name="cancellationToken">The caller's token.</param>
    /// <returns>The verified result, or null when the key does not exist.</returns>
    /// <exception cref="RpcException">A server, transport, protocol (INTERNAL) or checksum (DATA_LOSS) failure, or a timeout.</exception>
    /// <remarks>
    /// Allocation-free per chunk: each message is parsed into a pooled
    /// <see cref="DownloadFrame"/> whose data sits in an <see cref="ArrayPool{T}"/>
    /// buffer (<see cref="StreamingCodec.DownloadMethod"/>); the frame goes
    /// back to its pool as soon as its bytes are checksummed and written.
    /// </remarks>
    private async Task<DownloadResult?> DownloadCoreAsync(
        ByteString wireKey, long offset, long? length, IDownloadSink sink, CancellationToken cancellationToken)
    {
        var request = new DownloadRequest { Key = wireKey, Offset = (ulong)offset, ChunkSize = (uint)_options.DownloadChunkBytes };
        if (length is { } wanted)
        {
            request.Length = (ulong)wanted;
        }

        using var watch = new TransferWatch(_options.TransferIdleTimeout, cancellationToken);
        long total = -1, expected = 0, received = 0;
        uint crc = 0;
        bool complete = false;
        try
        {
            // The whole transfer stays on the lane (connection) it starts on.
            using AsyncServerStreamingCall<DownloadFrame> call = NextLane().Invoker.AsyncServerStreamingCall(
                StreamingCodec.DownloadMethod, host: null, new CallOptions(deadline: TransferDeadline(), cancellationToken: watch.Token), request);
            IAsyncStreamReader<DownloadFrame> reader = call.ResponseStream;
            while (await reader.MoveNext(watch.Token).ConfigureAwait(false))
            {
                watch.Progress();
                DownloadFrame message = reader.Current;
                try
                {
                    if (complete)
                    {
                        throw Violation("a message followed the one that carried crc32c");
                    }

                    if (total < 0)
                    {
                        // The first message: learn the value's size, fix the range.
                        if (!message.HasTotalSize || message.TotalSize > long.MaxValue)
                        {
                            throw Violation("the first message lacks a valid total_size");
                        }

                        total = (long)message.TotalSize;
                        if (offset > total)
                        {
                            throw Violation("a range past the end of the value was not refused");
                        }

                        expected = length is { } l && l < total - offset ? l : total - offset;
                        sink.Start(expected);
                    }
                    else if (message.HasTotalSize)
                    {
                        throw Violation("total_size arrived after the first message");
                    }

                    ReadOnlyMemory<byte> data = message.Data;
                    if (!data.IsEmpty)
                    {
                        if (data.Length > expected - received)
                        {
                            throw Violation("more data than the requested range");
                        }

                        crc = Crc32C.Append(crc, data.Span);
                        await sink.WriteAsync(data, watch.Token).ConfigureAwait(false);
                        received += data.Length;
                    }

                    if (message.HasCrc32C)
                    {
                        if (received != expected)
                        {
                            throw Violation($"crc32c arrived after {received} of {expected} bytes");
                        }

                        if (message.Crc32C != crc)
                        {
                            throw new RpcException(new Status(
                                StatusCode.DataLoss,
                                $"crc32c mismatch: the server sent 0x{message.Crc32C:x8}, the bytes received give 0x{crc:x8}."));
                        }

                        complete = true;
                    }
                }
                finally
                {
                    // Back to the pools whatever happened: the sink has taken
                    // (or refused) the bytes, and nothing below reads the frame.
                    message.Release();
                }
            }
        }
        catch (RpcException e) when (e.StatusCode == StatusCode.NotFound && total < 0)
        {
            return null;                    // refused before any byte: the key does not exist
        }
        catch (Exception e) when (watch.Stalled && e is RpcException or OperationCanceledException)
        {
            throw watch.StallError();
        }
        catch (OperationCanceledException e) when (cancellationToken.IsCancellationRequested)
        {
            throw Cancelled(e);             // (from the caller's destination stream, say)
        }

        if (!complete)
        {
            throw Violation(total < 0 ? "the call ended without any message" : "the stream ended without crc32c");
        }

        return new DownloadResult(total, offset, received, crc);
    }

    /// <summary>A Download response that breaks the contract (docs/PROTOCOL.md).</summary>
    /// <param name="what">What was wrong.</param>
    /// <returns>The exception to throw: <c>INTERNAL</c>, as gRPC reports protocol violations.</returns>
    private static RpcException Violation(string what) =>
        new(new Status(StatusCode.Internal, $"Download protocol violation: {what}."));

    /// <summary>Validates an upload length against <see cref="KvNodeClientOptions.MaxUploadBytes"/>.</summary>
    /// <param name="length">The value's length.</param>
    /// <param name="paramName">Parameter name for the exception.</param>
    /// <exception cref="ArgumentOutOfRangeException">Negative, or above the limit.</exception>
    private void CheckUploadLength(long length, string paramName)
    {
        if (length < 0 || length > _options.MaxUploadBytes)
        {
            throw new ArgumentOutOfRangeException(
                paramName, length, $"Uploads are limited to 0..{_options.MaxUploadBytes} bytes (KvNodeClientOptions.MaxUploadBytes).");
        }
    }

    /// <summary>Validates a download range before any traffic.</summary>
    /// <param name="offset">Range start.</param>
    /// <param name="length">Range length or null.</param>
    /// <exception cref="ArgumentOutOfRangeException">A negative offset or length.</exception>
    private static void CheckRange(long offset, long? length)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        if (length is < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(length), length, "Must not be negative.");
        }
    }

    /// <summary>The absolute deadline for a transfer starting now, or null for none.</summary>
    /// <returns>UTC deadline, or <see langword="null"/> when <see cref="KvNodeClientOptions.TransferTimeout"/> is infinite.</returns>
    private DateTime? TransferDeadline() =>
        _options.TransferTimeout == System.Threading.Timeout.InfiniteTimeSpan ? null : DateTime.UtcNow + _options.TransferTimeout;

    /// <summary>
    /// A transfer's cancellation: the caller's token, plus an idle timer that
    /// fires when no chunk moved for <see cref="KvNodeClientOptions.TransferIdleTimeout"/>.
    /// </summary>
    /// <remarks>
    /// Each <see cref="Progress"/> re-arms the timer (<see cref="CancellationTokenSource.CancelAfter(TimeSpan)"/>
    /// reschedules), so a long transfer that keeps moving never fires it. When
    /// it fires, the gRPC call is cancelled (RST_STREAM: the server releases
    /// the call's reservation or pin) and <see cref="StallError"/> replaces the
    /// generic CANCELLED the caller would otherwise see.
    /// </remarks>
    private sealed class TransferWatch : IDisposable
    {
        private readonly CancellationTokenSource _cts;
        private readonly CancellationToken _caller;
        private readonly TimeSpan _idle;

        /// <summary>Starts the watch; the idle timer runs from now.</summary>
        /// <param name="idle">Idle limit, or <see cref="Timeout.InfiniteTimeSpan"/>.</param>
        /// <param name="caller">The caller's token.</param>
        public TransferWatch(TimeSpan idle, CancellationToken caller)
        {
            _caller = caller;
            _idle = idle;
            _cts = CancellationTokenSource.CreateLinkedTokenSource(caller);
            Progress();
        }

        /// <summary>The token every step of the transfer observes.</summary>
        public CancellationToken Token => _cts.Token;

        /// <summary>True when the idle timer — not the caller — cancelled the transfer.</summary>
        public bool Stalled => _cts.IsCancellationRequested && !_caller.IsCancellationRequested;

        /// <summary>Records progress: the idle timer starts over.</summary>
        public void Progress()
        {
            if (_idle != System.Threading.Timeout.InfiniteTimeSpan)
            {
                _cts.CancelAfter(_idle);
            }
        }

        /// <summary>The error a stalled transfer surfaces.</summary>
        /// <returns><c>DEADLINE_EXCEEDED</c> naming the setting that fired.</returns>
        public RpcException StallError() => new(new Status(
            StatusCode.DeadlineExceeded,
            $"The transfer made no progress for {_idle.TotalSeconds:0.###} s (KvNodeClientOptions.TransferIdleTimeout)."));

        /// <summary>Stops the timer.</summary>
        public void Dispose() => _cts.Dispose();
    }

    /// <summary>Where an upload's chunks come from.</summary>
    private interface IChunkSource
    {
        /// <summary>The next <paramref name="count"/> bytes of the value.</summary>
        /// <param name="count">Bytes wanted (1+; never more than remain).</param>
        /// <param name="cancellationToken">Cancels a read.</param>
        /// <returns>Exactly <paramref name="count"/> bytes, valid until the next call.</returns>
        /// <exception cref="EndOfStreamException">The source ended early.</exception>
        ValueTask<ReadOnlyMemory<byte>> NextAsync(int count, CancellationToken cancellationToken);

        /// <summary>Returns any pooled buffer.</summary>
        void Release();
    }

    /// <summary>Chunks sliced out of a value in memory: no copy at all.</summary>
    /// <param name="value">The whole value.</param>
    private sealed class MemoryChunks(ReadOnlyMemory<byte> value) : IChunkSource
    {
        private int _offset;

        /// <inheritdoc/>
        public ValueTask<ReadOnlyMemory<byte>> NextAsync(int count, CancellationToken cancellationToken)
        {
            ReadOnlyMemory<byte> chunk = value.Slice(_offset, count);
            _offset += count;
            return ValueTask.FromResult(chunk);
        }

        /// <inheritdoc/>
        public void Release()
        {
        }
    }

    /// <summary>Chunks read from a stream into one pooled buffer, reused for every chunk.</summary>
    /// <param name="stream">The source stream (not owned).</param>
    private sealed class StreamChunks(Stream stream) : IChunkSource
    {
        private byte[]? _buffer;

        /// <inheritdoc/>
        public async ValueTask<ReadOnlyMemory<byte>> NextAsync(int count, CancellationToken cancellationToken)
        {
            if (_buffer is null || _buffer.Length < count)
            {
                Release();
                _buffer = ArrayPool<byte>.Shared.Rent(count);
            }

            // ReadAtLeastAsync loops over short reads; throwing on end of stream
            // turns a source that is shorter than announced into EndOfStreamException.
            await stream.ReadAtLeastAsync(_buffer.AsMemory(0, count), count, throwOnEndOfStream: true, cancellationToken)
                .ConfigureAwait(false);
            return _buffer.AsMemory(0, count);
        }

        /// <inheritdoc/>
        public void Release()
        {
            if (_buffer is not null)
            {
                ArrayPool<byte>.Shared.Return(_buffer);
                _buffer = null;
            }
        }
    }

    /// <summary>Where a download's bytes go.</summary>
    private interface IDownloadSink
    {
        /// <summary>Called once, when the first message announced the size.</summary>
        /// <param name="expected">Bytes the range will deliver.</param>
        /// <exception cref="RpcException">The sink cannot take that many bytes.</exception>
        void Start(long expected);

        /// <summary>Takes the next bytes of the range.</summary>
        /// <param name="data">The bytes (valid only during the call).</param>
        /// <param name="cancellationToken">Cancels a write.</param>
        /// <returns>A task that completes when the bytes are taken.</returns>
        ValueTask WriteAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken);
    }

    /// <summary>Collects a range into one exact-size array.</summary>
    /// <param name="limit">Largest range accepted in memory (<see cref="KvNodeClientOptions.MaxUploadBytes"/>).</param>
    private sealed class ArraySink(int limit) : IDownloadSink
    {
        private int _filled;

        /// <summary>The collected bytes (empty before <see cref="Start"/>).</summary>
        public byte[] Array { get; private set; } = [];

        /// <inheritdoc/>
        public void Start(long expected)
        {
            if (expected > limit)
            {
                throw new RpcException(new Status(
                    StatusCode.ResourceExhausted,
                    $"The range is {expected} bytes, above KvNodeClientOptions.MaxUploadBytes ({limit}): read it with DownloadToAsync into a stream."));
            }

            Array = expected == 0 ? [] : new byte[expected];
        }

        /// <inheritdoc/>
        public ValueTask WriteAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken)
        {
            data.Span.CopyTo(Array.AsSpan(_filled));
            _filled += data.Length;
            return ValueTask.CompletedTask;
        }
    }

    /// <summary>Writes a range to a caller's stream as it arrives.</summary>
    /// <param name="stream">The destination (not owned).</param>
    private sealed class StreamSink(Stream stream) : IDownloadSink
    {
        /// <inheritdoc/>
        public void Start(long expected)
        {
        }

        /// <inheritdoc/>
        public ValueTask WriteAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken) =>
            stream.WriteAsync(data, cancellationToken);
    }
}
