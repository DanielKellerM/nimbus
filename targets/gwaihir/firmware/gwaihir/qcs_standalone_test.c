// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Standalone gwaihir-cluster harness for the QCS replayer (Track-B step 2).
//
// Runs the QCS DISPATCH path on the STANDALONE cluster Verilator sim -- no CVA6
// host, no L2-SPM, non-serialized. It hand-builds the SAME 1-DISPATCH gemm job
// the host would (cheshire/qcs_gemm_seed.c), but into a LOCAL buffer, then calls
// qcs_replay_stream directly and brackets it with mcycle. This measures the
// cluster-side offload cost (replay + broadcast fan-out + kernel) and validates
// the barrier pairing (RISK: a per-core internal-barrier divergence wedges the
// cluster at the dispatch broadcast) BEFORE committing a serialized co-sim run.
//
// Job (matched to the IREE host / kernel; see qcs_gemm_seed.c for the 3-way
// cross-check): gemm_square @gemm64 = linalg.matmul_transpose_b, C[i,j] =
// sum_k A[i,k]*B[j,k], 16x16 f64, executable_id 0 / export_ordinal 0, {1,1,1}
// grid, 0 constants, 3 bindings A/B/C. A[i]=i+1, B[i]=2*(i+1) -> golden
// C[0]=2992, C[255]=1976752 (exact in f64).

#include <stdint.h>
#include <stdio.h>

#include <encoding.h>  // read_csr(mcycle)

#include "snrt.h"

#include "qcs_replay.h"  // qcs_fw_region_t, qcs_replay_stream + cluster_command_stream.h structs

// The iree+xdsl gemm64 kernel static-library query (as main.c registers it): the
// dispatch entries are LOCAL symbols reachable only via this global query.
#ifdef __cplusplus
extern "C" {
#endif
extern const iree_hal_executable_library_header_t**
quidditch_gemm64_dispatch_0_library_query(
    iree_hal_executable_library_version_t max_version,
    const iree_hal_executable_environment_v0_t* environment);
#ifdef __cplusplus
}  // extern "C"
#endif

#define N 16
#define MAT_BYTES ((uint64_t)(N * N) * sizeof(double))  // 2048

// Compact local QCS region. Unlike the L2-SPM aperture there is no firmware image
// at the base, so we pack tight: descriptor / stream near 0, A/B/C page-aligned.
#define OFF_DESC 0x0100u
#define OFF_STREAM 0x0200u
#define OFF_A 0x1000u
#define OFF_B 0x1800u
#define OFF_C 0x2000u
#define REGION_SIZE 0x4000u  // 16 KiB (C ends at 0x2800)
#define JOB_ID 0xC0FFEEu     // doorbell != 0

static uint8_t g_region[REGION_SIZE] __attribute__((aligned(64)));
static qcs_replay_table_t g_table;

// Register both halves of every export at its ordinal under executable_id 0
// (identical to main.c's gw_register_kernels).
static void register_kernels(qcs_replay_table_t* table) {
  qcs_replay_table_init(table);
  const iree_hal_executable_library_header_t** hdr =
      quidditch_gemm64_dispatch_0_library_query(/*max_version=*/6u,
                                                /*environment=*/NULL);
  if (!hdr) return;
  const quidditch_executable_library_v0_t* lib =
      (const quidditch_executable_library_v0_t*)hdr;
  for (uint32_t ord = 0; ord < lib->exports.count; ++ord) {
    iree_hal_executable_dispatch_v0_t compute_fn = lib->exports.compute_core_ptrs[ord];
    iree_hal_executable_dispatch_v0_t dma_fn =
        lib->exports.dma_core_ptrs ? lib->exports.dma_core_ptrs[ord] : NULL;
    if (compute_fn) qcs_replay_register(table, /*executable_id=*/0, ord, compute_fn, dma_fn);
  }
}

// Fill A/B/C and hand-build the 1-DISPATCH descriptor + stream (see qcs_gemm_seed.c).
static void seed_job(void) {
  double* A = (double*)(void*)(g_region + OFF_A);
  double* B = (double*)(void*)(g_region + OFF_B);
  double* C = (double*)(void*)(g_region + OFF_C);
  for (int i = 0; i < N * N; ++i) {
    A[i] = (double)(i + 1);
    B[i] = (double)(2 * (i + 1));
    C[i] = 0.0;
  }

  qcs_dispatch_t* d = (qcs_dispatch_t*)(void*)(g_region + OFF_STREAM);
  uint32_t bindings_off = (uint32_t)sizeof(qcs_dispatch_t);  // constant_count == 0
  uint32_t stream_len = bindings_off + 3u * (uint32_t)sizeof(qcs_binding_t);
  stream_len = (stream_len + (QCS_RECORD_ALIGN - 1)) & ~(QCS_RECORD_ALIGN - 1);
  for (uint32_t b = 0; b < stream_len; ++b) ((uint8_t*)d)[b] = 0;
  d->header.type = QCS_CMD_DISPATCH;
  d->header.size = stream_len;
  d->executable_id = 0u;
  d->export_ordinal = 0u;
  d->flags = 0u;
  d->workgroup_count[0] = d->workgroup_count[1] = d->workgroup_count[2] = 1u;
  d->workgroup_size[0] = d->workgroup_size[1] = d->workgroup_size[2] = 1u;
  d->dynamic_local_memory = 0u;
  d->workgroup_count_ptr = 0u;
  d->constant_count = 0u;
  d->binding_count = 3u;
  qcs_binding_t* binds = (qcs_binding_t*)((uint8_t*)d + bindings_off);
  binds[0].device_ptr = OFF_A; binds[0].length = MAT_BYTES;
  binds[1].device_ptr = OFF_B; binds[1].length = MAT_BYTES;
  binds[2].device_ptr = OFF_C; binds[2].length = MAT_BYTES;

  qcs_job_descriptor_t* job = (qcs_job_descriptor_t*)(void*)(g_region + OFF_DESC);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->feature_flags = 0u;
  job->completion = 0u;
  job->status = 0;
  job->record_count = 1u;
  job->reserved = 0u;
  job->executable_table_id = 0u;
  job->cmd_stream_ptr = OFF_STREAM;
  job->cmd_stream_len = stream_len;
  __atomic_thread_fence(__ATOMIC_RELEASE);
  job->doorbell = JOB_ID;  // last
}

int main(void) {
  qcs_fw_region_t region;
  region.base = g_region;
  region.size = REGION_SIZE;

  if (snrt_is_dm_core()) {
    // line-buffer stdout: 1 printf = 1 tohost write, else the fesvr htif-poll
    // re-drains fragments (the Track-A harness.c.in fix).
    static char _obuf[256];
    setvbuf(stdout, _obuf, _IOLBF, sizeof _obuf);
    register_kernels(&g_table);
    seed_job();
  }
  // Publish the table + job to the compute cores before they enter the replay.
  snrt_cluster_hw_barrier();

  const qcs_job_descriptor_t* job =
      (const qcs_job_descriptor_t*)(const void*)(g_region + OFF_DESC);

  // ROI: the DM core drives the replay + broadcast fan-out; compute cores run the
  // worker loop. All cores MUST call it (the fan-out barriers pair across them).
  uint32_t t0 = read_csr(mcycle);
  int rc = qcs_replay_stream(&region, job, &g_table);
  uint32_t t1 = read_csr(mcycle);

  if (snrt_is_dm_core()) {
    const double* C = (const double*)(const void*)(g_region + OFF_C);
    int64_t c0 = (int64_t)C[0];
    int64_t c255 = (int64_t)C[N * N - 1];
    int pass = (rc == QCS_REPLAY_OK) && (c0 == 2992) && (c255 == 1976752);
    printf("[QCS] replay_cycles=%u rc=%d C[0]=%ld C[255]=%ld -> %s\n",
           (unsigned)(t1 - t0), rc, (long)c0, (long)c255, pass ? "PASS" : "FAIL");
  }
  return 0;  // report via the printf; exit clean (autotuner-style parse)
}
