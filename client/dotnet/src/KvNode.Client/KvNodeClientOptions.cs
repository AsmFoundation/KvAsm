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
    /// <remarks>
    /// The attempts recover from a connection that breaks after it was
    /// established: the retry opens a new one at once. They do not ride out a
    /// server that refuses connections. A failed connection attempt puts the
    /// channel into gRPC's reconnect backoff (1 s at first, growing), and
    /// during it every attempt fails fast with <c>UNAVAILABLE</c>; all attempts
    /// fit in about 150 ms. That fast failure is deliberate, so callers learn
    /// at once that the server is unreachable. Retry at the application level
    /// if a restart must be waited out. A reset that arrives while the
    /// connection is still being established counts as a failed attempt;
    /// whether a peer's immediate reset lands there is up to the OS.
    /// </remarks>
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
    /// Value bytes per upload message (default 1 MiB, 1 byte..4 MiB). Larger
    /// chunks mean fewer messages — and each message costs a gRPC/HTTP/2
    /// write with its own bookkeeping and allocations — smaller ones
    /// smoother flow control; the server copies each chunk straight into the
    /// stored record either way. Measured (2026-09-30, 1 MiB values): 256 KiB
    /// chunks 1.34x the throughput of 64 KiB, 1 MiB another 1.15x, with 41%
    /// and then 17% fewer bytes allocated per upload. The memory it costs is
    /// one chunk per upload in progress (a pooled buffer for stream sources).
    /// </summary>
    public int UploadChunkBytes { get; init; } = 1024 * 1024;

    /// <summary>
    /// Value bytes per download message the server is asked for (default 0 =
    /// the server's choice, 64 KiB; otherwise 1 byte..1 MiB, the server's
    /// maximum). Unlike uploads, bigger is not faster here: gRPC reads a whole
    /// message before handing it over, so 256 KiB messages pipelined worse and
    /// measured 4-10% slower than the default.
    /// </summary>
    public int DownloadChunkBytes { get; init; }

    /// <summary>
    /// HTTP/2 receive window each call starts with (default 16 MiB, .NET's
    /// maximum; 65,535..16 MiB): how many bytes of a Download — or of a large
    /// Get — the server may send before this process has consumed them. It
    /// decides download speed on a fast link. .NET returns credit with a
    /// WINDOW_UPDATE frame every eighth of the window and only as the data is
    /// read, so a window smaller than the transfer makes the server wait for
    /// credit — at the end of every transfer, and whenever .NET is slow to
    /// return it — and every update costs a frame each way; .NET's own
    /// default (64 KiB, grown per call from a round-trip estimate) pays both
    /// many times per MiB. With 16 MiB a transfer of up to 2 MiB needs no
    /// stream-level update at all. Measured (2026-09-30, one host; 4
    /// concurrent downloads of 1 MiB / 2 of 16 MiB): 16 MiB against .NET's
    /// default 1.50x / 1.20x the throughput with 30% / 20% less client CPU
    /// per GB (6 alternating rounds, all agreeing); the screens that chose it
    /// (3-4 rounds): 4 MiB against .NET's default 1.42x / 1.12x, 1 MiB against
    /// 4 MiB 0.81x / 0.85x, 16 MiB against 4 MiB 1.10x / 1.06x and against
    /// 8 MiB 1.00x / 1.07x. Uploads are unaffected: this window only paces
    /// what the client receives.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The price is memory whenever the application consumes a download
    /// slower than the network delivers it: .NET then holds up to this many
    /// received bytes for each such call (never more than the transfer
    /// itself). Nothing caps the sum — .NET returns connection-level credit
    /// as data arrives, not as it is read — so 100 concurrent downloads into
    /// slow destinations may hold 100 windows, 1,600 MiB at the default.
    /// Lower it for that pattern (8 MiB lost nothing on 1 MiB values; 1 MiB
    /// and less cost the speed above). .NET's own window grows to the same
    /// 16 MiB on a long fast path, so the default adds no new worst case there.
    /// </para>
    /// <para>
    /// A channel setting: it applies to the channel the client creates and to
    /// <see cref="KvNodeClient.CreateChannelOptions"/>; a channel passed to the
    /// <see cref="KvNodeClient(Grpc.Core.ChannelBase, KvNodeClientOptions?)"/>
    /// constructor keeps its own. When the process caps the window lower
    /// (<c>DOTNET_SYSTEM_NET_HTTP_SOCKETSHTTPHANDLER_FLOWCONTROL_MAXSTREAMWINDOWSIZE</c>),
    /// that cap wins and .NET's default window stays in force.
    /// </para>
    /// </remarks>
    public int StreamReceiveWindowBytes { get; init; } = MaxStreamReceiveWindowBytes;

    /// <summary>
    /// HTTP/2 connections the client opens to the server and spreads its calls
    /// over, each call taking the next one in turn (default 4; 1..64). One
    /// connection runs every frame of every concurrent call through .NET's
    /// single reader and single writer loop, and it is served by one server
    /// worker thread for its whole life (KvNode pins a connection to the
    /// worker that accepted it), so it caps what one client can move.
    /// Measured (2026-09-30, one server worker, 6 alternating rounds, 4
    /// connections against 1): 4 concurrent 1 MiB uploads 2.82x the
    /// throughput with 41% less client CPU per GB, 2 concurrent 16 MiB uploads
    /// 1.73x, downloads 1.32x / 1.41x (then bound by the one server core),
    /// unary Gets with 64 in flight 1.38x (5% more client CPU each); one call
    /// at a time is unchanged. Against a 4-worker server (8 concurrent 1 MiB
    /// transfers, screens of 3 rounds) 4 connections moved 7.7 GB/s up and
    /// 11.4 GB/s down against one's 3.0 and 4.1; 8 added 1.26x on uploads,
    /// nothing on downloads, 1.12x on Gets — raise it toward the number of
    /// concurrent transfers for more.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The price: one TCP connection and HTTP/2 session each (opened lazily,
    /// by the first call that takes it), with its own keep-alive pings, and
    /// the server's per-connection state; 5% (one server worker) to 27% (four)
    /// more client CPU per Get, the cost of keeping more connections busy at
    /// once. Which server worker takes a connection is the kernel's choice
    /// (<c>SO_REUSEPORT</c> hashing), so a few connections may share one
    /// worker — more connections spread more evenly. Set 1 for a client
    /// that makes one call at a time and must hold a single connection.
    /// Concurrent calls on different connections are unordered, exactly as
    /// concurrent calls on one connection already are; a call that completed
    /// is visible to every later call on any connection.
    /// </para>
    /// <para>
    /// A channel setting: it applies when the client creates its own channels
    /// (the <see cref="Uri"/> constructor); a client given a channel uses that
    /// one channel.
    /// </para>
    /// </remarks>
    public int Connections { get; init; } = 4;

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

        // The range SocketsHttpHandler.InitialHttp2StreamWindowSize accepts in
        // a process that leaves .NET's maximum at its default: checked here so
        // a bad value fails when the client is created, not on first use.
        if (StreamReceiveWindowBytes is < MinStreamReceiveWindowBytes or > MaxStreamReceiveWindowBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(StreamReceiveWindowBytes), StreamReceiveWindowBytes,
                $"Must be {MinStreamReceiveWindowBytes}..{MaxStreamReceiveWindowBytes} (.NET's HTTP/2 window limits).");
        }

        if (Connections is < 1 or > MaxConnections)
        {
            throw new ArgumentOutOfRangeException(nameof(Connections), Connections, $"Must be 1..{MaxConnections}.");
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

    /// <summary>Smallest <see cref="StreamReceiveWindowBytes"/>: HTTP/2's own initial window (RFC 9113 §6.9.2), below which .NET refuses.</summary>
    internal const int MinStreamReceiveWindowBytes = 65535;

    /// <summary>Largest <see cref="StreamReceiveWindowBytes"/>: .NET's default maximum stream window.</summary>
    internal const int MaxStreamReceiveWindowBytes = 16 * 1024 * 1024;

    /// <summary>
    /// Largest <see cref="Connections"/>: far above what one client process
    /// can use (the gains come from spreading work over cores), and a bound
    /// on what one client may hold open against a server.
    /// </summary>
    internal const int MaxConnections = 64;
}
