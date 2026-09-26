#!/usr/bin/env bash
# =============================================================================
# run.sh — end-to-end checks of the real kvnode binary (`make itest`).
#
# Usage: run.sh KVNODE ITEST MKSNAP
#   KVNODE  server binary (build/kvnode)
#   ITEST   build/tests/itest_grpc
#   MKSNAP  build/tests/mksnap
# Env: KVNODE_ITEST_PORT (default 50151) — the port every phase uses.
#
# Phases (each starts a fresh server; any failure prints the server log and
# exits non-zero, which fails `make itest` and the Docker build). The server
# speaks HTTP/2 through its own engine (h2n.asm, ADR 0007/0008); the client
# (itest_grpc, nghttp2's client API) and h2spec are independent
# implementations, so the engine is never only checked against itself.
#   1. protocol, limits, pipelining, cancellation, multi-connection atomicity
#   2. graceful shutdown: an idle client must receive GOAWAY(NO_ERROR) and a
#      clean close, and the server must exit 0 after SIGTERM
#  2b. phase 1's suite again through per-worker private L1/L2 record caches
#      (hcache.asm) with SWAR tag scans
#   3. idle timeout: an idle connection is closed with GOAWAY after ~1 s
#   4. snapshot source: 20k records load at startup and are served exactly
#   5. startup refusals: corrupt/missing snapshot (exit 1), invalid settings
#      (exit 2), a port already served by another instance (exit 1)
#   6. HTTP/2 conformance: h2spec (when installed — the Docker toolchain
#      always has it) must fail exactly the documented baseline in
#      h2spec-known-failures.txt (empty: 146/146): any new failure or skip
#      fails the run
#   7. fuzzing the HTTP/2 input path (fuzz_h2: garbage, mutated requests,
#      frame soup, random HPACK, truncation, split writes), liveness checked
#      every 50 connections and memory growth bounded; then the full suite
#      on the same server, which must still exit 0
# Env (from the Makefile): FUZZ_BIN, FUZZ_ITERATIONS (default 3000),
# FUZZ_SEED (default 1; a failure prints the seed that reproduces it).
# =============================================================================
set -euo pipefail

KVNODE=${1:?usage: run.sh KVNODE ITEST MKSNAP}
ITEST=${2:?usage: run.sh KVNODE ITEST MKSNAP}
MKSNAP=${3:?usage: run.sh KVNODE ITEST MKSNAP}
PORT=${KVNODE_ITEST_PORT:-50151}
MAX_KEY=1024
MAX_VALUE=1048576
WORK=$(mktemp -d)
PID=
LOG=

# Kills a server left running by a failed phase and removes scratch files.
cleanup() {
  if [[ -n "$PID" ]]; then kill -9 "$PID" 2>/dev/null || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# fail MESSAGE [LOGFILE] — report, show LOGFILE (default: the running
# server's log), exit 1.
fail() {
  local show=${2:-$LOG}
  echo "itest: FAIL: $1" >&2
  if [[ -n "$show" && -f "$show" ]]; then sed 's/^/  | /' "$show" >&2; fi
  exit 1
}

# alive PID — true while the process runs. A child that exited but was not
# yet reaped is a zombie and still answers `kill -0`, so read its state.
alive() {
  [[ -r /proc/$1/status ]] && ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status"
}

# start_server NAME [VAR=VALUE...] — start kvnode in the background with the
# given settings (KVNODE_PORT preset, overridable), set PID and LOG, and wait
# until it logs "listening"; fails if it exits first or takes over 10 s.
start_server() {
  LOG="$WORK/$1.log"
  shift
  : > "$LOG"                               # exists before the first poll below
  env KVNODE_PORT="$PORT" "$@" "$KVNODE" 2> "$LOG" &
  PID=$!
  local deadline=$((SECONDS + 10))
  while ! grep -q 'msg="listening"' "$LOG"; do
    alive "$PID" || { wait "$PID" || true; PID=; fail "server exited during startup"; }
    ((SECONDS < deadline)) || fail "server not listening after 10 s"
    sleep 0.02
  done
}

# stop_server — SIGTERM, then require exit status 0 within 15 s (the default
# shutdown grace is 10 s).
stop_server() {
  kill -TERM "$PID"
  local deadline=$((SECONDS + 15)) rc=0
  while alive "$PID"; do
    ((SECONDS < deadline)) || fail "server still running 15 s after SIGTERM"
    sleep 0.02
  done
  wait "$PID" || rc=$?
  PID=
  ((rc == 0)) || fail "server exited with status $rc after SIGTERM"
  grep -q 'msg="stopped"' "$LOG" || fail "no 'stopped' log line"
}

# refuse NAME EXPECTED_STATUS LOG_PATTERN [VAR=VALUE...] — run kvnode in the
# foreground; it must exit by itself with EXPECTED_STATUS and log the pattern.
# Uses its own log file: LOG keeps pointing at a server that may still be
# running (phase 5 probes the port while the phase 4 server serves it).
refuse() {
  local name=$1 want=$2 pattern=$3 rc=0 rlog="$WORK/refuse.log"
  shift 3
  timeout 20 env KVNODE_PORT="$PORT" "$@" "$KVNODE" 2> "$rlog" || rc=$?
  ((rc == want)) || fail "$name: exit status $rc, expected $want" "$rlog"
  grep -qF -- "$pattern" "$rlog" || fail "$name: log lacks: $pattern" "$rlog"
  echo "  refused: $name (exit $rc)"
}

echo "itest: phase 1 — protocol, limits, pipelining, cancellation, concurrency"
start_server main KVNODE_WORKERS=4 KVNODE_CAPACITY=200000 \
  KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE
"$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" || fail "itest_grpc suite"

echo "itest: phase 2 — graceful shutdown: idle client + in-flight request"
"$ITEST" --wait-goaway 127.0.0.1 "$PORT" 20 &
HOLD=$!
"$ITEST" --drain-check 127.0.0.1 "$PORT" 20 &
DRAIN=$!
sleep 0.3                                  # let both connect and open their streams
stop_server
wait "$HOLD" || fail "idle client did not get GOAWAY(NO_ERROR) and a clean close"
wait "$DRAIN" || fail "in-flight request was not completed (NOT_SERVING) during drain"

echo "itest: phase 2b — the same suite through private L1/L2 caches and SWAR scans"
# Tiny caches (L1 256, L2 4096 entries) so entries conflict and are reused
# while the suite's writers invalidate them; SWAR because SSE2 is the default.
start_server cached KVNODE_WORKERS=4 KVNODE_CAPACITY=200000 \
  KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE \
  KVNODE_CACHE_L1_BITS=8 KVNODE_CACHE_L2_BITS=12 KVNODE_SCAN=swar
grep -qF 'scan=swar' "$LOG" || fail "scan mode not logged"
grep -qF 'cache_l1_bits=8 cache_l2_bits=12' "$LOG" || fail "cache settings not logged"
"$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" || fail "itest_grpc suite with private caches"
stop_server

echo "itest: phase 3 — idle timeout"
start_server idle KVNODE_WORKERS=1 KVNODE_IDLE_TIMEOUT_SEC=1
t0=$(date +%s%N)
"$ITEST" --wait-goaway 127.0.0.1 "$PORT" 10 || fail "idle connection not closed with GOAWAY"
ms=$((($(date +%s%N) - t0) / 1000000))
((ms >= 900)) || fail "idle close after ${ms} ms, before the 1 s timeout"
echo "  idle connection closed after ${ms} ms"
stop_server

echo "itest: phase 4 — snapshot as the data source"
"$MKSNAP" "$WORK/snap.kvs" 20000
start_server snapshot KVNODE_WORKERS=2 KVNODE_SNAPSHOT="$WORK/snap.kvs"
grep -qF 'msg="snapshot loaded" records=20000' "$LOG" || fail "snapshot load not logged"
"$ITEST" --expect-snapshot 127.0.0.1 "$PORT" 20000 || fail "snapshot records not served"

echo "itest: phase 5 — startup refusals"
refuse "second instance on a served port" 1 'msg="port already in use' KVNODE_WORKERS=1
stop_server                                # the phase 4 server, still running until now
"$MKSNAP" "$WORK/bad.kvs" 100 corrupt
refuse "corrupt snapshot" 1 'reason="checksum mismatch"' KVNODE_SNAPSHOT="$WORK/bad.kvs"
refuse "missing snapshot" 1 'reason="cannot open or map file"' \
  KVNODE_SNAPSHOT="$WORK/missing.kvs"
refuse "non-numeric port" 2 'name=KVNODE_PORT value=http' KVNODE_PORT=http
refuse "zero max streams" 2 'name=KVNODE_MAX_STREAMS value=0' KVNODE_MAX_STREAMS=0
refuse "negative workers" 2 'name=KVNODE_WORKERS value=-1' KVNODE_WORKERS=-1
refuse "capacity overflow" 2 'name=KVNODE_CAPACITY' KVNODE_CAPACITY=99999999999999999999
refuse "unknown scan mode" 2 'name=KVNODE_SCAN value=avx9 allowed=swar|sse2' KVNODE_SCAN=avx9
refuse "empty scan name" 2 'name=KVNODE_SCAN value= allowed=swar|sse2' KVNODE_SCAN=
refuse "cache too large" 2 'name=KVNODE_CACHE_L1_BITS value=25' KVNODE_CACHE_L1_BITS=25

# h2spec_check NAME BASELINE [VAR=VALUE...] — run h2spec against a fresh
# server; the failures must be exactly BASELINE's (a new failure or skip
# fails the run; a listed case that passes is only noted).
h2spec_check() {
  local name=$1 baseline=$2
  shift 2
  start_server "$name" KVNODE_WORKERS=2 "$@"
  # h2spec exits non-zero whenever anything fails; the baseline decides.
  h2spec -h 127.0.0.1 -p "$PORT" -o 2 -j "$WORK/$name.xml" > "$WORK/$name.txt" 2>&1 || true
  stop_server
  # JUnit: <testcase package=".." classname="..">, then <error> (failed) or
  # <skipped> on the next line; a passing case closes on the same line.
  awk '/<testcase /{
         match($0, /package="[^"]*"/);   p = substr($0, RSTART + 9, RLENGTH - 10)
         match($0, /classname="[^"]*"/); c = substr($0, RSTART + 11, RLENGTH - 12)
         w = 1; next }
       w && /<error>/   { print p " | " c; w = 0; next }
       w && /<skipped>/ { print "SKIPPED " p " | " c; w = 0; next }
       { w = 0 }' "$WORK/$name.xml" | sort > "$WORK/$name.fail"
  (grep -v '^#' "$(dirname "$0")/$baseline" | grep . || true) | sort > "$WORK/$name.known"
  grep -q "tests," "$WORK/$name.txt" || fail "h2spec did not run" "$WORK/$name.txt"
  local unexpected fixed
  unexpected=$(comm -23 "$WORK/$name.fail" "$WORK/$name.known")
  fixed=$(comm -13 "$WORK/$name.fail" "$WORK/$name.known")
  [[ -z "$unexpected" ]] || fail "h2spec regressions (not in $baseline):
$unexpected" "$WORK/$name.txt"
  [[ -z "$fixed" ]] || echo "  note: now passing, remove from $baseline: $fixed"
  echo "  $(grep "tests," "$WORK/$name.txt") — all failures in the documented baseline"
}

echo "itest: phase 6 — HTTP/2 conformance (h2spec) against the known baseline (none)"
if command -v h2spec > /dev/null; then
  h2spec_check h2spec h2spec-known-failures.txt
else
  echo "  skipped: h2spec not installed"
fi

echo "itest: phase 7 — fuzzing the HTTP/2 input path, then the suite on the same server"
if [[ -n "${FUZZ_BIN:-}" ]]; then
  start_server fuzz KVNODE_WORKERS=2 KVNODE_CAPACITY=200000 \
    KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE
  "$FUZZ_BIN" 127.0.0.1 "$PORT" "${FUZZ_ITERATIONS:-3000}" "${FUZZ_SEED:-1}" "$PID" \
    || fail "fuzz_h2 found a failure"
  "$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" || fail "suite after fuzzing"
  stop_server                              # exit status 0 required
else
  echo "  skipped: FUZZ_BIN not set (run through make itest)"
fi

echo "itest: all phases passed"
