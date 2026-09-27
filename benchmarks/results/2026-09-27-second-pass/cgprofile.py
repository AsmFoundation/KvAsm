#!/usr/bin/env python3
"""cgprofile.py — self instructions per request, per function, from a callgrind dump.

Usage: cgprofile.py DUMP NM PER_REQUEST_FUNCTION [TOP]

Self cost = the instructions executed in a function's own code. In the
callgrind format a function block (`fn=`) is followed by cost lines
(`<position> <Ir>`); a `calls=` line announces a call, and the cost line
right after it is that call's INCLUSIVE cost, which is skipped here.
Addresses are mapped to symbols with `nm -n` (see cgcount.py for why).
"""
import bisect
import collections
import re
import sys

dump, nmfile, per = sys.argv[1], sys.argv[2], sys.argv[3]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 25

addrs, syms = [], []
for line in open(nmfile):
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTiW" and "." not in parts[2]:
        addrs.append(int(parts[0], 16))
        syms.append(parts[2])


def resolve(name):
    """'0x...' or a name -> enclosing function symbol."""
    m = re.fullmatch(r"0x([0-9a-f]+)", name.strip())
    if not m:
        return name.strip()
    i = bisect.bisect_right(addrs, int(m.group(1), 16)) - 1
    return syms[i] if i >= 0 else name


names, selfc, calls = {}, collections.Counter(), collections.Counter()
fn = cfn = None
skip_next = False
for raw in open(dump, errors="replace"):
    line = raw.strip()
    if not line:
        continue
    if line.startswith(("fn=", "cfn=")):
        key = "cfn=" if line.startswith("cfn=") else "fn="
        m = re.match(re.escape(key) + r"\((\d+)\)(?: (.*))?", line)
        if m:
            if m.group(2):
                names[m.group(1)] = resolve(m.group(2))
            if key == "fn=":
                fn = names.get(m.group(1), m.group(1))
            else:
                cfn = names.get(m.group(1), m.group(1))
        continue
    if line.startswith("calls="):
        calls[cfn] += int(line.split("=")[1].split()[0])
        skip_next = True
        continue
    if re.match(r"^[+\-*]?\d*\s+\d+", line) or re.match(r"^[+\-*]\s*\d*\s+\d+", line):
        parts = line.split()
        if skip_next:
            skip_next = False
            continue
        if fn is not None and len(parts) >= 2:
            selfc[fn] += int(parts[-1])

reqs = calls.get(per, 0)
tot = sum(selfc.values())
print(f"total {tot:,} Ir, requests {reqs:,}, {tot / reqs:,.0f} Ir/request")
for f, c in selfc.most_common(top):
    print(f"  {c / reqs:7.1f} Ir/req  {c / tot * 100:5.1f}%  {f}")
