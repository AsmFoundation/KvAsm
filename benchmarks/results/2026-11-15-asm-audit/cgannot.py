#!/usr/bin/env python3
"""cgannot.py — exact per-function and per-source-line instruction counts per
request from a callgrind dump made with --dump-instr=yes.

Usage:
  cgannot.py DUMP NM REQS_FN                      per-function table (self Ir/request)
  cgannot.py DUMP NM REQS_FN --src ROOT FN [FN..] annotated source of the functions
  cgannot.py DUMP NM REQS_FN --lines N            the N hottest source lines

  DUMP     callgrind out file (counters zeroed before the workload)
  NM       `nm -n` of the same binary (NASM's DWARF has no function entries,
           so callgrind names functions by address; nm restores the names)
  REQS_FN  a function called exactly once per request (its call count is the
           divisor), or a literal number of requests prefixed with '='
  ROOT     directory holding server/src/*.asm of the same revision

Why per line: every NASM source line is one instruction (macros aside), so
"Ir / requests" on a line is how many times that instruction ran per request —
1.00 on the straight path, 16.0 inside a per-batch loop, 0.06 on a per-read path.

The "(n calls/req)" column counts call instructions only: a function entered
by a tail jump (kv_siphash128 from kv_store_hash, kv_grpc_dispatch from
request_end) shows 0.00 calls although it runs once per request. Its cost is
still attributed correctly, because cost is assigned by instruction address.
"""
import bisect
import collections
import os
import re
import sys

# The sources are UTF-8 (section signs, dashes in comments): print them as
# such whatever the console's code page is.
sys.stdout.reconfigure(encoding="utf-8")
args = sys.argv[1:]
dump, nmfile, per = args[0], args[1], args[2]
rest = args[3:]

addrs, syms = [], []
for line in open(nmfile):
    parts = line.split()
    if len(parts) == 3 and parts[1] in "tTiW" and "." not in parts[2]:
        addrs.append(int(parts[0], 16))
        syms.append(parts[2])


def sym_of(addr):
    """Enclosing global/function symbol of a code address."""
    i = bisect.bisect_right(addrs, addr) - 1
    return syms[i] if i >= 0 else hex(addr)


def resolve(name):
    m = re.fullmatch(r"0x([0-9a-f]+)", name.strip())
    return sym_of(int(m.group(1), 16)) if m else name.strip()


names = {"fn": {}, "fl": {}, "ob": {}}
self_fn = collections.Counter()          # function symbol -> self Ir
line_cost = collections.Counter()        # (file, line) -> self Ir
addr_cost = collections.Counter()        # address -> self Ir
calls = collections.Counter()            # callee symbol -> calls
cur_file = None
fn_file = None
cfn = None
skip_next = False
addr = 0
lineno = 0
total = 0


def name_of(kind, text):
    """Decode callgrind's '(id) name' / '(id)' name compression."""
    m = re.match(r"\((\d+)\)(?: (.*))?$", text.strip())
    if not m:
        return text.strip()
    if m.group(2) is not None:
        names[kind][m.group(1)] = m.group(2)
    return names[kind].get(m.group(1), m.group(1))


def subpos(tok, prev, hexa):
    """One (possibly relative) subposition."""
    if tok == "*":
        return prev
    if tok[0] in "+-":
        return prev + int(tok, 16 if hexa and tok[1:].startswith("0x") else 10) if not hexa else prev + int(tok, 10)
    return int(tok, 16) if hexa else int(tok)


for raw in open(dump, errors="replace"):
    line = raw.rstrip("\n")
    if not line:
        continue
    if line.startswith(("summary:", "totals:")):
        total = int(line.split()[1])
        continue
    if line.startswith("fl="):
        fn_file = cur_file = name_of("fl", line[3:])
        continue
    if line.startswith(("fi=", "fe=")):
        cur_file = name_of("fl", line[3:])
        continue
    if line.startswith("fn="):
        name_of("fn", line[3:])
        cur_file = fn_file
        continue
    if line.startswith("cfn="):
        cfn = resolve(name_of("fn", line[4:]))
        continue
    if line.startswith(("cfi=", "cfl=")):
        name_of("fl", line[4:])
        continue
    if line.startswith(("ob=", "cob=")):
        continue
    if line.startswith("calls="):
        calls[cfn] += int(line.split("=")[1].split()[0])
        skip_next = True
        continue
    m = re.match(r"^([+\-*]|0x[0-9a-f]+|[+\-]\d+)", line)
    if not m:
        continue
    toks = line.split()
    if len(toks) < 3:
        continue
    a, l, ir = toks[0], toks[1], int(toks[2])
    # address subposition: absolute hex "0x..", relative "+N"/"-N" (decimal), or "*"
    if a == "*":
        pass
    elif a[0] in "+-":
        addr += int(a)
    else:
        addr = int(a, 16)
    if l == "*":
        pass
    elif l[0] in "+-":
        lineno += int(l)
    else:
        lineno = int(l)
    if skip_next:                       # inclusive cost of the call just announced
        skip_next = False
        continue
    self_fn[sym_of(addr)] += ir
    line_cost[(os.path.basename(cur_file or "?"), lineno)] += ir
    addr_cost[addr] += ir

if per.startswith("="):
    reqs = int(per[1:])
else:
    reqs = calls.get(per, 0)
if not reqs:
    sys.exit(f"no calls of {per} found; known: {sorted(calls)[:40]}")
tot = sum(self_fn.values())

if not rest:
    print(f"total {tot:,} Ir, requests {reqs:,}, {tot / reqs:,.1f} Ir/request")
    for f, c in self_fn.most_common(60):
        if c / reqs < 0.5:
            break
        print(f"  {c / reqs:8.1f} Ir/req  {c / tot * 100:5.1f}%  {f}   ({calls.get(f, 0) / reqs:.2f} calls/req)")
elif rest[0] == "--lines":
    n = int(rest[1])
    for (f, l), c in line_cost.most_common(n):
        print(f"  {c / reqs:8.2f}  {f}:{l}")
elif rest[0] == "--src":
    root = rest[1]
    want = set(rest[2:])
    # Source ranges of the wanted functions: from their label line to the next
    # global label line of the same file.
    for fname in sorted({f for (f, _l) in line_cost}):
        path = os.path.join(root, fname)
        if not os.path.exists(path):
            continue
        src = open(path, encoding="utf-8", errors="replace").read().split("\n")
        labels = [(i + 1, mm.group(1)) for i, s in enumerate(src)
                  for mm in [re.match(r"^([A-Za-z_][A-Za-z0-9_]*):", s)] if mm]
        for idx, (ln, lab) in enumerate(labels):
            if lab not in want:
                continue
            end = labels[idx + 1][0] if idx + 1 < len(labels) else len(src) + 1
            fsum = sum(c for (f, l), c in line_cost.items() if f == fname and ln <= l < end)
            print(f"==== {lab}  ({fname}:{ln}-{end - 1})  {fsum / reqs:.1f} Ir/request")
            for l in range(ln, end):
                c = line_cost.get((fname, l), 0)
                text = src[l - 1]
                if c == 0 and (not text.strip() or text.lstrip().startswith(";")):
                    continue
                mark = f"{c / reqs:7.2f}" if c else "      ."
                print(f"{mark} {l:5d} {text}")
