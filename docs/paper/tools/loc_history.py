"""Line counts per artifact category at every commit of the KvNode repository.

Feeds the paper's development-timeline figure and its effort table: how the NASM
server, the tests, the .NET client, the benchmark tooling and the documentation grew
commit by commit. Blobs are streamed through one ``git cat-file --batch`` process, so
the whole history is read in a single pass instead of one ``git show`` per file.

Run from the repository root: ``python loc_history.py > loc_history.json``
"""

from __future__ import annotations

import json
import subprocess
import sys
from fnmatch import fnmatch

#: Category -> glob patterns (first match wins, in this order). Raw benchmark data
#: (``raw.jsonl``, SVG charts, logs) is deliberately not code and is not counted.
#: Footgun: ``fnmatch``'s ``*`` also matches ``/``, so a broad pattern such as
#: ``benchmarks/*.md`` would swallow every results README; the narrower
#: ``results_docs`` category must therefore come first.
CATEGORIES: list[tuple[str, tuple[str, ...]]] = [
    ("results_docs", ("benchmarks/results/*/README.md",)),
    ("asm", ("server/src/*.asm", "server/include/*.inc", "server/bench/ds/*.asm")),
    ("server_tests", ("server/tests/*", "server/tests/*/*")),
    ("server_bench_tools", ("server/bench/*.c", "server/tools/*", "server/Makefile")),
    ("dotnet", ("client/*.cs", "client/*/*.cs", "client/*/*/*.cs", "client/*/*/*/*.cs",
                "client/*/*/*/*/*.cs")),
    ("bench_harness", ("benchmarks/*.py", "benchmarks/kvbench/*")),
    ("docs", ("docs/*.md", "docs/*/*.md", "README.md", "benchmarks/*.md",
              "client/dotnet/README.md")),
]


def git(*args: str) -> str:
    """Run a git command in the current repository and return its stdout.

    Args:
        *args: Arguments after ``git``.

    Returns:
        Decoded standard output.

    Raises:
        subprocess.CalledProcessError: If git exits non-zero (e.g. not a repository).
    """
    return subprocess.run(("git",) + args, check=True, capture_output=True, text=True,
                          encoding="utf-8", errors="replace").stdout


def categorize(path: str) -> str | None:
    """Map a repository path to its artifact category.

    Args:
        path: A path as listed by ``git ls-tree`` (forward slashes).

    Returns:
        The first category whose patterns match, or ``None`` for paths that are
        not counted (raw results, images, configuration).
    """
    for name, patterns in CATEGORIES:
        if any(fnmatch(path, p) for p in patterns):
            return name
    return None


def main() -> int:
    """Print, for every commit (oldest first), lines per category as JSON.

    Returns:
        Process exit code 0.

    Raises:
        subprocess.CalledProcessError: If a git command fails.
        RuntimeError: If ``git cat-file --batch`` answers with an unexpected header,
            which would otherwise desynchronize every later blob.
    """
    # --first-parent follows the line HEAD was built on; without it, commits of a
    # parallel branch interleave by date and the series jumps back and forth.
    commits = git("log", "--first-parent", "--reverse", "--format=%H %cI %s").splitlines()
    cat = subprocess.Popen(("git", "cat-file", "--batch"), stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE)
    blob_lines: dict[str, int] = {}

    def lines_of(sha: str) -> int:
        """Count newline-terminated lines of a blob, caching by blob id.

        Args:
            sha: The blob's object id.

        Returns:
            Number of ``\\n`` bytes, plus one for a final unterminated line.
        """
        if sha in blob_lines:
            return blob_lines[sha]
        cat.stdin.write((sha + "\n").encode())
        cat.stdin.flush()
        header = cat.stdout.readline().split()
        if len(header) != 3 or header[1] != b"blob":
            raise RuntimeError(f"unexpected cat-file header for {sha}: {header!r}")
        size = int(header[2])
        data = cat.stdout.read(size)
        cat.stdout.read(1)  # the newline git appends after every object
        n = data.count(b"\n") + (1 if data and not data.endswith(b"\n") else 0)
        blob_lines[sha] = n
        return n

    out = []
    for line in commits:
        sha, when, subject = line.split(" ", 2)
        counts: dict[str, int] = {name: 0 for name, _ in CATEGORIES}
        for entry in git("ls-tree", "-r", sha).splitlines():
            meta, path = entry.split("\t", 1)
            _mode, kind, obj = meta.split()
            category = categorize(path)
            if kind == "blob" and category:
                counts[category] += lines_of(obj)
        out.append({"commit": sha[:7], "date": when, "subject": subject, **counts})
    cat.stdin.close()
    cat.wait()
    json.dump(out, sys.stdout, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
