// KvNode.Client.Bench — measures the .NET client's streaming calls against a
// running server: N concurrent loops of UploadAsync (from memory),
// DownloadToAsync (into Stream.Null) or — as a guard — unary GetAsync over a
// fixed key set, a warm-up, then a measured window. Prints one JSON object:
//   transfers, value MB/s, p50/p99 per transfer, client CPU (the whole
//   process: thread pool, gRPC, GC), bytes the process allocated per transfer
//   and per MiB moved, GC collections, and — with --server-pid, when the
//   server's /proc is visible (same PID namespace) — the server's CPU from
//   its threads' schedstat, as kvbench measures it.
// Why allocations matter: the client promises bounded memory whatever the
// value's size; per-chunk garbage (a message object, a ByteString, a copy of
// every chunk) turns a 16 MiB download into 256 short-lived objects and
// megabytes of gen0 churn. This tool is how that is counted, before and
// after, with the server and the client pinned to their own CPUs.
//
// Usage: KvNode.Client.Bench --address http://127.0.0.1:PORT --mode upload|download|get
//          [--size BYTES] [--concurrency N] [--keys K] [--warmup S] [--seconds S]
//          [--chunk BYTES] [--server-pid PID] [--window BYTES] [--connections N]
//          [--clients N]
//   --mode get measures unary GetAsync of --size-byte values (Put first), the
//   guard for changes aimed at transfers.
//   --chunk 0 (the default) keeps the client library's own chunk defaults.
//   --window / --connections set KvNodeClientOptions.StreamReceiveWindowBytes
//   / Connections; 0 (the default) keeps the library's own.
//   --clients N spreads the loops over N separate clients (loop i uses
//   client i % N), each with its own connections.
using System.Diagnostics;
using System.Globalization;
using System.Text;
using KvNode.Client;

return await Bench.RunAsync(args).ConfigureAwait(false);

/// <summary>The benchmark: options, loops, measurement window, report.</summary>
internal static class Bench
{
    /// <summary>Largest number of latency samples kept per loop (preallocated: no garbage while measuring).</summary>
    private const int SampleCap = 1 << 18;

    /// <summary>
    /// Parses the options, builds the clients (each with its own
    /// <c>--connections</c> HTTP/2 connections), runs the phases and prints the
    /// JSON report; disposes every client however the run ends.
    /// </summary>
    /// <param name="args">Command line (see the file header).</param>
    /// <returns>0 on success, 1 when a transfer failed or returned wrong data, 2 for bad usage.</returns>
    public static async Task<int> RunAsync(string[] args)
    {
        Options o;
        try
        {
            o = Options.Parse(args);
        }
        catch (Exception e) when (e is ArgumentException or FormatException or OverflowException)
        {
            // A bad option or a number that does not parse: usage, not a crash.
            Console.Error.WriteLine($"KvNode.Client.Bench: {e.Message}");
            return 2;
        }

        // --chunk 0 keeps the library's own defaults, so two builds of the
        // client compare as a user would run them (their defaults differ).
        var defaults = new KvNodeClientOptions();
        var clientOptions = new KvNodeClientOptions
        {
            // Uploads above the default 64 MiB would be refused client-side;
            // get mode Puts its values, which MaxValueBytes must admit.
            MaxUploadBytes = Math.Max(64 * 1024 * 1024, o.Size),
            MaxValueBytes = Math.Max(defaults.MaxValueBytes, o.Mode == "get" ? o.Size : 0),
            UploadChunkBytes = o.Chunk > 0 ? o.Chunk : defaults.UploadChunkBytes,
            DownloadChunkBytes = o.Chunk > 0 ? Math.Min(o.Chunk, 1024 * 1024) : defaults.DownloadChunkBytes,
            TransferIdleTimeout = TimeSpan.FromSeconds(30),
            StreamReceiveWindowBytes = o.Window > 0 ? o.Window : defaults.StreamReceiveWindowBytes,
            Connections = o.Connections > 0 ? o.Connections : defaults.Connections,
        };

        // The library builds its own channels (the Uri constructor), exactly
        // as an application would; each client owns and disposes them.
        var clients = new KvNodeClient[o.Clients];
        try
        {
            for (int i = 0; i < clients.Length; i++)
            {
                clients[i] = new KvNodeClient(new Uri(o.Address), clientOptions);
            }

            return await RunLoopsAsync(o, clients).ConfigureAwait(false);
        }
        finally
        {
            foreach (KvNodeClient? c in clients)
            {
                c?.Dispose();
            }
        }
    }

    /// <summary>Preloads the keys (downloads), runs the loops through warm-up and the measured window, reports.</summary>
    /// <param name="o">Options.</param>
    /// <param name="clients">The clients; loop i uses <c>clients[i % clients.Length]</c>.</param>
    /// <returns>0 on success, 1 when a transfer failed or returned wrong data.</returns>
    private static async Task<int> RunLoopsAsync(Options o, KvNodeClient[] clients)
    {
        // The preload goes through the first client: which connection stored
        // a key makes no difference to the server's store.
        KvNodeClient client = clients[0];
        byte[] value = new byte[o.Size];
        for (int i = 0; i < value.Length; i++)
        {
            value[i] = (byte)('a' + (i % 26));
        }

        byte[][] keys = new byte[o.Keys][];
        for (int i = 0; i < keys.Length; i++)
        {
            keys[i] = Encoding.ASCII.GetBytes($"bench:{i:D6}");
        }

        if (o.Mode == "download")
        {
            // Every key exists before the loops start: a miss would be a failed run.
            foreach (byte[] key in keys)
            {
                await client.UploadAsync(key, value).ConfigureAwait(false);
            }
        }
        else if (o.Mode == "get")
        {
            // Unary values go in with Put, as Get's own path would see them.
            foreach (byte[] key in keys)
            {
                await client.PutAsync(key, value).ConfigureAwait(false);
            }
        }

        var state = new RunState(o.Concurrency, SampleCap);
        Task[] loops = new Task[o.Concurrency];
        for (int i = 0; i < loops.Length; i++)
        {
            int id = i;
            KvNodeClient mine = clients[i % clients.Length];
            loops[i] = Task.Run(() => LoopAsync(mine, o, keys, value, state, id));
        }

        await Task.Delay(TimeSpan.FromSeconds(o.Warmup)).ConfigureAwait(false);
        Snapshot start = Snapshot.Take(o.ServerPid, state);
        state.Measuring = true;
        await Task.Delay(TimeSpan.FromSeconds(o.Seconds)).ConfigureAwait(false);
        state.Measuring = false;
        Snapshot end = Snapshot.Take(o.ServerPid, state);
        state.Stop = true;
        try
        {
            await Task.WhenAll(loops).ConfigureAwait(false);
        }
        catch (Exception e) when (state.Error is null)
        {
            state.Error = e.GetType().Name + ": " + e.Message;
        }

        Report(o, state, start, end);
        return state.Error is null ? 0 : 1;
    }

    /// <summary>One concurrent transfer loop: runs until <see cref="RunState.Stop"/>.</summary>
    /// <param name="client">The shared client (thread-safe).</param>
    /// <param name="o">Options.</param>
    /// <param name="keys">The key set.</param>
    /// <param name="value">The value uploaded (and expected back, by length).</param>
    /// <param name="state">Shared counters and this loop's sample buffer.</param>
    /// <param name="id">Loop index (its sample buffer and random stream).</param>
    /// <returns>A task that completes when the loop stops.</returns>
    /// <exception cref="InvalidDataException">A download or get returned no value or the wrong length.</exception>
    private static async Task LoopAsync(KvNodeClient client, Options o, byte[][] keys, byte[] value, RunState state, int id)
    {
        var rng = new Random(id + 1);
        long[] samples = state.Samples[id];
        try
        {
            while (!state.Stop)
            {
                byte[] key = keys[rng.Next(keys.Length)];
                long t0 = Stopwatch.GetTimestamp();
                switch (o.Mode)
                {
                    case "upload":
                        await client.UploadAsync(key, value).ConfigureAwait(false);
                        break;
                    case "download":
                        DownloadResult? r = await client.DownloadToAsync(key, Stream.Null).ConfigureAwait(false);
                        if (r is null || r.Length != value.Length)
                        {
                            throw new InvalidDataException($"download of {Encoding.ASCII.GetString(key)} returned {r?.Length.ToString(CultureInfo.InvariantCulture) ?? "no value"}");
                        }

                        break;
                    default:
                        ReadOnlyMemory<byte>? v = await client.GetAsync(key).ConfigureAwait(false);
                        if (v is not { } got || got.Length != value.Length)
                        {
                            throw new InvalidDataException($"get of {Encoding.ASCII.GetString(key)} returned {(v is { } g ? g.Length.ToString(CultureInfo.InvariantCulture) : "no value")}");
                        }

                        break;
                }

                long elapsed = Stopwatch.GetTimestamp() - t0;
                if (state.Measuring)
                {
                    int n = state.Counts[id]++;
                    if (n < samples.Length)
                    {
                        samples[n] = elapsed;
                    }
                }

                Interlocked.Increment(ref state.Transfers);
            }
        }
        catch (Exception e)
        {
            state.Error ??= e.GetType().Name + ": " + e.Message;
            state.Stop = true;
        }
    }

    /// <summary>Prints the JSON report of the measured window.</summary>
    /// <param name="o">Options.</param>
    /// <param name="state">Final counters and samples.</param>
    /// <param name="a">Window start.</param>
    /// <param name="b">Window end.</param>
    private static void Report(Options o, RunState state, Snapshot a, Snapshot b)
    {
        double secs = (b.Wall - a.Wall) / (double)Stopwatch.Frequency;
        long transfers = b.Transfers - a.Transfers;
        double bytes = (double)transfers * o.Size;
        var all = new List<long>();
        for (int i = 0; i < state.Samples.Length; i++)
        {
            all.AddRange(state.Samples[i].AsSpan(0, Math.Min(state.Counts[i], SampleCap)).ToArray());
        }

        all.Sort();
        double Pct(double p) => all.Count == 0 ? 0 : all[(int)Math.Min(all.Count - 1, Math.Ceiling(p / 100 * all.Count) - 1)] * 1e6 / Stopwatch.Frequency;
        double serverCpu = a.ServerCpuNs >= 0 && b.ServerCpuNs >= 0 ? (b.ServerCpuNs - a.ServerCpuNs) / 1e9 : -1;
        var s = new StringBuilder();
        s.Append(CultureInfo.InvariantCulture, $"{{\"tool\":\"KvNode.Client.Bench\",\"mode\":\"{o.Mode}\",\"size\":{o.Size},\"concurrency\":{o.Concurrency},");
        s.Append(CultureInfo.InvariantCulture, $"\"chunk\":{o.Chunk},\"window\":{o.Window},\"connections\":{o.Connections},\"clients\":{o.Clients},\"keys\":{o.Keys},\"seconds\":{secs:F3},\"transfers\":{transfers},");
        s.Append(CultureInfo.InvariantCulture, $"\"value_bytes_per_sec\":{bytes / secs:F0},\"p50_us\":{Pct(50):F1},\"p99_us\":{Pct(99):F1},");
        s.Append(CultureInfo.InvariantCulture, $"\"client_cpu_s\":{(b.ClientCpu - a.ClientCpu).TotalSeconds:F3},\"server_cpu_s\":{serverCpu:F3},");
        s.Append(CultureInfo.InvariantCulture, $"\"alloc_bytes\":{b.Allocated - a.Allocated},\"alloc_per_transfer\":{(transfers > 0 ? (b.Allocated - a.Allocated) / (double)transfers : 0):F0},");
        s.Append(CultureInfo.InvariantCulture, $"\"alloc_per_mib\":{(bytes > 0 ? (b.Allocated - a.Allocated) / (bytes / 1048576) : 0):F0},");
        s.Append(CultureInfo.InvariantCulture, $"\"gc0\":{b.Gc0 - a.Gc0},\"gc1\":{b.Gc1 - a.Gc1},\"gc2\":{b.Gc2 - a.Gc2},");
        s.Append(CultureInfo.InvariantCulture, $"\"valid\":{(state.Error is null ? "true" : "false")},\"error\":\"{(state.Error ?? "").Replace("\"", "'", StringComparison.Ordinal)}\"}}");
        Console.WriteLine(s.ToString());
    }
}

/// <summary>Command-line options (see the file header).</summary>
internal sealed record Options(string Address, string Mode, int Size, int Concurrency, int Keys, double Warmup, double Seconds, int Chunk, int ServerPid, int Window, int Connections, int Clients)
{
    /// <summary>Parses the command line.</summary>
    /// <param name="args">Arguments.</param>
    /// <returns>The options.</returns>
    /// <exception cref="ArgumentException">A missing, unknown or out-of-range option.</exception>
    /// <exception cref="FormatException">A numeric option's value is not a number.</exception>
    /// <exception cref="OverflowException">A numeric option's value does not fit its type.</exception>
    public static Options Parse(string[] args)
    {
        string? address = null, mode = null;
        int size = 1 << 20, concurrency = 4, keys = 64, chunk = 0, serverPid = 0, window = 0, connections = 0, clients = 1;
        double warmup = 2, seconds = 6;
        for (int i = 0; i < args.Length; i++)
        {
            string Next() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"{args[i]} needs a value");
            switch (args[i])
            {
                case "--address": address = Next(); break;
                case "--mode": mode = Next(); break;
                case "--size": size = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--concurrency": concurrency = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--keys": keys = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--warmup": warmup = double.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--seconds": seconds = double.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--chunk": chunk = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--server-pid": serverPid = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--window": window = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--connections": connections = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                case "--clients": clients = int.Parse(Next(), CultureInfo.InvariantCulture); break;
                default: throw new ArgumentException($"unknown option {args[i]}");
            }
        }

        if (address is null || mode is not ("upload" or "download" or "get"))
        {
            throw new ArgumentException("--address and --mode upload|download|get are required");
        }

        if (size < 0 || concurrency < 1 || keys < 1 || seconds <= 0 || warmup < 0 || chunk < 0 || clients < 1)
        {
            throw new ArgumentException("size, chunk >= 0; concurrency, keys, clients >= 1; seconds > 0");
        }

        // 0 keeps the library's default; other values must be ones the
        // library accepts, or the client constructor would throw mid-run.
        if (connections is < 0 or > 64)
        {
            throw new ArgumentException("--connections is 0 (the library's) or 1..64");
        }

        // .NET's handler refuses other windows with an exception at channel
        // creation; say so here, as a usage error.
        if (window != 0 && window is < 65535 or > 16 * 1024 * 1024)
        {
            throw new ArgumentException("--window is 0 (the library's) or 65535..16777216");
        }

        return new Options(address, mode, size, concurrency, keys, warmup, seconds, chunk, serverPid, window, connections, clients);
    }
}

/// <summary>State shared by the loops and the main thread.</summary>
/// <param name="loops">Number of loops.</param>
/// <param name="sampleCap">Latency samples kept per loop.</param>
internal sealed class RunState(int loops, int sampleCap)
{
    /// <summary>Completed transfers in any phase (Interlocked).</summary>
    public long Transfers;

    /// <summary>Set by the main thread: record latencies from now on.</summary>
    public volatile bool Measuring;

    /// <summary>Set by the main thread (or a failing loop): the loops end.</summary>
    public volatile bool Stop;

    /// <summary>The first failure, if any (the run is then invalid).</summary>
    public volatile string? Error;

    /// <summary>Per-loop latency samples in Stopwatch ticks (preallocated).</summary>
    public long[][] Samples { get; } = Enumerable.Range(0, loops).Select(_ => new long[sampleCap]).ToArray();

    /// <summary>Per-loop number of measured transfers (each loop writes only its own slot).</summary>
    public int[] Counts { get; } = new int[loops];
}

/// <summary>Counters read at a window boundary.</summary>
internal readonly record struct Snapshot(long Wall, long Transfers, TimeSpan ClientCpu, long Allocated, int Gc0, int Gc1, int Gc2, long ServerCpuNs)
{
    /// <summary>Reads every counter now.</summary>
    /// <param name="serverPid">Server process id (0 = not measured).</param>
    /// <param name="state">The run's shared state.</param>
    /// <returns>The snapshot.</returns>
    public static Snapshot Take(int serverPid, RunState state)
    {
        using var self = Process.GetCurrentProcess();
        return new Snapshot(
            Stopwatch.GetTimestamp(), Interlocked.Read(ref state.Transfers), self.TotalProcessorTime,
            GC.GetTotalAllocatedBytes(precise: true), GC.CollectionCount(0), GC.CollectionCount(1), GC.CollectionCount(2),
            ReadServerCpuNs(serverPid));
    }

    /// <summary>
    /// CPU time of every thread of the server: the first field ("time spent on
    /// the cpu", ns) of /proc/PID/task/TID/schedstat, summed — exact where
    /// /proc/PID/stat has 10 ms ticks.
    /// </summary>
    /// <param name="pid">Server process id; 0 skips the measurement.</param>
    /// <returns>Nanoseconds, or -1 when not measured or not visible.</returns>
    private static long ReadServerCpuNs(int pid)
    {
        if (pid <= 0 || !Directory.Exists($"/proc/{pid}/task"))
        {
            return -1;
        }

        long sum = 0;
        foreach (string task in Directory.EnumerateDirectories($"/proc/{pid}/task"))
        {
            try
            {
                string text = File.ReadAllText(Path.Combine(task, "schedstat"));
                sum += long.Parse(text.AsSpan(0, text.IndexOf(' ', StringComparison.Ordinal)), CultureInfo.InvariantCulture);
            }
            catch (IOException)
            {
                // a thread that exited between the listing and the read
            }
        }

        return sum;
    }
}
