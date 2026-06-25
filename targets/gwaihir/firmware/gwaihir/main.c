// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// rv32 Snitch-cluster firmware entry for the QCS replayer (host-device split,
// Phase 2, increment 1).
//
// Boot/offload contract (see sw/cheshire/tests/simple_offload.c, the host side):
//   * The CVA6 host writes a qcs_job_descriptor_t + command stream into L2 SPM
//     (base GW_L2_SPM_BASE_ADDR(0)) and wakes the cluster via cl_clint_set.
//   * The job descriptor lives at a fixed offset in the L2-SPM aperture; the
//     command stream + device buffers follow it. QCS device-PAs are offsets
//     into this aperture (region.base == L2-SPM base).
//   * Every cluster core enters main(); the DM core drives the replay and the
//     compute cores run dispatched workgroup grids (qcs_replay_stream).
//   * On completion the DM core publishes status into the descriptor and the
//     host polls it (Phase-0 no-IRQ handshake).
//
// MILESTONE: this is the compile+link target. The real iree+xdsl gemm_square
// kernel object (lib/gemm_square_kernel.o, an xDSL-lowered rv32 Snitch kernel)
// is now linked in and registered for (executable_id=0, export_ordinal=0). The
// STUB kernel is retained behind QCS_USE_STUB_KERNEL as a fallback.

#include <stdint.h>

#include "snrt.h"

#include "qcs_replay.h"

// gwaihir generated address map: GW_L2_SPM_BASE_ADDR / GW_L2_SPM_TOTAL_SIZE.
#include "gw_raw_addrmap.h"

// The host places the job descriptor at QCS_JOB_DESCRIPTOR_OFFSET into the L2-SPM
// aperture — above the firmware .text at the region base (offset 0 would collide).
#define QCS_JOB_DESCRIPTOR_PA QCS_JOB_DESCRIPTOR_OFFSET

//===----------------------------------------------------------------------===//
// Real iree+xdsl gemm_square kernel
//===----------------------------------------------------------------------===//
// The kernel object (lib/gemm_square_kernel.o) exports exactly one *global*
// symbol of interest: the IREE static-library query
// `quidditch_gemm64_dispatch_0_library_query`. The actual dispatch entry points
// (e.g. `gemm64_dispatch_0_matmul_16x16x16_f64$iree_to_xdsl`, the IREE->xDSL
// adapter that fans out to the `$xdsl_kernel*` compute partitions / `$dma` DMA
// partition) are LOCAL symbols, reachable only through this query's export
// table -- so we MUST register via the library query rather than an extern
// declaration. We register every export's compute-core fn at the matching
// ordinal (robust: works for any export count).
//
// Header layout: see qcs_kernel_abi.h (vendored fork of executable_library v0).
// We declare the query directly (the generated gemm_square_kernel.h pulls in the
// upstream iree/hal/local/executable_library.h, which this dependency-free rv32
// firmware build deliberately does not have on its include path).
#ifndef QCS_USE_STUB_KERNEL
#ifdef __cplusplus
extern "C" {
#endif
// C linkage: the kernel object exports an unmangled C symbol; this firmware TU
// is compiled with -x c++, so the declaration must be extern "C".
extern const iree_hal_executable_library_header_t**
quidditch_gemm64_dispatch_0_library_query(
    iree_hal_executable_library_version_t max_version,
    const iree_hal_executable_environment_v0_t* environment);
#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // !QCS_USE_STUB_KERNEL

//===----------------------------------------------------------------------===//
// Stub kernel (matches iree_hal_executable_dispatch_v0_t exactly)
//===----------------------------------------------------------------------===//
// Touches the dispatch/workgroup state the same way a real kernel would so the
// optimizer cannot strip the call and the ABI plumbing is exercised. The real
// iree+xdsl export (e.g. `gemm64_dispatch_0`) replaces this; the signature and
// state-field access are identical.
__attribute__((unused)) static int qcs_stub_kernel(
    const iree_hal_executable_environment_v0_t* environment,
    const iree_hal_executable_dispatch_state_v0_t* state,
    const iree_hal_executable_workgroup_state_v0_t* workgroup) {
  (void)environment;
  // A trivial, side-effect-free identity: copy binding[0] onto itself for the
  // bytes this workgroup "owns". Real kernels read constants/bindings the same
  // way. Guarded so the stub never traps on a 0-binding dispatch.
  if (state->binding_count >= 1 && state->binding_ptrs &&
      state->binding_ptrs[0] && state->binding_lengths &&
      state->binding_lengths[0] >= sizeof(uint32_t)) {
    volatile uint32_t* p = (volatile uint32_t*)state->binding_ptrs[0];
    uint32_t v = p[0];
    p[0] = v + workgroup->workgroup_id_x * 0u;  // no-op, keeps the load live
  }
  return 0;
}

//===----------------------------------------------------------------------===//
// Kernel registration
//===----------------------------------------------------------------------===//
// Maps (executable_id, export_ordinal) -> kernel fn. The host's QCS DISPATCH
// records carry these indices; they must agree with the executable table the
// host compiled against (qcs_job_descriptor_t.executable_table_id).
//
// NEXT STEP — real kernel: the iree+xdsl split emits a kernel object whose
// export symbol is e.g. `gemm64_dispatch_0` (the xDSL module name +
// `_dispatch_<ordinal>`), declared as an iree_hal_executable_dispatch_v0_t.
// Declare it `extern` here and register it for the matching ids:
//     extern int gemm64_dispatch_0(
//         const iree_hal_executable_environment_v0_t*,
//         const iree_hal_executable_dispatch_state_v0_t*,
//         const iree_hal_executable_workgroup_state_v0_t*);
//     qcs_replay_register(table, /*executable_id=*/0, /*export_ordinal=*/0,
//                         gemm64_dispatch_0);
// and add the kernel .o/.a to SRCS / a per-app LIB in app.mk. (IREE's
// static-library query header `iree_hal_executable_library_query` is the other
// integration option: register every export from the library_v0 table.)
static void gw_register_kernels(qcs_replay_table_t* table) {
  qcs_replay_table_init(table);
#ifdef QCS_USE_STUB_KERNEL
  qcs_replay_register(table, /*executable_id=*/0, /*export_ordinal=*/0,
                      /*compute_fn=*/qcs_stub_kernel, /*dma_fn=*/NULL);
#else
  // Query the iree+xdsl static library and register BOTH halves of every export
  // at its ordinal under executable_id 0 (the host's QCS DISPATCH records carry
  // executable_id 0 / export_ordinal for gemm_square). harness.c registers both
  // exports.compute_core_ptrs[ord] (the compute half, run on the compute cores)
  // and exports.dma_core_ptrs[ord] (the DMA half, run once on the DM core). The
  // dma_core_ptrs array itself may be absent (NULL) on a library with no DMA
  // half, and individual ordinals may be NULL -- in either case we register a
  // NULL dma_fn and the DM core falls back to just waiting for that dispatch.
  // max_version is the highest library ABI the caller (this firmware) supports;
  // the v0.6 library_query returns NULL when it is < 6 (VERSION_0_6).
  const iree_hal_executable_library_header_t** lib_hdr =
      quidditch_gemm64_dispatch_0_library_query(
          /*max_version=*/6u, /*environment=*/NULL);
  if (lib_hdr) {
    const quidditch_executable_library_v0_t* lib =
        (const quidditch_executable_library_v0_t*)lib_hdr;
    for (uint32_t ord = 0; ord < lib->exports.count; ++ord) {
      iree_hal_executable_dispatch_v0_t compute_fn =
          lib->exports.compute_core_ptrs[ord];
      iree_hal_executable_dispatch_v0_t dma_fn =
          lib->exports.dma_core_ptrs ? lib->exports.dma_core_ptrs[ord] : NULL;
      if (compute_fn) {
        qcs_replay_register(table, /*executable_id=*/0, ord, compute_fn, dma_fn);
      }
    }
  }
#endif  // QCS_USE_STUB_KERNEL
}

//===----------------------------------------------------------------------===//
// Entry
//===----------------------------------------------------------------------===//

// One table, shared across the cluster (registration is idempotent; the DM core
// is the only one that reads it during dispatch).
static qcs_replay_table_t g_table;

int main() {
  // BOOT/WAKE MODEL (mirrors sw/cheshire/tests/simple_offload.c + simple.c):
  // The host entries ALL SNRT_CLUSTER_NUM clusters' boot scratch regs and wakes
  // cluster 0 ONLY; cluster 0's snRuntime (snrt_wake_up in crt0) wakes the rest.
  // EVERY cluster's cores therefore enter snrt_main(), run snrt_init_libs() ->
  // the world=SNRT_CLUSTER_NUM global software barrier (so all 16 clusters MUST
  // arrive -- this is why the host must wake them all, and why the world=1 hack
  // was removed), then crt0 calls main() here. ONLY cluster 0 carries the QCS
  // job (its descriptor + command stream in L2 SPM); the other 15 clusters have
  // no work and return immediately. On return, crt0's POST_BARRIER + snrt_exit
  // (snitch_cluster_start.h: writes (rc<<1)|1 to scratch[0][core_idx]) reports
  // per-core completion into the host's return_code_array -- the authoritative
  // "all cores of all clusters done" gate the host now polls (simple_offload's
  // protocol). Cluster 0 ADDITIONALLY writes the gemm result + descriptor
  // completion below for the QCS no-IRQ handshake.
  if (snrt_cluster_idx() != 0) {
    // No QCS job for non-cluster-0 clusters: just return so crt0 reports this
    // cluster's per-core completion (snrt_exit) and the host's all-clusters
    // wait can resolve. Do NOT touch the descriptor (only cluster 0 owns it).
    return 0;
  }

  qcs_fw_region_t region;
  region.base = (uint8_t*)(uintptr_t)GW_L2_SPM_BASE_ADDR(0);
  region.size = (uint64_t)GW_L2_SPM_TOTAL_SIZE;

  const qcs_job_descriptor_t* job =
      (const qcs_job_descriptor_t*)qcs_fw_pa_to_ptr(&region,
                                                    QCS_JOB_DESCRIPTOR_PA);

  // The DM core owns the kernel table; build it before the workers need it.
  if (snrt_is_dm_core()) {
    gw_register_kernels(&g_table);
  }
  // Sync so the table is published before any compute core enters the loop.
  snrt_cluster_hw_barrier();

  // NO-JOB GUARD (principled): a host that wakes cluster 0 WITHOUT publishing a
  // QCS descriptor (e.g. the proven simple_offload bring-up host, which only
  // seeds the boot scratch regs) leaves the descriptor page zeroed -> magic 0,
  // version 0, doorbell 0. With no valid job there is no work, so cluster 0
  // behaves exactly like the other clusters and like simple.elf: return 0
  // cleanly so crt0's snrt_exit reports per-core completion (rc 0) and the
  // host's all-cores-done / sum-0 gate resolves. This runs AFTER snrt_init_libs'
  // global (world) barrier (which already completed before main) and after the
  // cluster-local table barrier, so neither barrier is perturbed. Every cluster-0
  // core reads the SAME shared descriptor word and takes the SAME branch, so no
  // core enters qcs_replay_stream and the cluster-hw-barrier pairing inside it
  // can never diverge. A real job (doorbell != 0, valid magic/version) skips
  // this and proceeds to replay as before.
  if (job->magic != QCS_MAGIC || job->version != QCS_VERSION ||
      job->doorbell == 0) {
    return 0;
  }

  int rc = qcs_replay_stream(&region, job, &g_table);

  // DM core publishes the result back into the descriptor (Phase-0 handshake:
  // status first, then completion, with a fence between).
  if (snrt_is_dm_core()) {
    qcs_job_descriptor_t* wjob = (qcs_job_descriptor_t*)job;
    wjob->status = (int32_t)rc;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    wjob->completion = job->doorbell;  // echo the submitted job id
    wjob->doorbell = 0;                // ack the doorbell
  }

  return rc == QCS_REPLAY_OK ? 0 : 1;
}
