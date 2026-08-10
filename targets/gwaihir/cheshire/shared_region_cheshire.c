// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Cheshire/CVA6 HW-wired implementation of the QCS shared region (gwaihir
// host-device split, Phase 2). Replaces the spike/static-arena
// shared_region.c: the "shared aperture" IS the gwaihir L2 SPM, reachable by
// both the CVA6 host and the Snitch cluster's DM core.
//
//   region.base  == GW_L2_SPM_BASE_ADDR(0) == 0x70000000  (device-PA 0)
//   device-PA    == offset into the L2-SPM aperture
//
// IMPORTANT (firmware/host layout contract):
//   The qcs_replay firmware is preloaded at the cluster entry 0x70000000 and
//   occupies the low ~64 KiB of the L2-SPM aperture (its .text/.rodata/.data/
//   .bss). The job descriptor + command stream + A/B/C device buffers must NOT
//   overlap that image, so we place the descriptor at QCS_JOB_DESCRIPTOR_OFFSET and bump-
//   allocate everything above it. The firmware reads the descriptor at the same
//   offset (its main.c QCS_JOB_DESCRIPTOR_PA must equal QCS_JOB_DESCRIPTOR_OFFSET).
//
// Doorbell:  qcs_doorbell_ring writes the cluster boot scratch regs + rings
//            cl_clint_set (msip) to wake the Snitch DM core, following the boot
//            model of sw/cheshire/tests/simple_offload.c (but writes the wake regs
//            32-bit, see the note in qcs_doorbell_ring). There is NO auto-complete.
// Wait:      qcs_doorbell_wait_completion polls the real completion word the DM
//            core writes back into the L2-SPM job descriptor.

#include "shared_region.h"

#include <string.h>

// gwaihir generated address map (L2 SPM base/size, cluster scratch/clint regs).
#include "gw_raw_addrmap.h"

// DEBUG: host-readable progress markers written by the cluster DM core (see
// qcs_replay.c QCS_DEBUG_BLOCK_OFFSET). Header-only UART print (needs ssize_t).
#include <sys/types.h>
#include "hostio.h"

// The job descriptor sits at QCS_JOB_DESCRIPTOR_OFFSET (canonical in
// cluster_command_stream.h, pulled in via shared_region.h): 64 KiB into the
// L2-SPM aperture, above the qcs_replay firmware image (~54 KiB, .text..bss end
// 0x7000d6f8), 4 KiB-aligned. MUST match the firmware's QCS_JOB_DESCRIPTOR_PA.

// The L2-SPM aperture base as a raw pointer. device-PA 0 == this address.
#define L2_SPM_BASE ((uintptr_t)GW_L2_SPM_BASE_ADDR(0))
#define L2_SPM_BYTES ((uint64_t)GW_L2_SPM_TOTAL_SIZE)

// Per-cluster boot/doorbell registers (see gw_raw_addrmap.h + simple_offload.c).
// The boot model entries ALL clusters and wakes cluster 0 only, exactly as
// simple_offload.c uses gwaihir_addrmap.cluster[i].peripheral_reg.scratch /
// cl_clint_set -- so we address them per-cluster via the generated macros.
#define CL_SCRATCH(cl, idx) \
  ((volatile uint64_t*)(uintptr_t)GW_CLUSTER_PERIPHERAL_REG_SCRATCH_BASE_ADDR((cl), (idx)))
#define CL_CLINT_SET(cl) \
  ((volatile uint64_t*)(uintptr_t)GW_CLUSTER_PERIPHERAL_REG_CL_CLINT_SET_BASE_ADDR((cl)))

// Cluster topology (from the cluster cfg snitch_cluster_cfg.h: CFG_CLUSTER_NR_CORES
// == 9, SNRT_CLUSTER_NUM == GW_CLUSTER_NUM == 16). The host mirrors these so its
// entry loop + completion wait span exactly the cores the snRuntime reports into.
#ifndef QCS_CLUSTER_NR_CORES
#define QCS_CLUSTER_NR_CORES 9  // 8 compute + 1 DM core
#endif
#ifndef QCS_CLUSTER_NUM
#define QCS_CLUSTER_NUM GW_CLUSTER_NUM  // 16
#endif

// return_code_array: the proven simple_offload.c completion protocol. The
// snRuntime's snrt_exit (snitch_cluster_start.h) writes (rc<<1)|1 into
// scratch[0][core_idx] for EVERY core of EVERY cluster; the host points each
// cluster's scratch[0] at return_code_array[i] and polls bit 0 of all (i,j).
// Lives in the LAST 4 KiB of the (uncached) L2-SPM aperture, exactly as
// simple_offload.c places it: GW_L2_SPM_BASE_ADDR(0)+GW_L2_SPM_TOTAL_SIZE-0x1000.
#define RETURN_CODE_ADDR (L2_SPM_BASE + L2_SPM_BYTES - 0x1000u)
static volatile uint32_t (*const g_return_code_array)[QCS_CLUSTER_NR_CORES] =
    (volatile uint32_t (*)[QCS_CLUSTER_NR_CORES])(uintptr_t)RETURN_CODE_ADDR;

int qcs_shared_region_create(qcs_shared_region_t* region, const char* path,
                             uint64_t size) {
  (void)path;
  (void)size;
  region->base = (void*)L2_SPM_BASE;
  region->size = L2_SPM_BYTES;
  region->fd = -1;
  // The descriptor lives at QCS_JOB_DESCRIPTOR_OFFSET so qcs_shared_job() (and every host
  // accessor) targets base+0x10000, ABOVE the firmware image at the region base.
  region->desc_offset = QCS_JOB_DESCRIPTOR_OFFSET;
  // Zero ONLY the descriptor page (NOT the whole aperture: the low 64 KiB holds
  // the preloaded firmware image and must not be clobbered).
  volatile uint8_t* desc = (volatile uint8_t*)(L2_SPM_BASE + QCS_JOB_DESCRIPTOR_OFFSET);
  for (uint32_t i = 0; i < QCS_SHARED_ARENA_OFFSET; ++i) desc[i] = 0;
  return 0;
}

int qcs_shared_region_open(qcs_shared_region_t* region, const char* path) {
  (void)path;
  region->base = (void*)L2_SPM_BASE;
  region->size = L2_SPM_BYTES;
  region->fd = -1;
  region->desc_offset = QCS_JOB_DESCRIPTOR_OFFSET;
  return 0;
}

void qcs_shared_region_close(qcs_shared_region_t* region) {
  region->base = NULL;
  region->size = 0;
  region->fd = -1;
  region->desc_offset = 0;
}

// Bump allocator. The bump cursor is a device-PA. We start the arena right
// after the descriptor page (QCS_JOB_DESCRIPTOR_OFFSET + one page) so the descriptor,
// stream, and buffers never overlap the firmware image (which lives below
// QCS_JOB_DESCRIPTOR_OFFSET).
uint64_t qcs_shared_alloc(qcs_shared_region_t* region, uint64_t* bump,
                          uint64_t bytes, uint64_t align) {
  if (align == 0u || (align & (align - 1u)) != 0u) return 0;
  // Floor the very first allocation to just above the descriptor page.
  uint64_t arena_floor = QCS_JOB_DESCRIPTOR_OFFSET + QCS_SHARED_ARENA_OFFSET;
  if (*bump < arena_floor) *bump = arena_floor;
  uint64_t start = (*bump + (align - 1u)) & ~(align - 1u);
  // Reserve the LAST 4 KiB of the aperture for the simple_offload-style
  // return_code_array (RETURN_CODE_ADDR); the arena must never allocate into it.
  uint64_t arena_ceiling = region->size - 0x1000u;
  if (start > arena_ceiling || bytes > arena_ceiling - start) return 0;
  *bump = start + bytes;
  return start;
}

//===----------------------------------------------------------------------===//
// Doorbell / completion handshake (HW: cl_clint_set wake + L2-SPM poll).
//===----------------------------------------------------------------------===//

void qcs_doorbell_ring(qcs_job_descriptor_t* job, uint32_t job_id) {
  // Publish the submitted job id into the descriptor doorbell (release).
  __atomic_store_n(&job->doorbell, job_id, __ATOMIC_RELEASE);
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // Cluster boot contract -- the boot model of sw/cheshire/tests/simple_offload.c
  // (that file uses 64-bit stores; here the wake regs are written 32-bit, see below):
  // entry ALL SNRT_CLUSTER_NUM clusters' boot scratch regs, zero the per-core
  // return-code slots, THEN wake cluster 0 only. Cluster 0's snRuntime wakes the
  // rest (snrt_wake_up in crt0); all clusters reach the world barrier; on return
  // crt0's snrt_exit reports each core's completion into return_code_array.
  //   scratch[1] = firmware entry point (L2 SPM base; same image for all clusters)
  //   scratch[0] = &return_code_array[i] (where this cluster's snrt_exit writes)
  for (uint32_t i = 0; i < QCS_CLUSTER_NUM; ++i) {
    // Wake regs written as 32-bit halves. EMPIRICAL: the mesh wakes this way but not
    // with a single 64-bit store; root cause not pinned (simple_offload.c uses 64-bit
    // and works, so likely an ordering artifact -- an AXI trace on the wake store would
    // settle it). Do NOT revert to a 64-bit store without that trace: it hangs mode 3.
    volatile uint32_t* s1 = (volatile uint32_t*)CL_SCRATCH(i, 1);
    s1[0] = (uint32_t)(uintptr_t)L2_SPM_BASE; s1[1] = 0u;          // entry 0x70000000
    volatile uint32_t* s0 = (volatile uint32_t*)CL_SCRATCH(i, 0);
    s0[0] = (uint32_t)(uintptr_t)&g_return_code_array[i][0]; s0[1] = 0u;
    for (uint32_t j = 0; j < QCS_CLUSTER_NR_CORES; ++j) {
      g_return_code_array[i][j] = 0u;
    }
  }
  __atomic_thread_fence(__ATOMIC_RELEASE);

  // Ring the doorbell: set msip for all cores in CLUSTER 0 ONLY. Cluster 0's DM
  // core wakes (snrt_wake_up wakes the other 15 clusters), builds the kernel
  // table, and runs the QCS replay; the other clusters boot, pass the world
  // barrier, and return immediately (no QCS job -- see firmware main.c guard).
  *(volatile uint32_t*)CL_CLINT_SET(0) = (uint32_t)((1u << QCS_CLUSTER_NR_CORES) - 1u);
}

uint32_t qcs_doorbell_wait(qcs_job_descriptor_t* job) {
  return __atomic_load_n(&job->doorbell, __ATOMIC_ACQUIRE);
}

void qcs_doorbell_complete(qcs_job_descriptor_t* job, uint32_t job_id,
                           int32_t status) {
  // Host side never calls this on HW (the firmware completes); kept for ABI.
  __atomic_store_n(&job->doorbell, 0u, __ATOMIC_RELAXED);
  job->status = status;
  __atomic_store_n(&job->completion, job_id, __ATOMIC_RELEASE);
}

// DEBUG block written by the cluster DM core (qcs_replay.c). Same fixed PA:
// L2-SPM base + QCS_DEBUG_BLOCK_OFFSET. u64 words [magic,phase,A,B,C,dma,cx,bc].
// QCS_DEBUG_BLOCK_OFFSET + QCS_DBG_MAGIC are canonical (cluster_command_stream.h).

// Count how many (cluster,core) return-code slots have reported done (bit 0).
// This is simple_offload.c's "all_finished" gate; here it is a boot-health
// signal printed alongside the descriptor-completion poll. Returns the count;
// total expected == QCS_CLUSTER_NUM * QCS_CLUSTER_NR_CORES.
static uint32_t qcs_return_codes_done(void) {
  uint32_t done = 0;
  for (uint32_t i = 0; i < QCS_CLUSTER_NUM; ++i) {
    for (uint32_t j = 0; j < QCS_CLUSTER_NR_CORES; ++j) {
      if (g_return_code_array[i][j] & 1u) ++done;
    }
  }
  return done;
}

int32_t qcs_doorbell_wait_completion(qcs_job_descriptor_t* job,
                                     uint32_t job_id) {
  // COMPLETION GATE: gate on the SINGLE descriptor completion word. Cluster-0's
  // DM core writes job->completion=job_id with an explicit fence AFTER the
  // inter-cluster global barrier (main.c), so completion==job_id already implies
  // every cluster finished AND the store drained through the Snitch store buffer
  // + NoC to L2 -- the volatile view + per-spin fence force a fresh uncached
  // L2 read (no CSE/hoist), which is the real fix for the earlier "flaky" read.
  // Do NOT scan the return-code array on the gate hot path: it is
  // QCS_CLUSTER_NUM*QCS_CLUSTER_NR_CORES uncached NoC loads (144 at 16 clusters),
  // ~ms of SIM-TIME per iteration, so the host polled only ~twice per run and the
  // VCS wall-clock killed the sim before it observed the already-set completion.
  // Scan the slots only for the (rare) progress print.
  volatile uint64_t* dbg =
      (volatile uint64_t*)(uintptr_t)(L2_SPM_BASE + QCS_DEBUG_BLOCK_OFFSET);
  volatile qcs_job_descriptor_t* vjob = (volatile qcs_job_descriptor_t*)job;
  const uint32_t rc_total = QCS_CLUSTER_NUM * QCS_CLUSTER_NR_CORES;
  uint64_t spins = 0;
  uint64_t last_phase = (uint64_t)-1;
  for (;;) {
    __asm__ volatile("fence" ::: "memory");
    if (vjob->completion == job_id) {
      host_puts("[host] poll: COMPLETE cores_done=");
      host_putu((unsigned long)qcs_return_codes_done());  // once, for the report
      host_puts("/"); host_putu((unsigned long)rc_total); host_puts("\n");
      return vjob->status;
    }
    // Progress print on phase change or every ~4M spins (the 144-slot scan runs
    // only inside this branch, never on the gate hot path).
    uint64_t phase = (dbg[0] == QCS_DBG_MAGIC) ? dbg[1] : 0;
    if (phase != last_phase || (spins & 0x3fffffull) == 0) {
      last_phase = phase;
      host_puts("[host] poll: cluster PHASE=");
      host_putu((unsigned long)phase);
      host_puts(" cores_done="); host_putu((unsigned long)qcs_return_codes_done());
      host_puts("/"); host_putu((unsigned long)rc_total);
      if (dbg[0] == QCS_DBG_MAGIC) {
        host_puts(" A=0x"); host_puthex64(dbg[2]);
        host_puts(" B=0x"); host_puthex64(dbg[3]);
        host_puts(" C=0x"); host_puthex64(dbg[4]);
        host_puts(" dma_fn=0x"); host_puthex64(dbg[5]);
        host_puts(" cx="); host_putu((unsigned long)dbg[6]);
        host_puts(" nbind="); host_putu((unsigned long)dbg[7]);
      }
      host_puts("\n");
    }
    ++spins;
  }
}
