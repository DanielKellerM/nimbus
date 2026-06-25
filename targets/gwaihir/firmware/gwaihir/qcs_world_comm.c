// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// REVERTED: single-cluster (world=1) communicator override -- REMOVED.
//
// This file previously strong-overrode the snRuntime's weak
// `snrt_comm_world_info` with a size=1 world communicator so the boot-time
// global software barrier (snrt_init_libs -> snrt_comm_init) would resolve for
// cluster 0 alone. That was a WORKAROUND, not a fix: it masked the real root
// cause of our recurring cluster deadlocks -- the host woke ONLY cluster 0, so
// the other 15 clusters never booted and never arrived at the
// world=SNRT_CLUSTER_NUM (=16) global barrier that cluster 0 was (correctly)
// waiting on.
//
// The correct fix follows gwaihir's PROVEN cluster-boot model
// (sw/cheshire/tests/simple_offload.c + sw/tests/src/simple.c):
//   * the host entries ALL SNRT_CLUSTER_NUM clusters' boot scratch regs and
//     wakes cluster 0 only; cluster 0's snRuntime wakes the rest, so all 16
//     clusters reach the world barrier and it resolves with the DEFAULT
//     snrt_comm_world_info.size == SNRT_CLUSTER_NUM (no override needed);
//   * non-cluster-0 clusters have no QCS job and return immediately from main()
//     (see main.c's snrt_cluster_idx() guard), reporting per-core completion via
//     crt0's snrt_exit into the host's return_code_array.
//
// This file is therefore intentionally EMPTY (no strong symbol) and is NOT
// listed in app.mk SRCS. It is retained only as a record of the revert.
