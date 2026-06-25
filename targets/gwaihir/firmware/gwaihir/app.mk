# Copyright 2026 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# QCS replayer: rv32 Snitch-cluster firmware that replays a host-written QCS
# command stream from L2 SPM (host-device split, Phase 2).

APP              := qcs_replay
$(APP)_BUILD_DIR ?= $(GW_SNITCH_SW_DIR)/apps/$(APP)/build
SRC_DIR          := $(GW_SNITCH_SW_DIR)/apps/$(APP)/src

# All sources are compiled + linked into one ELF by the common.mk recipe
# (which passes $(SRCS) to a single $(SN_RISCV_CXX) -x c++ invocation). No
# datagen step: the input is supplied at runtime by the host via L2 SPM.
# NOTE: qcs_world_comm.c is intentionally NOT linked. It used to strong-override
# snrt_comm_world_info with a size=1 world communicator to dodge the boot-time
# global barrier. That was masking the real bug: the host woke only cluster 0,
# so the other 15 never reached the world=SNRT_CLUSTER_NUM(=16) barrier. The fix
# is the simple_offload boot model (host entries+wakes all clusters), so the
# default weak snrt_comm_world_info (size=SNRT_CLUSTER_NUM) is now correct and
# the override is removed (see qcs_world_comm.c header for the full history).
SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/qcs_replay.c \
        $(SRC_DIR)/cluster_command_stream.c \
        $(SRC_DIR)/quidditch_snrt_exports.c

$(APP)_INCDIRS := $(SRC_DIR)
# Header for the prebuilt iree+xdsl kernel static-library query.
$(APP)_INCDIRS += $(GW_SNITCH_SW_DIR)/apps/$(APP)/lib

# Prebuilt iree+xdsl gemm_square kernel object, packaged as a static library so
# it joins the final link via common.mk's $(APP)_LIBS -> -L/-l mechanism (the
# common.mk link recipe compiles $(SRCS) with `-x c++` and cannot accept a bare
# .o on the command line, but it does add $(APP)_LIBS to the link). The .a is
# committed-prebuilt; regenerate from lib/gemm_square_kernel.o with llvm-ar if
# the kernel changes. Define QCS_USE_STUB_KERNEL to fall back to the stub.
$(APP)_LIBS += $(GW_SNITCH_SW_DIR)/apps/$(APP)/lib/libgemm_square_kernel.a

include $(SN_ROOT)/sw/kernels/common.mk
