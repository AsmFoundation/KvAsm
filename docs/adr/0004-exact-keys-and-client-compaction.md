# ADR 0004: Exact keys on the server, long-key compaction in clients

- **Status:** accepted; amended twice (see [Amendments](#amendments)),
  both on 2026-11-15: the placement hash needs one 64-bit word, and it
  is AES-128 wherever the CPU has AES-NI, with SipHash-2-4 as the
  fallback
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
- **Considered, not adopted:** SipHash-1-3 (43.2 / 28.2 cycles with 64
  bits). Fewer rounds are a smaller security margin for a smaller gain
  than the next amendment's.

### AES-128 is the key hash where the CPU has AES-NI (2026-11-15)

Decision 2 chose SipHash for one property: a keyed function whose output
nobody outside the process can predict. The owner decided to take that
property from AES-128 instead wherever the CPU offers it, on one condition:
no other operation may be affected.

- **The hash** (`server/src/aeshash.asm`), shaped like CMAC (NIST SP
  800-38B):
  - a key of up to 16 bytes: `AES_K(pad0(key) XOR mask[len])`, one
    encryption;
  - a longer key: chained in 16-byte blocks (`s = AES_K(block XOR s)`),
    the last block XORed with the mask of its length before the final
    encryption;
  - `K` is the per-process secret — the same 16 random bytes that key
    SipHash, expanded to the AES-128 round keys — and `mask[n] = AES_K(n +
    1)` are 17 secret blocks, one per final length, so "ab" and "ab\0"
    start from unrelated blocks.
- **Why it is at least as safe:** AES-128 under a secret key is a
  pseudo-random permutation, and CMAC over it a pseudo-random function —
  the standard argument, for a primitive with far more analysis behind it
  than any hash-table hash. It is all ten rounds: reduced-round "AES-like"
  table hashes are a different thing. AES-NI executes in constant time.
  The key and the masks never leave the process.
- **Which hash runs:** `KVNODE_HASH=auto` (the default) takes AES when
  CPUID reports AES-NI and SSSE3, and SipHash-2-4 (one 64-bit word, the
  amendment above) otherwise. `siphash` and `aes` force one; forcing `aes`
  on a CPU without AES-NI refuses to start (exit 2). The startup line names
  the hash in force. Every x86-64 server CPU since 2010–2011 has AES-NI.
- **A key block per table** (448 bytes: 11 round keys and the 17 masks),
  derived from the seed at creation whenever the CPU can run AES, so the
  process-wide mode never meets a table without one. The mode is set once,
  before any key is hashed: a key hashes to different buckets under the two
  hashes.
- **The "no SIMD requirement" rule holds:** nothing requires AES-NI. Like
  the SSE2 tag scan and the CRC-32C paths, it is used when present. It is
  legacy-encoded SSE: no AVX state and no frequency license.
- **Measured** (`benchmarks/results/2026-11-15-asm-audit`):
  - per 16-byte key, in core cycles: 43.8 as a dependency chain and 7.6
    when hashes overlap, against 66.0 and 50.0 for SipHash-2-4;
  - the request path in process (no kernel, keys in cache): a GET
    103.3 → 88.3 ns (−14.5%); a health check, which hashes nothing,
    59.5 → 59.2 ns;
  - instructions per request (callgrind): a GET 1,292 → 1,134, a SET
    1,596 → 1,438;
  - end to end, one worker, against the 64-bit SipHash build: server CPU
    per GET 0.953× with busy-polling clients (3 of 4 quads, 0.857–1.165)
    and 0.966× with blocking clients (3 of 3, 0.943–0.986).
- **The owner's condition — no other operation affected:**
  each measured as server CPU per request, AES against the 64-bit
  SipHash build, one worker, quads of A B B A:
  - 100% SET: 0.989× (0.973–1.004);
  - 50% GET / 50% SET: 0.984× (0.970–0.994);
  - Upload of 65,536-byte values: 0.992× (0.972–1.006);
  - Download of 65,536-byte values: 1.001× (0.961–1.039);
  - a health check hashes nothing and runs the same instructions.

  None is slower beyond the runs' own spread; the operations that hash a
  key gain a little.
- **Verified:** `test_aeshash` checks the assembly against an AES-128
  written from FIPS-197 in C (the Appendix C.1 vector, the key schedule,
  every key length 0–300, guard pages for overreads) and stores and finds
  20,000 keys in two tables under each hash. The end-to-end suite runs
  under AES; its phase 2b forces SipHash, so the fallback stays proven.
