using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;

namespace KvNode.Client.Tests;

/// <summary>Faults a <see cref="FaultProxy"/> applies to the connections it accepts from then on.</summary>
/// <remarks>
/// Byte counts are per connection and per direction, from the connection's
/// first byte (HTTP/2 preface and SETTINGS included); -1 disables a fault.
/// "Up" is client → server, "down" server → client.
/// </remarks>
internal sealed record Faults
{
    /// <summary>Reset both sides (TCP RST) once this many bytes went up.</summary>
    public long AbortUpAfter { get; init; } = -1;

    /// <summary>Reset both sides once this many bytes came down.</summary>
    public long AbortDownAfter { get; init; } = -1;

    /// <summary>
    /// Stop forwarding up after this many bytes — and stop READING from the
    /// client, so its TCP send buffer fills: a path that hangs without
    /// breaking. The other direction keeps flowing.
    /// </summary>
    public long StallUpAfter { get; init; } = -1;

    /// <summary>Stop forwarding (and reading from the server) down after this many bytes.</summary>
    public long StallDownAfter { get; init; } = -1;

    /// <summary>Pace the server → client direction to about this many bytes per second (0 = no limit).</summary>
    public int ThrottleDownBytesPerSecond { get; init; }

    /// <summary>Reset this many newly accepted connections at once, before any byte moves.</summary>
    public int RejectConnections { get; init; }
}

/// <summary>
/// A TCP proxy between a client and the server container that breaks the
/// connection on purpose — the network interruptions a real deployment sees
/// (resets, hangs, slow links, refused connections), injected at exact byte
/// counts so every test is deterministic.
/// </summary>
/// <remarks>
/// HTTP/2 cleartext is plain TCP, so the proxy works at the byte level and the
/// client and server see nothing but the fault. Each connection gets its own
/// two pumps and its own byte counts; set <see cref="Faults"/> BEFORE the
/// client opens its connections (a new <see cref="KvNodeClient"/> opens new
/// ones, one per <see cref="KvNodeClientOptions.Connections"/> lane, each on
/// the first call that takes it).
/// </remarks>
internal sealed class FaultProxy : IAsyncDisposable
{
    private readonly TcpListener _listener;
    private readonly string _upstreamHost;
    private readonly int _upstreamPort;
    private readonly CancellationTokenSource _stop = new();
    private readonly ConcurrentBag<Socket> _sockets = [];
    private readonly Task _accepting;
    private int _connections;
    private int _rejectsLeft;
    private Faults _faults = new();

    /// <summary>Starts listening on an ephemeral loopback port.</summary>
    /// <param name="upstream">The server's h2c address.</param>
    public FaultProxy(Uri upstream)
    {
        _upstreamHost = upstream.Host;
        _upstreamPort = upstream.Port;
        _listener = new TcpListener(IPAddress.Loopback, 0);
        _listener.Start();
        _accepting = AcceptLoopAsync();
    }

    /// <summary>The address clients connect to (h2c).</summary>
    public Uri Address => new($"http://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}");

    /// <summary>Faults applied to connections accepted from now on.</summary>
    public Faults Faults
    {
        get => _faults;
        set
        {
            _faults = value;
            Volatile.Write(ref _rejectsLeft, value.RejectConnections);
        }
    }

    /// <summary>Connections accepted so far (rejected ones included).</summary>
    public int Connections => Volatile.Read(ref _connections);

    /// <summary>Stops accepting and resets every connection.</summary>
    /// <returns>A task that completes when the accept loop ended.</returns>
    public async ValueTask DisposeAsync()
    {
        await _stop.CancelAsync();
        _listener.Stop();
        foreach (Socket socket in _sockets)
        {
            Reset(socket);
        }

        try
        {
            await _accepting;
        }
        catch (OperationCanceledException)
        {
        }

        _stop.Dispose();
    }

    /// <summary>Accepts connections until disposed; each is served on its own tasks.</summary>
    /// <returns>A task that ends when the listener stops.</returns>
    private async Task AcceptLoopAsync()
    {
        while (!_stop.IsCancellationRequested)
        {
            Socket client;
            try
            {
                client = await _listener.AcceptSocketAsync(_stop.Token);
            }
            catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException)
            {
                return;
            }

            Interlocked.Increment(ref _connections);
            _sockets.Add(client);
            if (Interlocked.Decrement(ref _rejectsLeft) >= 0)
            {
                Reset(client);                 // refused: RST before any byte
                continue;
            }

            _ = ServeAsync(client, _faults);
        }
    }

    /// <summary>Connects upstream and runs both directions with the faults of this connection.</summary>
    /// <param name="client">The accepted client socket.</param>
    /// <param name="faults">The faults in force when it was accepted.</param>
    /// <returns>A task that ends when both directions ended.</returns>
    private async Task ServeAsync(Socket client, Faults faults)
    {
        var server = new Socket(SocketType.Stream, ProtocolType.Tcp) { NoDelay = true };
        _sockets.Add(server);
        client.NoDelay = true;
        try
        {
            await server.ConnectAsync(_upstreamHost, _upstreamPort, _stop.Token);
        }
        catch (Exception e) when (e is SocketException or OperationCanceledException or ObjectDisposedException)
        {
            Reset(client);
            Reset(server);
            return;
        }

        void Abort()
        {
            Reset(client);
            Reset(server);
        }

        await Task.WhenAll(
            PumpAsync(client, server, faults.AbortUpAfter, faults.StallUpAfter, 0, Abort),
            PumpAsync(server, client, faults.AbortDownAfter, faults.StallDownAfter, faults.ThrottleDownBytesPerSecond, Abort));
    }

    /// <summary>Copies one direction, applying its faults.</summary>
    /// <param name="from">Socket read from.</param>
    /// <param name="to">Socket written to.</param>
    /// <param name="abortAfter">Reset both sockets once this many bytes passed (-1: never).</param>
    /// <param name="stallAfter">Stop reading and forwarding once this many bytes passed (-1: never).</param>
    /// <param name="bytesPerSecond">Pacing (0: none).</param>
    /// <param name="abort">Resets both sockets.</param>
    /// <returns>A task that ends at EOF, on a fault or when the proxy stops.</returns>
    /// <remarks>
    /// Reads are capped so a fault triggers at EXACTLY its byte count: the
    /// bytes before it are forwarded, the byte that crosses it is not.
    /// </remarks>
    private async Task PumpAsync(Socket from, Socket to, long abortAfter, long stallAfter, int bytesPerSecond, Action abort)
    {
        byte[] buffer = new byte[16384];
        long moved = 0;
        try
        {
            while (true)
            {
                if (stallAfter >= 0 && moved >= stallAfter)
                {
                    // Hang: stop reading, so TCP back-pressure builds up behind us.
                    await Task.Delay(System.Threading.Timeout.Infinite, _stop.Token);
                }

                int max = buffer.Length;
                if (stallAfter >= 0)
                {
                    max = (int)Math.Min(max, stallAfter - moved);
                }

                int n = await from.ReceiveAsync(buffer.AsMemory(0, max), SocketFlags.None, _stop.Token);
                if (n == 0)
                {
                    to.Shutdown(SocketShutdown.Send);   // pass the half-close on
                    return;
                }

                if (abortAfter >= 0 && moved + n > abortAfter)
                {
                    int keep = (int)(abortAfter - moved);
                    await SendAllAsync(to, buffer.AsMemory(0, keep));
                    abort();
                    return;
                }

                await SendAllAsync(to, buffer.AsMemory(0, n));
                moved += n;
                if (bytesPerSecond > 0)
                {
                    await Task.Delay(TimeSpan.FromSeconds((double)n / bytesPerSecond), _stop.Token);
                }
            }
        }
        catch (Exception e) when (e is SocketException or OperationCanceledException or ObjectDisposedException)
        {
            abort();
        }
    }

    /// <summary>Sends every byte (a stream socket may take less per call).</summary>
    /// <param name="socket">The socket.</param>
    /// <param name="data">The bytes.</param>
    /// <returns>A task that completes when all bytes were handed to the kernel.</returns>
    private async Task SendAllAsync(Socket socket, ReadOnlyMemory<byte> data)
    {
        while (!data.IsEmpty)
        {
            int sent = await socket.SendAsync(data, SocketFlags.None, _stop.Token);
            data = data[sent..];
        }
    }

    /// <summary>Closes a socket with RST (linger 0), as a crashed peer or a middlebox would.</summary>
    /// <param name="socket">The socket (may be closed already).</param>
    private static void Reset(Socket socket)
    {
        try
        {
            socket.LingerState = new LingerOption(true, 0);
            socket.Close();
        }
        catch (Exception e) when (e is SocketException or ObjectDisposedException)
        {
            // already gone
        }
    }
}
