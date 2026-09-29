namespace KvNode.Client;

/// <summary>Settings for a <see cref="KvNodeClient"/>.</summary>
/// <remarks>
/// <see cref="MaxKeyBytes"/>, <see cref="MaxValueBytes"/> and
/// <see cref="MaxUploadBytes"/> should mirror the server's
/// <c>KVNODE_MAX_KEY_BYTES</c> / <c>KVNODE_MAX_VALUE_BYTES</c> /
/// <c>KVNODE_MAX_UPLOAD_BYTES</c>: the client then rejects oversize input
/// locally (an <see cref="ArgumentOutOfRangeException"/>, no network round
/// trip) and sizes its gRPC message limits to fit the largest legal value.
/// Setting them higher than the server's only moves the rejection to the
/// server (<c>INVALID_ARGUMENT</c> / <c>RESOURCE_EXHAUSTED</c>).
/// </remarks>
public sealed record KvNodeClientOptions
{
    /// <summary>Default options: the server's default limits, compaction on, 5 s deadline.</summary>
    public static KvNodeClientOptions Default { get; } = new();

    /// <summary>
    /// Apply the <see cref="KeyCompaction"/> contract to keys longer than 64 bytes
    /// (default <see langword="true"/>). Every client of one server must agree on
    /// this, or the same logical long key is stored under two different keys.
    /// </summary>
    public bool CompactLongKeys { get; init; } = true;

    /// <summary>Largest key sent, after compaction (server default: 1024).</summary>
    public int MaxKeyBytes { get; init; } = 1024;

    /// <summary>Largest value sent or expected (server default: 1 MiB).</summary>
    public int MaxValueBytes { get; init; } = 1024 * 1024;

    /// <summary>
    /// Deadline applied to every call (default 5 s); <see cref="Timeout.InfiniteTimeSpan"/>
    /// disables it. A deadline is what turns a dead peer into a prompt
    /// <c>DEADLINE_EXCEEDED</c> instead of a hung caller.
    /// </summary>
    public TimeSpan Timeout { get; init; } = TimeSpan.FromSeconds(5);

    /// <summary>
    /// Total attempts for read-only calls (Get, Download, health) on
    /// <c>UNAVAILABLE</c>, with exponential backoff (default 3; 1 disables
    /// retries). A Download is retried only until its first response message
    /// arrives (gRPC commits the call then): a transfer broken mid-way is never
    /// silently restarted, because the value may have changed in between.
    /// Put, Delete and Upload are never retried: a retry after a lost reply
    /// would report created/deleted = false for an operation that did succeed.
    /// </summary>
    public int ReadAttempts { get; init; } = 3;

    /// <summary>
    /// Largest value an upload sends (default 64 MiB, the server's
    /// <c>KVNODE_MAX_UPLOAD_BYTES</c> default; at most 2^31 − 1). Mirror the
    /// server's setting: a larger upload then fails locally with
    /// <see cref="ArgumentOutOfRangeException"/> before any byte is sent. It
    /// also bounds what <c>DownloadAsync</c> agrees to hold in memory.
    /// </summary>
    public int MaxUploadBytes { get; init; } = 64 * 1024 * 1024;

    /// <summary>
    /// Value bytes per upload message (default 64 KiB, 1 byte..4 MiB). Larger
    /// chunks mean fewer messages (less per-message overhead), smaller ones
    /// smoother flow control; the server copies each chunk straight into the
    /// stored record either way.
    /// </summary>
    public int UploadChunkBytes { get; init; } = 64 * 1024;

    /// <summary>
    /// Value bytes per download message the server is asked for (default 0 =
    /// the server's choice, 64 KiB; otherwise 1 byte..1 MiB, the server's
    /// maximum).
    /// </summary>
    public int DownloadChunkBytes { get; init; }

    /// <summary>
    /// Deadline of a whole upload or download (default 10 minutes);
    /// <see cref="Timeout.InfiniteTimeSpan"/> disables it. Separate from
    /// <see cref="Timeout"/>, which suits unary calls, not a 64 MiB transfer
    /// over a slow link. A transfer that stops moving is caught sooner by
    /// <see cref="TransferIdleTimeout"/>.
    /// </summary>
    public TimeSpan TransferTimeout { get; init; } = TimeSpan.FromMinutes(10);

    /// <summary>
    /// Longest time an upload or download may make no progress — no chunk
    /// accepted by the transport, no chunk received — before it is cancelled
    /// with <c>DEADLINE_EXCEEDED</c> (default 30 s;
    /// <see cref="Timeout.InfiniteTimeSpan"/> disables it). Catches a peer or
    /// path that stalls without breaking the connection (HTTP/2 keep-alive
    /// pings only detect a dead connection), however long the transfer.
    /// </summary>
    public TimeSpan TransferIdleTimeout { get; init; } = TimeSpan.FromSeconds(30);

    /// <summary>Checks the options for consistency.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A limit is out of range.</exception>
    internal void Validate()
    {
        int minKey = CompactLongKeys ? KeyCompaction.CompactedKeyBytes : 1;
        if (MaxKeyBytes < minKey)
        {
            throw new ArgumentOutOfRangeException(
                nameof(MaxKeyBytes), MaxKeyBytes,
                $"Must be at least {minKey} (compacted keys are {KeyCompaction.CompactedKeyBytes} bytes).");
        }

        if (MaxValueBytes < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxValueBytes), MaxValueBytes, "Must not be negative.");
        }

        if (Timeout <= TimeSpan.Zero && Timeout != System.Threading.Timeout.InfiniteTimeSpan)
        {
            throw new ArgumentOutOfRangeException(nameof(Timeout), Timeout, "Must be positive or infinite.");
        }

        if (ReadAttempts is < 1 or > 5)
        {
            throw new ArgumentOutOfRangeException(nameof(ReadAttempts), ReadAttempts, "Must be 1..5 (gRPC caps retries at 5 attempts).");
        }

        // int already caps it at 2^31 - 1, the server's own maximum.
        if (MaxUploadBytes < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxUploadBytes), MaxUploadBytes, "Must not be negative.");
        }

        if (UploadChunkBytes is < 1 or > MaxUploadChunkBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(UploadChunkBytes), UploadChunkBytes, $"Must be 1..{MaxUploadChunkBytes}.");
        }

        if (DownloadChunkBytes is < 0 or > MaxDownloadChunkBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(DownloadChunkBytes), DownloadChunkBytes, $"Must be 0 (server default) or 1..{MaxDownloadChunkBytes}.");
        }

        if (TransferTimeout <= TimeSpan.Zero && TransferTimeout != System.Threading.Timeout.InfiniteTimeSpan)
        {
            throw new ArgumentOutOfRangeException(nameof(TransferTimeout), TransferTimeout, "Must be positive or infinite.");
        }

        // CancellationTokenSource.CancelAfter takes at most int.MaxValue ms.
        if ((TransferIdleTimeout <= TimeSpan.Zero || TransferIdleTimeout.TotalMilliseconds > int.MaxValue) &&
            TransferIdleTimeout != System.Threading.Timeout.InfiniteTimeSpan)
        {
            throw new ArgumentOutOfRangeException(
                nameof(TransferIdleTimeout), TransferIdleTimeout, "Must be positive (at most ~24.8 days) or infinite.");
        }
    }

    /// <summary>Largest <see cref="UploadChunkBytes"/>: keeps every upload message far below common proxy limits.</summary>
    internal const int MaxUploadChunkBytes = 4 * 1024 * 1024;

    /// <summary>Largest <see cref="DownloadChunkBytes"/>: the server never sends larger chunks.</summary>
    internal const int MaxDownloadChunkBytes = 1024 * 1024;
}
