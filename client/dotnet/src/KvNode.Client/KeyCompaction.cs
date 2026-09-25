using System.Security.Cryptography;

namespace KvNode.Client;

/// <summary>
/// The KvNode long-key contract: keys longer than <see cref="MaxRawKeyBytes"/>
/// are replaced, client-side, by <c>0xFF ‖ SHA-512(key)</c> (65 bytes).
/// Every official client and the snapshot writer apply exactly this rule, so a
/// long key written by one of them is found by all others.
/// </summary>
/// <remarks>
/// <para>
/// Why: long keys cost the server memory and exact-compare time on every
/// lookup. A fixed 65-byte stand-in bounds both, without trusting clients with
/// the server's bucket placement (the server still hashes every key with its
/// own secret SipHash key, so clients cannot engineer collisions).
/// </para>
/// <para>
/// Why the rule is collision-safe: raw keys are at most 64 bytes and compacted
/// keys are exactly 65, so the two spaces are disjoint by length; two long keys
/// only collide if SHA-512 does (about 2^-256 per pair).
/// </para>
/// <para>
/// Footgun: a client that disables compaction and sends its own 65-byte key
/// starting with 0xFF shares the compacted key space. Keep compaction on
/// (the default) in every client that talks to the same server.
/// </para>
/// </remarks>
public static class KeyCompaction
{
    /// <summary>Keys up to this length are sent unchanged.</summary>
    public const int MaxRawKeyBytes = 64;

    /// <summary>First byte of every compacted key.</summary>
    public const byte Marker = 0xFF;

    /// <summary>Length of every compacted key: the marker plus a SHA-512 digest.</summary>
    public const int CompactedKeyBytes = 1 + SHA512.HashSizeInBytes;

    /// <summary>Returns the key exactly as it is sent to (and stored by) the server.</summary>
    /// <param name="key">The application key; must not be empty.</param>
    /// <param name="compactLongKeys">
    /// <see langword="true"/> (the contract) to compact keys longer than
    /// <see cref="MaxRawKeyBytes"/>; <see langword="false"/> to return them unchanged.
    /// </param>
    /// <returns>A new array: the key itself, or its 65-byte compacted form.</returns>
    /// <exception cref="ArgumentException"><paramref name="key"/> is empty.</exception>
    public static byte[] Encode(ReadOnlySpan<byte> key, bool compactLongKeys = true)
    {
        if (key.IsEmpty)
        {
            throw new ArgumentException("A key must contain at least one byte.", nameof(key));
        }

        if (!compactLongKeys || key.Length <= MaxRawKeyBytes)
        {
            return key.ToArray();
        }

        var compacted = new byte[CompactedKeyBytes];
        compacted[0] = Marker;
        SHA512.HashData(key, compacted.AsSpan(1));
        return compacted;
    }

    /// <summary>Tells whether a stored (wire) key is a compacted long key.</summary>
    /// <param name="wireKey">A key as stored by the server.</param>
    /// <returns>
    /// <see langword="true"/> for 65-byte keys starting with <see cref="Marker"/>
    /// (always true for keys produced by <see cref="Encode"/> from long keys).
    /// </returns>
    public static bool IsCompacted(ReadOnlySpan<byte> wireKey) =>
        wireKey.Length == CompactedKeyBytes && wireKey[0] == Marker;
}
