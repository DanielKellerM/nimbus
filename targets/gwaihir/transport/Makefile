# Phase-0/1 host/cluster transport proofs (dev-box, no IREE/RTL deps).
CFLAGS ?= -std=c11 -Wall -Wextra -O2

# Phase 0a/0b: the command-stream ABI serialize/replay roundtrip.
test_command_stream: test_command_stream.c cluster_command_stream.c \
                     cluster_command_stream.h
	$(CC) $(CFLAGS) test_command_stream.c cluster_command_stream.c -o $@

# Phase 1: two-process shared-mmap transport + doorbell handshake.
test_shared_region: test_shared_region.c shared_region.c \
                    cluster_command_stream.c shared_region.h \
                    cluster_command_stream.h
	$(CC) $(CFLAGS) test_shared_region.c shared_region.c \
	      cluster_command_stream.c -o $@

.PHONY: check clean
check: test_command_stream test_shared_region
	./test_command_stream
	./test_shared_region

clean:
	rm -f test_command_stream test_shared_region
