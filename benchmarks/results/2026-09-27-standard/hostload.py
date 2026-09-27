"""Samples CPU used on the Windows host OUTSIDE the Docker/WSL2 VM, the same
way benchmarks/run.py's HostLoad does (host busy CPU-seconds minus the
vmmemWSL process's CPU-seconds), and names the heaviest outside processes.

Why: a benchmark suite shares the physical cores with every other program on
this workstation; run.py warns above 2 logical CPUs of outside load. This
probe answers "is the host quiet enough to start?" before committing hours.

Usage: python hostload.py [seconds]   (default 10)
"""
import sys
import time

import psutil

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
VM_NAMES = ("vmmemwsl", "vmmem")

# Prime per-process CPU counters so the second cpu_percent() call measures
# the window (psutil's first call always returns 0.0).
procs = {}
for p in psutil.process_iter(["name", "pid"]):
    try:
        p.cpu_percent(None)
        procs[p.pid] = p
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        pass
vm = [p for p in procs.values() if (p.info["name"] or "").lower() in VM_NAMES]


def vm_seconds() -> float:
    """Cumulative CPU time of the VM's host process(es), which is everything
    the containers (and anything else inside the VM) used.

    Returns:
        User + system CPU-seconds summed over vmmemWSL/vmmem since they
        started. A process that exited meanwhile (Docker Desktop restarting)
        is skipped, so the window's VM share then reads low and the
        outside load high - the probe errs towards "busy", never "quiet".
    """
    total = 0.0
    for p in vm:
        try:
            ct = p.cpu_times()
            total += ct.user + ct.system
        except psutil.NoSuchProcess:
            pass
    return total


t0 = time.monotonic()
h0 = psutil.cpu_times()
v0 = vm_seconds()
time.sleep(SECONDS)
t1 = time.monotonic()
h1 = psutil.cpu_times()
v1 = vm_seconds()

# Windows: "system" already includes interrupt/DPC time (see run.py).
host_busy = ((h1.user + h1.system) - (h0.user + h0.system)) / (t1 - t0)
vm_busy = (v1 - v0) / (t1 - t0)
print(f"window {t1 - t0:.1f}s  host busy {host_busy:.2f} CPUs  VM {vm_busy:.2f}  "
      f"outside {host_busy - vm_busy:.2f} of {psutil.cpu_count()} logical CPUs")

# Aggregate by process name: many jobs are pools of identically named workers.
by_name: dict[str, float] = {}
for p in procs.values():
    try:
        pct = p.cpu_percent(None) / 100.0  # in logical CPUs
    except (psutil.NoSuchProcess, psutil.AccessDenied):
        continue
    name = p.info["name"] or "?"
    by_name[name] = by_name.get(name, 0.0) + pct
for name, cpus in sorted(by_name.items(), key=lambda kv: -kv[1])[:12]:
    if cpus >= 0.05:
        print(f"  {cpus:6.2f}  {name}")
