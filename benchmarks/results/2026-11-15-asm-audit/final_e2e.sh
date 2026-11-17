#!/usr/bin/env bash
# =============================================================================
# final_e2e.sh — the end-to-end A/B runs of the final tree (inside kvasm-dev):
# abseq.sh quads (A B B A in one slot) of server CPU per request and requests
# per second, one worker.
#
# Usage: final_e2e.sh PART      PART = large | small
#   large   head2 vs landA with 16,384 / 100,000 / 1,048,576-byte values
#           (replies longer than a frame, and longer than the 64 KiB mark)
#   small   head2 vs fuse with 100-byte values: GET (busy-polling and blocking
#           clients), 100% SET, 50/50
# Output: /pc/out/final-e2e-PART.out
#
# Placement: server on vCPU 8, kvbench on vCPUs 9-14 (6 threads x 24
# connections x 16 in flight). The benchmark suite that shares this VM runs
# its servers on 0-7 and its clients on 16-31, with its monitor on 15 — so
# these runs and that suite share no vCPU, but they do share the host's
# physical cores: read the quads' spread before the means.
# =============================================================================
set -u
PART=$1
export AB_SCPU=8 AB_CCPU=9-14 AB_THREADS=6 AB_CONNS=24 AB_SECS=4
H=/pc/head2/server/build/kvasm
A=/pc/landA/server/build/kvasm
F=/pc/fuse/server/build/kvasm
OUT=/pc/out/final-e2e-$PART.out
{
echo "## abseq.sh: one slot (server vCPU 8; kvbench 6 threads x 24 connections x 16 in flight on vCPUs 9-14), quads of A B B A,"
echo "## server CPU per request from schedstat. Another session's benchmark suite ran in the same VM (vCPUs 0-7, 16-31)."
echo "## head2 = 5735d7a; landA = + the four audit patches with replies of any length; fuse = the final tree."
if [ "$PART" = large ]; then
  echo
  echo "# 16,384-byte values (2 frames per reply), 2,000 keys: A = head2, B = landA"
  bash /pc/probe/abseq.sh "$H" "$A" 3 2000 16384 ""
  echo
  echo "# 100,000-byte values (7 frames, past the 64 KiB mark), 400 keys: A = head2, B = landA"
  bash /pc/probe/abseq.sh "$H" "$A" 3 400 100000 ""
  echo
  echo "# 1,048,576-byte values (the largest unary value), 48 keys, 4 in flight per connection: A = head2, B = landA"
  bash /pc/probe/abseq.sh "$H" "$A" 3 48 1048576 "--depth 4"
else
  echo
  echo "# 100-byte values, 1M keys, busy-polling clients (the server's own cost): A = head2, B = fuse"
  bash /pc/probe/abseq.sh "$H" "$F" 4 1000000 100 "--busy-poll"
  echo
  echo "# 100-byte values, 1M keys, blocking clients: A = head2, B = fuse"
  bash /pc/probe/abseq.sh "$H" "$F" 3 1000000 100 ""
  echo
  echo "# 100% SET, 100-byte values, 1M keys: A = head2, B = fuse"
  bash /pc/probe/abseq.sh "$H" "$F" 3 1000000 100 "--read-ratio 0"
  echo
  echo "# 50/50, 100-byte values, 1M keys: A = head2, B = fuse"
  bash /pc/probe/abseq.sh "$H" "$F" 3 1000000 100 "--read-ratio 0.5"
fi
} > "$OUT" 2>&1
echo DONE >> "$OUT"
