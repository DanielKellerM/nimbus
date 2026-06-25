// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Cluster-side (rv32 Snitch firmware) QCS replayer for the host-device split.
//
// The Cheshire/CVA6 host serializes a recorded command buffer into a flat QCS
// command stream in L2 SPM, publishes a qcs_job_descriptor_t, and rings the
// cluster doorbell. This is the CLUSTER side of that contract: the DM core
// reads the QCS stream and replays it onto the 8 compute cores.
//
//   COPY/FILL/UPDATE -> iDMA / memset / memcpy (DM core)
//   DISPATCH         -> build an iree_hal_executable_dispatch_state_v0_t +
//                       workgroup_state from the record and invoke the kernel fn
//                       over the workgroup grid via the snRuntime fan-out
//                       (DM core assigns, compute cores run, hw_barrier).
//
// This is the rv32 / real-snRuntime port of the Phase-1 dev-box replayer in
// Quidditch's runtime/host/firmware/cluster_replay.c (which used pthreads +
// plain memcpy). The logic, error codes, and untrusted-stream bounds discipline
// are carried over unchanged; only the I/O primitives and the dispatch fan-out
// are device-specific.
//
// UNTRUSTED STREAM: every device-PA + length from the stream is bounds-checked
// against the L2-SPM aperture [region.base, region.base + region.size) before
// being turned into a pointer, so a malformed/hostile stream yields an error
// code instead of a trap.

#ifndef QCS_REPLAY_H_
#define QCS_REPLAY_H_

#include <stddef.h>
#include <stdint.h>

#include "cluster_command_stream.h"
#include "qcs_kernel_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Firmware shared region
//===----------------------------------------------------------------------===//
// Firmware analogue of the dev-box qcs_shared_region_t (which was mmap-based).
// `base` is the host-VA (here: the rv32 cluster's view) of device-PA 0 of the
// L2-SPM aperture; QCS device-PAs are treated as offsets into [0, size), exactly
// as in the reference replayer. On gwaihir base == GW_L2_SPM_BASE_ADDR(0).

typedef struct qcs_fw_region_t {
  uint8_t* base;  // cluster pointer to device-PA 0 (L2 SPM base)
  uint64_t size;  // aperture size in bytes
} qcs_fw_region_t;

// device-PA (offset into the aperture) -> cluster pointer.
static inline void* qcs_fw_pa_to_ptr(const qcs_fw_region_t* region,
                                     uint64_t pa) {
  return region->base + (uintptr_t)pa;
}

//===----------------------------------------------------------------------===//
// Executable (kernel) table
//===----------------------------------------------------------------------===//

#define QCS_REPLAY_MAX_KERNELS 64

// Sane cap on a dispatch's total workgroup count (x*y*z) from untrusted data.
#define QCS_REPLAY_MAX_WORKGROUPS (1u << 24)

typedef struct qcs_replay_entry_t {
  uint32_t executable_id;
  uint32_t export_ordinal;
  int used;
  // A Quidditch dispatch has TWO halves (see runtime dispatch.c / harness.c):
  // the compute half runs on the 8 compute cores over the workgroup grid, and
  // the DMA half runs on the DM core (once per dispatch) to stream the data.
  // `dma_fn` may be NULL for a kernel with no DMA half (then the DM core just
  // waits while the compute cores run, as in the compute-only version).
  iree_hal_executable_dispatch_v0_t compute_fn;
  iree_hal_executable_dispatch_v0_t dma_fn;
} qcs_replay_entry_t;

typedef struct qcs_replay_table_t {
  qcs_replay_entry_t entries[QCS_REPLAY_MAX_KERNELS];
  uint32_t count;
} qcs_replay_table_t;

void qcs_replay_table_init(qcs_replay_table_t* table);

// Registers BOTH halves of a kernel for (executable_id, export_ordinal): the
// compute-core fn (run on the compute cores over the grid) and the DM-core DMA
// fn (run once per dispatch on the DM core). `dma_fn` may be NULL for a kernel
// with no DMA half. Replaces an existing entry with the same key. Returns 0 on
// success, -1 if full.
int qcs_replay_register(qcs_replay_table_t* table, uint32_t executable_id,
                        uint32_t export_ordinal,
                        iree_hal_executable_dispatch_v0_t compute_fn,
                        iree_hal_executable_dispatch_v0_t dma_fn);

//===----------------------------------------------------------------------===//
// Replay
//===----------------------------------------------------------------------===//

// Replays the QCS stream referenced by `job` over `region` using `table`.
// MUST be called by all cluster cores (the DISPATCH fan-out synchronizes the
// 8 compute cores + DM core via cluster hardware barriers). Returns 0 on
// success or a negative error code. The return value is only meaningful on the
// DM core; compute cores return after the final barrier.
int qcs_replay_stream(qcs_fw_region_t* region, const qcs_job_descriptor_t* job,
                      const qcs_replay_table_t* table);

enum {
  QCS_REPLAY_OK = 0,
  QCS_REPLAY_ERR_ARGS = -1,
  QCS_REPLAY_ERR_STREAM = -2,
  QCS_REPLAY_ERR_MALFORMED = -3,
  QCS_REPLAY_ERR_BOUNDS = -4,
  QCS_REPLAY_ERR_NO_KERNEL = -5,
  QCS_REPLAY_ERR_KERNEL = -6,
  QCS_REPLAY_ERR_BAD_FILL = -7,
  QCS_REPLAY_ERR_BAD_DISPATCH = -8,
  QCS_REPLAY_ERR_BAD_GRID = -9,
  QCS_REPLAY_ERR_LOCAL_MEM = -10,  // dynamic_local_memory exceeds L1 scratch
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QCS_REPLAY_H_
