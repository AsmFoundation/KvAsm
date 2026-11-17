using DotNet.Testcontainers.Builders;
using DotNet.Testcontainers.Containers;
using Xunit;

[assembly: AssemblyFixture(typeof(KvAsm.Client.Tests.KvAsmServer))]

namespace KvAsm.Client.Tests;

/// <summary>
/// One real KvAsm server (the production container image) shared by every
/// integration test in the assembly, started through Testcontainers.
/// </summary>
/// <remarks>
/// <para>
/// Image: <c>KVASM_IMAGE</c>, default <see cref="DefaultImage"/> (build it
/// with <c>docker build --target runtime -t kvasm:0.3.0 .</c> at the
/// repository root). CI sets <c>KVASM_IMAGE</c> to the image it just built
/// and tested, so the suite always runs against this commit's server.
/// </para>
/// <para>
/// Without Docker or the image, integration tests are skipped with the reason
/// — unless <c>KVASM_REQUIRE_DOCKER=1</c>, which turns that into a failure so
/// CI can never pass by silently skipping the tests that matter most.
/// </para>
/// </remarks>
public sealed class KvAsmServer : IAsyncLifetime
{
    /// <summary>
    /// Image used when <c>KVASM_IMAGE</c> is not set: the local tag of the
    /// current release version, so a stale image of an older version is never
    /// picked up by accident after a version bump (the tag no longer matches).
    /// </summary>
    public const string DefaultImage = "kvasm:0.3.0";

    /// <summary>Port the server listens on inside the container.</summary>
    public const int ContainerPort = 50051;

    private IContainer? _container;

    /// <summary>Server address (h2c), set once the container is listening.</summary>
    public Uri? Address { get; private set; }

    /// <summary>Why the server could not be started, or null when it runs.</summary>
    public string? UnavailableReason { get; private set; }

    /// <summary>The image under test.</summary>
    public static string Image =>
        Environment.GetEnvironmentVariable("KVASM_IMAGE") is { Length: > 0 } image ? image : DefaultImage;

    /// <summary>True when integration tests must fail rather than skip without Docker.</summary>
    public static bool DockerRequired => Environment.GetEnvironmentVariable("KVASM_REQUIRE_DOCKER") == "1";

    /// <summary>Starts the shared server, or records why it cannot run.</summary>
    /// <returns>A task that completes when the server listens (or the failure is recorded).</returns>
    /// <exception cref="Exception">Startup failed and <see cref="DockerRequired"/> is set.</exception>
    public async ValueTask InitializeAsync()
    {
        try
        {
            (_container, Address) = await StartAsync(
                new Dictionary<string, string> { ["KVASM_WORKERS"] = "2" }, snapshot: null, CancellationToken.None);
        }
        catch (Exception e) when (!DockerRequired)
        {
            UnavailableReason = $"KvAsm container '{Image}' unavailable ({e.GetType().Name}: {e.Message}). " +
                                "Build the image or set KVASM_REQUIRE_DOCKER=1 to fail instead of skipping.";
        }
    }

    /// <summary>Stops and removes the container.</summary>
    /// <returns>A task that completes when the container is gone.</returns>
    public async ValueTask DisposeAsync()
    {
        if (_container is not null)
        {
            await _container.DisposeAsync();
        }
    }

    /// <summary>Skips the calling test when no server is available.</summary>
    /// <returns>The server address.</returns>
    public Uri RequireAddress()
    {
        if (UnavailableReason is not null)
        {
            Assert.Skip(UnavailableReason);
        }

        return Address!;
    }

    /// <summary>Starts a dedicated server container (for tests that need their own configuration).</summary>
    /// <param name="environment">KVASM_* settings.</param>
    /// <param name="snapshot">
    /// Snapshot bytes to place at <c>/data/seed.kvs</c> (and point
    /// <c>KVASM_SNAPSHOT</c> at), or null for none.
    /// </param>
    /// <param name="cancellationToken">Cancels the startup.</param>
    /// <returns>The running container (caller disposes) and its h2c address.</returns>
    /// <exception cref="Exception">Docker is unavailable, the image is missing, or the server did not start.</exception>
    /// <remarks>
    /// Readiness is the server's own <c>msg="listening"</c> log line: the image
    /// is FROM scratch, so wait strategies that exec a shell in the container
    /// cannot work, and a log-based wait is exact anyway.
    /// </remarks>
    public static async Task<(IContainer Container, Uri Address)> StartAsync(
        IReadOnlyDictionary<string, string> environment, byte[]? snapshot, CancellationToken cancellationToken)
    {
        ContainerBuilder builder = new ContainerBuilder(Image)
            .WithPortBinding(ContainerPort, assignRandomHostPort: true)
            .WithWaitStrategy(Wait.ForUnixContainer().UntilMessageIsLogged("msg=\"listening\""));
        foreach ((string name, string value) in environment)
        {
            builder = builder.WithEnvironment(name, value);
        }

        if (snapshot is not null)
        {
            builder = builder
                .WithResourceMapping(snapshot, "/data/seed.kvs")
                .WithEnvironment("KVASM_SNAPSHOT", "/data/seed.kvs");
        }

        IContainer container = builder.Build();
        try
        {
            await container.StartAsync(cancellationToken);
        }
        catch
        {
            await container.DisposeAsync();
            throw;
        }

        return (container, new Uri($"http://{container.Hostname}:{container.GetMappedPublicPort(ContainerPort)}"));
    }
}
