using System.Runtime.CompilerServices;

namespace KvNode.Client;

/// <summary>
/// Writes a complete snapshot file from a record sequence of unknown length,
/// replacing the target atomically.
/// </summary>
/// <remarks>
/// <para>
/// How: records stream into a temporary file next to the target (header count
/// 0), the count is patched in, a second sequential pass computes the
/// checksum (it covers the header, so it cannot be computed before the count
/// is known), the trailer is appended, the file is flushed to disk, and only
/// then renamed over the target. Cost: the data is read back once.
/// </para>
/// <para>
/// Why: a server that restarts while an export runs must see either the old
/// complete snapshot or the new complete one — never a partial file. On any
/// failure the temporary file is deleted and the target is left untouched.
/// </para>
/// </remarks>
public static class SnapshotFile
{
    private const int BufferBytes = 64 * 1024;

    /// <summary>Writes all records to <paramref name="path"/> atomically.</summary>
    /// <param name="path">Target file; replaced only after the new snapshot is complete and durable.</param>
    /// <param name="records">The records; duplicate keys are allowed (the last one wins).</param>
    /// <param name="options">Writer options; <see cref="SnapshotWriterOptions.Default"/> when null.</param>
    /// <param name="cancellationToken">Cancels the export; the target is left untouched.</param>
    /// <returns>The number of records written.</returns>
    /// <exception cref="ArgumentException"><paramref name="path"/> is empty, or a key is empty.</exception>
    /// <exception cref="ArgumentNullException"><paramref name="path"/> or <paramref name="records"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A key or value exceeds its configured limit, or an option is out of range.</exception>
    /// <exception cref="IOException">Writing, reading back or renaming failed.</exception>
    /// <exception cref="UnauthorizedAccessException">The directory or target is not writable.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    public static async Task<long> WriteAsync(
        string path,
        IAsyncEnumerable<KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>> records,
        SnapshotWriterOptions? options = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrEmpty(path);
        ArgumentNullException.ThrowIfNull(records);
        options ??= SnapshotWriterOptions.Default;
        options.Validate();

        string target = Path.GetFullPath(path);
        string temp = target + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            long count;
            var file = new FileStream(temp, new FileStreamOptions
            {
                Mode = FileMode.CreateNew,
                Access = FileAccess.ReadWrite,
                Share = FileShare.None,
                BufferSize = 0, // the writer buffers; a second buffer would only copy
                Options = FileOptions.Asynchronous,
            });
            await using (file.ConfigureAwait(false))
            {
                // Pass 1: header with count 0, then every record.
                var writer = new SnapshotWriter(file, 0, options, leaveOpen: true, deferred: true);
                await using (writer.ConfigureAwait(false))
                {
                    await foreach (KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>> record in
                                   records.WithCancellation(cancellationToken).ConfigureAwait(false))
                    {
                        await writer.WriteAsync(record.Key, record.Value, cancellationToken).ConfigureAwait(false);
                    }

                    await writer.FlushDeferredAsync(cancellationToken).ConfigureAwait(false);
                    count = writer.RecordsWritten;
                }

                long end = file.Length;
                var header = new byte[SnapshotFormat.HeaderBytes];
                SnapshotFormat.WriteHeader(header, count);
                file.Position = 0;
                await file.WriteAsync(header, cancellationToken).ConfigureAwait(false);

                // Pass 2: checksum everything before the trailer, as the loader will.
                file.Position = 0;
                var checksum = new SipHash128(0, 0);
                var buffer = new byte[BufferBytes];
                for (long remaining = end; remaining > 0;)
                {
                    int read = await file.ReadAsync(buffer.AsMemory(0, (int)Math.Min(buffer.Length, remaining)), cancellationToken)
                        .ConfigureAwait(false);
                    if (read == 0)
                    {
                        throw new IOException($"Snapshot '{temp}' shrank while it was being written.");
                    }

                    checksum.Append(buffer.AsSpan(0, read));
                    remaining -= read;
                }

                var trailer = new byte[SnapshotFormat.TrailerBytes];
                SnapshotFormat.WriteTrailer(trailer, checksum.Finish().Low);
                file.Position = end;
                await file.WriteAsync(trailer, cancellationToken).ConfigureAwait(false);
                await file.FlushAsync(cancellationToken).ConfigureAwait(false);

                // Durable before visible: without this, a power loss after the
                // rename could publish a file whose blocks never reached disk.
                file.Flush(flushToDisk: true);
            }

            File.Move(temp, target, overwrite: true);
            return count;
        }
        catch
        {
            TryDelete(temp);
            throw;
        }
    }

    /// <summary>Writes all records from a synchronous sequence to <paramref name="path"/> atomically.</summary>
    /// <param name="path">Target file; replaced only after the new snapshot is complete and durable.</param>
    /// <param name="records">The records; duplicate keys are allowed (the last one wins).</param>
    /// <param name="options">Writer options; <see cref="SnapshotWriterOptions.Default"/> when null.</param>
    /// <param name="cancellationToken">Cancels the export; the target is left untouched.</param>
    /// <returns>The number of records written.</returns>
    /// <exception cref="ArgumentException"><paramref name="path"/> is empty, or a key is empty.</exception>
    /// <exception cref="ArgumentNullException"><paramref name="path"/> or <paramref name="records"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A key or value exceeds its configured limit, or an option is out of range.</exception>
    /// <exception cref="IOException">Writing, reading back or renaming failed.</exception>
    /// <exception cref="UnauthorizedAccessException">The directory or target is not writable.</exception>
    /// <exception cref="OperationCanceledException">Cancelled.</exception>
    public static Task<long> WriteAsync(
        string path,
        IEnumerable<KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>> records,
        SnapshotWriterOptions? options = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(records);
        return WriteAsync(path, ToAsync(records), options, cancellationToken);
    }

    /// <summary>Adapts a synchronous sequence to the asynchronous overload.</summary>
    /// <param name="records">The records.</param>
    /// <param name="cancellationToken">Stops the enumeration.</param>
    /// <returns>The same records as an asynchronous sequence.</returns>
    private static async IAsyncEnumerable<KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>> ToAsync(
        IEnumerable<KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>>> records,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        foreach (KeyValuePair<ReadOnlyMemory<byte>, ReadOnlyMemory<byte>> record in records)
        {
            cancellationToken.ThrowIfCancellationRequested();
            yield return record;
        }

        await Task.CompletedTask.ConfigureAwait(false);
    }

    /// <summary>Best-effort removal of a temporary file after a failure.</summary>
    /// <param name="path">The temporary file.</param>
    /// <remarks>Never throws: the original exception is the one worth reporting.</remarks>
    private static void TryDelete(string path)
    {
        try
        {
            File.Delete(path);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
