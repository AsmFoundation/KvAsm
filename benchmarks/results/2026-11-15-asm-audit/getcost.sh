#!/usr/bin/env bash
# =============================================================================
# getcost.sh — what one request costs a single kvnode worker, natively:
# requests/s, server CPU per request (user/kernel, ns-exact from schedstat via
# kvbench --server-pid), minor page faults and syscalls-free counters from
# /proc/PID/stat. One line per (binary, workload) so variants can be diffed.
#
# Usage (inside kvnode-dev):
#   getcost.sh BINARY LABEL KEYS VALUE_SIZE "KVBENCH_RUN_ARGS" [SECONDS] [SRV_CPU] [CLI_CPUS] [PORT]
# Defaults: 6 s measured, server on vCPU 12, client on vCPUs 13-15, port 50352
# (vCPUs and port chosen to stay off the ones other sessions benchmark on).
# The keys are preloaded with Puts before the measured run.
#
# Why /proc counters: page faults and CPU time are exact counts that do not
# depend on how busy the rest of the host is, unlike throughput.
# =============================================================================
set -euo pipefail
BIN=$1; LABEL=$2; KEYS=$3; VS=$4; RUNARGS=$5
SECS=${6:-6}; SCPU=${7:-12}; CCPUS=${8:-13-15}; PORT=${9:-50352}
KVB=/src/benchmarks/kvbench/build/kvbench
LOG=/tmp/getcost.$PORT.log

KVNODE_PORT=$PORT KVNODE_WORKERS=1 KVNODE_CAPACITY=$((KEYS * 2 + 1000)) \
    taskset -c "$SCPU" "$BIN" 2> "$LOG" &
SPID=$!
trap 'kill -TERM $SPID 2>/dev/null || true; wait $SPID 2>/dev/null || true' EXIT
for _ in $(seq 1 200); do grep -q 'msg="listening"' "$LOG" && break; sleep 0.05; done

taskset -c "$CCPUS" "$KVB" --protocol grpc --port "$PORT" --keys "$KEYS" --value-size "$VS" \
    --load-only --settle 0.2 --threads 2 --connections 4 > /dev/null

# /proc/PID/stat fields: 10 minflt, 12 majflt, 14 utime, 15 stime (clock ticks).
snap() { awk '{print $10, $14, $15}' "/proc/$SPID/stat"; }
taskset -c "$CCPUS" "$KVB" --protocol grpc --port "$PORT" --keys "$KEYS" --value-size "$VS" \
    --threads 3 --connections 12 --depth 16 $RUNARGS --warmup 1 --duration "$SECS" \
    --server-pid "$SPID" > /tmp/getcost.$PORT.json &
CPID=$!
sleep 1.3
read -r F0 U0 S0 < <(snap)
T0=$(date +%s.%N)
sleep "$((SECS - 1))"
read -r F1 U1 S1 < <(snap)
T1=$(date +%s.%N)
wait $CPID
python3 - "$LABEL" "$VS" "$F0" "$F1" "$U0" "$U1" "$S0" "$S1" "$T0" "$T1" /tmp/getcost.$PORT.json <<'EOF'
import json, sys
label, vs, f0, f1, u0, u1, s0, s1, t0, t1, path = sys.argv[1:]
r = json.load(open(path))["result"]
ops = r["ops_per_sec"]
dt = float(t1) - float(t0)
reqs = ops * dt                      # requests inside the /proc sampling window
flt = (int(f1) - int(f0)) / reqs if reqs else float("nan")
hz = 100.0
usr = (int(u1) - int(u0)) / hz / reqs * 1e9 if reqs else float("nan")
sys_ = (int(s1) - int(s0)) / hz / reqs * 1e9 if reqs else float("nan")
# schedstat-exact server CPU over kvbench's own measured window
cpu = r["server_cpu_s"] / r["ops"] * 1e9 if r["ops"] else float("nan")
print(f"{label:22s} value {int(vs):6d}  {ops:10,.0f} req/s  CPU/req {cpu:7.0f} ns "
      f"(ticks: user {usr:6.0f}, kernel {sys_:6.0f})  minor faults/req {flt:6.3f}  "
      f"p50 {r['latency_us']['p50']:7.0f} us  client util {r['client_cpu_util']:.2f}  errors {r['errors']} bad {r['bad_values']} misses {r['misses']}")
EOF
