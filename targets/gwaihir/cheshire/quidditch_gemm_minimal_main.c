// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// gwaihir host-device split -- MINIMAL host (the "size fix").
//
// Functionally identical submission to quidditch_gemm_main.c, but with the IREE
// host VM excised: no iree_vm_instance / iree_hal_module / EmitC vm-c module /
// iree_vm_context / iree_vm_invoke, and no 384 KiB VM bookkeeping arena. Instead
// this fills a qcs_writer with the SAME fixed gemm64 dispatch the VM/HAL emitted
// (executable_id=0, export_ordinal=0, workgroup_count={16,1,1}, 0 constants,
// 3 bindings A/B/C of 0x8000 bytes each) and rings the cluster doorbell.
//
// Because the dispatch is fixed-shape, the binding device-PAs and the workgroup
// grid are derived STATICALLY at build time here, rather than reproducing IREE
// HAL's dynamic allocate-buffer + resolve-ref-to-PA logic. Every value below is
// byte-checked against the Phase-0 golden dump emitted by the VM host (see
// QCS_DUMP_GOLDEN in cluster_device.c) -- both hosts go through the same
// qcs_write_dispatch, so the QCS stream must match to the byte.
//
// The A/B/C model buffers live in the L2-SPM arena (device-PAs 0x12000+), their
// natural cluster-DMA'd home -- NOT in Cheshire SPM. Only ~10-15 KiB of host
// .text + the golden reference in .bss stay resident, so the whole host fits the
// 128 KiB Cheshire SPM under hybrid.ld (was 137.6 KiB from DRAM with the VM).

#include <stdint.h>
#include <string.h>
#include <sys/types.h>  // ssize_t, required by hostio.h's _write prototype

#include "cluster_command_stream.h"  // qcs_writer + qcs_write_dispatch + descriptor
#include "hostio.h"                   // header-only UART: host_puts/host_putu/host_puthex64
#include "shared_region.h"            // qcs_shared_region_create + qcs_pa_to_ptr + doorbell

#define N 64
#define BYTES ((uint64_t)N * N * sizeof(double))  // 0x8000 = 32768

// Fixed device-PA layout in the L2-SPM aperture (region base 0x70000000, device
// PA == byte offset). Constraints, all satisfied below:
//   * >= arena floor 0x11000 (above firmware image + descriptor page at 0x10000)
//   * < return_code_array at GW_L2_SPM_TOTAL_SIZE-0x1000 (= 0x1ff000)
//   * disjoint from hybrid.ld's host-data window (l2data ORIGIN 0x70040000):
//     everything here ends at 0x2a000, well below 0x40000.
// Bindings A/B/C are 0x8000 apart (their exact length), the stream gets a page.
#define PA_STREAM 0x00011000ull  // QCS command stream (record is ~112 B; a page is ample)
#define PA_A 0x00012000ull       // 0x12000 .. 0x1a000
#define PA_B 0x0001a000ull       // 0x1a000 .. 0x22000
#define PA_C 0x00022000ull       // 0x22000 .. 0x2a000

extern void cheshire_console_init(void);

// Same deterministic inputs as quidditch_gemm_main.c: distinct/asymmetric so a
// transpose/operand-swap miscompile can't hide, exact in f64.
static void fill_inputs(double* a, double* b) {
  for (int i = 0; i < N * N; ++i) {
    a[i] = (double)(1 * (i + 1));
    b[i] = (double)(2 * (i + 1));
  }
}

// Golden C = A @ B^T (matmul_transpose_b): C[i,j] = sum_k A[i,k] * B[j,k].
static void golden_gemm(const double* a, const double* b, double* c) {
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      double acc = 0.0;
      for (int k = 0; k < N; ++k) acc += a[i * N + k] * b[j * N + k];
      c[i * N + j] = acc;
    }
}

int main(void) {
  cheshire_console_init();
  host_puts("\n[host-min] === minimal QCS gemm host (no IREE VM) ===\n");

  qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, NULL, 0) != 0) {
    host_puts("[host-min] FAIL: shared_region_create\n");
    return 3;
  }

  // Device VAs: host and cluster share the L2-SPM aperture 1:1 (VA == base + PA).
  double* A = (double*)qcs_pa_to_ptr(&region, PA_A);
  double* B = (double*)qcs_pa_to_ptr(&region, PA_B);
  double* C = (double*)qcs_pa_to_ptr(&region, PA_C);

  // Inputs written directly into L2-SPM; zero the output tile (defensive -- the
  // kernel is write-only into C via the linalg.fill, but this also proves the
  // host store path reaches the aperture the cluster reads).
  fill_inputs(A, B);
  memset(C, 0, (size_t)BYTES);
  __asm__ volatile("fence" ::: "memory");

  // Build the fixed gemm64 dispatch -- byte-identical to the VM/HAL golden.
  void* stream = qcs_pa_to_ptr(&region, PA_STREAM);
  qcs_writer_t w;
  qcs_writer_init(&w, stream, 4096);
  const uint32_t wg_count[3] = {16, 1, 1};  // 64x64 = 4x4 blocks of 16x16 -> 16 workgroups
  const uint32_t wg_size[3] = {1, 1, 1};    // cluster executable advertises {1,1,1}
  const qcs_binding_t bindings[3] = {
      {PA_A, BYTES},
      {PA_B, BYTES},
      {PA_C, BYTES},
  };
  int rc = qcs_write_dispatch(&w, /*executable_id=*/0u, /*export_ordinal=*/0u,
                              wg_count, wg_size, /*dynamic_local_memory=*/0u,
                              /*constant_count=*/0u, /*constants=*/NULL,
                              /*binding_count=*/3u, bindings);
  if (rc != 0 || w.overflowed) {
    host_puts("[host-min] FAIL: QCS stream write\n");
    return 2;
  }
  host_puts("[host-min] QCS dispatch built: size=0x");
  host_puthex64((uint64_t)w.size);
  host_puts(" records=");
  host_putu((unsigned long)w.record_count);
  host_puts("\n");

  // Publish the job descriptor + ring the doorbell (mirrors
  // iree_hal_cluster_device_submit_table exactly).
  qcs_job_descriptor_t* job = qcs_shared_job(&region);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->feature_flags = 0;
  job->status = 0;
  job->record_count = w.record_count;
  job->reserved = 0;
  job->executable_table_id = 0;
  job->cmd_stream_ptr = PA_STREAM;
  job->cmd_stream_len = w.size;
  __asm__ volatile("fence" ::: "memory");

  host_puts("[host-min] ringing doorbell (cl_clint_set) ...\n");
  qcs_doorbell_ring(job, 1u);
  int32_t status = qcs_doorbell_wait_completion(job, 1u);
  if (status != 0) {
    host_puts("[host-min] FAIL: cluster status=");
    host_putu((unsigned long)(long)status);
    host_puts("\n");
    return 11;
  }
  host_puts("[host-min] cluster completed OK\n");

  // Verify C == A @ B^T against a host-side golden.
  static double C_gold[N * N];
  golden_gemm(A, B, C_gold);
  __asm__ volatile("fence" ::: "memory");

  int mism = 0;
  for (int i = 0; i < N * N; ++i) {
    double diff = C[i] - C_gold[i];
    if (diff < 0) diff = -diff;
    double tol = 1e-6 * (C_gold[i] < 0 ? -C_gold[i] : C_gold[i]) + 1e-9;
    if (diff > tol) ++mism;
  }
  host_puts("[host-min] C[0]=");
  host_putu((unsigned long)(long)C[0]);
  host_puts(" gold=");
  host_putu((unsigned long)(long)C_gold[0]);
  host_puts(" C[4095]=");
  host_putu((unsigned long)(long)C[N * N - 1]);
  host_puts(" gold=");
  host_putu((unsigned long)(long)C_gold[N * N - 1]);
  host_puts("\n");

  if (mism == 0) {
    host_puts("[host-min] *** PASS: C == A @ B^T ***\n");
    return 0;
  }
  host_puts("[host-min] *** FAIL mismatches=");
  host_putu((unsigned long)mism);
  host_puts(" ***\n");
  return 20;
}
