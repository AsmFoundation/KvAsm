# ADR 0004: Exact keys on the server, long-key compaction in clients

- **Status:** accepted
- **Date:** 2026-09-26

## Context

The original idea was to identify keys by two 16-byte hashes (split into
chained 8-byte parts), with clients hashing long keys themselves and the
server hashing short ones. Two concerns shaped the decision. First, "the
chance of two hashes colliding is too large": any hash-only identity can
silently return another key's value. Second, a hash computed by the client
decides where the server stores data.

## Decision

1. **The server stores and compares exact keys.** Hashes only choose a
   bucket and a tag; equality is a byte-exact comparison
   (`kv_keyeq`). A hash collision costs one extra compare. It can never
   return the wrong value.
2. **The server never trusts client-provided hashes.** It hashes every key
   with SipHash-2-4-128 under a secret per-process key. A client that chose
   bucket placement could fill one bucket pair on purpose and push the
   table into cuckoo-displacement failures (hash flooding).
3. **Clients compact long keys:** a key longer than 64 bytes is sent as
   `0xFF ‖ SHA-512(key)` (65 bytes). This keeps the useful half of the
   original idea, since long keys shrink before they cross the network, but
   the result is still an exact key to the server. Collisions between two
   long keys require a SHA-512 collision, and raw keys (at most 64 bytes)
   can never equal a compacted key (exactly 65 bytes).

## Consequences

- Every client language must implement the rule identically, and it is on
  by default. `docs/PROTOCOL.md` specifies it, and the .NET tests pin it,
  including a check that a raw-mode client finds the compacted key.
- The server cannot recover original long keys (it has no enumeration API
  anyway).
- The snapshot writer applies the same rule, so snapshot data and client
  lookups agree.
