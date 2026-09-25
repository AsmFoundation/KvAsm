namespace KvNode.Client;

/// <summary>Settings for <see cref="SnapshotWriter"/> and <see cref="SnapshotFile"/>.</summary>
/// <remarks>
/// Mirror the target server's limits: the server refuses to start on a
/// snapshot with any key or value over its <c>KVNODE_MAX_KEY_BYTES</c> /
/// <c>KVNODE_MAX_VALUE_BYTES</c>, so checking here turns a failed deployment
/// into an immediate <see cref="ArgumentOutOfRangeException"/> at export time.
/// </remarks>
public sealed record SnapshotWriterOptions
{
    /// <summary>Default options: compaction on, the server's default limits.</summary>
    public static SnapshotWriterOptions Default { get; } = new();

    /// <summary>
    /// Apply the <see cref="KeyCompaction"/> contract (default <see langword="true"/>).
    /// Must match the clients that will read the data, or long keys written here
    /// will not be found by them.
    /// </summary>
    public bool CompactLongKeys { get; init; } = true;

    /// <summary>Largest stored key, after compaction (server default: 1024).</summary>
    public int MaxKeyBytes { get; init; } = 1024;

    /// <summary>Largest value (server default: 1 MiB).</summary>
    public int MaxValueBytes { get; init; } = 1024 * 1024;

    /// <summary>Checks the options for consistency.</summary>
    /// <exception cref="ArgumentOutOfRangeException">A limit is out of range.</exception>
    internal void Validate()
    {
        int minKey = CompactLongKeys ? KeyCompaction.CompactedKeyBytes : 1;
        if (MaxKeyBytes < minKey)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxKeyBytes), MaxKeyBytes, $"Must be at least {minKey}.");
        }

        if (MaxValueBytes < 0)
        {
            throw new ArgumentOutOfRangeException(nameof(MaxValueBytes), MaxValueBytes, "Must not be negative.");
        }
    }
}
