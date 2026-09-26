#!/usr/bin/env python3
"""memprobe.py — measure kvnode's memory use (run inside the dev container).

Scenarios, each on a fresh server:
  idle-W     fresh server, W workers, default capacity (1M keys): RSS and
             its breakdown by mapping
  keys       1 worker, 1M keys x 100-byte values loaded by kvbench
  conns      1 worker, 1000 idle HTTP/2 connections (handshake done), then
             the same connections after one Get each
  load       1 worker, 1M keys, kvbench 64 x 16 closed loop: RSS mid-run

RSS is read from /proc/PID/smaps_rollup and /proc/PID/smaps (VmRSS in
/proc/PID/status is stale on this 5.15 WSL2 kernel).
"""
import os
import re
import socket
import struct
import subprocess
import sys
import time

SERVER = "/src/server/build/kvnode"
KVBENCH = "/src/benchmarks/kvbench/build/kvbench"
PORT = 50061
LOG = "/tmp/memprobe_server.log"


def start(env_extra):
    """Starts kvnode with the given settings; returns the Popen once it listens."""
    env = dict(os.environ)
    env.update({"KVNODE_PORT": str(PORT)})
    env.update(env_extra)
    log = open(LOG, "w")
    p = subprocess.Popen([SERVER], env=env, stderr=log, stdout=subprocess.DEVNULL)
    deadline = time.time() + 30
    while time.time() < deadline:
        if 'msg="listening"' in open(LOG).read():
            time.sleep(0.2)
            return p
        if p.poll() is not None:
            sys.exit("server exited: " + open(LOG).read())
        time.sleep(0.02)
    sys.exit("server not listening")


def stop(p):
    """SIGTERM and wait."""
    p.terminate()
    p.wait(timeout=30)


def rollup(pid):
    """smaps_rollup fields in KiB."""
    d = {}
    for line in open(f"/proc/{pid}/smaps_rollup"):
        m = re.match(r"(\w+):\s+(\d+) kB", line)
        if m:
            d[m.group(1)] = int(m.group(2))
    return d


def maps(pid):
    """Per-mapping size and residency (KiB)."""
    res = []
    cur = None
    for line in open(f"/proc/{pid}/smaps"):
        if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
            parts = line.split()
            a, b = [int(x, 16) for x in parts[0].split("-")]
            cur = {"size": (b - a) // 1024, "perm": parts[1],
                   "name": parts[5] if len(parts) > 5 else "",
                   "Rss": 0, "Anonymous": 0, "AnonHugePages": 0, "Private_Dirty": 0}
            res.append(cur)
        else:
            m = re.match(r"(\w+):\s+(\d+) kB", line)
            if m and cur is not None and m.group(1) in cur:
                cur[m.group(1)] = int(m.group(2))
    return res


def show_maps(pid, title):
    """Prints every mapping with RSS > 0 plus totals."""
    print(f"--- {title}: mappings (KiB) ---")
    total = 0
    for m in maps(pid):
        if m["Rss"] == 0 and m["size"] < 1024:
            continue
        total += m["Rss"]
        print(f"  size {m['size']:>9}  rss {m['Rss']:>7}  anon {m['Anonymous']:>7}  "
              f"thp {m['AnonHugePages']:>6}  {m['perm']}  {m['name']}")
    print(f"  total rss {total} KiB")


def sockstat():
    """TCP line of /proc/net/sockstat (mem in pages)."""
    for line in open("/proc/net/sockstat"):
        if line.startswith("TCP:"):
            return line.strip()
    return ""


# ---- a minimal HTTP/2 client for idle connections ------------------------------
PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"


def frame(ftype, flags, sid, payload):
    """One HTTP/2 frame."""
    return struct.pack(">I", len(payload))[1:] + bytes([ftype, flags]) + struct.pack(">I", sid) + payload


SETTINGS = frame(4, 0, 0, b"")
SETTINGS_ACK = frame(4, 1, 0, b"")


def hpack_lit(name_idx, value):
    """Literal without indexing, indexed name (4-bit prefix), raw value (< 127 bytes)."""
    if name_idx < 15:
        head = bytes([name_idx])
    else:
        head = bytes([0x0F, name_idx - 15])
    return head + bytes([len(value)]) + value


def get_request(sid, key):
    """HEADERS + DATA of one unary Get."""
    block = (b"\x83"                                   # :method POST (static 3)
             + b"\x86"                                 # :scheme http (static 6)
             + hpack_lit(4, b"/kvnode.v1.KvNode/Get")  # :path (static name 4)
             + hpack_lit(1, b"localhost")              # :authority (static name 1)
             + hpack_lit(31, b"application/grpc")      # content-type (static name 31)
             + b"\x00\x02te\x08trailers")              # te: trailers (new name)
    pb = b"\x0a" + bytes([len(key)]) + key            # GetRequest{key}
    msg = b"\x00" + struct.pack(">I", len(pb)) + pb
    return frame(1, 0x4, sid, block) + frame(0, 0x1, sid, msg)


def read_some(s, timeout=2.0):
    """Reads whatever arrives within the timeout."""
    s.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            s.settimeout(0.05)
    except socket.timeout:
        pass
    return data


def open_idle(n):
    """Opens n HTTP/2 connections and completes the SETTINGS exchange on each."""
    socks = []
    for _ in range(n):
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall(PREFACE + SETTINGS)
        socks.append(s)
    for s in socks:
        got = read_some(s, 2.0)
        assert got[3] == 4, "no server SETTINGS"
        s.sendall(SETTINGS_ACK)
    time.sleep(0.5)
    return socks


def one_get_each(socks):
    """Sends one Get per connection and waits for its response."""
    for s in socks:
        s.sendall(get_request(1, b"key:000000000042"))
    ok = 0
    for s in socks:
        got = read_some(s, 2.0)
        if b"\x00\x00\x00" in got:
            ok += 1
    time.sleep(0.5)
    return ok


def kvbench(args):
    """Runs kvbench, returns its stdout."""
    r = subprocess.run([KVBENCH, "--protocol", "grpc", "--port", str(PORT)] + args,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("kvbench failed: " + r.stderr[-2000:])
    return r.stdout


def main():
    """Runs every scenario and prints the numbers."""
    print("binary:", os.path.getsize(SERVER), "bytes (unstripped)")
    print(sockstat())

    # idle servers, 1 / 4 / 32 workers, default capacity 1,000,000
    for w in (1, 4, 32):
        p = start({"KVNODE_WORKERS": str(w)})
        r = rollup(p.pid)
        print(f"idle-{w}: Rss {r['Rss']} KiB, Anonymous {r['Anonymous']}, "
              f"AnonHugePages {r['AnonHugePages']}, Pss {r['Pss']}")
        if w in (1, 32):
            show_maps(p.pid, f"idle-{w}")
        stop(p)

    # capacity alone: 1 worker at 1, 1M, 10M, 100M capacity (empty table)
    for cap in (1000, 1000000, 10000000, 100000000):
        p = start({"KVNODE_WORKERS": "1", "KVNODE_CAPACITY": str(cap)})
        r = rollup(p.pid)
        print(f"capacity-{cap}: Rss {r['Rss']} KiB, AnonHugePages {r['AnonHugePages']}")
        stop(p)

    # keys: 1 worker, 1M x 100 B
    p = start({"KVNODE_WORKERS": "1", "KVNODE_CAPACITY": "1250000"})
    r0 = rollup(p.pid)
    kvbench(["--keys", "1000000", "--value-size", "100", "--load-only", "--settle", "1",
             "--threads", "4", "--connections", "16"])
    r1 = rollup(p.pid)
    print(f"keys: before {r0['Rss']} KiB, after 1M x 100 B {r1['Rss']} KiB, "
          f"delta {(r1['Rss'] - r0['Rss']) * 1024 / 1e6:.1f} B/key, THP {r1['AnonHugePages']}")
    show_maps(p.pid, "after 1M keys")
    stop(p)

    # keys with 1 KiB values (100k) and 16 B values (1M) for the per-key model
    for vs, n in ((16, 1000000), (1024, 200000)):
        p = start({"KVNODE_WORKERS": "1", "KVNODE_CAPACITY": str(int(n * 1.25))})
        r0 = rollup(p.pid)
        kvbench(["--keys", str(n), "--value-size", str(vs), "--load-only", "--settle", "1",
                 "--threads", "4", "--connections", "16"])
        r1 = rollup(p.pid)
        print(f"keys-v{vs}: {n} keys, delta {(r1['Rss'] - r0['Rss']) * 1024 / n:.1f} B/key "
              f"(value {vs} B, key 16 B)")
        stop(p)

    # connections: 1 worker, 1000 idle, then one Get each
    p = start({"KVNODE_WORKERS": "1", "KVNODE_CAPACITY": "1000"})
    kvbench(["--keys", "100", "--value-size", "100", "--load-only", "--settle", "0.5",
             "--threads", "1", "--connections", "1"])
    time.sleep(0.5)
    r0 = rollup(p.pid)
    s0 = sockstat()
    socks = open_idle(1000)
    r1 = rollup(p.pid)
    s1 = sockstat()
    print(f"conns: 1000 idle h2 connections: Rss {r0['Rss']} -> {r1['Rss']} KiB = "
          f"{(r1['Rss'] - r0['Rss']) * 1024 / 1000:.0f} B/connection")
    print("  sockstat before:", s0)
    print("  sockstat idle:  ", s1)
    ok = one_get_each(socks)
    r2 = rollup(p.pid)
    s2 = sockstat()
    print(f"conns: after one Get each ({ok} answered): Rss {r2['Rss']} KiB = "
          f"{(r2['Rss'] - r0['Rss']) * 1024 / 1000:.0f} B/connection")
    print("  sockstat after gets:", s2)
    show_maps(p.pid, "1000 connections after one Get each")
    for s in socks:
        s.close()
    time.sleep(1.0)
    r3 = rollup(p.pid)
    print(f"conns: all closed: Rss {r3['Rss']} KiB (pool/arena memory is kept for reuse)")
    stop(p)

    # under load: 1 worker, 1M keys, 64 x 16
    p = start({"KVNODE_WORKERS": "1", "KVNODE_CAPACITY": "1250000"})
    kvbench(["--keys", "1000000", "--value-size", "100", "--load-only", "--settle", "0.5",
             "--threads", "4", "--connections", "16"])
    r0 = rollup(p.pid)
    b = subprocess.Popen([KVBENCH, "--protocol", "grpc", "--port", str(PORT), "--keys", "1000000",
                          "--threads", "8", "--connections", "64", "--depth", "16",
                          "--warmup", "2", "--duration", "6"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    peak = 0
    for _ in range(14):
        time.sleep(0.5)
        peak = max(peak, rollup(p.pid)["Rss"])
    out, _ = b.communicate()
    m = re.search(r'"ops_per_sec":([0-9.]+)', out)
    print(f"load: 1M keys loaded {r0['Rss']} KiB; peak during 64x16 GET load {peak} KiB "
          f"(+{peak - r0['Rss']} KiB); {m.group(1) if m else '?'} ops/s")
    stop(p)


main()
