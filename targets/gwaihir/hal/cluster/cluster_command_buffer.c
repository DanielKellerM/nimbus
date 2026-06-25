// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "runtime/host/hal/cluster/cluster_command_buffer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "runtime/host/transport/cluster_command_stream.h"

//===----------------------------------------------------------------------===//
// iree_hal_cluster_command_buffer_t
//===----------------------------------------------------------------------===//

// Maximum distinct bindings we resolve for a single dispatch. The QCS
// dispatch record carries an arbitrary count; this just bounds the on-stack
// translation scratch.
#define IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS 32

// Maximum distinct executables tracked in the host-owned executable table.
#define IREE_HAL_CLUSTER_MAX_EXECUTABLES 64

typedef struct iree_hal_cluster_command_buffer_t {
  iree_hal_command_buffer_t base;
  iree_allocator_t host_allocator;

  // Shared region used to translate host-VA mappings -> device-PA.
  qcs_shared_region_t* region;

  // Writer over the caller-provided QCS stream buffer.
  qcs_writer_t writer;

  // Host-owned executable table: distinct iree_hal_executable_t* -> id.
  iree_hal_executable_t* executables[IREE_HAL_CLUSTER_MAX_EXECUTABLES];
  uint32_t executable_count;

  // First error encountered while recording (e.g. mapping failure or stream
  // overflow). Reported back at `end`.
  iree_status_t record_status;
} iree_hal_cluster_command_buffer_t;

static const iree_hal_command_buffer_vtable_t
    iree_hal_cluster_command_buffer_vtable;

static iree_hal_cluster_command_buffer_t* iree_hal_cluster_command_buffer_cast(
    iree_hal_command_buffer_t* base_value) {
  IREE_HAL_ASSERT_TYPE(base_value, &iree_hal_cluster_command_buffer_vtable);
  return (iree_hal_cluster_command_buffer_t*)base_value;
}

bool iree_hal_cluster_command_buffer_isa(
    iree_hal_command_buffer_t* command_buffer) {
  return iree_hal_resource_is(&command_buffer->resource,
                              &iree_hal_cluster_command_buffer_vtable);
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

// Resolves a buffer reference to its device-physical address by mapping the
// buffer at |ref.offset| and computing (mapped_ptr - region_base). Unmaps
// before returning; the PA stays valid because the heap wrap is stable for the
// buffer's lifetime.
static iree_status_t iree_hal_cluster_command_buffer_ref_to_pa(
    iree_hal_cluster_command_buffer_t* command_buffer,
    iree_hal_buffer_ref_t ref, uint64_t* out_pa) {
  *out_pa = 0;
  if (!ref.buffer) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "binding buffer is NULL");
  }
  iree_hal_buffer_mapping_t mapping = {{0}};
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_range(
      ref.buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_ANY,
      ref.offset, ref.length ? ref.length : 1, &mapping));
  *out_pa = qcs_ptr_to_pa(command_buffer->region, mapping.contents.data);
  iree_hal_buffer_unmap_range(&mapping);
  return iree_ok_status();
}

// Returns the host-owned id for |executable|, allocating a new one if unseen.
static iree_status_t iree_hal_cluster_command_buffer_executable_id(
    iree_hal_cluster_command_buffer_t* command_buffer,
    iree_hal_executable_t* executable, uint32_t* out_id) {
  for (uint32_t i = 0; i < command_buffer->executable_count; ++i) {
    if (command_buffer->executables[i] == executable) {
      *out_id = i;
      return iree_ok_status();
    }
  }
  if (command_buffer->executable_count >= IREE_HAL_CLUSTER_MAX_EXECUTABLES) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "executable table full (max %d)",
                            IREE_HAL_CLUSTER_MAX_EXECUTABLES);
  }
  uint32_t id = command_buffer->executable_count++;
  command_buffer->executables[id] = executable;
  *out_id = id;
  return iree_ok_status();
}

// Records the first error; later errors are dropped.
static void iree_hal_cluster_command_buffer_fail(
    iree_hal_cluster_command_buffer_t* command_buffer, iree_status_t status) {
  if (iree_status_is_ok(command_buffer->record_status)) {
    command_buffer->record_status = status;
  } else {
    iree_status_ignore(status);
  }
}

//===----------------------------------------------------------------------===//
// Lifetime
//===----------------------------------------------------------------------===//

static void iree_hal_cluster_command_buffer_destroy(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  iree_allocator_t host_allocator = command_buffer->host_allocator;
  iree_status_ignore(command_buffer->record_status);
  iree_allocator_free(host_allocator, command_buffer);
}

iree_status_t iree_hal_cluster_command_buffer_create(
    qcs_shared_region_t* region, iree_hal_allocator_t* device_allocator,
    void* stream_ptr, uint32_t stream_capacity,
    iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t command_categories,
    iree_hal_queue_affinity_t queue_affinity, iree_host_size_t binding_capacity,
    iree_allocator_t host_allocator,
    iree_hal_command_buffer_t** out_command_buffer) {
  IREE_ASSERT_ARGUMENT(region);
  IREE_ASSERT_ARGUMENT(stream_ptr);
  IREE_ASSERT_ARGUMENT(out_command_buffer);
  *out_command_buffer = NULL;

  if ((((uintptr_t)stream_ptr) & (QCS_RECORD_ALIGN - 1)) != 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "QCS stream buffer must be 8-byte aligned");
  }
  // Binding tables are resolved by IREE only at submission, so a recorder that
  // serializes at record time cannot honor them yet. Reject rather than emit
  // dispatches with unresolved (NULL-buffer) refs.
  if (binding_capacity > 0) {
    return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                            "cluster command buffer does not support indirect "
                            "binding tables");
  }

  iree_host_size_t total_size =
      sizeof(iree_hal_cluster_command_buffer_t) +
      iree_hal_command_buffer_validation_state_size(mode, binding_capacity);

  uint8_t* storage = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, total_size, (void**)&storage));
  memset(storage, 0, sizeof(iree_hal_cluster_command_buffer_t));

  iree_hal_cluster_command_buffer_t* command_buffer =
      (iree_hal_cluster_command_buffer_t*)storage;
  iree_hal_command_buffer_initialize(
      device_allocator, mode, command_categories, queue_affinity,
      binding_capacity,
      storage + sizeof(iree_hal_cluster_command_buffer_t),
      &iree_hal_cluster_command_buffer_vtable, &command_buffer->base);
  command_buffer->host_allocator = host_allocator;
  command_buffer->region = region;
  command_buffer->executable_count = 0;
  command_buffer->record_status = iree_ok_status();
  qcs_writer_init(&command_buffer->writer, stream_ptr, stream_capacity);

  *out_command_buffer = &command_buffer->base;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Accessors
//===----------------------------------------------------------------------===//

uint32_t iree_hal_cluster_command_buffer_size(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  return command_buffer->writer.size;
}

uint32_t iree_hal_cluster_command_buffer_record_count(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  return command_buffer->writer.record_count;
}

uint64_t iree_hal_cluster_command_buffer_stream_pa(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  return qcs_ptr_to_pa(command_buffer->region, command_buffer->writer.data);
}

//===----------------------------------------------------------------------===//
// Recording control
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_begin(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  // Reset the stream and executable table for a fresh recording.
  qcs_writer_init(&command_buffer->writer, command_buffer->writer.data,
                  command_buffer->writer.capacity);
  command_buffer->executable_count = 0;
  iree_status_ignore(command_buffer->record_status);
  command_buffer->record_status = iree_ok_status();
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_end(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  if (!iree_status_is_ok(command_buffer->record_status)) {
    iree_status_t status = command_buffer->record_status;
    command_buffer->record_status = iree_ok_status();
    return status;
  }
  if (command_buffer->writer.overflowed) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "QCS stream overflowed during recording");
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Debug groups
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_begin_debug_group(
    iree_hal_command_buffer_t* base_command_buffer, iree_string_view_t label,
    iree_hal_label_color_t label_color,
    const iree_hal_label_location_t* location) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_end_debug_group(
    iree_hal_command_buffer_t* base_command_buffer) {
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Synchronization (no-ops: the cluster replays synchronously in stream order)
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_execution_barrier(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_execution_stage_t source_stage_mask,
    iree_hal_execution_stage_t target_stage_mask,
    iree_hal_execution_barrier_flags_t flags,
    iree_host_size_t memory_barrier_count,
    const iree_hal_memory_barrier_t* memory_barriers,
    iree_host_size_t buffer_barrier_count,
    const iree_hal_buffer_barrier_t* buffer_barriers) {
  // The stream is replayed in order, so barriers are implicit.
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_signal_event(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_event_t* event,
    iree_hal_execution_stage_t source_stage_mask) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_reset_event(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_event_t* event,
    iree_hal_execution_stage_t source_stage_mask) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_wait_events(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_host_size_t event_count, const iree_hal_event_t** events,
    iree_hal_execution_stage_t source_stage_mask,
    iree_hal_execution_stage_t target_stage_mask,
    iree_host_size_t memory_barrier_count,
    const iree_hal_memory_barrier_t* memory_barriers,
    iree_host_size_t buffer_barrier_count,
    const iree_hal_buffer_barrier_t* buffer_barriers) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_advise_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_ref_t buffer_ref, iree_hal_memory_advise_flags_t flags,
    uint64_t arg0, uint64_t arg1) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_collective(
    iree_hal_command_buffer_t* base_command_buffer, iree_hal_channel_t* channel,
    iree_hal_collective_op_t op, uint32_t param,
    iree_hal_buffer_ref_t send_binding, iree_hal_buffer_ref_t recv_binding,
    iree_device_size_t element_count) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "collectives not implemented on the cluster");
}

//===----------------------------------------------------------------------===//
// Transfer commands -> QCS records
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_fill_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_ref_t target_ref, const void* pattern,
    iree_host_size_t pattern_length, iree_hal_fill_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);

  uint64_t dst_pa = 0;
  iree_status_t status = iree_hal_cluster_command_buffer_ref_to_pa(
      command_buffer, target_ref, &dst_pa);
  if (!iree_status_is_ok(status)) {
    iree_hal_cluster_command_buffer_fail(command_buffer, status);
    return iree_ok_status();
  }

  // Pack up to 8 bytes of the pattern unit into a little-endian uint64.
  uint64_t pattern_u64 = 0;
  iree_host_size_t copy_len = pattern_length > 8 ? 8 : pattern_length;
  memcpy(&pattern_u64, pattern, copy_len);

  if (qcs_write_fill(&command_buffer->writer, dst_pa,
                     (uint64_t)target_ref.length, pattern_u64,
                     (uint32_t)pattern_length) != 0) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "QCS fill write failed (overflow or bad pattern)"));
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_update_buffer(
    iree_hal_command_buffer_t* base_command_buffer, const void* source_buffer,
    iree_host_size_t source_offset, iree_hal_buffer_ref_t target_ref,
    iree_hal_update_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);

  uint64_t dst_pa = 0;
  iree_status_t status = iree_hal_cluster_command_buffer_ref_to_pa(
      command_buffer, target_ref, &dst_pa);
  if (!iree_status_is_ok(status)) {
    iree_hal_cluster_command_buffer_fail(command_buffer, status);
    return iree_ok_status();
  }

  const uint8_t* src = (const uint8_t*)source_buffer + source_offset;
  if (qcs_write_update(&command_buffer->writer, dst_pa, src,
                       (uint64_t)target_ref.length) != 0) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "QCS update write failed (overflow)"));
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_copy_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_ref_t source_ref, iree_hal_buffer_ref_t target_ref,
    iree_hal_copy_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);

  uint64_t src_pa = 0;
  uint64_t dst_pa = 0;
  iree_status_t status = iree_hal_cluster_command_buffer_ref_to_pa(
      command_buffer, source_ref, &src_pa);
  if (iree_status_is_ok(status)) {
    status = iree_hal_cluster_command_buffer_ref_to_pa(command_buffer,
                                                       target_ref, &dst_pa);
  }
  if (!iree_status_is_ok(status)) {
    iree_hal_cluster_command_buffer_fail(command_buffer, status);
    return iree_ok_status();
  }

  if (qcs_write_copy(&command_buffer->writer, src_pa, dst_pa,
                     (uint64_t)target_ref.length) != 0) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "QCS copy write failed (overflow)"));
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Dispatch -> QCS DISPATCH record
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_dispatch(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_executable_t* executable,
    iree_hal_executable_export_ordinal_t export_ordinal,
    const iree_hal_dispatch_config_t config, iree_const_byte_span_t constants,
    iree_hal_buffer_ref_list_t bindings, iree_hal_dispatch_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);

  if (iree_hal_dispatch_uses_custom_arguments(flags)) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                         "custom dispatch arguments not supported"));
    return iree_ok_status();
  }

  if (bindings.count > IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "too many bindings (%" PRIhsz " > %d)", bindings.count,
                         IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS));
    return iree_ok_status();
  }

  // Host-owned executable id.
  uint32_t executable_id = 0;
  iree_status_t status = iree_hal_cluster_command_buffer_executable_id(
      command_buffer, executable, &executable_id);
  if (!iree_status_is_ok(status)) {
    iree_hal_cluster_command_buffer_fail(command_buffer, status);
    return iree_ok_status();
  }

  // Resolve bindings -> device-PAs.
  qcs_binding_t qcs_bindings[IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS];
  for (iree_host_size_t i = 0; i < bindings.count; ++i) {
    uint64_t pa = 0;
    status = iree_hal_cluster_command_buffer_ref_to_pa(
        command_buffer, bindings.values[i], &pa);
    if (!iree_status_is_ok(status)) {
      iree_hal_cluster_command_buffer_fail(command_buffer, status);
      return iree_ok_status();
    }
    qcs_bindings[i].device_ptr = pa;
    qcs_bindings[i].length = (uint64_t)bindings.values[i].length;
  }

  // Push constants are a dense uint32 array; a non-multiple-of-4 span would
  // silently drop a trailing partial word, so reject it.
  if ((constants.data_length % sizeof(uint32_t)) != 0) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                         "push-constant span not a multiple of 4 bytes"));
    return iree_ok_status();
  }
  const uint32_t* constants_u32 = (const uint32_t*)constants.data;
  uint32_t constant_count =
      (uint32_t)(constants.data_length / sizeof(uint32_t));
  uint32_t dyn_l1 = config.dynamic_workgroup_local_memory;

  int rc;
  if (iree_hal_dispatch_uses_indirect_parameters(flags)) {
    uint64_t wgc_pa = 0;
    status = iree_hal_cluster_command_buffer_ref_to_pa(
        command_buffer, config.workgroup_count_ref, &wgc_pa);
    if (!iree_status_is_ok(status)) {
      iree_hal_cluster_command_buffer_fail(command_buffer, status);
      return iree_ok_status();
    }
    rc = qcs_write_dispatch_indirect(
        &command_buffer->writer, executable_id, (uint32_t)export_ordinal,
        wgc_pa, config.workgroup_size, dyn_l1, constant_count, constants_u32,
        (uint32_t)bindings.count, qcs_bindings);
  } else {
    rc = qcs_write_dispatch(&command_buffer->writer, executable_id,
                            (uint32_t)export_ordinal, config.workgroup_count,
                            config.workgroup_size, dyn_l1, constant_count,
                            constants_u32, (uint32_t)bindings.count,
                            qcs_bindings);
  }
  if (rc != 0) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer,
        iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                         "QCS dispatch write failed (overflow)"));
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Vtable
//===----------------------------------------------------------------------===//

static const iree_hal_command_buffer_vtable_t
    iree_hal_cluster_command_buffer_vtable = {
        .destroy = iree_hal_cluster_command_buffer_destroy,
        .begin = iree_hal_cluster_command_buffer_begin,
        .end = iree_hal_cluster_command_buffer_end,
        .begin_debug_group = iree_hal_cluster_command_buffer_begin_debug_group,
        .end_debug_group = iree_hal_cluster_command_buffer_end_debug_group,
        .execution_barrier = iree_hal_cluster_command_buffer_execution_barrier,
        .signal_event = iree_hal_cluster_command_buffer_signal_event,
        .reset_event = iree_hal_cluster_command_buffer_reset_event,
        .wait_events = iree_hal_cluster_command_buffer_wait_events,
        .advise_buffer = iree_hal_cluster_command_buffer_advise_buffer,
        .fill_buffer = iree_hal_cluster_command_buffer_fill_buffer,
        .update_buffer = iree_hal_cluster_command_buffer_update_buffer,
        .copy_buffer = iree_hal_cluster_command_buffer_copy_buffer,
        .collective = iree_hal_cluster_command_buffer_collective,
        .dispatch = iree_hal_cluster_command_buffer_dispatch,
};
