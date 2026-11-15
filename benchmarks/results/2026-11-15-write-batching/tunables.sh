#!/bin/sh
# =============================================================================
# tunables.sh — run a kvnode binary under extra glibc malloc tunables, for an
# A/B of allocator settings with no code change (abw.py runs this script as
# variant B; taskset and exec keep the server's pid, so abw.py's CPU
# accounting still reads the server itself).
#
# Usage: KVNODE_REAL=/tmp/kvnode-step2 TUNABLES=glibc.malloc.tcache_count=1024 \
#        tunables.sh
# The static server reads GLIBC_TUNABLES at startup like any glibc program.
# =============================================================================
exec env GLIBC_TUNABLES="${TUNABLES:?set TUNABLES}" "${KVNODE_REAL:?set KVNODE_REAL}"
