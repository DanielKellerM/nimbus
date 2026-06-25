# Phase-1 cluster-side QCS replayer (host x86). Replays a QCS command stream
# from the shared region, invoking host-C STUB kernels via the IREE
# executable-library ABI for DISPATCH (real rv32 kernels are Phase 2).
#
# The replayer only needs the *header-only* executable-library ABI struct
# definitions (iree/hal/local/executable_library.h), so the test links no IREE
# archives -- the lightest link that compiles. HOSTBUILD is still referenced for
# the generated runtime headers (config.h etc.) on the include path, matching
# the sibling cluster HAL Makefile.
HOSTBUILD ?= /scratch/dankeller/snitch-compiler/Quidditch-artifacts/build-host-iree
SRC := $(abspath ../../..)

# IREE's headers are not -Werror-clean under this clang, so no -Werror here.
CFLAGS ?= -std=c11 -Wall -Wextra -g
SAN ?= -fsanitize=address
CPPFLAGS := -I$(SRC)/iree/runtime/src -I$(SRC)/runtime/runtime/src \
            -I$(HOSTBUILD)/runtime/src -I$(SRC)
# Header-only ABI: no IREE archives needed. Keep the libm/pthread/dl trio for
# parity with the sibling Makefile in case a future kernel needs them.
LDLIBS := -lpthread -ldl -lm

TRANSPORT := $(SRC)/runtime/host/transport/shared_region.c \
             $(SRC)/runtime/host/transport/cluster_command_stream.c

SRCS := test_cluster_replay.c cluster_replay.c $(TRANSPORT)

test_cluster_replay: $(SRCS) cluster_replay.h
	$(CC) $(CFLAGS) $(SAN) $(CPPFLAGS) $(SRCS) $(LDLIBS) -o $@

.PHONY: check clean
check: test_cluster_replay
	./test_cluster_replay

clean:
	rm -f test_cluster_replay
