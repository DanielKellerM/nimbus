# Phase-1 capstone: two-process GEMM running the host-device split end to end on
# the dev box (no RTL). The HOST process drives the cluster HAL device (real
# IREE record + submit) and a forked CLUSTER process replays the QCS stream
# through the host-C stub GEMM kernel. Links the host IREE archives.
HOSTBUILD ?= /scratch/dankeller/snitch-compiler/Quidditch-artifacts/build-host-iree
SRC := $(abspath ../../..)

# IREE's headers are not -Werror-clean under this clang, so no -Werror here.
CFLAGS ?= -std=c11 -Wall -Wextra -g
SAN ?= -fsanitize=address
CPPFLAGS := -I$(SRC)/iree/runtime/src -I$(SRC)/runtime/runtime/src \
            -I$(HOSTBUILD)/runtime/src -I$(SRC)

# The cluster HAL device + command buffer + allocator, the cluster-side
# replayer, and the shared-region/command-stream transport.
HAL := $(SRC)/runtime/host/hal/cluster/cluster_device.c \
       $(SRC)/runtime/host/hal/cluster/cluster_command_buffer.c \
       $(SRC)/runtime/host/hal/cluster/cluster_allocator.c
FIRMWARE := $(SRC)/runtime/host/firmware/cluster_replay.c
TRANSPORT := $(SRC)/runtime/host/transport/shared_region.c \
             $(SRC)/runtime/host/transport/cluster_command_stream.c

SRCS := phase1_gemm.c $(HAL) $(FIRMWARE) $(TRANSPORT)

# Whole-archive group so the cross-archive IREE symbol references resolve.
IREE_LIBS := $(shell find $(HOSTBUILD) -name 'libiree_*.a')
LDFLAGS := -Wl,--start-group $(IREE_LIBS) -Wl,--end-group -lpthread -ldl -lm

phase1_gemm: $(SRCS)
	$(CC) $(CFLAGS) $(SAN) $(CPPFLAGS) $(SRCS) $(LDFLAGS) -o $@

.PHONY: check clean
check: phase1_gemm
	./phase1_gemm

clean:
	rm -f phase1_gemm
