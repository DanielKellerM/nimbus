# Copyright 2026 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# QCS replayer: rv32 Snitch-cluster firmware that replays a host-written QCS
# command stream from L2 SPM (host-device split, Phase 2).

APP              := qcs_replay
$(APP)_BUILD_DIR ?= $(GW_SNITCH_SW_DIR)/apps/$(APP)/build
SRC_DIR          := $(GW_SNITCH_SW_DIR)/apps/$(APP)/src

# All sources compile+link into one ELF via common.mk; no datagen (the host supplies
# input at runtime via L2 SPM). qcs_world_comm.c is intentionally NOT linked: its
# size=1 world-communicator override is obsolete under the simple_offload boot model
# (host wakes all clusters), so the default weak snrt_comm_world_info is correct (see
# that file's header for the full history).
SRCS := $(SRC_DIR)/main.c \
        $(SRC_DIR)/qcs_replay.c \
        $(SRC_DIR)/cluster_command_stream.c \
        $(SRC_DIR)/quidditch_snrt_exports.c

$(APP)_INCDIRS := $(SRC_DIR)
# Header for the prebuilt iree+xdsl kernel static-library query.
$(APP)_INCDIRS += $(GW_SNITCH_SW_DIR)/apps/$(APP)/lib

# The compiled model's iree+xdsl kernel static library + its library-query symbol.
# Both are model-derived (produce_kernel_lib.sh / link_into_gwaihir_tree.sh emit
# them); default to gemm_square so the stock deploy is unchanged. Override for any
# other model, e.g. the MLP front-door:
#   make ... QCS_KERNEL_LIB=libmlp_kernel.a \
#            QCS_KERNEL_LIBRARY_QUERY=quidditch_mlp_linked_quidditch_library_query
# The .a joins the final link via common.mk's $(APP)_LIBS -> -L/-l mechanism (the
# link recipe compiles $(SRCS) with `-x c++` and cannot take a bare .o). Define
# QCS_USE_STUB_KERNEL to fall back to the stub.
QCS_KERNEL_LIB           ?= libgemm_square_kernel.a
QCS_KERNEL_LIBRARY_QUERY ?= quidditch_gemm64_dispatch_0_library_query
$(APP)_LIBS += $(GW_SNITCH_SW_DIR)/apps/$(APP)/lib/$(QCS_KERNEL_LIB)
# Set BEFORE the include so common.mk snapshots it into the app's target-specific CFLAGS.
$(APP)_RISCV_CFLAGS += -DQCS_KERNEL_LIBRARY_QUERY=$(QCS_KERNEL_LIBRARY_QUERY)

include $(SN_ROOT)/sw/kernels/common.mk
