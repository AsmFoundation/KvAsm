#!/usr/bin/env bash
# =============================================================================
# mutate.sh — does fuzz_h2's request matrix notice a broken fused request
# path? Each mutation changes one thing in a copy of h2n.asm (inside
# kvnode-dev), rebuilds the server, and runs the matrix alone against it
# (fuzz_h2 with 0 fuzz iterations: liveness, the matrix, liveness).
#
# Usage: mutate.sh VARIANT     (the tree under /pc/VARIANT; its own fuzz_h2 is used)
# Output: one line per mutation —
#   "<name>: CAUGHT (exit 1) — <fuzz_h2's last line>"   the matrix failed, as it must
#   "<name>: MISSED (exit 0)"                            the matrix has no case for it
#   "<name>: NOT APPLIED" / "BUILD FAILED"               the mutation itself is stale
# and first a control: the unmutated tree must pass.
#
# Why: a test that never failed proves little. The matrix was written against
# RFC 9113, then shown to pass on the engine without the fused path; this
# shows the other half — that each condition and each step of the fused path
# is watched by some case.
# =============================================================================
set -u
V=$1
SRC=/pc/$V
PORT=50367
FUZZ=$SRC/server/build/tests/fuzz_h2

run_matrix() {  # server-binary [limits] -> prints "exit <rc> — <last line>"
  # With "limits": fuzz_h2 --limits against a server with the small limits of
  # itest phase 7b; otherwise the request matrix against default limits.
  rm -f /tmp/mut-srv.log               # or the wait below reads the previous server's "listening"
  if [ "${2:-}" = limits ]; then
    KVNODE_PORT=$PORT KVNODE_WORKERS=1 KVNODE_MAX_KEY_BYTES=16 KVNODE_MAX_VALUE_BYTES=64         taskset -c 8-14 "$1" 2> /tmp/mut-srv.log &
  else
    KVNODE_PORT=$PORT KVNODE_WORKERS=2 KVNODE_CAPACITY=200000 taskset -c 8-14 "$1" 2> /tmp/mut-srv.log &
  fi
  local spid=$!
  for _ in $(seq 1 100); do grep -q 'msg="listening"' /tmp/mut-srv.log 2> /dev/null && break; sleep 0.05; done
  local rc
  if [ "${2:-}" = limits ]; then
    timeout 90 "$FUZZ" --limits 127.0.0.1 $PORT > /tmp/mut-out.log 2>&1; rc=$?
  else
    timeout 90 "$FUZZ" 127.0.0.1 $PORT 0 1 "$spid" > /tmp/mut-out.log 2>&1; rc=$?
  fi
  kill -TERM "$spid" 2> /dev/null
  wait "$spid" 2> /dev/null
  echo "exit $rc — $(tail -1 /tmp/mut-out.log)"
}

mutate() {  # name, sed expression applied to src/h2n.asm, [limits]
  local name=$1 expr=$2 mode=${3:-}
  rm -rf /pc/mut && cp -r "$SRC" /pc/mut
  cd /pc/mut/server || return
  sed -i "$expr" src/h2n.asm
  if cmp -s src/h2n.asm "$SRC/server/src/h2n.asm"; then echo "$name: NOT APPLIED"; return; fi
  # The object and the binary are removed, not left to make's timestamps: the
  # copy and the edit can land in the same clock tick (file times are coarse),
  # and make then finds "nothing to be done" and the unmutated server is tested
  # — which is exactly what the first run of this script did for five mutations.
  rm -f build/obj/h2n.o build/kvnode
  if ! taskset -c 8-14 make -j7 all > /tmp/mut-build.log 2>&1; then
    echo "$name: BUILD FAILED — $(grep -m1 -i 'error' /tmp/mut-build.log)"; return
  fi
  if cmp -s build/kvnode "$SRC/server/build/kvnode"; then echo "$name: BINARY UNCHANGED"; return; fi
  local res
  res=$(run_matrix /pc/mut/server/build/kvnode "$mode")
  case "$res" in
    "exit 0 "*) echo "$name: MISSED ($res)" ;;
    *)          echo "$name: CAUGHT ($res)" ;;
  esac
}

echo "control (unmutated $V), request matrix: $(run_matrix "$SRC/server/build/kvnode")"
echo "control (unmutated $V), --limits:       $(run_matrix "$SRC/server/build/kvnode" limits)"

# 1. the fused DATA frame is not counted against the connection window
mutate "no connection-window accounting" '/H2.recv_window\], rcx/,+1d'
# 2. any DATA frame is fused, whatever its flags (padded, not END_STREAM)
mutate "flags not checked" 's/cmp     word \[rax + 3\], FT_DATA | (FF_END_STREAM << 8)/cmp     byte [rax + 3], FT_DATA/'
# 3. a DATA frame of another stream is fused
mutate "stream id not checked" '/cmp     ecx, \[r12 + 5\]/,+1d'
# 4. header_block hands back a stream on every path (also one it ended or freed)
mutate "stream handed back after END_STREAM" 's/xor     edx, edx                    ; no open, unanswered stream to hand back/mov     rdx, rbp/'
# 5. the stream is not marked half-closed (remote)
mutate "SN_REMOTE_CLOSED not set" '/SN_REMOTE_CLOSED ; request_end.s step/d'
# 6. the frame loop is not stepped over the fused DATA frame
mutate "DATA frame not stepped over" '/mov     r14, rsi                    ; its payload and length: .next steps over/,+1d'
# 7. a header-block near miss decodes the block as trailers (r8 not restored)
mutate "near miss: r8 not restored" '/xor     r8d, r8d                    ; a request block (it was 0; blocks_equal used r8)/d'
# 8. a declared content-length is not left to the loop
mutate "content-length not left to the loop" '/jge     .next                       ; a declared content-length: the loop checks it/d'
# 9. an oversized frame is fused instead of raising FRAME_SIZE_ERROR
mutate "frame size not checked" '/ja      .next                       ; oversized: the loop raises FRAME_SIZE_ERROR/d'
# 10. the next frame's header is read although it has not arrived whole
mutate "frame header not whole" '/ja      .next                       ; its header is not whole: the loop carries it over/d'
# 11. the payload is used although it has not arrived whole
mutate "payload not whole" '/ja      .next                       ; the payload is not whole yet/d'
# 12. an Upload's DATA frame is taken for a unary body
mutate "Upload not left to the loop" '/je      .next                       ; client streaming consumes DATA as it arrives/d'
# 13. a frame over max_request is borrowed (fuzz_h2 --limits, small-limits server)
mutate "request size not checked" '/ja      .next                       ; beyond max_request: the copy path marks it/d' limits
rm -rf /pc/mut
