#!/bin/sh
# vmwatch.sh - in-VM interference monitor for a benchmark session.
#
# Why it exists: run.py's host-load sampler (psutil, on Windows) sees only
# CPU used OUTSIDE the Docker Desktop VM. Work INSIDE the VM - another Claude
# session's build or A/B benchmark in the kvnode-dev container, a helper
# container, a WSL distro - competes for the same vCPUs yet is invisible to
# it (it hides inside vmmemWSL, which the sampler subtracts as "ours"). This
# script makes that work visible, so every run's measured window can be
# checked for overlap afterwards (vmwatch_report.py) and contaminated
# blocks re-run.
#
# What it records, one line every PERIOD seconds, all counters CUMULATIVE
# (the reader takes differences, so a missed or late sample loses no data):
#   <epoch s> cpu <busy jiffies of vCPU 0> ... <vCPU N-1> | <cid12>=<ns> ...
# - per-vCPU busy = user + nice + system + irq + softirq from /proc/stat,
#   which Docker does not namespace: it is the whole VM's view (USER_HZ=100);
# - per-container CPU = cpuacct.usage (ns) of every cgroup under docker/,
#   keyed by the first 12 hex digits of the container id.
#
# Run it pinned to a vCPU the suite never uses (server 0-7, client 16-31),
# with the host cgroup tree mounted read-only, e.g.:
#   docker run -d --name kvnode-vmwatch --cpuset-cpus 15 \
#     -v /sys/fs/cgroup:/host-cgroup:ro -v <dir>:/w:ro alpine:3 sh /w/vmwatch.sh
# Output goes to stdout, i.e. `docker logs kvnode-vmwatch`: the json-file log
# lives inside the VM, so sampling never writes across the Windows file share.
#
# Cost: two short-lived processes (date, awk) per period on an otherwise idle
# vCPU - well under 0.1% of one vCPU. Assumes cgroup v1 (cgroupfs), which is
# what run.py's environment.json recorded for this host; on cgroup v2 the
# per-container files are cpu.stat and this script would need adapting.

PERIOD="${PERIOD:-5}"
CG="${CG:-/host-cgroup/cpuacct/docker}"

while :; do
    # One awk pass over /proc/stat and every container's counter keeps the
    # sample to a single fork (plus date), so the monitor itself stays cheap.
    awk -v t="$(date +%s)" '
        FILENAME == "/proc/stat" {
            # Per-vCPU lines only ("cpu0".."cpuN"); the aggregate "cpu" line
            # is derivable and would only double the log.
            if ($1 ~ /^cpu[0-9]+$/) cpus = cpus " " ($2 + $3 + $4 + $7 + $8)
            next
        }
        {
            # FILENAME is <CG>/<64-hex id>/cpuacct.usage: the id is the
            # next-to-last path component.
            n = split(FILENAME, p, "/")
            ctrs = ctrs " " substr(p[n - 1], 1, 12) "=" $1
        }
        END { print t " cpu" cpus " |" ctrs }
    ' /proc/stat "$CG"/*/cpuacct.usage 2>/dev/null
    sleep "$PERIOD"
done
