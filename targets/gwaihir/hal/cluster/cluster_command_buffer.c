// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// QCS-emitting IREE HAL command buffer (host side of the host<->cluster split).
//
// IREE's reusable command buffers record bindings as *indirect* references into
// a binding table that is only supplied at submission (queue_execute). A
// recorder that serializes immediately therefore cannot resolve bindings to
// device-PAs at record time. This implementation DEFERS: each command is
// captured into an in-memory record list (carrying the raw iree_hal_buffer_ref_t
// with its buffer_slot), and `iree_hal_cluster_command_buffer_emit` resolves
// every ref against the submission binding table and writes the flat QCS stream.
//
// Device buffers come from the slice-2a allocator (heap-wrapped region_base+PA);
// a resolved ref's device-PA is recovered by mapping it and computing
// pa = qcs_ptr_to_pa(region, mapped_ptr).

#include "runtime/host/hal/cluster/cluster_command_buffer.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "runtime/host/transport/cluster_command_stream.h"

#define IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS 32
#define IREE_HAL_CLUSTER_MAX_EXECUTABLES 64
#define IREE_HAL_CLUSTER_MAX_RECORDS 256
#define IREE_HAL_CLUSTER_MAX_CONSTANTS 64
// Match IREE's update_buffer contract (IREE_HAL_COMMAND_BUFFER_MAX_UPDATE_SIZE).
#define IREE_HAL_CLUSTER_INLINE_UPDATE_BYTES (64u * 1024u)
static_assert(IREE_HAL_CLUSTER_INLINE_UPDATE_BYTES >=
                  IREE_HAL_COMMAND_BUFFER_MAX_UPDATE_SIZE,
              "inline update cap must cover IREE's max update size");

typedef enum {
  CLUSTER_REC_DISPATCH = 1,
  CLUSTER_REC_COPY,
  CLUSTER_REC_FILL,
  CLUSTER_REC_UPDATE,
} cluster_rec_kind_t;

typedef struct cluster_deferred_record_t {
  cluster_rec_kind_t kind;
  // DISPATCH
  uint32_t executable_id;
  uint32_t export_ordinal;
  uint32_t flags;           // 0 or QCS_DISPATCH_FLAG_INDIRECT
  uint32_t workgroup_count[3];
  uint32_t workgroup_size[3];
  uint32_t dynamic_local_memory;
  uint32_t constant_count;
  uint32_t constants[IREE_HAL_CLUSTER_MAX_CONSTANTS];
  uint32_t binding_count;
  iree_hal_buffer_ref_t bindings[IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS];
  iree_hal_buffer_ref_t workgroup_count_ref;  // indirect dispatch
  // COPY/FILL/UPDATE
  iree_hal_buffer_ref_t src_ref;     // COPY source
  iree_hal_buffer_ref_t dst_ref;     // COPY/FILL/UPDATE target
  uint64_t length;
  uint64_t fill_pattern;
  uint32_t fill_pattern_length;
  uint32_t update_length;
  uint8_t update_data[IREE_HAL_CLUSTER_INLINE_UPDATE_BYTES];
} cluster_deferred_record_t;

typedef struct iree_hal_cluster_command_buffer_t {
  iree_hal_command_buffer_t base;
  iree_allocator_t host_allocator;

  qcs_shared_region_t* region;

  // QCS stream buffer (filled at emit time).
  void* stream_ptr;
  uint32_t stream_capacity;
  qcs_writer_t writer;  // valid after emit

  iree_hal_executable_t* executables[IREE_HAL_CLUSTER_MAX_EXECUTABLES];
  uint32_t executable_count;

  // Deferred command list.
  cluster_deferred_record_t records[IREE_HAL_CLUSTER_MAX_RECORDS];
  uint32_t record_count;

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

static void iree_hal_cluster_command_buffer_fail(
    iree_hal_cluster_command_buffer_t* command_buffer, iree_status_t status) {
  if (iree_status_is_ok(command_buffer->record_status)) {
    command_buffer->record_status = status;
  } else {
    iree_status_ignore(status);
  }
}

static cluster_deferred_record_t* iree_hal_cluster_alloc_record(
    iree_hal_cluster_command_buffer_t* command_buffer,
    cluster_rec_kind_t kind) {
  if (command_buffer->record_count >= IREE_HAL_CLUSTER_MAX_RECORDS) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer, iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                         "cluster command buffer record limit"));
    return NULL;
  }
  cluster_deferred_record_t* r =
      &command_buffer->records[command_buffer->record_count++];
  memset(r, 0, sizeof(*r));
  r->kind = kind;
  return r;
}

// Retains a direct ref's buffer so it survives between record and emit (emit
// maps it). Indirect refs (buffer==NULL) are resolved from the binding table at
// submit and owned by the caller, so are not retained here.
static void iree_hal_cluster_retain_direct_ref(iree_hal_buffer_ref_t ref) {
  if (ref.buffer) iree_hal_buffer_retain(ref.buffer);
}

// Releases every direct-ref buffer retained while recording |r|.
static void iree_hal_cluster_release_record_refs(
    const cluster_deferred_record_t* r) {
  switch (r->kind) {
    case CLUSTER_REC_DISPATCH:
      for (uint32_t b = 0; b < r->binding_count; ++b) {
        if (r->bindings[b].buffer) iree_hal_buffer_release(r->bindings[b].buffer);
      }
      if ((r->flags & QCS_DISPATCH_FLAG_INDIRECT) &&
          r->workgroup_count_ref.buffer) {
        iree_hal_buffer_release(r->workgroup_count_ref.buffer);
      }
      break;
    case CLUSTER_REC_COPY:
      if (r->src_ref.buffer) iree_hal_buffer_release(r->src_ref.buffer);
      if (r->dst_ref.buffer) iree_hal_buffer_release(r->dst_ref.buffer);
      break;
    case CLUSTER_REC_FILL:
    case CLUSTER_REC_UPDATE:
      if (r->dst_ref.buffer) iree_hal_buffer_release(r->dst_ref.buffer);
      break;
  }
}

// Releases all retained direct-ref buffers and resets the record list.
static void iree_hal_cluster_release_all_records(
    iree_hal_cluster_command_buffer_t* command_buffer) {
  for (uint32_t i = 0; i < command_buffer->record_count; ++i) {
    iree_hal_cluster_release_record_refs(&command_buffer->records[i]);
  }
  command_buffer->record_count = 0;
}

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
                            "executable table full");
  }
  uint32_t id = command_buffer->executable_count++;
  command_buffer->executables[id] = executable;
  *out_id = id;
  return iree_ok_status();
}

// Resolve a (possibly indirect) ref against the binding table to a device-PA.
static iree_status_t iree_hal_cluster_resolve_ref_to_pa(
    iree_hal_cluster_command_buffer_t* command_buffer,
    iree_hal_buffer_ref_t ref, iree_hal_buffer_binding_table_t binding_table,
    uint64_t* out_pa) {
  *out_pa = 0;
  iree_hal_buffer_ref_t resolved;
  IREE_RETURN_IF_ERROR(
      iree_hal_buffer_binding_table_resolve_ref(binding_table, ref, &resolved));
  if (!resolved.buffer) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "binding resolved to NULL buffer");
  }
  iree_hal_buffer_mapping_t mapping = {{0}};
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_range(
      resolved.buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_ANY,
      resolved.offset, resolved.length ? resolved.length : 1, &mapping));
  *out_pa = qcs_ptr_to_pa(command_buffer->region, mapping.contents.data);
  iree_hal_buffer_unmap_range(&mapping);
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Lifetime
//===----------------------------------------------------------------------===//

static void iree_hal_cluster_command_buffer_destroy(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  iree_allocator_t host_allocator = command_buffer->host_allocator;
  iree_hal_cluster_release_all_records(command_buffer);
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
  command_buffer->stream_ptr = stream_ptr;
  command_buffer->stream_capacity = stream_capacity;
  command_buffer->executable_count = 0;
  command_buffer->record_count = 0;
  command_buffer->record_status = iree_ok_status();
  qcs_writer_init(&command_buffer->writer, stream_ptr, stream_capacity);

  *out_command_buffer = &command_buffer->base;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Emit: resolve deferred records against the binding table -> QCS stream
//===----------------------------------------------------------------------===//

iree_status_t iree_hal_cluster_command_buffer_emit(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_binding_table_t binding_table) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  if (!iree_status_is_ok(command_buffer->record_status)) {
    iree_status_t status = command_buffer->record_status;
    command_buffer->record_status = iree_ok_status();
    return status;
  }
  qcs_writer_init(&command_buffer->writer, command_buffer->stream_ptr,
                  command_buffer->stream_capacity);
  qcs_writer_t* w = &command_buffer->writer;

  for (uint32_t i = 0; i < command_buffer->record_count; ++i) {
    const cluster_deferred_record_t* r = &command_buffer->records[i];
    switch (r->kind) {
      case CLUSTER_REC_DISPATCH: {
        qcs_binding_t qcs_bindings[IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS];
        for (uint32_t b = 0; b < r->binding_count; ++b) {
          uint64_t pa = 0;
          IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
              command_buffer, r->bindings[b], binding_table, &pa));
          qcs_bindings[b].device_ptr = pa;
          qcs_bindings[b].length = (uint64_t)r->bindings[b].length;
        }
        int rc;
        if (r->flags & QCS_DISPATCH_FLAG_INDIRECT) {
          uint64_t wgc_pa = 0;
          IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
              command_buffer, r->workgroup_count_ref, binding_table, &wgc_pa));
          rc = qcs_write_dispatch_indirect(
              w, r->executable_id, r->export_ordinal, wgc_pa,
              r->workgroup_size, r->dynamic_local_memory, r->constant_count,
              r->constants, r->binding_count, qcs_bindings);
        } else {
          rc = qcs_write_dispatch(w, r->executable_id, r->export_ordinal,
                                  r->workgroup_count, r->workgroup_size,
                                  r->dynamic_local_memory, r->constant_count,
                                  r->constants, r->binding_count, qcs_bindings);
        }
        if (rc != 0) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "QCS dispatch write overflow");
        }
        break;
      }
      case CLUSTER_REC_COPY: {
        uint64_t src_pa = 0, dst_pa = 0;
        IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
            command_buffer, r->src_ref, binding_table, &src_pa));
        IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
            command_buffer, r->dst_ref, binding_table, &dst_pa));
        if (qcs_write_copy(w, src_pa, dst_pa, r->length) != 0) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "QCS copy write overflow");
        }
        break;
      }
      case CLUSTER_REC_FILL: {
        uint64_t dst_pa = 0;
        IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
            command_buffer, r->dst_ref, binding_table, &dst_pa));
        if (qcs_write_fill(w, dst_pa, r->length, r->fill_pattern,
                           r->fill_pattern_length) != 0) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "QCS fill write overflow");
        }
        break;
      }
      case CLUSTER_REC_UPDATE: {
        uint64_t dst_pa = 0;
        IREE_RETURN_IF_ERROR(iree_hal_cluster_resolve_ref_to_pa(
            command_buffer, r->dst_ref, binding_table, &dst_pa));
        if (qcs_write_update(w, dst_pa, r->update_data, r->update_length) != 0) {
          return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "QCS update write overflow");
        }
        break;
      }
    }
  }
  if (w->overflowed) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "QCS stream overflowed");
  }
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
  return qcs_ptr_to_pa(command_buffer->region, command_buffer->stream_ptr);
}

//===----------------------------------------------------------------------===//
// Recording control
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_begin(
    iree_hal_command_buffer_t* base_command_buffer) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  command_buffer->executable_count = 0;
  // Re-recording: drop refs retained by a previous begin/end cycle.
  iree_hal_cluster_release_all_records(command_buffer);
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
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Debug groups / sync (no-ops)
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
static iree_status_t iree_hal_cluster_command_buffer_execution_barrier(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_execution_stage_t source_stage_mask,
    iree_hal_execution_stage_t target_stage_mask,
    iree_hal_execution_barrier_flags_t flags,
    iree_host_size_t memory_barrier_count,
    const iree_hal_memory_barrier_t* memory_barriers,
    iree_host_size_t buffer_barrier_count,
    const iree_hal_buffer_barrier_t* buffer_barriers) {
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
// Transfer commands -> deferred records
//===----------------------------------------------------------------------===//

static iree_status_t iree_hal_cluster_command_buffer_fill_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_ref_t target_ref, const void* pattern,
    iree_host_size_t pattern_length, iree_hal_fill_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  cluster_deferred_record_t* r =
      iree_hal_cluster_alloc_record(command_buffer, CLUSTER_REC_FILL);
  if (!r) return iree_ok_status();
  r->dst_ref = target_ref;
  iree_hal_cluster_retain_direct_ref(target_ref);
  r->length = (uint64_t)target_ref.length;
  uint64_t pattern_u64 = 0;
  iree_host_size_t copy_len = pattern_length > 8 ? 8 : pattern_length;
  memcpy(&pattern_u64, pattern, copy_len);
  r->fill_pattern = pattern_u64;
  r->fill_pattern_length = (uint32_t)pattern_length;
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_update_buffer(
    iree_hal_command_buffer_t* base_command_buffer, const void* source_buffer,
    iree_host_size_t source_offset, iree_hal_buffer_ref_t target_ref,
    iree_hal_update_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  if ((uint64_t)target_ref.length > IREE_HAL_CLUSTER_INLINE_UPDATE_BYTES) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer, iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                         "update payload too large"));
    return iree_ok_status();
  }
  cluster_deferred_record_t* r =
      iree_hal_cluster_alloc_record(command_buffer, CLUSTER_REC_UPDATE);
  if (!r) return iree_ok_status();
  r->dst_ref = target_ref;
  iree_hal_cluster_retain_direct_ref(target_ref);
  r->update_length = (uint32_t)target_ref.length;
  memcpy(r->update_data, (const uint8_t*)source_buffer + source_offset,
         r->update_length);
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_command_buffer_copy_buffer(
    iree_hal_command_buffer_t* base_command_buffer,
    iree_hal_buffer_ref_t source_ref, iree_hal_buffer_ref_t target_ref,
    iree_hal_copy_flags_t flags) {
  iree_hal_cluster_command_buffer_t* command_buffer =
      iree_hal_cluster_command_buffer_cast(base_command_buffer);
  cluster_deferred_record_t* r =
      iree_hal_cluster_alloc_record(command_buffer, CLUSTER_REC_COPY);
  if (!r) return iree_ok_status();
  r->src_ref = source_ref;
  r->dst_ref = target_ref;
  iree_hal_cluster_retain_direct_ref(source_ref);
  iree_hal_cluster_retain_direct_ref(target_ref);
  r->length = (uint64_t)target_ref.length;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Dispatch -> deferred record
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
        command_buffer, iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                                         "custom dispatch arguments"));
    return iree_ok_status();
  }
  if (bindings.count > IREE_HAL_CLUSTER_MAX_DISPATCH_BINDINGS) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer, iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                         "too many bindings"));
    return iree_ok_status();
  }
  if ((constants.data_length % sizeof(uint32_t)) != 0 ||
      constants.data_length / sizeof(uint32_t) >
          IREE_HAL_CLUSTER_MAX_CONSTANTS) {
    iree_hal_cluster_command_buffer_fail(
        command_buffer, iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                         "bad/too-many push constants"));
    return iree_ok_status();
  }

  uint32_t executable_id = 0;
  iree_status_t status = iree_hal_cluster_command_buffer_executable_id(
      command_buffer, executable, &executable_id);
  if (!iree_status_is_ok(status)) {
    iree_hal_cluster_command_buffer_fail(command_buffer, status);
    return iree_ok_status();
  }

  cluster_deferred_record_t* r =
      iree_hal_cluster_alloc_record(command_buffer, CLUSTER_REC_DISPATCH);
  if (!r) return iree_ok_status();
  r->executable_id = executable_id;
  r->export_ordinal = (uint32_t)export_ordinal;
  r->dynamic_local_memory = config.dynamic_workgroup_local_memory;
  r->constant_count = (uint32_t)(constants.data_length / sizeof(uint32_t));
  if (r->constant_count) {
    memcpy(r->constants, constants.data, constants.data_length);
  }
  r->binding_count = (uint32_t)bindings.count;
  for (iree_host_size_t i = 0; i < bindings.count; ++i) {
    r->bindings[i] = bindings.values[i];
    iree_hal_cluster_retain_direct_ref(bindings.values[i]);
  }
  for (int i = 0; i < 3; ++i) {
    r->workgroup_size[i] = config.workgroup_size[i];
  }
  if (iree_hal_dispatch_uses_indirect_parameters(flags)) {
    r->flags = QCS_DISPATCH_FLAG_INDIRECT;
    r->workgroup_count_ref = config.workgroup_count_ref;
    iree_hal_cluster_retain_direct_ref(config.workgroup_count_ref);
  } else {
    for (int i = 0; i < 3; ++i) {
      r->workgroup_count[i] = config.workgroup_count[i];
    }
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
