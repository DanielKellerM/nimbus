# Phase-1 cluster HAL (host x86) -- links against a host IREE runtime build.
# Point HOSTBUILD at a host IREE runtime build (recipe in
# docs/host-device-split-phase1.md). Use a ninja >= 1.8 (e.g. `uvx ninja`).
HOSTBUILD ?= /scratch/dankeller/snitch-compiler/Quidditch-artifacts/build-host-iree
SRC := $(abspath ../../../..)
IREE_LIBS := $(shell find $(HOSTBUILD) -name 'libiree_*.a')

# IREE's own headers are not -Werror-clean under this clang (an unused static
# in tracing.h), so we run -Wall -Wextra without -Werror here.
CFLAGS ?= -std=c11 -Wall -Wextra -g
SAN ?= -fsanitize=address
CPPFLAGS := -I$(SRC)/iree/runtime/src -I$(HOSTBUILD)/runtime/src -I$(SRC)
LDLIBS := -Wl,--start-group $(IREE_LIBS) -Wl,--end-group -lpthread -ldl -lm

TRANSPORT := ../../transport/shared_region.c ../../transport/cluster_command_stream.c

SRCS := test_cluster_allocator.c cluster_allocator.c $(TRANSPORT)

CB_SRCS := test_cluster_command_buffer.c cluster_command_buffer.c \
           cluster_allocator.c $(TRANSPORT)

DEV_SRCS := test_cluster_device.c cluster_device.c cluster_command_buffer.c \
            cluster_allocator.c $(TRANSPORT)

test_cluster_allocator: $(SRCS) cluster_allocator.h
	$(CC) $(CFLAGS) $(SAN) $(CPPFLAGS) $(SRCS) $(LDLIBS) -o $@

test_cluster_command_buffer: $(CB_SRCS) cluster_command_buffer.h cluster_allocator.h
	$(CC) $(CFLAGS) $(SAN) $(CPPFLAGS) $(CB_SRCS) $(LDLIBS) -o $@

test_cluster_device: $(DEV_SRCS) cluster_device.h cluster_command_buffer.h cluster_allocator.h
	$(CC) $(CFLAGS) $(SAN) $(CPPFLAGS) $(DEV_SRCS) $(LDLIBS) -o $@

.PHONY: check clean
check: test_cluster_allocator test_cluster_command_buffer test_cluster_device
	./test_cluster_allocator
	./test_cluster_command_buffer
	./test_cluster_device

clean:
	rm -f test_cluster_allocator test_cluster_command_buffer test_cluster_device
