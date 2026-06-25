// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 (slice 2c) minimal custom IREE HAL device for the host<->cluster
// split. This is the *host* side that ties slices 2a + 2b together:
//
//   - device_allocator()        -> the slice-2a shared-arena allocator
//                                  (runtime/host/hal/cluster/cluster_allocator.h)
//   - create_command_buffer()   -> a slice-2b QCS-emitting command buffer
//                                  (runtime/host/hal/cluster/cluster_command_buffer.h)
//                                  whose QCS stream buffer is itself allocated
//                                  from the shared arena, so its on-device
//                                  address (cmd_stream_ptr) is a real device-PA.
//   - queue_execute()           -> publish the recorded QCS stream into the
//                                  shared job descriptor, ring the doorbell,
//                                  spin-wait for completion, then signal the
//                                  signal semaphores (synchronous, concurrency
//                                  == 1; no async timelines).
//
// Most of the iree_hal_device_t vtable is stubbed: this slice exercises only
// record + submit, not executable loading / events / channels / queue-ordered
// allocations. Those return UNIMPLEMENTED or are trivial no-ops.
//
// Semaphores: because submission is fully synchronous (queue_execute blocks
// until the cluster reports completion) we use a self-contained trivial host
// semaphore (a monotonic uint64 guarded by a mutex/condvar). The host IREE
// build does not include iree/hal/utils/semaphore_base or the local_sync
// driver, so we cannot reuse those; the trivial semaphore only touches the
// inline iree_hal_resource_* helpers and the public semaphore vtable.
//
// POSIX/dev-box host-side plumbing only (x86 host build).

#ifndef QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_DEVICE_H_
#define QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_DEVICE_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "runtime/host/transport/shared_region.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a cluster HAL device named |id| over the shared |region|. The device
// creates and owns a slice-2a cluster allocator over |region| (retrieved via
// iree_hal_device_allocator). The device borrows |region| for its lifetime: the
// caller owns |region| and must outlive the device. |out_device| must be
// released by the caller with iree_hal_device_release.
iree_status_t iree_hal_cluster_device_create(iree_string_view_t id,
                                             qcs_shared_region_t* region,
                                             iree_allocator_t host_allocator,
                                             iree_hal_device_t** out_device);

// Synchronous submit helper used by queue_execute and exposed directly for
// tests / callers that don't want to build the semaphore lists. Publishes the
// command buffer's recorded QCS stream into the shared job descriptor, rings
// the doorbell, and spin-waits for completion. Returns INTERNAL if the cluster
// reports a non-zero status. |command_buffer| must be a cluster command buffer
// created by this device (so its QCS stream device-PA is known).
iree_status_t iree_hal_cluster_device_submit(
    iree_hal_device_t* device, iree_hal_command_buffer_t* command_buffer);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_DEVICE_H_
