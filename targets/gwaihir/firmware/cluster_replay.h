// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Cluster-side QCS replayer (host-device split, Phase 1 slice 3).
//
// The host serializes a recorded command buffer into a flat QCS command stream
// in the shared region (see runtime/host/transport/) and rings a doorbell. This
// is the CLUSTER side of that contract: it reads the QCS stream and replays it.
//
// On the dev box this runs as a plain process/thread (no RTL, no rv32): COPY/
// FILL/UPDATE are plain memcpy/memset, and DISPATCH invokes host-C STUB kernels
// through the IREE executable-library ABI (iree_hal_executable_dispatch_v0_t).
// Real rv32 kernels and the snrt_* multi-core fan-out are a Phase-2 refinement.
//
// The QCS stream is UNTRUSTED host data per the ABI header: every device-PA and
// length is bounds-checked against [0, region->size) before being dereferenced,
// so a malformed stream yields an error instead of a segfault.

#ifndef QUIDDITCH_HOST_FIRMWARE_CLUSTER_REPLAY_H_
#define QUIDDITCH_HOST_FIRMWARE_CLUSTER_REPLAY_H_

#include <stdint.h>

#include "iree/hal/local/executable_library.h"
#include "runtime/host/transport/cluster_command_stream.h"
#include "runtime/host/transport/shared_region.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of stub kernels the replayer table can hold. Phase-1 tests
// register a handful; bump if needed.
#define CLUSTER_REPLAY_MAX_KERNELS 64

// Sane cap on a dispatch's total workgroup count (x*y*z). The grid dims come
// from untrusted stream/device data; without a cap a hostile huge count hangs
// the replay loop. 16M workgroups is far above any real Phase-1 job.
#define CLUSTER_REPLAY_MAX_WORKGROUPS (1u << 24)

// One registered stub kernel, keyed by (executable_id, export_ordinal).
typedef struct cluster_replay_entry_t {
  uint32_t executable_id;
  uint32_t export_ordinal;
  int used;
  iree_hal_executable_dispatch_v0_t fn;
} cluster_replay_entry_t;

// The replayer's executable table. Zero-initialize before use (or call
// cluster_replay_table_init).
typedef struct cluster_replay_table_t {
  cluster_replay_entry_t entries[CLUSTER_REPLAY_MAX_KERNELS];
  uint32_t count;
} cluster_replay_table_t;

// Clears the table.
void cluster_replay_table_init(cluster_replay_table_t* table);

// Registers a stub kernel for (executable_id, export_ordinal). Replaces an
// existing entry with the same key. Returns 0 on success, -1 if the table is
// full.
int cluster_replay_register(cluster_replay_table_t* table,
                            uint32_t executable_id, uint32_t export_ordinal,
                            iree_hal_executable_dispatch_v0_t fn);

// Replays the QCS command stream referenced by `job` over `region`, using the
// stub kernels in `table` for DISPATCH records. Returns 0 on success, or a
// negative error code on a malformed/out-of-bounds stream, a missing kernel, or
// a kernel returning nonzero. Never dereferences an out-of-region device-PA.
int cluster_replay_stream(qcs_shared_region_t* region,
                          const qcs_job_descriptor_t* job,
                          const cluster_replay_table_t* table);

// Error codes (negative). 0 is success.
enum {
  CLUSTER_REPLAY_OK = 0,
  CLUSTER_REPLAY_ERR_ARGS = -1,        // null args / bad job descriptor
  CLUSTER_REPLAY_ERR_STREAM = -2,      // stream ptr/len out of region
  CLUSTER_REPLAY_ERR_MALFORMED = -3,   // reader rejected a record
  CLUSTER_REPLAY_ERR_BOUNDS = -4,      // a device-PA + length escaped the region
  CLUSTER_REPLAY_ERR_NO_KERNEL = -5,   // DISPATCH for an unregistered kernel
  CLUSTER_REPLAY_ERR_KERNEL = -6,      // a stub kernel returned nonzero
  CLUSTER_REPLAY_ERR_BAD_FILL = -7,    // FILL pattern_length invalid
  CLUSTER_REPLAY_ERR_BAD_DISPATCH = -8,  // dispatch geometry exceeds ABI limits
  CLUSTER_REPLAY_ERR_BAD_GRID = -9,    // workgroup count exceeds the grid cap
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_FIRMWARE_CLUSTER_REPLAY_H_
