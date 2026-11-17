#!/usr/bin/env python3
"""gen_huffman.py — generate server/include/huffman.inc, the HPACK Huffman decoder table.

Usage:
    python3 server/tools/gen_huffman.py > server/include/huffman.inc

HPACK (RFC 7541, Appendix B) defines a static Huffman code of 257 symbols
(bytes 0..255 and EOS = 256). That code is *canonical*: sorted by (code
length, symbol), each code is the previous code plus one, shifted left when
the length grows. So the 257 code lengths below determine every code. The
generator reconstructs the codes, checks them against codes printed in the
RFC, and builds the decoder that h2n's HPACK decoder (hpack.asm) runs.

Decoder: a finite-state machine that consumes 4 bits at a time.
  * States are the 256 internal nodes of the code tree (0 = root).
  * For each state and each 4-bit input there is one entry with the next
    state and three flags:
        ACCEPTED (1)  the bits since the last symbol are a valid end of a
                      string: all ones and at most 7 bits (RFC 7541 §5.2 —
                      padding must be a prefix of EOS, shorter than a byte)
        SYM      (2)  a symbol was completed; it is in the entry's top byte
        FAIL     (4)  EOS was decoded, which the RFC forbids inside a string
  * Codes are at least 5 bits long, so one nibble completes at most one
    symbol.
Entry layout (one dword): bits 0-15 next state, 16-23 flags, 24-31 symbol.
256 states x 16 entries x 4 bytes = 16 KiB.

The table is checked before it is written:
  * every symbol round-trips through encode (canonical codes) + FSM decode;
  * RFC 7541 Appendix C.4 / C.6 Huffman vectors decode to their strings;
  * padding rules: 7 one-bits accepted, 8 rejected, zero padding rejected,
    EOS inside a string rejected.
Any failed check aborts with a non-zero exit and writes nothing.
The output is committed, so building KvAsm never needs Python.
"""

import sys

# Code length of symbols 0..256 (RFC 7541 Appendix B, "len in bits").
LENGTHS = [
    13, 23, 28, 28, 28, 28, 28, 28, 28, 24, 30, 28, 28, 30, 28, 28,
    28, 28, 28, 28, 28, 28, 30, 28, 28, 28, 28, 28, 28, 28, 28, 28,
    6, 10, 10, 12, 13, 6, 8, 11, 10, 10, 8, 11, 8, 6, 6, 6,
    5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 8, 15, 6, 12, 10,
    13, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 8, 13, 19, 13, 14, 6,
    15, 5, 6, 5, 6, 5, 6, 6, 6, 5, 7, 7, 6, 6, 6, 5,
    6, 7, 6, 5, 5, 6, 7, 7, 7, 7, 7, 15, 11, 14, 13, 28,
    20, 22, 20, 20, 22, 22, 22, 23, 22, 23, 23, 23, 23, 23, 24, 23,
    24, 24, 22, 23, 24, 23, 23, 23, 23, 21, 22, 23, 22, 23, 23, 24,
    22, 21, 20, 22, 22, 23, 23, 21, 23, 22, 22, 24, 21, 22, 23, 23,
    21, 21, 22, 21, 23, 22, 23, 23, 20, 22, 22, 22, 23, 22, 22, 23,
    26, 26, 20, 19, 22, 23, 22, 25, 26, 26, 26, 27, 27, 26, 24, 25,
    19, 21, 26, 27, 27, 26, 27, 24, 21, 21, 26, 26, 28, 27, 27, 27,
    20, 24, 20, 21, 22, 21, 21, 23, 22, 22, 25, 25, 24, 24, 26, 23,
    26, 27, 26, 26, 27, 27, 27, 27, 27, 28, 27, 27, 27, 27, 27, 26,
    30,
]
EOS = 256
ACCEPTED, SYM, FAIL = 1, 2, 4

# Codes as printed in RFC 7541 Appendix B (hex, LSB-aligned), spot checks.
RFC_CODES = {
    0: 0x1FF8, 32: 0x14, 48: 0x0, 49: 0x1, 97: 0x3, 101: 0x5, 58: 0x5C,
    127: 0xFFFFFFC, 255: 0x3FFFFEE, EOS: 0x3FFFFFFF,
}

# (Huffman bytes, decoded string) from RFC 7541 Appendix C.4 and C.6.
RFC_VECTORS = [
    ("f1e3c2e5f23a6ba0ab90f4ff", b"www.example.com"),
    ("a8eb10649cbf", b"no-cache"),
    ("25a849e95ba97d7f", b"custom-key"),
    ("25a849e95bb8e8b4bf", b"custom-value"),
    ("6402", b"302"),
    ("aec3771a4b", b"private"),
    ("d07abe941054d444a8200595040b8166e082a62d1bff", b"Mon, 21 Oct 2013 20:13:21 GMT"),
    ("9d29ad171863c78f0b97c8e9ae82ae43d3", b"https://www.example.com"),
]


def fail(msg: str) -> None:
    """Aborts generation.

    Args:
        msg: what went wrong.

    Raises:
        SystemExit: always (exit status 1, nothing written).
    """
    print(f"gen_huffman: {msg}", file=sys.stderr)
    raise SystemExit(1)


def canonical_codes() -> list[int]:
    """Reconstructs the canonical code of every symbol from LENGTHS.

    Returns:
        codes[symbol] as an integer holding LENGTHS[symbol] bits.
    """
    if len(LENGTHS) != 257:
        fail(f"{len(LENGTHS)} lengths, expected 257")
    order = sorted(range(257), key=lambda s: (LENGTHS[s], s))
    codes = [0] * 257
    code, prev = 0, LENGTHS[order[0]]
    for k, sym in enumerate(order):
        if k:
            code = (code + 1) << (LENGTHS[sym] - prev)
        codes[sym] = code
        prev = LENGTHS[sym]
    if code != (1 << LENGTHS[order[-1]]) - 1:
        fail("the lengths do not form a complete prefix code")
    return codes


def build_tree(codes: list[int]) -> tuple[list[list], list[int], list[bool]]:
    """Builds the code tree.

    Args:
        codes: canonical codes.

    Returns:
        (children, depth, all_ones) per internal node: children[n][bit] is
        ("n", node) or ("s", symbol); all_ones[n] tells whether the path from
        the root to n consists of one-bits only.
    """
    children: list[list] = [[None, None]]
    depth, ones = [0], [True]
    for sym in range(257):
        n, bits = 0, LENGTHS[sym]
        for i in range(bits - 1, -1, -1):
            bit = (codes[sym] >> i) & 1
            if i == 0:
                if children[n][bit] is not None:
                    fail(f"code of symbol {sym} collides")
                children[n][bit] = ("s", sym)
            else:
                nxt = children[n][bit]
                if nxt is None:
                    children.append([None, None])
                    depth.append(depth[n] + 1)
                    ones.append(ones[n] and bit == 1)
                    nxt = ("n", len(children) - 1)
                    children[n][bit] = nxt
                elif nxt[0] != "n":
                    fail(f"code of symbol {sym} passes through a leaf")
                n = nxt[1]
    if len(children) != 256:
        fail(f"{len(children)} internal nodes, expected 256")
    return children, depth, ones


def build_fsm(children, depth, ones) -> list[list[int]]:
    """Builds the 4-bit decoding table.

    Returns:
        table[state][nibble] as packed dwords (see the module docstring).
    """
    table = []
    for state in range(256):
        row = []
        for nib in range(16):
            cur, sym, flags = state, 0, 0
            for i in (3, 2, 1, 0):
                kind, val = children[cur][(nib >> i) & 1]
                if kind == "s":
                    if val == EOS:
                        flags = FAIL
                        cur = 0
                        break
                    sym, flags, cur = val, SYM, 0
                else:
                    cur = val
            if not flags & FAIL and ones[cur] and depth[cur] <= 7:
                flags |= ACCEPTED
            row.append(cur | (flags << 16) | (sym << 24))
        table.append(row)
    return table


def decode(table: list[list[int]], data: bytes) -> bytes | None:
    """Runs the FSM exactly as hpack.asm does.

    Args:
        table: decoding table.
        data: Huffman-coded bytes.

    Returns:
        Decoded bytes, or None when the input is invalid.
    """
    out, state, accepted = bytearray(), 0, True
    for byte in data:
        for nib in (byte >> 4, byte & 15):
            entry = table[state][nib]
            flags = (entry >> 16) & 0xFF
            if flags & FAIL:
                return None
            if flags & SYM:
                out.append(entry >> 24)
            state, accepted = entry & 0xFFFF, bool(flags & ACCEPTED)
    return bytes(out) if accepted else None


def encode(codes: list[int], data: bytes, pad_bits: int | None = None) -> bytes:
    """Huffman-encodes bytes; pads with one-bits to a byte boundary.

    Args:
        codes: canonical codes.
        data: input.
        pad_bits: force this many trailing padding bits (tests of invalid
            padding); None pads minimally.

    Returns:
        The encoded bytes.
    """
    acc, n = 0, 0
    for byte in data:
        acc, n = (acc << LENGTHS[byte]) | codes[byte], n + LENGTHS[byte]
    pad = (-n) % 8 if pad_bits is None else pad_bits
    acc, n = (acc << pad) | ((1 << pad) - 1), n + pad
    if n % 8:
        fail("encode produced a partial byte")
    return acc.to_bytes(n // 8, "big")


def main() -> int:
    """Generates, verifies and prints the table.

    Returns:
        0 on success (verification failures exit through fail()).
    """
    codes = canonical_codes()
    for sym, code in RFC_CODES.items():
        if codes[sym] != code:
            fail(f"symbol {sym}: code {codes[sym]:#x}, RFC says {code:#x}")
    table = build_fsm(*build_tree(codes))
    for sym in range(256):
        data = bytes([sym, (sym * 7 + 3) & 0xFF, sym])
        if decode(table, encode(codes, data)) != data:
            fail(f"round trip failed for symbol {sym}")
    for hexstr, text in RFC_VECTORS:
        if decode(table, bytes.fromhex(hexstr)) != text:
            fail(f"RFC vector {text!r} did not decode")
        if encode(codes, text).hex() != hexstr:
            fail(f"RFC vector {text!r} did not encode")
    # Padding rules (RFC 7541 §5.2): 'a' is 5 bits (00011).
    if decode(table, bytes([0b00011111])) != b"a":
        fail("3 one-bits of padding were rejected")
    if decode(table, encode(codes, b"a", pad_bits=3 + 8)) is not None:
        fail("11 bits of padding (more than 7) were accepted")
    if decode(table, bytes([0b00011000])) is not None:
        fail("zero-bit padding was accepted")
    if decode(table, b"\xff\xff\xff\xff") is not None:  # EOS (30 ones) + 2 ones
        fail("EOS inside a string was accepted")

    # LF on every platform: Windows' text-mode stdout would emit CRLF, and the
    # repository's sources are LF (the output must be byte-identical anywhere).
    sys.stdout.reconfigure(newline="\n")
    w = sys.stdout.write
    w("; =============================================================================\n")
    w("; huffman.inc — HPACK Huffman decoding table (RFC 7541 Appendix B).\n")
    w("; GENERATED by server/tools/gen_huffman.py — do not edit; regenerate instead.\n")
    w(";\n")
    w("; huff_dec[state * 16 + nibble] = next | flags << 16 | symbol << 24\n")
    w(";   flags: 1 = ACCEPTED (valid end of string), 2 = SYM (symbol completed),\n")
    w(";          4 = FAIL (EOS decoded). State 0 is the root. 256 x 16 dwords.\n")
    w("; Verified at generation: all symbols round-trip, RFC 7541 C.4/C.6 vectors\n")
    w("; decode, over-long / zero-bit padding and EOS are rejected.\n")
    w("; =============================================================================\n")
    w("%define HUFF_ACCEPTED   1\n%define HUFF_SYM        2\n%define HUFF_FAIL       4\n\n")
    w("align 64\nhuff_dec:\n")
    for state, row in enumerate(table):
        w(f"    dd {', '.join(f'0x{e:08X}' for e in row)} ; {state}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
