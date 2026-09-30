using System.Text;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Health.V1;
using Grpc.Net.Client;
using Grpc.Net.Client.Configuration;
using KvNode.Client.V1;
using GrpcKvNode = global::KvNode.Client.V1.KvNode;

namespace KvNode.Client;

/// <summary>
/// Client for a KvNode server: Get, Put and Delete of binary or UTF-8 keys and
/// values; Upload and Download of large values (and byte ranges) as streams
/// of CRC-32C-verified chunks; plus the standard gRPC health check.
/// </summary>
/// <remarks>
/// <para>
/// Thread-safe and meant to be long-lived: one instance multiplexes any number
/// of concurrent calls over HTTP/2 (and opens extra connections when a
/// connection's stream limit is reached). Create one per server and share it;
/// creating one per call wastes a TCP + HTTP/2 handshake each time.
/// </para>
/// <para>
/// Errors surface as <see cref="RpcException"/> with the server's status:
/// <c>INVALID_ARGUMENT</c> (key/value limits, malformed request),
/// <c>RESOURCE_EXHAUSTED</c> (store full, memory limit, message too large),
/// <c>UNAVAILABLE</c> (connection problems; Get and Download are retried
/// automatically, see <see cref="KvNodeClientOptions.ReadAttempts"/>),
/// <c>DEADLINE_EXCEEDED</c> (see <see cref="KvNodeClientOptions.Timeout"/>,
/// and for transfers <see cref="KvNodeClientOptions.TransferTimeout"/> /
/// <see cref="KvNodeClientOptions.TransferIdleTimeout"/>), <c>CANCELLED</c>;
/// transfers add <c>NOT_FOUND</c>-as-null, <c>OUT_OF_RANGE</c> and
/// <c>DATA_LOSS</c>. Input that can be checked locally throws
/// <see cref="ArgumentException"/> before any network traffic.
/// </para>
/// <para>
/// Zero-copy: keys and values passed as <see cref="ReadOnlyMemory{T}"/> are
/// sent without copying, so they must not be modified until the returned task
/// completes. Values returned by <see cref="GetAsync(ReadOnlyMemory{byte}, CancellationToken)"/>
/// are immutable views of the response and never change afterwards.
/// </para>
/// </remarks>
public sealed partial class KvNodeClient : IDisposable
{
    /// <summary>The service name the server reports health for.</summary>
    public const string ServiceName = "kvnode.v1.KvNode";

    private readonly GrpcKvNode.KvNodeClient _kv;
    private readonly Health.HealthClient _health;

    /// <summary>
    /// The channel's call invoker, for the streaming calls' allocation-free
    /// methods (<see cref="StreamingCodec"/>): the same channel and settings
    /// as <see cref="_kv"/>, other marshallers.
    /// </summary>
    private readonly CallInvoker _invoker;
    private readonly GrpcChannel? _ownedChannel;
    private readonly KvNodeClientOptions _options;

    /// <summary>Connects to a server over plaintext HTTP/2 (h2c) or TLS.</summary>
    /// <param name="address">
    /// Server address, e.g. <c>http://localhost:50051</c> (h2c, as KvNode serves)
    /// or an <c>https://</c> address behind a TLS-terminating proxy.
    /// </param>
    /// <param name="options">Client settings; <see cref="KvNodeClientOptions.Default"/> when null.</param>
    /// <exception cref="ArgumentNullException"><paramref name="address"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException">An option is out of range.</exception>
    /// <remarks>The client owns the channel it creates and disposes it in <see cref="Dispose"/>.</remarks>
    public KvNodeClient(Uri address, KvNodeClientOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(address);
        _options = options ?? KvNodeClientOptions.Default;
        _options.Validate();
        _ownedChannel = GrpcChannel.ForAddress(address, CreateChannelOptions(_options));
        _kv = new GrpcKvNode.KvNodeClient(_ownedChannel);
        _health = new Health.HealthClient(_ownedChannel);
        _invoker = _ownedChannel.CreateCallInvoker();
    }

    /// <summary>Uses an existing channel (custom TLS, interceptors, test servers).</summary>
    /// <param name="channel">The channel; not owned, the caller disposes it.</param>
    /// <param name="options">Client settings; <see cref="KvNodeClientOptions.Default"/> when null.</param>
    /// <exception cref="ArgumentNullException"><paramref name="channel"/> is null.</exception>
    /// <exception cref="ArgumentOutOfRangeException">An option is out of range.</exception>
    /// <remarks>
    /// Retry policy and message-size limits are channel settings: configure them
    /// on the supplied channel (see <see cref="CreateChannelOptions"/>).
    /// </remarks>
    public KvNodeClient(ChannelBase channel, KvNodeClientOptions? options = null)
    {
        ArgumentNullException.ThrowIfNull(channel);
        _options = options ?? KvNodeClientOptions.Default;
        _options.Validate();
        _kv = new GrpcKvNode.KvNodeClient(channel);
        _health = new Health.HealthClient(channel);
        _invoker = channel.CreateCallInvoker();
    }

    /// <summary>
    /// Channel settings matching the given client options: message-size limits
    /// that fit the largest legal key/value and every Upload/Download chunk,
    /// retries for read-only calls, and HTTP/2 keep-alive pings so dead
    /// connections are detected.
    /// </summary>
    /// <param name="options">The client options to derive the settings from.</param>
    /// <returns>A new options object; callers may adjust it before creating a channel.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="options"/> is null.</exception>
    public static GrpcChannelOptions CreateChannelOptions(KvNodeClientOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        var channelOptions = new GrpcChannelOptions
        {
            // Protobuf framing adds a few bytes per field; 1 KiB of headroom
            // keeps a maximum-size value from tripping the transport limit.
            // Download messages carry up to the server's 1 MiB chunk maximum,
            // and an Upload's first message a key next to a whole chunk,
            // whatever MaxValueBytes says.
            MaxReceiveMessageSize = Math.Max(options.MaxValueBytes, KvNodeClientOptions.MaxDownloadChunkBytes) + 1024,
            MaxSendMessageSize = options.MaxKeyBytes + Math.Max(options.MaxValueBytes, options.UploadChunkBytes) + 1024,
            HttpHandler = new SocketsHttpHandler
            {
                EnableMultipleHttp2Connections = true,
                KeepAlivePingDelay = TimeSpan.FromSeconds(60),
                KeepAlivePingTimeout = TimeSpan.FromSeconds(20),
                PooledConnectionIdleTimeout = System.Threading.Timeout.InfiniteTimeSpan,
            },
        };

        if (options.ReadAttempts > 1)
        {
            // Download is read-only too; gRPC retries a server stream only until
            // its first response message (then the call is committed), so a
            // transfer broken mid-way surfaces instead of silently restarting
            // against a value that may have changed.
            var readOnly = new MethodConfig
            {
                Names =
                {
                    new MethodName { Service = ServiceName, Method = "Get" },
                    new MethodName { Service = ServiceName, Method = "Download" },
                    new MethodName { Service = "grpc.health.v1.Health", Method = "Check" },
                },
                RetryPolicy = new RetryPolicy
                {
                    MaxAttempts = options.ReadAttempts,
                    InitialBackoff = TimeSpan.FromMilliseconds(50),
                    MaxBackoff = TimeSpan.FromSeconds(1),
                    BackoffMultiplier = 2,
                    RetryableStatusCodes = { StatusCode.Unavailable },
                },
            };
            channelOptions.ServiceConfig = new ServiceConfig { MethodConfigs = { readOnly } };
        }

        return channelOptions;
    }

    /// <summary>Reads the value stored under a key.</summary>
    /// <param name="key">The key (1+ bytes; long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="cancellationToken">Cancels the call.</param>
    /// <returns>
    /// The value (possibly empty) when the key exists, or <see langword="null"/>
    /// when it does not. Empty and missing are different answers.
    /// </returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key exceeds <see cref="KvNodeClientOptions.MaxKeyBytes"/>.</exception>
    /// <exception cref="RpcException">
    /// The server or transport reported an error; <c>RESOURCE_EXHAUSTED</c> for
    /// a value above the server's <c>KVNODE_MAX_VALUE_BYTES</c>, which only an
    /// upload can store — read those with
    /// <see cref="DownloadAsync(ReadOnlyMemory{byte}, long, long?, CancellationToken)"/>.
    /// </exception>
    public async Task<ReadOnlyMemory<byte>?> GetAsync(ReadOnlyMemory<byte> key, CancellationToken cancellationToken = default)
    {
        var request = new GetRequest { Key = WireKey(key) };
        GetResponse response = await _kv.GetAsync(request, deadline: Deadline(), cancellationToken: cancellationToken)
            .ConfigureAwait(false);

        // Not `response.Found ? response.Value.Memory : null`: ReadOnlyMemory<T>
        // converts implicitly from T[], so the compiler would turn that `null`
        // into a null *array*, i.e. an empty memory, and "not found" would come
        // back as "found, empty". Only an explicit nullable keeps the two apart.
        if (!response.Found)
        {
            return null;
        }

        return response.Value.Memory;
    }

    /// <summary>Reads a UTF-8 value stored under a UTF-8 key.</summary>
    /// <param name="key">The key text (encoded as UTF-8).</param>
    /// <param name="cancellationToken">Cancels the call.</param>
    /// <returns>The decoded value, or <see langword="null"/> when the key does not exist.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="key"/> is null.</exception>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The encoded key is too long.</exception>
    /// <exception cref="RpcException">The server or transport reported an error.</exception>
    /// <remarks>Invalid UTF-8 in a stored value is decoded with replacement characters.</remarks>
    public async Task<string?> GetStringAsync(string key, CancellationToken cancellationToken = default)
    {
        ReadOnlyMemory<byte>? value = await GetAsync(Utf8(key, nameof(key)), cancellationToken).ConfigureAwait(false);
        return value is { } bytes ? Encoding.UTF8.GetString(bytes.Span) : null;
    }

    /// <summary>Stores a value under a key, replacing any previous value atomically.</summary>
    /// <param name="key">The key (1+ bytes; long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="value">The value (may be empty).</param>
    /// <param name="cancellationToken">Cancels the call (the Put may still have been applied).</param>
    /// <returns><see langword="true"/> if the key was created, <see langword="false"/> if an existing value was replaced.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key or value exceeds its configured limit.</exception>
    /// <exception cref="RpcException">
    /// The server or transport reported an error; <c>RESOURCE_EXHAUSTED</c> when
    /// the store is full or at its memory limit.
    /// </exception>
    /// <remarks>Readers observe either the old or the new value, never a mix.</remarks>
    public async Task<bool> PutAsync(ReadOnlyMemory<byte> key, ReadOnlyMemory<byte> value, CancellationToken cancellationToken = default)
    {
        if (value.Length > _options.MaxValueBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(value), value.Length, $"Values are limited to {_options.MaxValueBytes} bytes (KvNodeClientOptions.MaxValueBytes).");
        }

        var request = new PutRequest { Key = WireKey(key), Value = UnsafeByteOperations.UnsafeWrap(value) };
        PutResponse response = await _kv.PutAsync(request, deadline: Deadline(), cancellationToken: cancellationToken)
            .ConfigureAwait(false);
        return response.Created;
    }

    /// <summary>Stores a UTF-8 value under a UTF-8 key.</summary>
    /// <param name="key">The key text.</param>
    /// <param name="value">The value text.</param>
    /// <param name="cancellationToken">Cancels the call.</param>
    /// <returns><see langword="true"/> if the key was created, <see langword="false"/> if replaced.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="key"/> or <paramref name="value"/> is null.</exception>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key or value is too long.</exception>
    /// <exception cref="RpcException">The server or transport reported an error.</exception>
    public Task<bool> PutAsync(string key, string value, CancellationToken cancellationToken = default) =>
        PutAsync(Utf8(key, nameof(key)), Utf8(value, nameof(value)), cancellationToken);

    /// <summary>Removes a key.</summary>
    /// <param name="key">The key (long keys are compacted per <see cref="KeyCompaction"/>).</param>
    /// <param name="cancellationToken">Cancels the call (the Delete may still have been applied).</param>
    /// <returns><see langword="true"/> if the key existed and was removed, <see langword="false"/> if it was absent.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key is too long.</exception>
    /// <exception cref="RpcException">The server or transport reported an error.</exception>
    public async Task<bool> DeleteAsync(ReadOnlyMemory<byte> key, CancellationToken cancellationToken = default)
    {
        var request = new DeleteRequest { Key = WireKey(key) };
        DeleteResponse response = await _kv.DeleteAsync(request, deadline: Deadline(), cancellationToken: cancellationToken)
            .ConfigureAwait(false);
        return response.Deleted;
    }

    /// <summary>Removes a UTF-8 key.</summary>
    /// <param name="key">The key text.</param>
    /// <param name="cancellationToken">Cancels the call.</param>
    /// <returns><see langword="true"/> if the key existed and was removed.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="key"/> is null.</exception>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key is too long.</exception>
    /// <exception cref="RpcException">The server or transport reported an error.</exception>
    public Task<bool> DeleteAsync(string key, CancellationToken cancellationToken = default) =>
        DeleteAsync(Utf8(key, nameof(key)), cancellationToken);

    /// <summary>Asks the server whether it is serving (standard grpc.health.v1 Check).</summary>
    /// <param name="cancellationToken">Cancels the call.</param>
    /// <returns>
    /// <see langword="true"/> for SERVING; <see langword="false"/> for NOT_SERVING,
    /// which the server reports while it drains for shutdown.
    /// </returns>
    /// <exception cref="RpcException">The server is unreachable or reported an error.</exception>
    public async Task<bool> IsServingAsync(CancellationToken cancellationToken = default)
    {
        HealthCheckResponse response = await _health
            .CheckAsync(new HealthCheckRequest { Service = ServiceName }, deadline: Deadline(), cancellationToken: cancellationToken)
            .ConfigureAwait(false);
        return response.Status == HealthCheckResponse.Types.ServingStatus.Serving;
    }

    /// <summary>Disposes the channel if this client created it; in-flight calls are cancelled.</summary>
    public void Dispose() => _ownedChannel?.Dispose();

    /// <summary>Validates and (when configured) compacts a key for the wire.</summary>
    /// <param name="key">The application key.</param>
    /// <returns>The key as sent; wraps the caller's memory when no compaction applies.</returns>
    /// <exception cref="ArgumentException">The key is empty.</exception>
    /// <exception cref="ArgumentOutOfRangeException">The key exceeds <see cref="KvNodeClientOptions.MaxKeyBytes"/>.</exception>
    private ByteString WireKey(ReadOnlyMemory<byte> key)
    {
        if (key.IsEmpty)
        {
            throw new ArgumentException("A key must contain at least one byte.", nameof(key));
        }

        if (_options.CompactLongKeys && key.Length > KeyCompaction.MaxRawKeyBytes)
        {
            // A fresh array nobody else references, so wrapping it is safe.
            return UnsafeByteOperations.UnsafeWrap(KeyCompaction.Encode(key.Span));
        }

        if (key.Length > _options.MaxKeyBytes)
        {
            throw new ArgumentOutOfRangeException(
                nameof(key), key.Length, $"Keys are limited to {_options.MaxKeyBytes} bytes (KvNodeClientOptions.MaxKeyBytes).");
        }

        return UnsafeByteOperations.UnsafeWrap(key);
    }

    /// <summary>The absolute deadline for a call starting now, or null for none.</summary>
    /// <returns>UTC deadline, or <see langword="null"/> when the timeout is infinite.</returns>
    private DateTime? Deadline() =>
        _options.Timeout == System.Threading.Timeout.InfiniteTimeSpan ? null : DateTime.UtcNow + _options.Timeout;

    /// <summary>Encodes text as UTF-8.</summary>
    /// <param name="text">The text.</param>
    /// <param name="paramName">Parameter name for the exception.</param>
    /// <returns>The UTF-8 bytes.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="text"/> is null.</exception>
    private static byte[] Utf8(string text, string paramName)
    {
        ArgumentNullException.ThrowIfNull(text, paramName);
        return Encoding.UTF8.GetBytes(text);
    }
}
