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
# Every server must stop with "pinned=0 deferred=0" in its stopped line: a
# Download pin (or a record deferred to one) that outlived its call is a
# leak, whichever phase caused it (ADR 0010).
#   1. protocol, limits, pipelining, cancellation, multi-connection atomicity,
#      and the streaming calls: Upload/Download matrix, every refusal, every
#      DATA split, cancels, resets, ranges, windows, pins, a streaming race
#   2. graceful shutdown: an idle client must receive GOAWAY(NO_ERROR) and a
#      clean close, an in-flight unary call and an in-flight Upload and
#      Download must complete, and the server must exit 0 after SIGTERM
#  2b. phase 1's suite again through per-worker private L1/L2 record caches
#      (hcache.asm) with SWAR tag scans and the table CRC-32C (no SSE4.2)
#  2c. the suite's Upload/Download tests under each hardware CRC-32C path
#      forced in turn (sse42, pclmul, vpclmul — phase 1 ran auto); a path
#      the CPU lacks must be refused at startup (exit 2) instead
#   3. idle timeout: an idle connection is closed with GOAWAY after ~1 s;
#      stalled transfers end (and give their memory back), slow ones do not,
#      and a reader frozen at the TCP level is dropped without the GOAWAY it
#      could never receive
#   4. snapshot source: 20k records load at startup and are served exactly
#   5. startup refusals: corrupt/missing snapshot (exit 1), invalid settings
#      (exit 2), a port already served by another instance (exit 1)
#   6. HTTP/2 conformance: h2spec (when installed — the Docker toolchain
#      always has it) must fail exactly the documented baseline in
#      h2spec-known-failures.txt (empty: 146/146): any new failure or skip
#      fails the run
#   7. fuzzing the HTTP/2 input path (fuzz_h2: garbage, mutated requests,
#      frame soup, random HPACK, truncation, split writes, Upload and
#      Download soups), liveness checked every 50 connections and memory
#      growth bounded; then the full suite on the same server, which must
#      still exit 0. Before the fuzzing, fuzz_h2's request matrix: exact
#      answers at the frame boundaries of a unary request
#  7b. the request-size limit at the frame boundary (fuzz_h2 --limits): a
#      server with a 16-byte key limit and a 64-byte value limit, where one
#      DATA frame can be over the largest request
#   8. streaming limits: KVNODE_MAX_MEMORY_MB, the per-connection upload
#      budget, KVNODE_CAPACITY — and every reservation and pin coming back
#      however its call ends (cancel, reset connection, completion)
#   9. one worker: unary calls stay answered beside bulk downloads
#      (FLUSH_BUDGET)
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
# Above MAX_VALUE, so values only Upload can store (and Get must refuse)
# exist in the suite's phases; within the default 64 MiB connection budget.
MAX_UPLOAD=8388608
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
# shutdown grace is 10 s), and a stopped line reporting no pin left: after
# every connection is gone, a Download pin still held — or a replaced record
# still deferred to one — would be a leak of up to KVNODE_MAX_UPLOAD_BYTES.
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
  grep -q 'msg="stopped".* pinned=0 deferred=0' "$LOG" \
    || fail "pins outlived their calls: $(grep 'msg="stopped"' "$LOG")"
}

# await_ready FILE LINE PID — wait until the client PID has written LINE into
# FILE (its "ready" handshake: the server answered a PING sent after the
# client's connection and streams were set up). Fails, showing the client's
# output and the server log, if the client exits first or 20 s pass.
await_ready() {
  local deadline=$((SECONDS + 20))
  while ! grep -qF -- "$2" "$1" 2> /dev/null; do
    alive "$3" || { cat "$1" >&2; fail "client exited before it was ready: $2"; }
    ((SECONDS < deadline)) || { cat "$1" >&2; fail "client not ready after 20 s: $2"; }
    sleep 0.02
  done
}

# crc_mode_refused MODE — true when kvnode refuses KVNODE_CRC32C=MODE at
# startup the way a CPU without that path must be refused (exit 2 naming the
# mode and the missing instructions); false when it starts (the probe server
# is stopped again at once). Any other outcome fails the run.
crc_mode_refused() {
  local rlog="$WORK/crc-probe.log" pid rc=0 deadline=$((SECONDS + 10))
  : > "$rlog"
  env KVNODE_PORT="$PORT" KVNODE_WORKERS=1 KVNODE_CRC32C="$1" "$KVNODE" 2> "$rlog" &
  pid=$!
  while ! grep -q 'msg="listening"' "$rlog"; do
    if ! alive "$pid"; then
      wait "$pid" || rc=$?
      ((rc == 2)) && grep -qF "value=$1 reason=\"this CPU lacks the instructions it needs\"" "$rlog" \
        && return 0
      fail "crc32c=$1: startup ended with status $rc" "$rlog"
    fi
    ((SECONDS < deadline)) || { kill -9 "$pid"; fail "crc32c=$1 probe not listening after 10 s" "$rlog"; }
    sleep 0.02
  done
  kill -TERM "$pid"
  wait "$pid" || true
  return 1
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

echo "itest: phase 1 — protocol, limits, pipelining, cancellation, concurrency, streaming"
start_server main KVNODE_WORKERS=4 KVNODE_CAPACITY=200000 \
  KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE KVNODE_MAX_UPLOAD_BYTES=$MAX_UPLOAD
grep -qF "max_upload_bytes=$MAX_UPLOAD crc32c=" "$LOG" || fail "upload settings not logged"
"$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" "$MAX_UPLOAD" || fail "itest_grpc suite"

echo "itest: phase 2 — graceful shutdown: idle client + in-flight unary and streaming calls"
# The clients print "ready" once the server has answered a PING sent after
# their connection (and, for the drain checks, their streams) was set up;
# only then is SIGTERM sent. A fixed sleep here lost the race on a busy host:
# the stream arrived after the GOAWAY and was never accepted.
"$ITEST" --wait-goaway 127.0.0.1 "$PORT" 20 > "$WORK/hold.out" &
HOLD=$!
"$ITEST" --drain-check 127.0.0.1 "$PORT" 20 > "$WORK/drain.out" &
DRAIN=$!
"$ITEST" --drain-transfers 127.0.0.1 "$PORT" 20 > "$WORK/xfer.out" &
XFER=$!
await_ready "$WORK/hold.out" "wait-goaway: ready" "$HOLD"
await_ready "$WORK/drain.out" "drain-check: ready" "$DRAIN"
await_ready "$WORK/xfer.out" "drain-transfers: ready" "$XFER"
stop_server
wait "$HOLD" || { cat "$WORK/hold.out" >&2; fail "idle client did not get GOAWAY(NO_ERROR) and a clean close"; }
wait "$DRAIN" || { cat "$WORK/drain.out" >&2; fail "in-flight request was not completed (NOT_SERVING) during drain"; }
wait "$XFER" || { cat "$WORK/xfer.out" >&2; fail "in-flight Upload/Download were not completed during drain"; }
cat "$WORK/hold.out" "$WORK/drain.out" "$WORK/xfer.out"

echo "itest: phase 2b — the same suite through private L1/L2 caches, SWAR scans, table CRC-32C, SipHash"
# Tiny caches (L1 256, L2 4096 entries) so entries conflict and are reused
# while the suite's writers invalidate them; SWAR because SSE2 is the default;
# the table CRC-32C because auto picks a hardware path on any CPU this runs
# on; SipHash because auto picks the AES key hash wherever the CPU has AES-NI,
# and the fallback must stay proven end to end.
start_server cached KVNODE_WORKERS=4 KVNODE_CAPACITY=200000 \
  KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE KVNODE_MAX_UPLOAD_BYTES=$MAX_UPLOAD \
  KVNODE_CACHE_L1_BITS=8 KVNODE_CACHE_L2_BITS=12 KVNODE_SCAN=swar KVNODE_CRC32C=table KVNODE_HASH=siphash
grep -qF 'scan=swar' "$LOG" || fail "scan mode not logged"
grep -qF 'cache_l1_bits=8 cache_l2_bits=12' "$LOG" || fail "cache settings not logged"
grep -qF 'crc32c=table' "$LOG" || fail "crc32c mode not logged"
grep -qF 'hash=siphash' "$LOG" || fail "hash mode not logged"
"$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" "$MAX_UPLOAD" \
  || fail "itest_grpc suite with private caches"
stop_server

echo "itest: phase 2c — the streaming tests under each hardware CRC-32C path"
# Phase 1 ran auto (the fastest path the CPU has) and 2b the tables; here
# every hardware path is forced in turn, so a CPU that has them all checks
# them all end to end (every DATA split, ranges, the conformance vectors).
for mode in sse42 pclmul vpclmul; do
  if crc_mode_refused "$mode"; then
    echo "  crc32c=$mode: refused at startup (this CPU lacks it)"
    continue
  fi
  start_server "crc-$mode" KVNODE_WORKERS=2 KVNODE_CAPACITY=200000 \
    KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE \
    KVNODE_MAX_UPLOAD_BYTES=$MAX_UPLOAD KVNODE_CRC32C=$mode
  grep -qF "crc32c=$mode snapshot=" "$LOG" || fail "crc32c=$mode not logged as the mode in force"
  "$ITEST" --streaming 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" "$MAX_UPLOAD" \
    || fail "streaming suite with crc32c=$mode"
  stop_server
done

echo "itest: phase 3 — idle timeout: idle connections, stalled, slow and frozen transfers"
# The memory limit lets --stall observe, from outside, when a stalled call's
# pinned record or upload reservation is released (a 16 MiB reservation fits
# again); the budget bounds uploads per connection as in production.
start_server idle KVNODE_WORKERS=1 KVNODE_IDLE_TIMEOUT_SEC=1 KVNODE_MAX_MEMORY_MB=24 \
  KVNODE_CONN_BUFFER_MB=16 KVNODE_MAX_UPLOAD_BYTES=16777216
t0=$(date +%s%N)
"$ITEST" --wait-goaway 127.0.0.1 "$PORT" 10 || fail "idle connection not closed with GOAWAY"
ms=$((($(date +%s%N) - t0) / 1000000))
((ms >= 900)) || fail "idle close after ${ms} ms, before the 1 s timeout"
echo "  idle connection closed after ${ms} ms"
"$ITEST" --stall 127.0.0.1 "$PORT" || fail "idle timeout against transfers"
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
refuse "unknown crc32c mode" 2 'name=KVNODE_CRC32C value=fast allowed=auto|vpclmul|pclmul|sse42|table' \
  KVNODE_CRC32C=fast
refuse "crc32c mode case" 2 'name=KVNODE_CRC32C value=SSE42 allowed=auto|vpclmul|pclmul|sse42|table' \
  KVNODE_CRC32C=SSE42
refuse "unknown hash" 2 'name=KVNODE_HASH value=md5 allowed=auto|siphash|aes' KVNODE_HASH=md5
refuse "upload above the budget" 2 \
  'name=KVNODE_MAX_UPLOAD_BYTES value=2000000 allowed=0..1048576 reason="must not exceed KVNODE_CONN_BUFFER_MB' \
  KVNODE_CONN_BUFFER_MB=1 KVNODE_MAX_UPLOAD_BYTES=2000000
refuse "upload limit overflow" 2 'name=KVNODE_MAX_UPLOAD_BYTES value=2147483648 allowed=0..2147483647' \
  KVNODE_MAX_UPLOAD_BYTES=2147483648

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
    KVNODE_MAX_KEY_BYTES=$MAX_KEY KVNODE_MAX_VALUE_BYTES=$MAX_VALUE KVNODE_MAX_UPLOAD_BYTES=$MAX_UPLOAD
  "$FUZZ_BIN" 127.0.0.1 "$PORT" "${FUZZ_ITERATIONS:-3000}" "${FUZZ_SEED:-1}" "$PID" \
    || fail "fuzz_h2 found a failure"
  "$ITEST" 127.0.0.1 "$PORT" "$MAX_KEY" "$MAX_VALUE" "$MAX_UPLOAD" || fail "suite after fuzzing"
  stop_server                              # exit status 0 and no pin left required
else
  echo "  skipped: FUZZ_BIN not set (run through make itest)"
fi

echo "itest: phase 7b — the request-size limit at the frame boundary"
if [[ -n "${FUZZ_BIN:-}" ]]; then
  # 16 + 64 + 64 = 144 bytes is this server's largest message: fuzz_h2 --limits
  # sends Puts on either side of it, each as one DATA frame behind its HEADERS.
  start_server reqlimit KVNODE_WORKERS=1 KVNODE_MAX_KEY_BYTES=16 KVNODE_MAX_VALUE_BYTES=64
  "$FUZZ_BIN" --limits 127.0.0.1 "$PORT" || fail "request-size limit at the frame boundary"
  stop_server
else
  echo "  skipped: FUZZ_BIN not set (run through make itest)"
fi

echo "itest: phase 8 — streaming limits: memory, upload budget, capacity, releases"
# Small limits so --stream-limits can hit each one and watch it recover;
# the table CRC-32C here too (phase 2b already checks it logs).
start_server limits KVNODE_WORKERS=2 KVNODE_CAPACITY=64 KVNODE_MAX_MEMORY_MB=24 \
  KVNODE_CONN_BUFFER_MB=16 KVNODE_MAX_UPLOAD_BYTES=16777216 KVNODE_CRC32C=table
"$ITEST" --stream-limits 127.0.0.1 "$PORT" || fail "streaming limits"
stop_server

echo "itest: phase 9 — one worker: unary calls beside bulk downloads"
start_server fairness KVNODE_WORKERS=1
"$ITEST" --fairness 127.0.0.1 "$PORT" || fail "unary calls starved by downloads"
stop_server

echo "itest: all phases passed"
