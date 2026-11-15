# ADR 0004: Exact keys on the server, long-key compaction in clients

- **Status:** accepted; amended once (see [Amendments](#amendments)):
  the placement hash is one 64-bit SipHash-2-4 word since 2026-11-15
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

## Amendments

### One 64-bit hash word places a key (2026-11-15)

Decision 2 named SipHash-2-4-**128**. The table takes two things from a
key's hash: a bucket index and a one-byte tag. They came from the two words
of the 128-bit output — the index from the low bits of the first, the tag
from the top byte of the second — but all they need is to be disjoint bits
of one pseudo-random value. One 64-bit word has them: the index uses at
most its low 34 bits (2^36 keys), the tag its top 8.

- **The change:** the server hashes keys with SipHash-2-4 and the standard
  64-bit output (`kv_siphash64`), under the same per-process secret key.
  `kv_store_hash` returns that word as both `h1` and `h2`, so no caller
  changed.
- **What it saves:** the second word cost four more finalization rounds on
  every Get, Put, Delete, Upload and Download. Measured
  (`benchmarks/results/2026-11-15-asm-audit`): 66.0 / 50.0 core cycles per
  16-byte key (as a dependency chain / when hashes overlap) against
  86.6 / 70.6, and 7.7% off a GET's user-space path.
- **What does not change:**
  - the defense: the same keyed function, key and rounds, so nobody outside
    the process can predict a bucket or a tag any better than before;
  - exactness: keys are still compared byte for byte (decision 1);
  - the client compaction rule (decision 3);
  - the snapshot checksum, which stays SipHash-2-4-128 under the all-zero
    key (`docs/PROTOCOL.md`): `kv_siphash128` remains for it.
- **Nothing stored or sent depends on it.** Placement is a function of the
  per-process key, which is never persisted.
- **Private caches** (ADR 0006, off by default) index their two levels by
  `h2`. Both indices now come from the same word as the bucket index. Every
  candidate is still confirmed by an exact key compare, so only their
  conflict pattern can differ.
- **Considered, not adopted here:**
  - SipHash-1-3 (43.2 / 28.2 cycles with 64 bits): fewer rounds are a
    smaller security margin, which is a separate decision;
  - AES-128 as the keyed hash through AES-NI (43.8 / 7.6 cycles, 19.3% off
    a GET's user-space path): the larger gain, but it needs CPUID dispatch
    with SipHash as the fallback and a key block per table, and it changes
    the primitive this ADR chose. Left to the owner.
