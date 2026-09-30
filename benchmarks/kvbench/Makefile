# =============================================================================
# benchmarks/kvbench/Makefile — the load generator used for every system.
#
#   all    build/kvbench (static-PIE: runs in a FROM-scratch container)
#   test   unit tests of the parts results depend on (histogram, Zipf, key
#          format, protocol parsers, CRC-32C and its O(1) combination, the
#          Upload stream generator and the Download body verifier)
#
# Built inside the repository's toolchain image (pinned nghttp2), normally
# via `docker build --target kvbench .` at the repository root.
# =============================================================================

CC             ?= gcc
NGHTTP2_PREFIX ?= /opt/nghttp2
BUILD          := build

# Same strictness as the server: warnings are errors.
CFLAGS  := -std=c11 -O2 -g -Wall -Wextra -Werror -D_GNU_SOURCE -I$(NGHTTP2_PREFIX)/include
LDFLAGS := -Wl,-z,relro -Wl,-z,now
LIBS    := $(NGHTTP2_PREFIX)/lib/libnghttp2.a -lm -pthread

LIB_SRC := engine.c proto.c grpc.c stats.c crc32c.c
HEADERS := kvbench.h engine.h

.PHONY: all test clean

all: $(BUILD)/kvbench

$(BUILD)/kvbench: main.c $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -static-pie $(LDFLAGS) -o $@ main.c $(LIB_SRC) $(LIBS)

$(BUILD)/test_kvbench: test_kvbench.c $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_kvbench.c $(LIB_SRC) $(LIBS)

test: $(BUILD)/test_kvbench
	$(BUILD)/test_kvbench

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
