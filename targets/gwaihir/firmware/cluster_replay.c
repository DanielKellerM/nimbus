// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "cluster_replay.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Bounds checking
//===----------------------------------------------------------------------===//
// The QCS stream is untrusted host data. A device-PA `pa` plus `len` bytes is
// valid iff [pa, pa+len) lies entirely within [0, region->size). All arithmetic
// is done on uint64_t with explicit overflow guards so a wrapped (pa+len) can
// never appear in-range.

static int qcs_range_in_region(const qcs_shared_region_t* region, uint64_t pa,
                               uint64_t len) {
  if (len == 0) {
    // A zero-length access still needs a base inside [0, size].
    return pa <= region->size;
  }
  if (pa > region->size) return 0;
  if (len > region->size) return 0;
  if (pa > region->size - len) return 0;  // pa + len > size (no overflow)
  return 1;
}

//===----------------------------------------------------------------------===//
// Executable table
//===----------------------------------------------------------------------===//

void cluster_replay_table_init(cluster_replay_table_t* table) {
  if (!table) return;
  memset(table, 0, sizeof(*table));
}

int cluster_replay_register(cluster_replay_table_t* table,
                            uint32_t executable_id, uint32_t export_ordinal,
                            iree_hal_executable_dispatch_v0_t fn) {
  if (!table) return -1;
  // Replace an existing entry with the same key.
  for (uint32_t i = 0; i < table->count; ++i) {
    if (table->entries[i].used &&
        table->entries[i].executable_id == executable_id &&
        table->entries[i].export_ordinal == export_ordinal) {
      table->entries[i].fn = fn;
      return 0;
    }
  }
  if (table->count >= CLUSTER_REPLAY_MAX_KERNELS) return -1;
  cluster_replay_entry_t* e = &table->entries[table->count++];
  e->executable_id = executable_id;
  e->export_ordinal = export_ordinal;
  e->used = 1;
  e->fn = fn;
  return 0;
}

static iree_hal_executable_dispatch_v0_t cluster_replay_lookup(
    const cluster_replay_table_t* table, uint32_t executable_id,
    uint32_t export_ordinal) {
  for (uint32_t i = 0; i < table->count; ++i) {
    if (table->entries[i].used &&
        table->entries[i].executable_id == executable_id &&
        table->entries[i].export_ordinal == export_ordinal) {
      return table->entries[i].fn;
    }
  }
  return NULL;
}

//===----------------------------------------------------------------------===//
// Record handlers
//===----------------------------------------------------------------------===//

static int replay_copy(qcs_shared_region_t* region, const qcs_copy_t* copy) {
  if (!qcs_range_in_region(region, copy->src_ptr, copy->length) ||
      !qcs_range_in_region(region, copy->dst_ptr, copy->length)) {
    return CLUSTER_REPLAY_ERR_BOUNDS;
  }
  if (copy->length) {
    memcpy(qcs_pa_to_ptr(region, copy->dst_ptr),
           qcs_pa_to_ptr(region, copy->src_ptr), (size_t)copy->length);
  }
  return CLUSTER_REPLAY_OK;
}

static int replay_fill(qcs_shared_region_t* region, const qcs_fill_t* fill) {
  uint32_t plen = fill->pattern_length;
  if (plen != 1 && plen != 2 && plen != 4 && plen != 8) {
    return CLUSTER_REPLAY_ERR_BAD_FILL;
  }
  if (fill->length % plen != 0) return CLUSTER_REPLAY_ERR_BAD_FILL;
  if (!qcs_range_in_region(region, fill->dst_ptr, fill->length)) {
    return CLUSTER_REPLAY_ERR_BOUNDS;
  }
  uint8_t* dst = (uint8_t*)qcs_pa_to_ptr(region, fill->dst_ptr);
  // The pattern unit is the low `plen` bytes of `pattern`, little-endian.
  uint8_t unit[8];
  for (uint32_t b = 0; b < plen; ++b) {
    unit[b] = (uint8_t)((fill->pattern >> (8u * b)) & 0xffu);
  }
  for (uint64_t off = 0; off < fill->length; off += plen) {
    memcpy(dst + off, unit, plen);
  }
  return CLUSTER_REPLAY_OK;
}

static int replay_update(qcs_shared_region_t* region,
                         const qcs_update_t* update) {
  if (!qcs_range_in_region(region, update->dst_ptr, update->length)) {
    return CLUSTER_REPLAY_ERR_BOUNDS;
  }
  if (update->length) {
    memcpy(qcs_pa_to_ptr(region, update->dst_ptr), qcs_update_data(update),
           (size_t)update->length);
  }
  return CLUSTER_REPLAY_OK;
}

static int replay_dispatch(qcs_shared_region_t* region,
                           const qcs_dispatch_t* dispatch,
                           const cluster_replay_table_t* table) {
  iree_hal_executable_dispatch_v0_t fn = cluster_replay_lookup(
      table, dispatch->executable_id, dispatch->export_ordinal);
  if (!fn) return CLUSTER_REPLAY_ERR_NO_KERNEL;

  // Resolve the workgroup count. For an indirect dispatch the 3x uint32 count
  // lives in (untrusted) device memory and must be bounds-checked first.
  uint32_t count_x = dispatch->workgroup_count[0];
  uint32_t count_y = dispatch->workgroup_count[1];
  uint32_t count_z = dispatch->workgroup_count[2];
  if (dispatch->flags & QCS_DISPATCH_FLAG_INDIRECT) {
    // 4-byte aligned (a uint32 load on an unaligned PA traps on rv32) and the
    // full 12 bytes in-region before reading.
    if ((dispatch->workgroup_count_ptr & 3u) != 0 ||
        !qcs_range_in_region(region, dispatch->workgroup_count_ptr,
                             3u * sizeof(uint32_t))) {
      return CLUSTER_REPLAY_ERR_BOUNDS;
    }
    const uint32_t* wgc =
        (const uint32_t*)qcs_pa_to_ptr(region, dispatch->workgroup_count_ptr);
    count_x = wgc[0];
    count_y = wgc[1];
    count_z = wgc[2];
  }

  // The geometry is untrusted (stream- or device-provided). Reject anything past
  // the executable-library ABI limits (z dims uint16, binding_count uint8,
  // constant_count uint16) rather than silently truncating, and bound the total
  // grid so a hostile count can't hang the replay loop.
  if (count_z > UINT16_MAX || dispatch->workgroup_size[2] > UINT16_MAX ||
      dispatch->binding_count > UINT8_MAX ||
      dispatch->constant_count > UINT16_MAX) {
    return CLUSTER_REPLAY_ERR_BAD_DISPATCH;
  }
  if ((uint64_t)count_x * (uint64_t)count_y * (uint64_t)count_z >
      CLUSTER_REPLAY_MAX_WORKGROUPS) {
    return CLUSTER_REPLAY_ERR_BAD_GRID;
  }

  // Translate + bounds-check every binding to a host pointer up front.
  uint32_t binding_count = dispatch->binding_count;
  const qcs_binding_t* bindings = qcs_dispatch_bindings(dispatch);
  void** binding_ptrs = NULL;
  size_t* binding_lengths = NULL;
  if (binding_count) {
    binding_ptrs = (void**)calloc(binding_count, sizeof(void*));
    binding_lengths = (size_t*)calloc(binding_count, sizeof(size_t));
    if (!binding_ptrs || !binding_lengths) {
      free(binding_ptrs);
      free(binding_lengths);
      return CLUSTER_REPLAY_ERR_ARGS;
    }
    for (uint32_t i = 0; i < binding_count; ++i) {
      if (!qcs_range_in_region(region, bindings[i].device_ptr,
                               bindings[i].length)) {
        free(binding_ptrs);
        free(binding_lengths);
        return CLUSTER_REPLAY_ERR_BOUNDS;
      }
      binding_ptrs[i] = qcs_pa_to_ptr(region, bindings[i].device_ptr);
      binding_lengths[i] = (size_t)bindings[i].length;
    }
  }

  // A zeroed environment is fine for stub kernels (no specialization constants,
  // no imports, no processor info).
  iree_hal_executable_environment_v0_t environment;
  memset(&environment, 0, sizeof(environment));

  const uint32_t* constants = qcs_dispatch_constants(dispatch);

  // Build the per-dispatch state shared by every workgroup. Field semantics
  // mirror runtime/runtime/src/Quidditch/dispatch/dispatch.c and the host-side
  // command buffer: workgroup_size_* / workgroup_count_* are the grid dims,
  // constant_count + constants is the push-constant table, and binding_ptrs /
  // binding_lengths are 1:1 dense arrays of the used bindings.
  iree_hal_executable_dispatch_state_v0_t state;
  memset(&state, 0, sizeof(state));
  state.workgroup_size_x = dispatch->workgroup_size[0];
  state.workgroup_size_y = dispatch->workgroup_size[1];
  state.workgroup_size_z = (uint16_t)dispatch->workgroup_size[2];
  state.workgroup_count_x = count_x;
  state.workgroup_count_y = count_y;
  state.workgroup_count_z = (uint16_t)count_z;
  state.max_concurrency = 1;
  state.constant_count = (uint16_t)dispatch->constant_count;
  state.binding_count = (uint8_t)binding_count;
  state.constants = constants;
  state.binding_ptrs = binding_ptrs;
  state.binding_lengths = binding_lengths;

  // Per-workgroup scratch (workgroup local memory). dynamic_local_memory is the
  // requested byte count; allocate at least that. NULL when zero is fine.
  void* local_memory = NULL;
  uint32_t local_memory_size = dispatch->dynamic_local_memory;
  if (local_memory_size) {
    local_memory = malloc(local_memory_size);
    if (!local_memory) {
      free(binding_ptrs);
      free(binding_lengths);
      return CLUSTER_REPLAY_ERR_ARGS;
    }
  }

  // Phase-1: a single-threaded grid loop. The real snrt_* fan-out distributes
  // workgroups across the 8 compute cores (see dispatch.c); that is a Phase-2
  // refinement. Here we drive one workgroup at a time on this thread.
  int rc = CLUSTER_REPLAY_OK;
  for (uint32_t z = 0; z < count_z && rc == CLUSTER_REPLAY_OK; ++z) {
    for (uint32_t y = 0; y < count_y && rc == CLUSTER_REPLAY_OK; ++y) {
      for (uint32_t x = 0; x < count_x && rc == CLUSTER_REPLAY_OK; ++x) {
        iree_hal_executable_workgroup_state_v0_t wg;
        memset(&wg, 0, sizeof(wg));
        wg.workgroup_id_x = x;
        wg.workgroup_id_y = y;
        wg.workgroup_id_z = (uint16_t)z;
        wg.processor_id = 0;
        wg.local_memory = local_memory;
        wg.local_memory_size = local_memory_size;
        if (fn(&environment, &state, &wg) != 0) {
          rc = CLUSTER_REPLAY_ERR_KERNEL;
        }
      }
    }
  }

  free(local_memory);
  free(binding_ptrs);
  free(binding_lengths);
  return rc;
}

//===----------------------------------------------------------------------===//
// Stream replay
//===----------------------------------------------------------------------===//

int cluster_replay_stream(qcs_shared_region_t* region,
                          const qcs_job_descriptor_t* job,
                          const cluster_replay_table_t* table) {
  if (!region || !region->base || !job || !table) {
    return CLUSTER_REPLAY_ERR_ARGS;
  }
  if (job->magic != QCS_MAGIC || job->version != QCS_VERSION) {
    return CLUSTER_REPLAY_ERR_ARGS;
  }

  // The command stream itself is a device-PA range; bounds-check it before
  // handing it to the reader. cmd_stream_len is uint64 in the descriptor but
  // the reader takes uint32; reject anything that does not fit.
  if (job->cmd_stream_len > UINT32_MAX) return CLUSTER_REPLAY_ERR_STREAM;
  if (!qcs_range_in_region(region, job->cmd_stream_ptr, job->cmd_stream_len)) {
    return CLUSTER_REPLAY_ERR_STREAM;
  }

  qcs_reader_t reader;
  qcs_reader_init(&reader, qcs_pa_to_ptr(region, job->cmd_stream_ptr),
                  (uint32_t)job->cmd_stream_len);

  const qcs_record_header_t* header;
  while ((header = qcs_reader_next(&reader)) != NULL) {
    int rc;
    switch (header->type) {
      case QCS_CMD_COPY:
        rc = replay_copy(region, (const qcs_copy_t*)header);
        break;
      case QCS_CMD_FILL:
        rc = replay_fill(region, (const qcs_fill_t*)header);
        break;
      case QCS_CMD_UPDATE:
        rc = replay_update(region, (const qcs_update_t*)header);
        break;
      case QCS_CMD_DISPATCH:
        rc = replay_dispatch(region, (const qcs_dispatch_t*)header, table);
        break;
      default:
        // qcs_reader_next already rejects unknown types (returns NULL), so this
        // is defensive only.
        return CLUSTER_REPLAY_ERR_MALFORMED;
    }
    if (rc != CLUSTER_REPLAY_OK) return rc;
  }

  // The reader stops at end-of-stream OR on a malformed record. Distinguish the
  // two: if we did not consume the whole stream, the trailing bytes were a bad
  // record.
  if (reader.offset < reader.size) return CLUSTER_REPLAY_ERR_MALFORMED;

  return CLUSTER_REPLAY_OK;
}
