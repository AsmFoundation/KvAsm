#!/usr/bin/env python3
"""cgcount.py — per-request user-space instruction counts from a callgrind dump.

Usage: cgcount.py DUMP NM
  DUMP  a callgrind output file (counters zeroed before the workload)
  NM    `nm -n` of the same binary

Prints total instructions (Ir), the number of requests, Ir per request, and
per-request call counts of the functions the second pass targets.

Why the NM file: callgrind resolves KvNode's file and line (DWARF) but not
its function names — NASM's DWARF has no function entries — so every
function appears as its link-time address ("fn=(id) 0x...cf20"). Those are
mapped back to the enclosing symbol here (nearest preceding text symbol that
is not a local or macro label).

Counting: callgrind records each call edge as a `cfn=` line followed by a
`calls=N` line; summing N per callee gives exact call counts. A function
entered by a tail `jmp` is not a call edge, so requests are counted as calls
of get_execute (GET) or kv_store_put (SET) or as the ping reply count via
kv_h2n_reply_parts — whichever the workload makes one-per-request — given
on the command line as the third argument.
"""
import bisect
import collections
import re
import sys

dump, nmfile, per = sys.argv[1], sys.argv[2], sys.argv[3]

addrs, syms = [], []
for line in open(nmfile):
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTiW" and not parts[2].startswith(("..@", ".")):
        if "." in parts[2]:
            continue                       # NASM local labels: func.label
        addrs.append(int(parts[0], 16))
        syms.append(parts[2])


def resolve(name):
    """Maps '0x...' (or a real name) to the enclosing function symbol."""
    m = re.fullmatch(r"0x([0-9a-f]+)", name.strip())
    if not m:
        return name.strip()
    a = int(m.group(1), 16)
    i = bisect.bisect_right(addrs, a) - 1
    return syms[i] if i >= 0 else name


names = {}
calls = collections.Counter()
total = 0
cfn = None
for line in open(dump, errors="replace"):
    if line.startswith(("summary:", "totals:")):
        total = int(line.split()[1])
    elif line.startswith(("cfn=", "fn=")):
        key = "cfn=" if line.startswith("cfn=") else "fn="
        m = re.match(re.escape(key) + r"\((\d+)\)(?: (.*))?", line.strip())
        if m:
            if m.group(2):
                names[m.group(1)] = resolve(m.group(2))
            if key == "cfn=":
                cfn = names.get(m.group(1), m.group(1))
    elif line.startswith("calls=") and cfn is not None:
        calls[cfn] += int(line.split("=")[1].split()[0])
        cfn = None

reqs = calls.get(per, 0)
print(f"instructions {total:,}  requests ({per} calls) {reqs:,}  "
      f"Ir/request {total / reqs if reqs else float('nan'):,.0f}")
for fn in ("kv_pool_malloc", "kv_pool_free", "kv_pool_calloc", "memset", "__memset_avx2_unaligned_erms",
           "kv_h2n_reply_parts", "kv_h2n_reply_buf", "stream_open", "kv_store_put", "get_execute"):
    if calls.get(fn) and reqs:
        print(f"  {fn:32s} {calls[fn] / reqs:6.3f} calls/request")
