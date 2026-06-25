// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "qcs_replay.h"

#include <string.h>

#include "snrt.h"

//===----------------------------------------------------------------------===//
// Bounds checking (carried over verbatim from cluster_replay.c)
//===----------------------------------------------------------------------===//
// A device-PA `pa` plus `len` bytes is valid iff [pa, pa+len) lies entirely
// within [0, region->size). All arithmetic is on uint64_t with explicit
// overflow guards so a wrapped (pa+len) can never appear in-range.

static int qcs_range_in_region(const qcs_fw_region_t* region, uint64_t pa,
                               uint64_t len) {
  if (len == 0) return pa <= region->size;
  if (pa > region->size) return 0;
  if (len > region->size) return 0;
  if (pa > region->size - len) return 0;
  return 1;
}

//===----------------------------------------------------------------------===//
// Kernel table
//===----------------------------------------------------------------------===//

void qcs_replay_table_init(qcs_replay_table_t* table) {
  if (!table) return;
  memset(table, 0, sizeof(*table));
}

int qcs_replay_register(qcs_replay_table_t* table, uint32_t executable_id,
                        uint32_t export_ordinal,
                        iree_hal_executable_dispatch_v0_t compute_fn,
                        iree_hal_executable_dispatch_v0_t dma_fn) {
  if (!table) return -1;
  for (uint32_t i = 0; i < table->count; ++i) {
    if (table->entries[i].used &&
        table->entries[i].executable_id == executable_id &&
        table->entries[i].export_ordinal == export_ordinal) {
      table->entries[i].compute_fn = compute_fn;
      table->entries[i].dma_fn = dma_fn;
      return 0;
    }
  }
  if (table->count >= QCS_REPLAY_MAX_KERNELS) return -1;
  qcs_replay_entry_t* e = &table->entries[table->count++];
  e->executable_id = executable_id;
  e->export_ordinal = export_ordinal;
  e->used = 1;
  e->compute_fn = compute_fn;
  e->dma_fn = dma_fn;
  return 0;
}

// Looks up both halves for (executable_id, export_ordinal). Returns 0 and fills
// *compute_fn / *dma_fn on a hit (dma_fn may be NULL = no DMA half), -1 on miss.
static int qcs_replay_lookup(const qcs_replay_table_t* table,
                             uint32_t executable_id, uint32_t export_ordinal,
                             iree_hal_executable_dispatch_v0_t* compute_fn,
                             iree_hal_executable_dispatch_v0_t* dma_fn) {
  for (uint32_t i = 0; i < table->count; ++i) {
    if (table->entries[i].used &&
        table->entries[i].executable_id == executable_id &&
        table->entries[i].export_ordinal == export_ordinal) {
      *compute_fn = table->entries[i].compute_fn;
      *dma_fn = table->entries[i].dma_fn;
      return 0;
    }
  }
  return -1;
}

//===----------------------------------------------------------------------===//
// Cross-core dispatch hand-off (TCDM-resident, written by the DM core, read by
// the compute cores after the wake barrier). Lives in cluster L1.
//===----------------------------------------------------------------------===//
// The QCS ABI caps binding_count at UINT8_MAX, so the dense binding arrays are
// bounded and can be statically sized instead of heap-allocated (no malloc on
// the rv32 firmware data path).

#define QCS_REPLAY_MAX_BINDINGS 255u

typedef struct qcs_dispatch_args_t {
  // Both halves of the dispatch share the SAME environment + dispatch_state
  // (mirrors harness.c: one dispatch_state passed to compute_fn and dma_fn).
  // compute_fn runs on the compute cores over the grid; dma_fn runs once on the
  // DM core. dma_fn == NULL means the kernel has no DMA half.
  iree_hal_executable_dispatch_v0_t compute_fn;
  iree_hal_executable_dispatch_v0_t dma_fn;
  iree_hal_executable_environment_v0_t environment;
  iree_hal_executable_dispatch_state_v0_t state;
  void* binding_ptrs[QCS_REPLAY_MAX_BINDINGS];
  size_t binding_lengths[QCS_REPLAY_MAX_BINDINGS];
  const uint32_t* constants;
  void* local_memory;
  uint32_t local_memory_size;
  uint32_t count_x;
  uint32_t count_y;
  uint32_t count_z;
  volatile int rc;  // accumulated kernel return-code (0 == ok)
} qcs_dispatch_args_t;

// Compute-core grid loop: each compute core takes workgroups (x,y,z) whose
// linear index == core_idx (mod n_compute), invoking the kernel for each. This
// mirrors dispatch.c's per-core fan-out (DM core assigns, hw_barrier wakes the
// workers, they run, hw_barrier syncs back) but striped statically so no shared
// queue is needed for this milestone.
static void qcs_replay_compute_grid(qcs_dispatch_args_t* d) {
  uint32_t core = snrt_cluster_core_idx();
  uint32_t n = snrt_cluster_compute_core_num();
  uint64_t total =
      (uint64_t)d->count_x * (uint64_t)d->count_y * (uint64_t)d->count_z;
  for (uint64_t lin = core; lin < total; lin += n) {
    uint32_t x = (uint32_t)(lin % d->count_x);
    uint32_t rem = (uint32_t)(lin / d->count_x);
    uint32_t y = rem % d->count_y;
    uint32_t z = rem / d->count_y;

    iree_hal_executable_workgroup_state_v0_t wg;
    memset(&wg, 0, sizeof(wg));
    wg.workgroup_id_x = x;
    wg.workgroup_id_y = y;
    wg.workgroup_id_z = (uint16_t)z;
    wg.processor_id = core;
    wg.local_memory = d->local_memory;
    wg.local_memory_size = d->local_memory_size;
    if (d->compute_fn(&d->environment, &d->state, &wg) != 0) {
      d->rc = QCS_REPLAY_ERR_KERNEL;
    }
  }
}

// DM-core DMA half: run dma_fn ONCE per dispatch (NOT per workgroup), exactly as
// harness.c does (one dma_fn(&env, &state, &wg) call between the release barrier
// and the rejoin barrier). The compute half does its own intra-kernel work
// distribution; the DMA half streams the dispatch's data once. The DM core is
// not a compute core, so it presents workgroup_id (0,0,0) / its own processor_id
// -- the dma partition keys off the dispatch_state, not this id.
static void qcs_replay_dma_half(qcs_dispatch_args_t* d) {
  if (!d->dma_fn) return;
  iree_hal_executable_workgroup_state_v0_t wg;
  memset(&wg, 0, sizeof(wg));
  wg.workgroup_id_x = 0;
  wg.workgroup_id_y = 0;
  wg.workgroup_id_z = 0;
  wg.processor_id = snrt_cluster_core_idx();
  wg.local_memory = d->local_memory;
  wg.local_memory_size = d->local_memory_size;
  if (d->dma_fn(&d->environment, &d->state, &wg) != 0) {
    d->rc = QCS_REPLAY_ERR_KERNEL;
  }
}

//===----------------------------------------------------------------------===//
// Record handlers (DM core only)
//===----------------------------------------------------------------------===//

static int replay_copy(qcs_fw_region_t* region, const qcs_copy_t* copy) {
  if (!qcs_range_in_region(region, copy->src_ptr, copy->length) ||
      !qcs_range_in_region(region, copy->dst_ptr, copy->length)) {
    return QCS_REPLAY_ERR_BOUNDS;
  }
  if (copy->length) {
    // Device-side iDMA copy (PA -> PA within the L2-SPM aperture).
    snrt_dma_start_1d(qcs_fw_pa_to_ptr(region, copy->dst_ptr),
                      qcs_fw_pa_to_ptr(region, copy->src_ptr),
                      (size_t)copy->length);
    snrt_dma_wait_all();
  }
  return QCS_REPLAY_OK;
}

static int replay_fill(qcs_fw_region_t* region, const qcs_fill_t* fill) {
  uint32_t plen = fill->pattern_length;
  if (plen != 1 && plen != 2 && plen != 4 && plen != 8) {
    return QCS_REPLAY_ERR_BAD_FILL;
  }
  if (fill->length % plen != 0) return QCS_REPLAY_ERR_BAD_FILL;
  if (!qcs_range_in_region(region, fill->dst_ptr, fill->length)) {
    return QCS_REPLAY_ERR_BOUNDS;
  }
  uint8_t* dst = (uint8_t*)qcs_fw_pa_to_ptr(region, fill->dst_ptr);
  uint8_t unit[8];
  for (uint32_t b = 0; b < plen; ++b) {
    unit[b] = (uint8_t)((fill->pattern >> (8u * b)) & 0xffu);
  }
  for (uint64_t off = 0; off < fill->length; off += plen) {
    memcpy(dst + off, unit, plen);
  }
  return QCS_REPLAY_OK;
}

static int replay_update(qcs_fw_region_t* region, const qcs_update_t* update) {
  if (!qcs_range_in_region(region, update->dst_ptr, update->length)) {
    return QCS_REPLAY_ERR_BOUNDS;
  }
  if (update->length) {
    // The inline blob already lives in L2 SPM (it is part of the stream); DMA
    // it to its destination.
    snrt_dma_start_1d(qcs_fw_pa_to_ptr(region, update->dst_ptr),
                      (void*)qcs_update_data(update), (size_t)update->length);
    snrt_dma_wait_all();
  }
  return QCS_REPLAY_OK;
}

//===----------------------------------------------------------------------===//
// DISPATCH (DM core builds args + assigns; all cores run the grid)
//===----------------------------------------------------------------------===//

static int replay_dispatch_prepare(qcs_fw_region_t* region,
                                   const qcs_dispatch_t* dispatch,
                                   const qcs_replay_table_t* table,
                                   qcs_dispatch_args_t* out) {
  memset(out, 0, sizeof(*out));

  iree_hal_executable_dispatch_v0_t compute_fn = NULL;
  iree_hal_executable_dispatch_v0_t dma_fn = NULL;
  if (qcs_replay_lookup(table, dispatch->executable_id,
                        dispatch->export_ordinal, &compute_fn, &dma_fn) != 0 ||
      !compute_fn) {
    return QCS_REPLAY_ERR_NO_KERNEL;
  }

  uint32_t count_x = dispatch->workgroup_count[0];
  uint32_t count_y = dispatch->workgroup_count[1];
  uint32_t count_z = dispatch->workgroup_count[2];
  if (dispatch->flags & QCS_DISPATCH_FLAG_INDIRECT) {
    if ((dispatch->workgroup_count_ptr & 3u) != 0 ||
        !qcs_range_in_region(region, dispatch->workgroup_count_ptr,
                             3u * sizeof(uint32_t))) {
      return QCS_REPLAY_ERR_BOUNDS;
    }
    const uint32_t* wgc =
        (const uint32_t*)qcs_fw_pa_to_ptr(region, dispatch->workgroup_count_ptr);
    count_x = wgc[0];
    count_y = wgc[1];
    count_z = wgc[2];
  }

  if (count_z > UINT16_MAX || dispatch->workgroup_size[2] > UINT16_MAX ||
      dispatch->binding_count > QCS_REPLAY_MAX_BINDINGS ||
      dispatch->constant_count > UINT16_MAX) {
    return QCS_REPLAY_ERR_BAD_DISPATCH;
  }
  if ((uint64_t)count_x * (uint64_t)count_y * (uint64_t)count_z >
      QCS_REPLAY_MAX_WORKGROUPS) {
    return QCS_REPLAY_ERR_BAD_GRID;
  }
  // A degenerate grid (any zero dim) has no workgroups; nothing to do. Leave
  // both halves NULL so the stream loop runs neither half and skips the barrier
  // round-trip for this record (compute_fn == NULL is the "no work" gate).
  if (count_x == 0 || count_y == 0 || count_z == 0) {
    out->compute_fn = NULL;
    out->dma_fn = NULL;
    return QCS_REPLAY_OK;
  }

  // Translate + bounds-check every binding up front (no heap; bounded array).
  uint32_t binding_count = dispatch->binding_count;
  const qcs_binding_t* bindings = qcs_dispatch_bindings(dispatch);
  for (uint32_t i = 0; i < binding_count; ++i) {
    if (!qcs_range_in_region(region, bindings[i].device_ptr,
                             bindings[i].length)) {
      return QCS_REPLAY_ERR_BOUNDS;
    }
    out->binding_ptrs[i] = qcs_fw_pa_to_ptr(region, bindings[i].device_ptr);
    out->binding_lengths[i] = (size_t)bindings[i].length;
  }

  out->constants = qcs_dispatch_constants(dispatch);

  // Workgroup-local scratch (L1/TCDM). dynamic_local_memory is untrusted, so
  // bound it against the free TCDM (end - next) before handing the kernel the
  // bump pointer; an oversized request is rejected, not silently OOB'd.
  out->local_memory_size = dispatch->dynamic_local_memory;
  if (out->local_memory_size) {
    const snrt_allocator_t* l1 = snrt_l1_allocator();
    if ((uint64_t)out->local_memory_size > (uint64_t)(l1->end - l1->next)) {
      return QCS_REPLAY_ERR_LOCAL_MEM;
    }
    out->local_memory = snrt_l1_next();
  } else {
    out->local_memory = NULL;
  }

  out->compute_fn = compute_fn;
  out->dma_fn = dma_fn;  // may be NULL: kernel has no DMA half
  out->count_x = count_x;
  out->count_y = count_y;
  out->count_z = count_z;

  iree_hal_executable_dispatch_state_v0_t* st = &out->state;
  st->workgroup_size_x = dispatch->workgroup_size[0];
  st->workgroup_size_y = dispatch->workgroup_size[1];
  st->workgroup_size_z = (uint16_t)dispatch->workgroup_size[2];
  st->workgroup_count_x = count_x;
  st->workgroup_count_y = count_y;
  st->workgroup_count_z = (uint16_t)count_z;
  st->max_concurrency = (uint8_t)snrt_cluster_compute_core_num();
  st->constant_count = (uint16_t)dispatch->constant_count;
  st->binding_count = (uint8_t)binding_count;
  st->constants = out->constants;
  st->binding_ptrs = out->binding_ptrs;
  st->binding_lengths = out->binding_lengths;
  return QCS_REPLAY_OK;
}

//===----------------------------------------------------------------------===//
// Stream replay
//===----------------------------------------------------------------------===//
// Cluster execution model (mirrors dispatch.c):
//   * The DM core drives the stream: parses each record, performs COPY/FILL/
//     UPDATE via iDMA/memcpy, and for DISPATCH builds the shared args in TCDM.
//   * Compute cores spin at a cluster hardware barrier; the DM core releases
//     them (barrier) to run their slice of the grid, then both sides barrier
//     again to rejoin before the DM core advances to the next record.
// `g_args` and `g_phase` live in cluster L1 so every core sees the same copy.

typedef enum {
  QCS_PHASE_DISPATCH = 0,  // run a workgroup grid
  QCS_PHASE_DONE = 1,      // stream finished, workers exit
} qcs_phase_t;

static qcs_dispatch_args_t g_args;
static volatile qcs_phase_t g_phase;

int qcs_replay_stream(qcs_fw_region_t* region, const qcs_job_descriptor_t* job,
                      const qcs_replay_table_t* table) {
  int is_dm = snrt_is_dm_core();

  //===-- Compute cores: barrier-driven worker loop --------------------===//
  if (!is_dm) {
    for (;;) {
      snrt_cluster_hw_barrier();  // wait for the DM core to publish a phase
      if (g_phase == QCS_PHASE_DONE) break;
      if (g_args.compute_fn) qcs_replay_compute_grid(&g_args);
      snrt_cluster_hw_barrier();  // rejoin the DM core
    }
    return QCS_REPLAY_OK;
  }

  //===-- DM core: parse + drive ---------------------------------------===//
  int final_rc = QCS_REPLAY_OK;

  if (!region || !region->base || !job || !table) {
    final_rc = QCS_REPLAY_ERR_ARGS;
  } else if (region->size > UINTPTR_MAX) {
    // PAs are cast to uintptr_t in qcs_fw_pa_to_ptr; on rv32 an aperture > 4 GiB
    // would let an in-range uint64 PA truncate. L2 SPM is far below this.
    final_rc = QCS_REPLAY_ERR_ARGS;
  } else if (job->magic != QCS_MAGIC || job->version != QCS_VERSION) {
    final_rc = QCS_REPLAY_ERR_ARGS;
  } else if (job->cmd_stream_len > UINT32_MAX ||
             !qcs_range_in_region(region, job->cmd_stream_ptr,
                                  job->cmd_stream_len)) {
    final_rc = QCS_REPLAY_ERR_STREAM;
  } else {
    qcs_reader_t reader;
    qcs_reader_init(&reader, qcs_fw_pa_to_ptr(region, job->cmd_stream_ptr),
                    (uint32_t)job->cmd_stream_len);

    const qcs_record_header_t* header;
    while ((header = qcs_reader_next(&reader)) != NULL) {
      int rc = QCS_REPLAY_OK;
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
        case QCS_CMD_DISPATCH: {
          rc = replay_dispatch_prepare(region, (const qcs_dispatch_t*)header,
                                       table, &g_args);
          if (rc == QCS_REPLAY_OK && g_args.compute_fn) {
            g_args.rc = QCS_REPLAY_OK;
            g_phase = QCS_PHASE_DISPATCH;
            snrt_cluster_hw_barrier();  // release the workers onto the grid
            // Both halves run concurrently (mirrors harness.c lines 111-114 /
            // dispatch.c): the compute cores run compute_fn over the grid while
            // the DM core runs dma_fn ONCE for this dispatch. Same dispatch_state
            // for both; dma_fn == NULL falls back to the compute-only wait. The
            // barrier counts are unchanged (1 release + 1 rejoin per dispatch),
            // so this stays deadlock-free: the DM core doing work between its two
            // barriers only delays its own arrival at the rejoin barrier, exactly
            // like a slow compute core would.
            qcs_replay_dma_half(&g_args);
            snrt_cluster_hw_barrier();  // rejoin
            if (g_args.rc != QCS_REPLAY_OK) rc = g_args.rc;
          }
          break;
        }
        default:
          rc = QCS_REPLAY_ERR_MALFORMED;
          break;
      }
      if (rc != QCS_REPLAY_OK) {
        final_rc = rc;
        break;
      }
    }
    if (final_rc == QCS_REPLAY_OK && reader.offset < reader.size) {
      final_rc = QCS_REPLAY_ERR_MALFORMED;
    }
  }

  // Tell the workers to exit their loop.
  g_phase = QCS_PHASE_DONE;
  snrt_cluster_hw_barrier();
  return final_rc;
}
