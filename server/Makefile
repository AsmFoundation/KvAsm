# =============================================================================
# server/Makefile — KvNode server, native tests and benchmarks.
#
# Targets:
#   all      the kvnode server binary (build/kvnode)
#   test     build and run every native test; any failure exits non-zero
#   itest    run the real server end to end (tests/integration/run.sh):
#            gRPC protocol and limits, concurrency, shutdown, idle timeout,
#            snapshot loading, startup refusals
#   bench    run the full data-structure bake-off (build/bench/bench_ds)
#   clean    remove build/
#
# Variables:
#   NGHTTP2_PREFIX  install prefix of the static libnghttp2 (default
#                   /opt/nghttp2, provided by the Dockerfile toolchain stage)
#
# Warnings are errors for both NASM and C: in assembly a "harmless" warning
# (e.g. an unintended absolute relocation) is often a real bug.
# =============================================================================

NASM           ?= nasm
CC             ?= gcc
NGHTTP2_PREFIX ?= /opt/nghttp2
BUILD          := build

# Relocation warnings, class by class:
#  -w-reloc-rel-dword  every cross-section call/lea in x86-64 code is a 32-bit
#                      PC-relative relocation — exactly what a PIE needs.
#  -w-reloc-abs-qword  a 64-bit absolute pointer in a data table (nghttp2_nv
#                      arrays, the settings table) becomes R_X86_64_RELATIVE,
#                      which the loader applies; legal in a PIE *if* the table
#                      is in a writable section. That condition is enforced
#                      by the linker instead (-z text below), which checks the
#                      final placement rather than every relocation.
# The truly dangerous classes — 8/16/32-bit absolute relocations, which can
# silently truncate an address — stay enabled and fatal (-w+error).
NASMFLAGS := -f elf64 -g -F dwarf -I include/ -w+all -w-reloc-rel-dword \
             -w-reloc-abs-qword -w+error
CFLAGS    := -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I tests
# Hardening applied to every executable (server, tests, bench):
#  -z text        fail the link if any dynamic relocation targets a read-only
#                 segment (a pointer table accidentally left in .rodata)
#  -z relro/now   resolve all symbols at startup, then make the GOT and
#                 .data.rel.ro read-only (full RELRO)
#  -z noexecstack every object carries .note.GNU-stack; this makes the intent
#                 explicit at link level too
LDFLAGS   := -Wl,-z,text -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
LDLIBS    := -pthread
INCLUDES  := $(wildcard include/*.inc)

# Modules shared by the server, the tests and the benchmarks.
CORE_ASM  := src/siphash.asm src/util.asm src/cuckoo.asm src/wlock.asm \
             src/store.asm src/proto.asm src/log.asm src/snapshot.asm src/abi.asm \
             src/pool.asm src/hcache.asm
CORE_OBJ  := $(CORE_ASM:src/%.asm=$(BUILD)/obj/%.o)

# Network server on top of the core (needs nghttp2).
SERVER_ASM := src/config.asm src/grpc.asm src/worker.asm src/main.asm
SERVER_OBJ := $(SERVER_ASM:src/%.asm=$(BUILD)/obj/%.o)
SERVER_BIN := $(BUILD)/kvnode

# test_abi runs first: every other test trusts the C mirror of the layouts.
TESTS     := test_abi test_nghttp2_abi test_siphash test_util test_proto \
             test_store test_store_mt test_snapshot test_pool
TEST_BINS := $(TESTS:%=$(BUILD)/tests/%)

# Data-structure bake-off (docs/adr/0001): the rival structures only exist
# here, never in the server binary.
BENCH_ASM := bench/ds/chain.asm bench/ds/trie.asm
BENCH_OBJ := $(BENCH_ASM:bench/ds/%.asm=$(BUILD)/bench/%.o)
BENCH_BIN := $(BUILD)/bench/bench_ds

# Store-level lookup benchmark (docs/adr/0006): memory-hierarchy levels, SWAR
# vs SSE2 tag scans, private L1/L2 caches. Runs in --quick mode in `test`,
# where it verifies every variant's answers; `make bench-store` is the full run.
BENCH_STORE_BIN := $(BUILD)/bench/bench_store

# End-to-end tools: an nghttp2-based gRPC client and a snapshot writer.
ITEST_BIN  := $(BUILD)/tests/itest_grpc
MKSNAP_BIN := $(BUILD)/tests/mksnap
ITEST_DEPS := tests/integration/snapgen.h tests/test_common.h

.PHONY: all test itest bench bench-store clean

all: $(SERVER_BIN)

# gcc is only the link driver (rcrt1/crti for main). The server is a
# static-PIE: glibc and nghttp2 are linked in, the kernel maps it at a random
# address (ASLR kept), and it relocates itself at startup. It needs no files
# at all, so the runtime image is FROM scratch. Static glibc is safe here
# because the server never uses NSS (no user, group or host-name lookups).
# The tests run against this exact binary.
$(SERVER_BIN): $(CORE_OBJ) $(SERVER_OBJ)
	$(CC) -static-pie $(LDFLAGS) -o $@ $^ $(NGHTTP2_PREFIX)/lib/libnghttp2.a $(LDLIBS)

# The bake-off runs in --quick mode as part of `test`: it verifies every
# answer of every structure, which keeps the benchmark code honest.
test: $(TEST_BINS) $(BENCH_BIN) $(BENCH_STORE_BIN) $(SERVER_BIN)
	@set -e; for t in $(TEST_BINS); do $$t; done
	@$(BENCH_BIN) --quick > /dev/null && echo "bench_ds --quick: all checks passed"
	@$(BENCH_STORE_BIN) --quick > /dev/null && echo "bench_store --quick: all checks passed"

# bash explicitly: the script may lack its exec bit on a Windows checkout.
itest: $(SERVER_BIN) $(ITEST_BIN) $(MKSNAP_BIN)
	bash tests/integration/run.sh $(SERVER_BIN) $(ITEST_BIN) $(MKSNAP_BIN)

bench: $(BENCH_BIN)
	$(BENCH_BIN)

bench-store: $(BENCH_STORE_BIN)
	$(BENCH_STORE_BIN)

$(BENCH_STORE_BIN): bench/bench_store.c $(CORE_OBJ) tests/test_common.h tests/kvnode_abi.h | $(BUILD)/bench
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJ) $(LDLIBS) -lm

$(BUILD)/obj/%.o: src/%.asm $(INCLUDES) | $(BUILD)/obj
	$(NASM) $(NASMFLAGS) -o $@ $<

$(BUILD)/bench/%.o: bench/ds/%.asm $(INCLUDES) | $(BUILD)/bench
	$(NASM) $(NASMFLAGS) -o $@ $<

$(BENCH_BIN): bench/bench_ds.c $(CORE_OBJ) $(BENCH_OBJ) tests/test_common.h tests/kvnode_abi.h | $(BUILD)/bench
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJ) $(BENCH_OBJ) $(LDLIBS)

$(BUILD)/tests/test_nghttp2_abi: tests/test_nghttp2_abi.c $(CORE_OBJ) tests/test_common.h | $(BUILD)/tests
	$(CC) $(CFLAGS) $(LDFLAGS) -I $(NGHTTP2_PREFIX)/include -o $@ $< $(CORE_OBJ) $(LDLIBS)

$(BUILD)/tests/%: tests/%.c $(CORE_OBJ) tests/test_common.h tests/kvnode_abi.h | $(BUILD)/tests
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CORE_OBJ) $(LDLIBS)

# The client links only libnghttp2: it must not share code with the server
# beyond the wire protocol, or it could not catch the server's mistakes.
$(ITEST_BIN): tests/integration/itest_grpc.c $(ITEST_DEPS) | $(BUILD)/tests
	$(CC) $(CFLAGS) $(LDFLAGS) -I $(NGHTTP2_PREFIX)/include -o $@ $< \
	    $(NGHTTP2_PREFIX)/lib/libnghttp2.a $(LDLIBS)

# mksnap reuses the server's SipHash on purpose: its checksum must be the one
# the loader verifies (the C# writer is checked against the same vectors).
$(MKSNAP_BIN): tests/integration/mksnap.c $(BUILD)/obj/siphash.o $(ITEST_DEPS) tests/kvnode_abi.h | $(BUILD)/tests
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(BUILD)/obj/siphash.o $(LDLIBS)

$(BUILD)/obj $(BUILD)/tests $(BUILD)/bench:
	mkdir -p $@

clean:
	rm -rf $(BUILD)
