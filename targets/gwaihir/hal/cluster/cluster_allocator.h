// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 custom IREE HAL allocator for the host<->cluster split.
//
// Device buffers are carved out of the shared mmap region (qcs_shared_region_t)
// via the host-side bump allocator (qcs_shared_alloc). A buffer's
// "device-physical address" (PA) is its offset into that region. To avoid a
// custom iree_hal_buffer_t subclass we simply wrap region_base+PA with
// iree_hal_heap_buffer_wrap; the PA is recovered later by mapping the buffer
// and computing (mapped_ptr - region_base).
//
// POSIX/dev-box host-side plumbing only (x86 host build).

#ifndef QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_ALLOCATOR_H_
#define QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_ALLOCATOR_H_

#include "iree/base/api.h"
#include "iree/hal/allocator.h"
#include "runtime/host/transport/shared_region.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a shared-arena HAL allocator backed by |region|. The bump pointer is
// initialised to QCS_SHARED_ARENA_OFFSET. The allocator borrows |region| for
// its lifetime (the caller owns and must outlive it). |out_allocator| must be
// released by the caller.
iree_status_t iree_hal_cluster_allocator_create(
    qcs_shared_region_t* region, iree_allocator_t host_allocator,
    iree_hal_allocator_t** out_allocator);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_ALLOCATOR_H_
