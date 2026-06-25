// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 (slice 2b) QCS-emitting IREE HAL command buffer for the host<->cluster
// split.
//
// This is the *host* side of the split. The IREE VM/HAL runs on the CVA6 host
// and records a command buffer; instead of executing the dispatches/copies
// inline (as the Snitch-firmware command buffer in
// runtime/runtime/src/Quidditch/command_buffer/command_buffer.c does) this
// implementation SERIALIZES each command into a flat QCS command stream
// (runtime/host/transport/cluster_command_stream.h) the cluster will replay.
//
// Device buffers come from the slice-2a allocator
// (runtime/host/hal/cluster/cluster_allocator.h) which heap-wraps region_base+PA.
// To translate a binding into a device-physical address we map the buffer at its
// offset and compute pa = qcs_ptr_to_pa(region, mapped_ptr).
//
// POSIX/dev-box host-side plumbing only (x86 host build).

#ifndef QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_COMMAND_BUFFER_H_
#define QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_COMMAND_BUFFER_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "runtime/host/transport/shared_region.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates a command buffer that records into the caller-provided QCS stream
// buffer |stream_ptr| (capacity |stream_capacity| bytes; must be 8-byte
// aligned). |region| is used to translate buffer mappings into device-physical
// addresses. |device_allocator| is the cluster allocator (used for the HAL
// command-buffer base and validation). The command buffer borrows |region| and
// |stream_ptr| for its lifetime; the caller owns and must outlive them.
// |out_command_buffer| must be released by the caller.
iree_status_t iree_hal_cluster_command_buffer_create(
    qcs_shared_region_t* region, iree_hal_allocator_t* device_allocator,
    void* stream_ptr, uint32_t stream_capacity,
    iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t command_categories,
    iree_hal_queue_affinity_t queue_affinity, iree_host_size_t binding_capacity,
    iree_allocator_t host_allocator,
    iree_hal_command_buffer_t** out_command_buffer);

// Returns true if |command_buffer| is a cluster command buffer.
bool iree_hal_cluster_command_buffer_isa(
    iree_hal_command_buffer_t* command_buffer);

// Reads back the number of bytes written into the QCS stream so far (the
// qcs_writer_t::size after `end`).
uint32_t iree_hal_cluster_command_buffer_size(
    iree_hal_command_buffer_t* command_buffer);

// Reads back the number of records written into the QCS stream so far (the
// qcs_writer_t::record_count after `end`).
uint32_t iree_hal_cluster_command_buffer_record_count(
    iree_hal_command_buffer_t* command_buffer);

// Device-PA of this command buffer's QCS stream (offset of the stream buffer in
// the shared region). The submit path puts this in the job descriptor.
uint64_t iree_hal_cluster_command_buffer_stream_pa(
    iree_hal_command_buffer_t* command_buffer);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_HAL_CLUSTER_CLUSTER_COMMAND_BUFFER_H_
