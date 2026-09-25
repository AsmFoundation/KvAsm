namespace KvNode.Client;

/// <summary>
/// Streams a KvNode snapshot (the file a server loads at startup via
/// <c>KVNODE_SNAPSHOT</c>) to any writable stream in a single pass.
/// </summary>
/// <remarks>
/// <para>
/// The record count is part of the header and the checksum covers the
/// header, so the count must be declared up front; <see cref="Complete"/>
/// verifies it was honored. When the count is not known in advance, use
/// <see cref="SnapshotFile.WriteAsync(string, IAsyncEnumerable{KeyValuePair{ReadOnlyMemory{byte}, ReadOnlyMemory{byte}}}, SnapshotWriterOptions?, CancellationToken)"/>.
/// </para>
/// <para>
/// Duplicate keys are allowed; the server keeps the last one, exactly like a
/// sequence of Puts. Keys longer than 64 bytes are compacted
/// (<see cref="KeyCompaction"/>) unless disabled in the options.
/// </para>
/// <para>
/// Footgun, by design: disposing without <see cref="Complete"/> leaves the
/// stream without a trailer, and the server refuses such a file. An export
/// that crashes halfway can therefore never be mistaken for a complete one.
/// </para>
/// <para>Not thread-safe.</para>
/// </remarks>
public sealed class SnapshotWriter : IDisposable, IAsyncDisposable
{
    private const int BufferBytes = 64 * 1024;

    private readonly Stream _destination;
    private readonly bool _leaveOpen;
    private readonly SnapshotWriterOptions _options;

    /// <summary>Checksum of every byte staged so far; null in deferred mode.</summary>
    private readonly SipHash128? _checksum;

    private readonly byte[] _buffer = new byte[BufferBytes];
    private readonly byte[] _recordHeader = new byte[SnapshotFormat.RecordHeaderBytes];
    private int _buffered;
    private bool _completed;
    private bool _disposed;

    /// <summary>Starts a snapshot with a known number of records.</summary>
    /// <param name="destination">Writable stream; the snapshot starts at its current position.</param>
    /// <param name="recordCount">Exact number of records that will be written.</param>
    /// <param name="options">Writer options; <see cref="SnapshotWriterOptions.Default"/> when null.</param>
    /// <param name="leaveOpen"><see langword="true"/> to leave <paramref name="destination"/> open on dispose.</param>
    /// <exception cref="ArgumentNullException"><paramref name="destination"/> is null.</exception>
    /// <exception cref="ArgumentException"><paramref name="destination"/> is not writable.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="recordCount"/> is negative, or an option is out of range.</exception>
    public SnapshotWriter(Stream destination, long recordCount, SnapshotWriterOptions? options = null, bool leaveOpen = false)
        : this(destination, recordCount, options, leaveOpen, deferred: false)
    {
    }

    /// <summary>Shared constructor; deferred mode is used by <see cref="SnapshotFile"/>.</summary>
    /// <param name="destination">Writable stream.</param>
    /// <param name="recordCount">Declared count (ignored in deferred mode, header gets 0).</param>
    /// <param name="options">Writer options, or null for defaults.</param>
    /// <param name="leaveOpen">Leave the stream open on dispose.</param>
    /// <param name="deferred">
    /// Count unknown: write a zero count, skip the checksum and the count check;
    /// the caller patches the header and appends the trailer afterwards.
    /// </param>
    /// <exception cref="ArgumentNullException"><paramref name="destination"/> is null.</exception>
    /// <exception cref="ArgumentException"><paramref name="destination"/> is not writable.</exception>
    /// <exception cref="ArgumentOutOfRangeException"><paramref name="recordCount"/> is negative, or an option is out of range.</exception>
    internal SnapshotWriter(Stream destination, long recordCount, SnapshotWriterOptions? options, bool leaveOpen, bool deferred)
    {
        ArgumentNullException.ThrowIfNull(destination);
        ArgumentOutOfRangeException.ThrowIfNegative(recordCount);
        if (!destination.CanWrite)
        {
            throw new ArgumentException("The destination stream must be writable.", nameof(destination));
        }

        _options = options ?? SnapshotWriterOptions.Default;
        _options.Validate();
        _destination = destination;
        _leaveOpen = leaveOpen;
        RecordCount = deferred ? -1 : recordCount;
        _checksum = deferred ? null : new SipHash128(0, 0);

        Span<byte> header = stackalloc byte[SnapshotFormat.HeaderBytes];
        SnapshotFormat.WriteHeader(header, deferred ? 0 : recordCount);
        Stage(header);
    }

    /// <summary>The declared record count (-1 while the count is deferred).</summary>
    public long RecordCount { get; }

    /// <summary>Records written so far.</summary>
    public long RecordsWritten { get; private set; }

    /// <summary>Appends one record.</summary>
    /// <param name="key">The key (1+ bytes; compacted when longer than 64 bytes unless disabled).</param>
    /// <param name="value">The value (may be empty).</param>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key or value exceeds its configured limit.</exception>
    /// <exception cref="InvalidOperationException">All declared records were written, or the writer is completed.</exception>
    /// <exception cref="ObjectDisposedException">The writer was disposed.</exception>
    /// <exception cref="IOException">The destination stream failed.</exception>
    public void Write(ReadOnlySpan<byte> key, ReadOnlySpan<byte> value)
    {
        CheckRecord(value.Length);
        ReadOnlySpan<byte> wireKey = SnapshotFormat.WireKey(key, _options);
        SnapshotFormat.WriteRecordHeader(_recordHeader, wireKey.Length, value.Length);
        Stage(_recordHeader);
        Stage(wireKey);
        Stage(value);
        RecordsWritten++;
    }

    /// <summary>Appends one record, writing to the destination asynchronously.</summary>
    /// <param name="key">The key (1+ bytes; compacted when longer than 64 bytes unless disabled).</param>
    /// <param name="value">The value (may be empty).</param>
    /// <param name="cancellationToken">Cancels the write; the snapshot is then unusable.</param>
    /// <returns>A task that completes when the record is staged (and flushed if the buffer filled).</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key or value exceeds its configured limit.</exception>
    /// <exception cref="InvalidOperationException">All declared records were written, or the writer is completed.</exception>
    /// <exception cref="ObjectDisposedException">The writer was disposed.</exception>
    /// <exception cref="IOException">The destination stream failed.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    public async ValueTask WriteAsync(ReadOnlyMemory<byte> key, ReadOnlyMemory<byte> value, CancellationToken cancellationToken = default)
    {
        CheckRecord(value.Length);
        ReadOnlyMemory<byte> wireKey = SnapshotFormat.WireKey(key, _options);
        SnapshotFormat.WriteRecordHeader(_recordHeader, wireKey.Length, value.Length);
        await StageAsync(_recordHeader, cancellationToken).ConfigureAwait(false);
        await StageAsync(wireKey, cancellationToken).ConfigureAwait(false);
        await StageAsync(value, cancellationToken).ConfigureAwait(false);
        RecordsWritten++;
    }

    /// <summary>Writes the trailer and flushes: the snapshot is valid only after this.</summary>
    /// <exception cref="InvalidOperationException">
    /// Fewer records than declared were written, the writer is in deferred mode,
    /// or it was already completed.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The writer was disposed.</exception>
    /// <exception cref="IOException">The destination stream failed.</exception>
    public void Complete()
    {
        byte[] trailer = PrepareTrailer();
        FlushBuffer();
        _destination.Write(trailer);
        _destination.Flush();
        _completed = true;
    }

    /// <summary>Writes the trailer and flushes asynchronously: the snapshot is valid only after this.</summary>
    /// <param name="cancellationToken">Cancels the write; the snapshot is then unusable.</param>
    /// <returns>A task that completes when the snapshot is fully written and flushed.</returns>
    /// <exception cref="InvalidOperationException">
    /// Fewer records than declared were written, the writer is in deferred mode,
    /// or it was already completed.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The writer was disposed.</exception>
    /// <exception cref="IOException">The destination stream failed.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    public async ValueTask CompleteAsync(CancellationToken cancellationToken = default)
    {
        byte[] trailer = PrepareTrailer();
        await FlushBufferAsync(cancellationToken).ConfigureAwait(false);
        await _destination.WriteAsync(trailer, cancellationToken).ConfigureAwait(false);
        await _destination.FlushAsync(cancellationToken).ConfigureAwait(false);
        _completed = true;
    }

    /// <summary>Deferred mode only: writes staged bytes without a trailer (see <see cref="SnapshotFile"/>).</summary>
    /// <param name="cancellationToken">Cancels the write.</param>
    /// <returns>A task that completes when every staged byte reached the stream.</returns>
    /// <exception cref="IOException">The destination stream failed.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    internal async ValueTask FlushDeferredAsync(CancellationToken cancellationToken)
    {
        await FlushBufferAsync(cancellationToken).ConfigureAwait(false);
        await _destination.FlushAsync(cancellationToken).ConfigureAwait(false);
        _completed = true;
    }

    /// <summary>Releases the stream (unless <c>leaveOpen</c>); an uncompleted snapshot stays invalid.</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (!_leaveOpen)
        {
            _destination.Dispose();
        }
    }

    /// <summary>Releases the stream asynchronously (unless <c>leaveOpen</c>); an uncompleted snapshot stays invalid.</summary>
    /// <returns>A task that completes when the stream is released.</returns>
    public async ValueTask DisposeAsync()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (!_leaveOpen)
        {
            await _destination.DisposeAsync().ConfigureAwait(false);
        }
    }

    /// <summary>Checks writer state, the declared count and the value limit before a record.</summary>
    /// <param name="valueLength">The value length.</param>
    /// <exception cref="ObjectDisposedException">Disposed.</exception>
    /// <exception cref="InvalidOperationException">Completed, or every declared record was written.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The value exceeds its limit.</exception>
    /// <remarks>Runs before anything is staged, so a rejected record leaves no partial bytes behind.</remarks>
    private void CheckRecord(int valueLength)
    {
        ThrowIfUnusable();
        if (RecordCount >= 0 && RecordsWritten == RecordCount)
        {
            throw new InvalidOperationException($"All {RecordCount} declared records were already written.");
        }

        if (valueLength > _options.MaxValueBytes)
        {
            throw new ArgumentOutOfRangeException(
                "value", valueLength, $"Values are limited to {_options.MaxValueBytes} bytes (SnapshotWriterOptions.MaxValueBytes).");
        }
    }

    /// <summary>Finalizes the checksum after checking the record count.</summary>
    /// <returns>The 16-byte trailer.</returns>
    /// <exception cref="ObjectDisposedException">Disposed.</exception>
    /// <exception cref="InvalidOperationException">Completed, deferred, or short of the declared count.</exception>
    private byte[] PrepareTrailer()
    {
        ThrowIfUnusable();
        if (_checksum is null)
        {
            throw new InvalidOperationException("A deferred snapshot is completed by SnapshotFile, not by Complete().");
        }

        if (RecordsWritten != RecordCount)
        {
            throw new InvalidOperationException($"Declared {RecordCount} records but {RecordsWritten} were written.");
        }

        var trailer = new byte[SnapshotFormat.TrailerBytes];
        SnapshotFormat.WriteTrailer(trailer, _checksum.Finish().Low);
        return trailer;
    }

    /// <summary>Throws when the writer can no longer accept input.</summary>
    /// <exception cref="ObjectDisposedException">Disposed.</exception>
    /// <exception cref="InvalidOperationException">Completed.</exception>
    private void ThrowIfUnusable()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_completed)
        {
            throw new InvalidOperationException("The snapshot was already completed.");
        }
    }

    /// <summary>Checksums bytes and copies them into the buffer, writing it out whenever it fills.</summary>
    /// <param name="data">Bytes that belong to the checksummed part of the file.</param>
    /// <exception cref="IOException">The destination stream failed.</exception>
    private void Stage(ReadOnlySpan<byte> data)
    {
        _checksum?.Append(data);
        while (!data.IsEmpty)
        {
            int n = Math.Min(data.Length, _buffer.Length - _buffered);
            data[..n].CopyTo(_buffer.AsSpan(_buffered));
            _buffered += n;
            data = data[n..];
            if (_buffered == _buffer.Length)
            {
                FlushBuffer();
            }
        }
    }

    /// <summary>Asynchronous <see cref="Stage"/>.</summary>
    /// <param name="data">Bytes that belong to the checksummed part of the file.</param>
    /// <param name="cancellationToken">Cancels the write.</param>
    /// <returns>A task that completes when the bytes are staged.</returns>
    /// <exception cref="IOException">The destination stream failed.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    private async ValueTask StageAsync(ReadOnlyMemory<byte> data, CancellationToken cancellationToken)
    {
        _checksum?.Append(data.Span);
        while (!data.IsEmpty)
        {
            int n = Math.Min(data.Length, _buffer.Length - _buffered);
            data[..n].CopyTo(_buffer.AsMemory(_buffered));
            _buffered += n;
            data = data[n..];
            if (_buffered == _buffer.Length)
            {
                await FlushBufferAsync(cancellationToken).ConfigureAwait(false);
            }
        }
    }

    /// <summary>Writes the buffered bytes to the destination.</summary>
    /// <exception cref="IOException">The destination stream failed.</exception>
    private void FlushBuffer()
    {
        if (_buffered > 0)
        {
            _destination.Write(_buffer, 0, _buffered);
            _buffered = 0;
        }
    }

    /// <summary>Writes the buffered bytes to the destination asynchronously.</summary>
    /// <param name="cancellationToken">Cancels the write.</param>
    /// <returns>A task that completes when the buffer is written.</returns>
    /// <exception cref="IOException">The destination stream failed.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    private async ValueTask FlushBufferAsync(CancellationToken cancellationToken)
    {
        if (_buffered > 0)
        {
            await _destination.WriteAsync(_buffer.AsMemory(0, _buffered), cancellationToken).ConfigureAwait(false);
            _buffered = 0;
        }
    }
}
