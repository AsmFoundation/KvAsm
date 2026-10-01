"""Section sizes of ELF64 objects and executables, without binutils.

The paper quotes how much machine code KvNode's assembly actually is, apart from the
statically linked glibc that dominates the 1.1 MB binary. Git Bash on Windows has no
``size``/``readelf``, and the Linux dev container may be busy with other sessions'
benchmarks, so this reads the section header table directly.

Run: ``python elf_sizes.py <file.o|binary> ...`` (prints JSON).
"""

from __future__ import annotations

import json
import struct
import sys

SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8


def sections(path: str) -> list[dict]:
    """List the sections of an ELF64 little-endian file.

    Args:
        path: An x86-64 relocatable object or executable.

    Returns:
        One dict per section with its name, type, flags and size in bytes.

    Raises:
        ValueError: If the file is not ELF64 little-endian (the only format the
            KvNode toolchain emits); parsing anything else would misread offsets.
        OSError: If the file cannot be read.
    """
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: not an ELF64 little-endian file")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    headers = []
    for i in range(shnum):
        name_off, sh_type, flags, _addr, offset, size = struct.unpack_from(
            "<IIQQQQ", data, shoff + i * shentsize)
        headers.append((name_off, sh_type, flags, offset, size))
    # Section names live in the string table section named by e_shstrndx.
    str_off = headers[shstrndx][3]

    def name_at(off: int) -> str:
        """Read a NUL-terminated section name from the section-name string table.

        Args:
            off: Offset of the name inside the string table.

        Returns:
            The decoded name.
        """
        end = data.index(b"\x00", str_off + off)
        return data[str_off + off:end].decode("ascii", "replace")

    return [{"name": name_at(n), "type": t, "flags": f, "size": s}
            for n, t, f, _o, s in headers]


def summarize(path: str) -> dict:
    """Classify a file's allocated sections into code, read-only data, data and bss.

    Args:
        path: The ELF file to measure.

    Returns:
        Byte totals: ``text`` (executable), ``rodata`` (allocated, not writable,
        not executable), ``data`` (writable, with file contents) and ``bss``
        (writable, NOBITS). Relocatable objects mark sections ALLOC too, so the
        same classification works for ``.o`` files and the final binary.
    """
    totals = {"text": 0, "rodata": 0, "data": 0, "bss": 0}
    for sec in sections(path):
        flags = sec["flags"]
        if not flags & SHF_ALLOC:
            continue  # debug info, symbol tables, relocations: not loaded
        if flags & SHF_EXECINSTR:
            totals["text"] += sec["size"]
        elif not flags & SHF_WRITE:
            totals["rodata"] += sec["size"]
        elif sec["type"] == SHT_NOBITS:
            totals["bss"] += sec["size"]
        else:
            totals["data"] += sec["size"]
    return totals


def main(argv: list[str]) -> int:
    """Print per-file section totals and their sum as JSON.

    Args:
        argv: Paths of ELF files.

    Returns:
        0 on success, 2 when no file is given.
    """
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    per_file = {p: summarize(p) for p in argv}
    total = {k: sum(v[k] for v in per_file.values()) for k in ("text", "rodata", "data", "bss")}
    print(json.dumps({"files": per_file, "total": total}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
