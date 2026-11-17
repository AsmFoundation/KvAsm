# Security policy

## Supported versions

KvAsm is pre-1.0. Security fixes go into the latest minor release, and are
then released as a new patch version.

| Version | Supported |
|---|---|
| 0.2.x | yes |
| 0.1.0 | no (never released; the tag marks the first working server) |

## Reporting a vulnerability

**Do not open a public issue for a vulnerability.** Report it privately:

1. Use GitHub's private reporting form: the repository's **Security** tab,
   then **Report a vulnerability**
   (<https://github.com/AsmFoundation/KvAsm/security/advisories/new>). Only the
   maintainers can read the report.
2. Include:
   - the version, or commit and image digest;
   - the configuration (`KVASM_*` variables);
   - the server's log lines;
   - a reproduction. For protocol bugs, raw bytes or an `h2spec`-style
     case are ideal.

We aim to acknowledge a report within 7 days, and to have a fix or a
mitigation plan within 30 days of the acknowledgement. KvAsm is maintained
by one person, so a release can take longer; we will tell you if it does.
We will agree on a disclosure date with you and credit you in the advisory,
unless you prefer not to be named.

## Threat model

Know what KvAsm protects against before you deploy it
([docs/OPERATIONS.md](docs/OPERATIONS.md#security-notes)).

**KvAsm trusts its network.**

- It speaks cleartext HTTP/2 (h2c) and has no authentication or
  authorization. Anyone who can reach the port can read, write and delete
  any key.
- Expose it only on a trusted network, or behind a TLS-terminating proxy
  or service mesh that decides who connects.
- Reading or changing data through an open port is therefore not a
  vulnerability. It is the documented design.

**In scope: what we treat as vulnerabilities.**

- **Memory corruption from untrusted input:** anything a client can send
  that makes the server read or write out of bounds, crash, hang, or
  return another key's bytes. The HTTP/2 frames, HPACK blocks, gRPC
  messages, protobuf and snapshot files are all parsed in assembly.
- **Resource exhaustion beyond the configured limits.**
  - Memory, connections, streams and CPU must stay bounded by `KVASM_*`
    settings such as `KVASM_MAX_MEMORY_MB`, `KVASM_CONN_BUFFER_MB` and
    `KVASM_MAX_STREAMS`.
  - The HTTP/2 engine's flood defenses must hold: rapid-reset limits
    (CVE-2023-44487), CONTINUATION caps, and the refusal of oversized
    frames.
- **Hash flooding:** any way for clients to influence bucket placement
  despite the per-process hash secret (the AES-128 or SipHash-2-4 key).
- **Torn or mixed values:** a reader that observes part of one value and
  part of another, or a value never written.
- **Container escape or privilege issues** in the published image. The
  image is `FROM scratch` and runs as a non-root user. It needs no
  capabilities and no writable file system: `--read-only --cap-drop ALL
  --security-opt no-new-privileges` all work.

**Out of scope.**

- Attacks that need access to the host or the container runtime.
- Denial of service by a client that stays within configured limits that
  are too generous for the deployment. Tune the limits instead.
- Bugs in the benchmark harness, the paper tooling or the test clients.
  Report those as regular issues.

## How the server is hardened

- **Every build runs the conformance and fuzzing checks.** h2spec must pass
  146/146. A fuzzer drives 3,000 connections of malformed, truncated and
  mutated HTTP/2. A differential test checks the HPACK decoder against
  nghttp2's. The image is only built when all of them pass.
- **The binary is hardened.** It is a static PIE (ASLR kept) with full
  RELRO, a non-executable stack, and no writable code
  (`-z text`, `server/Makefile`).
- **Strict configuration.** An invalid setting stops startup (exit 2)
  instead of falling back to a default.
- **Snapshots are verified before use.** The size, the checksum and every
  record's bounds are checked before any record is stored.

Third-party code is limited to glibc and libgcc
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). A glibc security
update reaches users through a rebuilt image. Releases are built from the
current Debian trixie packages.
