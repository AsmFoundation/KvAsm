namespace KvNode.Client;

/// <summary>Settings for a <see cref="KvNodeClient"/>.</summary>
/// <remarks>
/// <see cref="MaxKeyBytes"/> and <see cref="MaxValueBytes"/> should mirror the
/// server's <c>KVNODE_MAX_KEY_BYTES</c> / <c>KVNODE_MAX_VALUE_BYTES</c>: the
/// client then rejects oversize input locally (an
/// <see cref="ArgumentOutOfRangeException"/>, no network round trip) and sizes
/// its gRPC message limits to fit the largest legal value. Setting them higher
/// than the server's only moves the rejection to the server
/// (<c>INVALID_ARGUMENT</c> / <c>RESOURCE_EXHAUSTED</c>).
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
    /// Total attempts for read-only calls (Get, health) on <c>UNAVAILABLE</c>,
    /// with exponential backoff (default 3; 1 disables retries). Put and Delete
    /// are never retried: a retry after a lost reply would report
    /// created/deleted = false for an operation that did succeed.
    /// </summary>
    public int ReadAttempts { get; init; } = 3;

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
    }
}
