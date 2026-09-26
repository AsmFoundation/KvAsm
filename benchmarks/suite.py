"""suite.py — what is benchmarked: systems, their configurations, experiments.

Everything that decides a comparison lives here as reviewable data, so a
reader can check each fairness rule against the code that applies it:

* every system is the latest *stable* release (no release candidates, no
  nightly builds), pinned to an exact version; image digests are recorded
  with every result;
* every server gets exactly N vCPUs (``--cpuset-cpus``) and is configured to
  use them, with the thread count tuned per system in experiment E0;
* persistence is disabled everywhere (KvNode has none), nothing is evicted,
  there is no TLS and no authentication;
* every system is driven by the same client (kvbench) with the same
  connections, pipeline depth, keys, values and key distribution.

See METHODOLOGY.md for the reasoning behind each rule.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field, replace
from typing import Callable

# ---------------------------------------------------------------------------
# Placement: server and client never share vCPUs.
# ---------------------------------------------------------------------------

CLIENT_CPUS = "16-31"
"""vCPUs of the load generator (16 of them; the server uses at most 8)."""

CLIENT_THREADS = 16
"""kvbench threads: one per client vCPU, so the client is never the limit."""


def server_cpus(cores: int) -> str:
    """cpuset for a server with ``cores`` vCPUs.

    Args:
        cores: core budget (1..8).

    Returns:
        A ``--cpuset-cpus`` value, always starting at vCPU 0.
    """
    return "0" if cores == 1 else f"0-{cores - 1}"


# ---------------------------------------------------------------------------
# Workloads
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Workload:
    """One client workload; every field maps to a kvbench option.

    Attributes:
        read_ratio: fraction of GETs (the rest are SETs).
        value_size: value bytes (keys are always 16 bytes).
        keys: key-space size; all keys are preloaded before measuring.
        zipf: Zipfian skew (YCSB scrambled), or None for uniform keys.
        connections: total client connections.
        depth: requests in flight per connection (1 = no pipelining).
        ping: protocol no-op instead of GET/SET (per-request floor).
        rate: open-loop target in ops/s, or None for closed loop.
    """

    read_ratio: float = 1.0
    value_size: int = 100
    keys: int = 1_000_000
    zipf: float | None = None
    connections: int = 64
    depth: int = 16
    ping: bool = False
    rate: float | None = None

    def key(self) -> str:
        """Stable, human-readable identity used to group repetitions.

        Returns:
            e.g. ``get100-v100-k1000000-uni-c64d16``.
        """
        mix = "ping" if self.ping else f"get{round(self.read_ratio * 100)}"
        dist = f"zipf{self.zipf}" if self.zipf else "uni"
        rate = f"-r{int(self.rate)}" if self.rate else ""
        return f"{mix}-v{self.value_size}-k{self.keys}-{dist}-c{self.connections}d{self.depth}{rate}"

    def kvbench_args(self) -> list[str]:
        """kvbench arguments for this workload (connection/threads included).

        Returns:
            Argument list.
        """
        args = ["--threads", str(CLIENT_THREADS), "--connections", str(self.connections),
                "--depth", str(self.depth), "--keys", str(self.keys),
                "--value-size", str(self.value_size), "--read-ratio", str(self.read_ratio)]
        if self.ping:
            args.append("--ping")
        if self.zipf:
            args += ["--zipf", str(self.zipf)]
        if self.rate:
            args += ["--rate", str(int(self.rate))]
        return args


# ---------------------------------------------------------------------------
# Systems
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class System:
    """A key-value server and how it is run fairly.

    Attributes:
        name: short identifier used in results.
        title: display name with the exact version.
        image: pinned image reference.
        protocol: kvbench protocol (resp, memcache, grpc).
        port: listening port inside the container.
        args: container arguments for (threads, workload).
        env: environment for (threads, workload).
        docker_flags: extra ``docker run`` flags.
        thread_knob: True if the thread count is configurable (tuned in E0).
        notes: why the configuration is what it is.
        build: how the image is obtained, if not a plain pull.
    """

    name: str
    title: str
    image: str
    protocol: str
    port: int
    args: Callable[[int, Workload], list[str]]
    env: Callable[[int, Workload], dict[str, str]] = lambda t, w: {}
    docker_flags: tuple[str, ...] = ()
    thread_knob: bool = True
    notes: str = ""
    build: str | None = None


def _memcached_mb(w: Workload) -> int:
    """memcached ``-m`` large enough that nothing is ever evicted.

    Args:
        w: workload.

    Returns:
        Megabytes: 4x the raw dataset, at least 4096 (memcached reserves
        memory by slab class, so raw size underestimates its need).
    """
    raw_mb = w.keys * (w.value_size + 16 + 64) / 2**20
    return max(4096, int(math.ceil(raw_mb * 4)))


SYSTEMS: dict[str, System] = {
    s.name: s
    for s in [
        System(
            name="kvnode", title="KvNode 0.1.0", image="kvnode:0.1.0", protocol="grpc", port=50051,
            args=lambda t, w: [],
            env=lambda t, w: {
                "KVNODE_WORKERS": str(t),
                # capacity is fixed at startup: size it for the key space
                "KVNODE_CAPACITY": str(max(1000, int(w.keys * 1.25))),
                "KVNODE_MAX_VALUE_BYTES": str(max(1 << 20, w.value_size)),
            },
            notes="gRPC over h2c; one worker thread per core (KVNODE_WORKERS).",
            build="docker build --target runtime -t kvnode:0.1.0 . (repository root)",
        ),
        # The two HTTP/2 engines of the same image, named explicitly so a
        # result never depends on which one is the default (ADR 0007). They
        # are variants: excluded from ALL, used by the N experiments only.
        System(
            name="kvnode-native", title="KvNode 0.1.0 native-h2", image="kvnode:0.1.0",
            protocol="grpc", port=50051,
            args=lambda t, w: [],
            env=lambda t, w: {
                "KVNODE_WORKERS": str(t),
                "KVNODE_CAPACITY": str(max(1000, int(w.keys * 1.25))),
                "KVNODE_MAX_VALUE_BYTES": str(max(1 << 20, w.value_size)),
                "KVNODE_H2_ENGINE": "native",
            },
            notes="KvNode with its own HTTP/2 engine (server/src/h2n.asm, KVNODE_H2_ENGINE=native).",
            build="docker build --target runtime -t kvnode:0.1.0 . (repository root)",
        ),
        System(
            name="kvnode-nghttp2", title="KvNode 0.1.0 nghttp2", image="kvnode:0.1.0",
            protocol="grpc", port=50051,
            args=lambda t, w: [],
            env=lambda t, w: {
                "KVNODE_WORKERS": str(t),
                "KVNODE_CAPACITY": str(max(1000, int(w.keys * 1.25))),
                "KVNODE_MAX_VALUE_BYTES": str(max(1 << 20, w.value_size)),
                "KVNODE_H2_ENGINE": "nghttp2",
            },
            notes="KvNode with nghttp2 1.70 as its HTTP/2 engine (KVNODE_H2_ENGINE=nghttp2).",
            build="docker build --target runtime -t kvnode:0.1.0 . (repository root)",
        ),
        System(
            name="redis", title="Redis 8.10.2", image="redis:8.10.2", protocol="resp", port=6379,
            args=lambda t, w: ["redis-server", "--save", "", "--appendonly", "no",
                               "--io-threads", str(t)],
            notes="Persistence off (--save '' --appendonly no); io-threads = tuned thread "
                  "count (commands still execute on the main thread).",
        ),
        System(
            name="valkey", title="Valkey 9.1.2", image="valkey/valkey:9.1.2", protocol="resp",
            port=6379,
            args=lambda t, w: ["valkey-server", "--save", "", "--appendonly", "no",
                               "--io-threads", str(t)],
            notes="Persistence off; io-threads = tuned thread count. 9.2 was only a "
                  "release candidate at test time.",
        ),
        System(
            name="dragonfly", title="Dragonfly 2.0.0", image="ghcr.io/dragonflydb/dragonfly:v2.0.0",
            protocol="resp", port=6379,
            args=lambda t, w: [f"--proactor_threads={t}", "--dbfilename="],
            docker_flags=("--ulimit", "memlock=-1"),
            notes="Snapshots off (--dbfilename=); proactor_threads = tuned thread count; "
                  "memlock unlimited as its docs require; io_uring is available on this "
                  "host (seccomp unconfined), so Dragonfly uses its preferred I/O engine.",
        ),
        System(
            name="garnet", title="Garnet 2.1.8", image="kvbench/garnet:2.1.8", protocol="resp",
            port=6379,
            args=lambda t, w: [],
            thread_knob=False,
            notes="Built from Microsoft's own Dockerfile at tag v2.1.8 (the newest published "
                  "image, 2.1.0, is eight releases older; 'latest' is an unreleased main "
                  "build). Defaults: AOF off; the .NET thread pool sizes itself from the "
                  "cpuset, so there is no thread knob to tune.",
            build="docker build -t kvbench/garnet:2.1.8 https://github.com/microsoft/garnet.git#v2.1.8",
        ),
        System(
            name="memcached", title="Memcached 1.6.45", image="memcached:1.6.45",
            protocol="memcache", port=11211,
            args=lambda t, w: ["-t", str(t), "-m", str(_memcached_mb(w)), "-c", "4096", "-U", "0"],
            notes="Meta protocol (mg/ms/mn); -t = tuned thread count; -m sized so nothing "
                  "is evicted; UDP off.",
        ),
    ]
}

VALIDATION_IMAGES = {
    "memtier": "redislabs/memtier_benchmark:2.5.1",
    "h2load": "kvnode-toolchain:latest",
}
"""Standard tools run against kvbench in the validation experiment."""

# ---------------------------------------------------------------------------
# Experiments
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Experiment:
    """A question and the run matrix that answers it.

    Attributes:
        id: short identifier (E0..E8).
        title: one-line name.
        question: what the experiment answers.
        systems: systems included.
        cores: server core budgets.
        workloads: client workloads.
        reps: repetitions per cell (each with a fresh server); repetition r
            of every cell runs in block r (see run.plan).
        warmup: seconds before measuring.
        duration: measured seconds.
        threads: "tuned" (E0's choice per system and core count) or
            "sweep" (E0 itself: N and N-1 for systems with a thread knob).
        kind: "kvbench" or "validation".
    """

    id: str
    title: str
    question: str
    systems: tuple[str, ...]
    cores: tuple[int, ...]
    workloads: tuple[Workload, ...]
    # 6, not 5: with blocked (paired) comparisons, 6 pairs is the smallest n
    # at which an exact two-sided sign test can reach p < 0.05 (2/64).
    reps: int = 6
    warmup: float = 5.0
    duration: float = 10.0
    threads: str = "tuned"
    kind: str = "kvbench"


VARIANTS = ("kvnode-native", "kvnode-nghttp2")
"""Configurations of a system already in the suite (KvNode's two HTTP/2
engines): measured by the N experiments only, so the general experiments do
not run KvNode three times."""
ALL = tuple(s for s in SYSTEMS if s not in VARIANTS)
BASE = Workload()  # 100% GET, 100-byte values, 1M keys, uniform, 64 conns x 16 deep
ENGINES = ("kvnode-native", "kvnode-nghttp2")

STANDARD: list[Experiment] = [
    Experiment(
        "E0", "Thread-count tuning",
        "Which thread count gives each system its best throughput for N cores (N or N-1)?",
        systems=tuple(s for s in ALL if SYSTEMS[s].thread_knob), cores=(2, 4, 8),
        workloads=(BASE,), reps=3, warmup=4, duration=8, threads="sweep"),
    Experiment(
        "E1", "Throughput scaling",
        "Peak GET throughput as the core budget grows from 1 to 8.",
        systems=ALL, cores=(1, 2, 4, 8), workloads=(BASE,)),
    Experiment(
        "E2", "Read/write mixes",
        "How do writes and skewed (Zipfian) access change throughput at 4 cores?",
        systems=ALL, cores=(4,),
        workloads=(replace(BASE, read_ratio=0.9), replace(BASE, read_ratio=0.5),
                   replace(BASE, read_ratio=0.0), replace(BASE, read_ratio=0.9, zipf=0.99))),
    Experiment(
        "E3", "No pipelining",
        "Throughput with one request in flight per connection (256 connections), at 4 cores.",
        systems=ALL, cores=(4,), workloads=(replace(BASE, connections=256, depth=1),)),
    Experiment(
        "E4", "Latency at fixed load",
        "Latency percentiles at identical offered loads (open loop, Poisson arrivals), "
        "at 25/50/75% of the slowest system's E1 peak at 4 cores.",
        systems=ALL, cores=(4,), workloads=(), reps=3),
    Experiment(
        "E5", "Per-request floor",
        "Cost of the cheapest round trip (PING / meta no-op / gRPC health check) on 1 core.",
        systems=ALL, cores=(1,), workloads=(replace(BASE, ping=True),)),
    Experiment(
        "E6", "Value size",
        "GET throughput with 1 KiB and 16 KiB values (about 1 GiB of data each), at 4 cores.",
        systems=ALL, cores=(4,),
        workloads=(replace(BASE, value_size=1024, keys=1_000_000),
                   replace(BASE, value_size=16384, keys=65_536)), reps=3),
    Experiment(
        "E7", "Concurrency sweep",
        "Throughput and latency from exactly 1 request in flight up to 1,024, at 4 cores: "
        "1x1, 16x1, 64x1, 64x4 and 64x16 connections x depth.",
        systems=ALL, cores=(4,),
        workloads=tuple(replace(BASE, connections=c, depth=d)
                        for c, d in ((1, 1), (16, 1), (64, 1), (64, 4), (64, 16))),
        reps=3),
    Experiment(
        "E8", "Client validation",
        "Is kvbench as efficient a client as each ecosystem's standard tool?",
        systems=("redis", "kvnode"), cores=(4,), workloads=(BASE,), reps=3, kind="validation"),
    Experiment(
        "N1", "Native HTTP/2 engine on 1 core",
        "Does KvNode's own HTTP/2 engine (h2n.asm) beat nghttp2 — and Redis and Valkey — on a "
        "single core? E1's workload.",
        systems=(*ENGINES, "redis", "valkey"), cores=(1,), workloads=(BASE,)),
    Experiment(
        "N2", "Native HTTP/2 engine scaling",
        "The two engines at 2 and 4 cores (E1's workload): does the gain hold as cores grow?",
        systems=ENGINES, cores=(2, 4), workloads=(BASE,)),
    Experiment(
        "N3", "Native HTTP/2 engine per-request floor",
        "E5's no-op round trip on 1 core with each engine, against PING on Redis and Valkey.",
        systems=(*ENGINES, "redis", "valkey"), cores=(1,), workloads=(replace(BASE, ping=True),)),
    Experiment(
        "S1", "Measurement stability",
        "Are the measurements repeatable? Ten fresh-server repetitions of E1's 4-core cell. "
        "It runs last, so comparing it with E1 also shows drift across the whole session.",
        systems=ALL, cores=(4,), workloads=(BASE,), reps=10),
]

E4_LOAD_FRACTIONS = (0.25, 0.5, 0.75)
"""E4 offered loads, as fractions of the slowest system's E1 median at 4 cores."""


def quick(experiments: list[Experiment]) -> list[Experiment]:
    """A fast smoke profile with the same structure (pipeline check only).

    Args:
        experiments: the standard profile.

    Returns:
        The same experiments with 1 repetition, short phases, smaller data
        and fewer core budgets — never use its numbers as results.
    """
    out = []
    for e in experiments:
        wls = tuple(replace(w, keys=min(w.keys, 100_000)) for w in e.workloads)
        out.append(replace(e, reps=1, warmup=1, duration=2, workloads=wls,
                           cores=tuple(c for c in e.cores if c <= 2) or e.cores[:1]))
    return out


PROFILES: dict[str, list[Experiment]] = {"standard": STANDARD, "quick": quick(STANDARD)}
