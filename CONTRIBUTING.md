# Contributing to KvNode

Thank you for helping. KvNode is a key-value server in x86-64 assembly.
Assembly has no type system, no bounds checks and no sanitizers, so the
project relies on two habits:

- **Correctness comes from independent checks.** Conformance suites,
  differential oracles, fuzzing and end-to-end tests against the real
  binary. One author's understanding of the protocol is not a check.
- **Performance comes from measurement.** A change that is supposed to be
  faster ships with a paired measurement that shows it.

This file explains how to build, test and measure, and what a pull request
needs.

## Build and test

Everything builds in Docker. The `toolchain` stage of the
[Dockerfile](Dockerfile) holds:

- NASM and GCC (Debian trixie);
- nghttp2 1.70.0, pinned and verified;
- h2spec 2.6.0.

```sh
# The whole server pipeline: assemble, unit tests, bake-off checks, the
# end-to-end suite (h2spec, fuzzing, concurrency, streaming, shutdown,
# snapshots). The image exists only if every suite passed.
docker build --target runtime -t kvnode:0.2.0 .

# Faster iterations in a long-lived dev container. On Windows Git Bash,
# set MSYS_NO_PATHCONV=1 so /src paths are not rewritten.
docker build --target toolchain -t kvnode-toolchain .
docker run -d --name kvnode-dev -v "$PWD:/src" -w /src kvnode-toolchain sleep infinity
docker exec kvnode-dev bash -c 'cd /src/server && make all test itest'

# The .NET client and its tests (run from client/dotnet: its global.json
# selects the Microsoft.Testing.Platform runner).
cd client/dotnet
KVNODE_REQUIRE_DOCKER=1 dotnet test --solution KvNode.Client.slnx

# The benchmark client (benchmarks/kvbench), with its unit tests.
docker build --target kvbench -t kvnode-kvbench .
```

The .NET integration tests start the `kvnode:0.2.0` image through
Testcontainers. `KVNODE_IMAGE` selects another tag. `KVNODE_REQUIRE_DOCKER=1`
turns a missing Docker into a failure instead of skipped tests.

## The gates every change passes

CI ([.github/workflows/ci.yml](.github/workflows/ci.yml)) runs all of these
on every push and pull request. Run them locally before you ask for a
review.

1. **`make all test itest`** in `server/`, through the Docker build:
   - native unit tests, including the multi-threaded store torture test;
   - the data-structure bake-off in its checking mode;
   - the end-to-end suite against the real binary. Its concurrency phases
     must report `errors 0, torn 0`.
2. **h2spec 146/146.** `server/tests/integration/h2spec-known-failures.txt`
   is empty, and it stays empty.
3. **Fuzzing.** Every build runs `fuzz_h2` over 3,000 connections.
   - A change to `h2n.asm`, `hpack.asm`, `grpc.asm` or `proto.asm` also
     needs a soak: `make itest FUZZ_ITERATIONS=100000 FUZZ_SEED=<n>`.
   - Name the seed in the pull request.
4. **The HPACK differential test** (`test_hpack`) against nghttp2's decoder.
5. **The .NET suite** with `KVNODE_REQUIRE_DOCKER=1`, all 380 tests or
   more.
6. **No warnings anywhere.** NASM runs with `-w+error`, C with `-Werror`,
   and .NET with `TreatWarningsAsErrors`.

**Regression tests must fail first.** A test written for a bug must fail
against the code before the fix. Check it by reverting the fix (a mutation
check) and say so in the pull request. Two plausible tests in this
repository's history passed against the broken code. Only the third
reproduced the bug.

## Performance changes

A change made for speed is accepted on a measurement, not on an argument.
[benchmarks/METHODOLOGY.md](benchmarks/METHODOLOGY.md) has the details. In
short:

- **Profile first.** Estimate the ceiling (Amdahl) before writing code.
  [The cookbook](.claude/skills/asm-perf-cookbook/SKILL.md) collects the
  techniques already tried here, with their measured outcomes.
- **Run a paired A/B.**
  - Run both variants interleaved in rotated order, or better, at the same
    time on separate pinned cores.
  - Use at least 6 blocks, and report the sign test (6/6 gives p = 0.031).
- **Check the host.** Other programs on the same machine were this
  project's largest source of noise. Record the outside load (`run.py`
  does) and never compare unpaired single runs.
- **Record the result:**
  - in a results README under `benchmarks/results/<date>-<topic>/`, with
    the scripts that produced it;
  - in the ADR the change belongs to;
  - in the cookbook, as applied, within noise, or rejected, with the reason.
- **Keep the claims true.** If a number in
  [docs/CLAIMS.md](docs/CLAIMS.md) or the README moves, update it in the
  same pull request.

Rejected ideas are results too. Several are recorded in the ADRs and the
cookbook, with the numbers that rejected them.

## Writing code

- **Assembly** (NASM, `server/src`, `server/include`):
  - Every file starts with a header: what it does, why it is built this
    way, and its threading rules.
  - Every function has a block comment with `@param` for each register
    argument, `@returns`, and `@remarks` for consequences and pitfalls.
    [`crc32c.asm`](server/src/crc32c.asm) is a good model.
  - An inline comment says *why*: validation order, ownership and
    lifetime, memory ordering, cleanup on failure. It never restates the
    instruction.
  - Layouts shared with C tests are mirrored in
    `server/tests/kvnode_abi.h`, and `test_abi` checks them.
- **C** (tests, tools, `kvbench`): C11 with `-Wall -Wextra -Werror`.
- **C#** (`client/dotnet`):
  - XML documentation on every member: each parameter, the return value
    and every exception;
  - nullable enabled;
  - analyzers at `latest`;
  - central package versions in `Directory.Packages.props`.
- **Formatting:** [.editorconfig](.editorconfig). Line endings are LF
  everywhere ([.gitattributes](.gitattributes)), because the build runs in
  Linux containers.

## Design decisions

- **Decision records.** A decision with alternatives gets a record in
  [docs/adr](docs/adr): context, decision, rejected alternatives with their
  numbers, and verification. Number it after the last one. An ADR is never
  rewritten afterwards: a correction goes into a status note or into a
  later ADR.
- **Proposals.** Designs that are not decided live in
  [docs/proposals](docs/proposals). The functions plan
  ([functions-plan.md](docs/proposals/functions-plan.md)) is the single plan:
  - new functions are merged into it, not proposed in a parallel document;
  - its evidence stays in benchmark READMEs.
- **The wire protocol** is `kvnode.v1`
  ([proto](proto/kvnode/v1/kvnode.proto), [docs/PROTOCOL.md](docs/PROTOCOL.md)).
  It only grows in backward-compatible ways: new fields, new RPCs. Clients
  in other languages must pass the conformance vectors that PROTOCOL.md
  names.

## Pull requests

- **One logical change per pull request,** with a description that says
  what changed, why, and how it was verified (the gates above, plus any
  measurement).
- **Commit messages** explain the reasoning and name the evidence: the ADR,
  the results directory, and the numbers.
  - Published history is never rewritten.
  - A wrong claim in an earlier commit is corrected forward, in a later
    commit.
- **Licensing.** By contributing, you agree that your contribution is
  licensed under the [Apache License 2.0](LICENSE), as its section 5
  provides. There is no separate contributor agreement.

AI-assisted contributions are welcome under the same gates. KvNode itself
was built that way ([docs/paper](docs/paper/README.md)). The gates do not
care who wrote the code, only whether it survives them.

## Reporting bugs and security issues

- **Bugs:** open an issue with the template. Include the server's log
  lines (they are structured, one event per line), the configuration
  variables, and a minimal reproduction.
- **Security:** never report a vulnerability in a public issue. Follow
  [SECURITY.md](SECURITY.md).

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).
