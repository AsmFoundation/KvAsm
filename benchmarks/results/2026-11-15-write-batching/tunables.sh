#!/bin/sh
# =============================================================================
# tunables.sh — run a kvasm binary under extra glibc malloc tunables, for an
# A/B of allocator settings with no code change (abw.py runs this script as
# variant B; taskset and exec keep the server's pid, so abw.py's CPU
# accounting still reads the server itself).
#
# Usage: KVASM_REAL=/tmp/kvasm-step2 TUNABLES=glibc.malloc.tcache_count=1024 \
#        tunables.sh
# The static server reads GLIBC_TUNABLES at startup like any glibc program.
# =============================================================================
exec env GLIBC_TUNABLES="${TUNABLES:?set TUNABLES}" "${KVASM_REAL:?set KVASM_REAL}"
