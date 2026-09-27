#!/usr/bin/env python3
"""idleprobe.py — does an idle kvnode spend CPU while it waits?

Starts kvnode with 4 workers, opens 1,000 HTTP/2 connections (preface and
SETTINGS exchanged, then silent), and over 10 s of silence reports:
  * CPU ticks the process used (utime + stime from /proc/PID/stat);
  * voluntary / involuntary context switches summed over its threads;
  * every system call it made, from `strace -c -f` over 5 s.
An event-driven server sleeps in epoll_wait: expect ~0 ticks and one
epoll_wait wake-up per worker per housekeeping tick (1 s).
"""
import os
import socket
import struct
import subprocess
import time

SERVER = "/src/server/build/kvnode"
PORT = 50071
PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"


def frame(ftype, flags, sid, payload):
    """One HTTP/2 frame."""
    return struct.pack(">I", len(payload))[1:] + bytes([ftype, flags]) + struct.pack(">I", sid) + payload


def ticks(pid):
    """utime + stime of the whole process, in clock ticks."""
    f = open(f"/proc/{pid}/stat").read().rsplit(")", 1)[1].split()
    return int(f[11]) + int(f[12])


def switches(pid):
    """(voluntary, involuntary) context switches summed over all threads."""
    v = n = 0
    for tid in os.listdir(f"/proc/{pid}/task"):
        for line in open(f"/proc/{pid}/task/{tid}/status"):
            if line.startswith("voluntary_ctxt_switches"):
                v += int(line.split()[1])
            elif line.startswith("nonvoluntary_ctxt_switches"):
                n += int(line.split()[1])
    return v, n


def main():
    env = dict(os.environ, KVNODE_PORT=str(PORT), KVNODE_WORKERS="4", KVNODE_CAPACITY="1000")
    log = open("/tmp/idleprobe.log", "w")
    p = subprocess.Popen([SERVER], env=env, stderr=log)
    while 'msg="listening"' not in open("/tmp/idleprobe.log").read():
        time.sleep(0.02)
    socks = []
    for _ in range(1000):
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall(PREFACE + frame(4, 0, 0, b""))
        socks.append(s)
    for s in socks:
        s.settimeout(2)
        s.recv(65536)                       # server SETTINGS + WINDOW_UPDATE
        s.sendall(frame(4, 1, 0, b""))      # SETTINGS ACK
    time.sleep(1.0)
    t0, c0, (v0, n0) = time.time(), ticks(p.pid), switches(p.pid)
    time.sleep(10.0)
    t1, c1, (v1, n1) = time.time(), ticks(p.pid), switches(p.pid)
    hz = os.sysconf("SC_CLK_TCK")
    print(f"idle 10 s, 4 workers, 1000 open connections: CPU {(c1 - c0) / hz * 1000:.0f} ms "
          f"({(c1 - c0) / hz / (t1 - t0) * 100:.3f}% of one core); context switches "
          f"voluntary {v1 - v0}, involuntary {n1 - n0}")
    r = subprocess.run(["timeout", "-s", "INT", "5", "strace", "-c", "-f", "-p", str(p.pid)],
                       capture_output=True, text=True)
    print("strace -c -f over 5 s of silence:")
    print("\n".join(line for line in r.stderr.splitlines() if line.strip()))
    for s in socks:
        s.close()
    p.terminate()
    p.wait()


main()
