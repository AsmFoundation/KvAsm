namespace KvNode.Client;

/// <summary>The outcome of a successful <see cref="KvNodeClient"/> upload.</summary>
/// <param name="Created">
/// <see langword="true"/> when the key did not exist before the upload,
/// <see langword="false"/> when an existing value was replaced.
/// </param>
/// <param name="Length">Bytes stored: the whole value.</param>
/// <param name="Crc32C">
/// CRC-32C of the value as the server stored it — already verified against
/// the client's own checksum before the upload returned, so it can be kept to
/// check later downloads end to end (see <see cref="KvNode.Client.Crc32C"/>).
/// </param>
/// <remarks>
/// The value became visible atomically when the upload completed: readers saw
/// the old value (or none) before, the new one after, never a part of it.
/// </remarks>
public sealed record UploadResult(bool Created, long Length, uint Crc32C);

/// <summary>The outcome of a successful <see cref="KvNodeClient"/> download.</summary>
/// <param name="TotalSize">
/// Size of the WHOLE value as the server holds it — not of the range read —
/// so a caller can plan further ranged reads.
/// </param>
/// <param name="Offset">First byte of the value that was read.</param>
/// <param name="Length">
/// Bytes read: the requested range, cut at the end of the value (a range that
/// starts exactly at the end is empty, not an error).
/// </param>
/// <param name="Crc32C">
/// CRC-32C of exactly the bytes read, as the server computed it while sending
/// them; the client verified it against its own before returning.
/// </param>
/// <remarks>
/// One download reads one version of the value: if the key was replaced or
/// deleted while the download ran, the bytes are still the value as it was
/// when the download started. Two separate downloads (for example ranges read
/// one after the other) may see different versions; compare
/// <see cref="TotalSize"/> or whole-value checksums when that matters.
/// </remarks>
public sealed record DownloadResult(long TotalSize, long Offset, long Length, uint Crc32C);
