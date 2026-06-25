// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Gate-(c) bring-up host: a LIGHT, SPM-resident Cheshire/CVA6 host that
// hand-builds the EXACT QCS gemm job the slow DRAM IREE host
// (cheshire/quidditch_gemm_main.c) would record, seeds it into L2-SPM, and wakes
// cluster 0 so the qcs_replay firmware runs the real iree+xdsl gemm kernel.
//
// WHY (fast kernel-dispatch test): the full IREE-VM host is slow even on the 2x2
// mini config (CVA6 boot + IREE VM/HAL recording dominate). This host skips the
// VM entirely and emits the SAME single DISPATCH record the HAL command buffer
// (cluster_command_buffer.c) emits for gemm64_dispatch_0, so it exercises
// qcs_replay's DISPATCH path: the broadcast fan-out and the kernel's compute
// half ($iree_to_xdsl) + DMA half ($dma) running in lockstep across cluster 0's
// 8 compute + 1 DM cores. If the per-core internal-barrier counts diverge the
// cluster wedges at PHASE=4 (RISK 1); if balanced the gemm completes + writes C.
//
// DISPATCH PARAMETERS (matched to the IREE host / kernel, cross-checked 3 ways):
//   * gemm_square.mlir: @gemm64(tensor<16x16xf64>, tensor<16x16xf64>) ->
//     tensor<16x16xf64>, linalg.matmul_transpose_b: C[i,j]=sum_k A[i,k]*B[j,k].
//     3 tensor operands -> 3 bindings A,B,C; no push constants.
//   * runtime/samples/gemm_square/harness.c (the proven direct driver mirroring
//     executable.c): workgroup_count={1,1,1}, binding_count=3,
//     binding_ptrs={A,B,C}, constant_count=attrs.constant_count(=0),
//     constants=NULL, dynamic_local_memory=attrs.local_memory_pages*4096(=0).
//   * gemm_square_kernel.o attrs: the only non-zero count is the binding count
//     (=3); constant_count=0, local_memory_pages=0. Single export ordinal 0 with
//     both an $iree_to_xdsl (compute) and a $dma half -> the firmware takes the
//     broadcast (dma_fn != NULL) protocol.
// The IREE HAL command buffer (cluster_command_buffer.c) forwards exactly these
// to qcs_write_dispatch: executable_id=0, export_ordinal=0, the {1,1,1} grid, 0
// constants, and the 3 bindings resolved to device-PAs of A,B,C. We reproduce
// that record byte-for-byte.
//
// A/B fill + golden (pinned by the IREE host, quidditch_gemm_main.c):
//   a[i]=i+1, b[i]=2*(i+1) (row-major 16x16 f64); matmul_transpose_b golden
//   C[0]=2992, C[255]=1976752 (verified independently).
//
// Boot/wake: identical to simple_offload.c / qcs_offload_nojob.c -- seed every
// cluster's scratch[1]=L2_SPM entry + scratch[0]=&return_code_array[i], zero the
// return codes, wake cluster 0 (cl_clint_set). Clusters 1-3 hit the cluster-idx
// guard and return; cluster 0 reads the descriptor, sees the valid job, and runs
// qcs_replay_stream -> the gemm dispatch.

#include <stdint.h>
#include <string.h>

#include "gw_addrmap.h"
#include "gw_raw_addrmap.h"

#include "snitch_cluster_cfg.h"

// Cheshire UART (for the human-readable PASS/FAIL + C values in the cosim log).
#include "regs/cheshire.h"
#include "dif/clint.h"
#include "dif/uart.h"
#include "params.h"
#include "util.h"

//===----------------------------------------------------------------------===//
// QCS ABI (vendored inline so this host stays single-file / dependency-light).
// Field layout MUST match sw/snitch/apps/qcs_replay/src/cluster_command_stream.h.
//===----------------------------------------------------------------------===//

#define QCS_MAGIC 0x31534351u    // 'QCS1'
#define QCS_VERSION 2u
#define QCS_RECORD_ALIGN 8u
#define QCS_JOB_DESCRIPTOR_OFFSET 0x10000u
#define QCS_CMD_DISPATCH 1u

typedef struct qcs_record_header_t {
  uint32_t type;
  uint32_t size;
} qcs_record_header_t;

typedef struct qcs_dispatch_t {
  qcs_record_header_t header;
  uint32_t executable_id;
  uint32_t export_ordinal;
  uint32_t flags;
  uint32_t workgroup_count[3];
  uint32_t workgroup_size[3];
  uint32_t dynamic_local_memory;
  uint64_t workgroup_count_ptr;
  uint32_t constant_count;
  uint32_t binding_count;
} qcs_dispatch_t;

typedef struct qcs_binding_t {
  uint64_t device_ptr;  // device-physical = offset into the L2-SPM aperture
  uint64_t length;
} qcs_binding_t;

typedef struct qcs_job_descriptor_t {
  uint32_t magic;
  uint32_t version;
  uint32_t feature_flags;
  uint32_t doorbell;
  uint32_t completion;
  int32_t status;
  uint32_t record_count;
  uint32_t reserved;
  uint64_t executable_table_id;
  uint64_t cmd_stream_ptr;
  uint64_t cmd_stream_len;
} qcs_job_descriptor_t;

//===----------------------------------------------------------------------===//
// L2-SPM layout (offsets into the GW_L2_SPM_BASE_ADDR(0) aperture).
//===----------------------------------------------------------------------===//
// region.base == GW_L2_SPM_BASE_ADDR(0) == 0x70000000; QCS device-PAs are byte
// offsets into [0, GW_L2_SPM_TOTAL_SIZE). The firmware .text/.data live at the
// region base (offset 0, ~50 KiB per the SLINK preload). Keep all of our
// structures above 0x10000 (descriptor) and below the return_code_array page.
#define L2_BASE         GW_L2_SPM_BASE_ADDR(0)
#define OFF_DESC        QCS_JOB_DESCRIPTOR_OFFSET       // 0x10000 (fixed)
// 0x10100 holds the firmware DEBUG block (QGDB phase markers); skip past it.
#define OFF_STREAM      0x11000u                        // QCS command stream
#define OFF_A           0x20000u                        // A 16x16 f64 (2048 B)
#define OFF_B           0x21000u                        // B 16x16 f64
#define OFF_C           0x22000u                        // C 16x16 f64

#define N 16
#define MAT_BYTES ((uint64_t)(N * N) * sizeof(double))  // 2048

#define JOB_ID 0xC0FFEEu  // doorbell != 0

// Return-code array in the last 4K page of the L2-SPM region (simple_offload).
#define RETURN_CODE_ADDR \
  (GW_L2_SPM_BASE_ADDR(0) + GW_L2_SPM_TOTAL_SIZE - 0x1000)
volatile uint32_t (*return_code_array)[CFG_CLUSTER_NR_CORES] =
    (uint32_t (*)[CFG_CLUSTER_NR_CORES])RETURN_CODE_ADDR;

// Firmware DEBUG block (sw/snitch/apps/qcs_replay/src/qcs_replay.c): the last
// PHASE the cluster reached. Lets us diagnose a wedge from the host side.
#define OFF_DBG     0x10100u
#define DBG_MAGIC   0x51474442ull  // "QGDB"

static volatile uint8_t* l2_ptr(uint32_t off) {
  return (volatile uint8_t*)(uintptr_t)(L2_BASE + off);
}

//===----------------------------------------------------------------------===//
// Tiny UART helpers (no printf dependency; integer + hex only).
//===----------------------------------------------------------------------===//
static void u_str(const char* s) {
  uart_write_str(&__uart_base_addr__, (void*)s, strlen(s));
}
static void u_dec(uint64_t v) {
  char buf[24];
  int i = 24;
  buf[--i] = '\0';
  if (v == 0) buf[--i] = '0';
  while (v && i > 0) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
  u_str(&buf[i]);
}
static void u_hex64(uint64_t v) {
  char buf[19];
  int i = 18;
  buf[--i] = '\0';
  for (int n = 0; n < 16; ++n) {
    uint32_t nib = (uint32_t)(v & 0xf);
    buf[--i] = (char)(nib < 10 ? ('0' + nib) : ('a' + nib - 10));
    v >>= 4;
  }
  u_str("0x");
  u_str(&buf[i]);
}

int main(void) {
  // --- Bring up the UART (helloworld.c pattern). ---
  uint32_t rtc_freq = CHS_REGS->rtc_freq.f.ref_freq;  // util.h provides CHS_REGS
  uint64_t reset_freq = clint_get_core_freq(rtc_freq, 2500);
  uart_init(&__uart_base_addr__, reset_freq, __BOOT_BAUDRATE);
  u_str("\r\n[seed] === gate (c): hand-built QCS gemm -> cluster dispatch ===\r\n");

  // --- 1. Lay out + fill A, B, C in L2-SPM. ---
  // A[i]=i+1, B[i]=2*(i+1) (f64, row-major 16x16); zero C. The CVA6 stores f64
  // directly into the L2-SPM aperture the cluster iDMA reads.
  volatile double* A = (volatile double*)l2_ptr(OFF_A);
  volatile double* B = (volatile double*)l2_ptr(OFF_B);
  volatile double* C = (volatile double*)l2_ptr(OFF_C);
  for (int i = 0; i < N * N; ++i) {
    A[i] = (double)(i + 1);
    B[i] = (double)(2 * (i + 1));
    C[i] = 0.0;
  }

  // --- 2. Build the QCS command stream (one DISPATCH record). ---
  // Match cluster_command_buffer.c's emission for gemm64_dispatch_0 exactly:
  // executable_id=0, export_ordinal=0, flags=0 (direct), workgroup_count={1,1,1},
  // workgroup_size={1,1,1} (the firmware/kernel ignore it), 0 constants, 3
  // bindings (A,B,C) as device-PAs (= aperture offsets). constant_count=0 so the
  // bindings start right after the qcs_dispatch_t header (8-aligned already).
  volatile qcs_dispatch_t* d = (volatile qcs_dispatch_t*)l2_ptr(OFF_STREAM);
  // Bindings immediately follow the (already 8-aligned) qcs_dispatch_t.
  uint32_t bindings_off = (uint32_t)sizeof(qcs_dispatch_t);  // constant_count==0
  uint32_t stream_len =
      bindings_off + 3u * (uint32_t)sizeof(qcs_binding_t);
  // round up to QCS_RECORD_ALIGN
  stream_len = (stream_len + (QCS_RECORD_ALIGN - 1)) & ~(QCS_RECORD_ALIGN - 1);

  // Zero the record first (the firmware reader validates the padded size).
  for (uint32_t b = 0; b < stream_len; ++b) ((volatile uint8_t*)d)[b] = 0;

  d->header.type = QCS_CMD_DISPATCH;
  d->header.size = stream_len;
  d->executable_id = 0u;
  d->export_ordinal = 0u;
  d->flags = 0u;
  d->workgroup_count[0] = 1u;
  d->workgroup_count[1] = 1u;
  d->workgroup_count[2] = 1u;
  d->workgroup_size[0] = 1u;
  d->workgroup_size[1] = 1u;
  d->workgroup_size[2] = 1u;
  d->dynamic_local_memory = 0u;
  d->workgroup_count_ptr = 0u;
  d->constant_count = 0u;
  d->binding_count = 3u;

  volatile qcs_binding_t* binds =
      (volatile qcs_binding_t*)((volatile uint8_t*)d + bindings_off);
  binds[0].device_ptr = OFF_A; binds[0].length = MAT_BYTES;  // A
  binds[1].device_ptr = OFF_B; binds[1].length = MAT_BYTES;  // B
  binds[2].device_ptr = OFF_C; binds[2].length = MAT_BYTES;  // C

  // --- 3. Publish the job descriptor at the fixed offset. ---
  // Zero a full page first so any firmware slack reads back defined (avoids the
  // X-propagation RegWriteKnown wedge that bit the no-job bring-up).
  volatile uint32_t* descpage = (volatile uint32_t*)l2_ptr(OFF_DESC);
  for (uint32_t w = 0; w < 0x1000u / sizeof(uint32_t); ++w) descpage[w] = 0u;

  volatile qcs_job_descriptor_t* job =
      (volatile qcs_job_descriptor_t*)l2_ptr(OFF_DESC);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->feature_flags = 0u;
  job->completion = 0u;
  job->status = 0;
  job->record_count = 1u;
  job->reserved = 0u;
  job->executable_table_id = 0u;  // host compiled against executable table 0
  job->cmd_stream_ptr = OFF_STREAM;
  job->cmd_stream_len = stream_len;
  // doorbell LAST (after the rest of the descriptor + stream are written), so the
  // cluster never observes a half-built job.
  __asm__ volatile("fence" ::: "memory");
  job->doorbell = JOB_ID;
  __asm__ volatile("fence" ::: "memory");

  u_str("[seed] QCS job published: stream@");
  u_hex64(OFF_STREAM); u_str(" len="); u_dec(stream_len);
  u_str(" bindings A="); u_hex64(L2_BASE + OFF_A);
  u_str(" B="); u_hex64(L2_BASE + OFF_B);
  u_str(" C="); u_hex64(L2_BASE + OFF_C);
  u_str("\r\n");

  // --- 4. Boot/wake (identical to simple_offload.c). ---
  for (int i = 0; i < SNRT_CLUSTER_NUM; i++) {
    *(volatile uint64_t *)&(gwaihir_addrmap.cluster[i].peripheral_reg.scratch[1].w) =
        (uintptr_t)&gwaihir_addrmap.l2_spm;
    *(volatile uint64_t *)&(gwaihir_addrmap.cluster[i].peripheral_reg.scratch[0].w) =
        (uintptr_t)&return_code_array[i];
    for (int j = 0; j < CFG_CLUSTER_NR_CORES; j++) {
      return_code_array[i][j] = 0;
    }
  }
  __asm__ volatile("fence" ::: "memory");

  u_str("[seed] waking cluster 0 ...\r\n");
  // Start all cores in cluster 0, which wakes the other clusters (world barrier).
  *(volatile uint64_t *)&(gwaihir_addrmap.cluster[0].peripheral_reg.cl_clint_set.w) =
      (1 << CFG_CLUSTER_NR_CORES) - 1;

  // --- 5. Wait until all cores of all clusters report done (simple_offload). ---
  int all_finished = 0;
  while (!all_finished) {
    all_finished = 1;
    for (int i = 0; i < SNRT_CLUSTER_NUM; i++) {
      for (int j = 0; j < CFG_CLUSTER_NR_CORES; j++) {
        if ((return_code_array[i][j] & 1) == 0) {
          all_finished = 0;
          break;
        }
      }
    }
  }

  // Sum the snitch return codes (rc<<1 | 1 per core); non-zero => a core failed.
  uint32_t rc_sum = 0;
  for (int i = 0; i < SNRT_CLUSTER_NUM; i++)
    for (int j = 0; j < CFG_CLUSTER_NR_CORES; j++)
      rc_sum += (return_code_array[i][j] >> 1);

  // --- 6. Read back the descriptor completion + C, verify the golden. ---
  __asm__ volatile("fence" ::: "memory");
  volatile uint64_t* dbg = (volatile uint64_t*)l2_ptr(OFF_DBG);
  u_str("[seed] cluster done: rc_sum="); u_dec(rc_sum);
  u_str(" desc.status="); u_dec((uint64_t)(uint32_t)job->status);
  u_str(" desc.completion="); u_hex64(job->completion);
  u_str(" dbg.phase=");
  u_dec(dbg[0] == DBG_MAGIC ? dbg[1] : (uint64_t)-1);
  u_str("\r\n");

  // Read C[0] and C[255] as integers (the golden is exact-representable in f64).
  double c0 = C[0];
  double c255 = C[N * N - 1];
  uint64_t c0_i = (uint64_t)(int64_t)c0;
  uint64_t c255_i = (uint64_t)(int64_t)c255;
  u_str("[seed] C[0]="); u_dec(c0_i);
  u_str(" C[255]="); u_dec(c255_i);
  u_str(" (expected 2992 / 1976752)\r\n");

  int pass = (c0_i == 2992u) && (c255_i == 1976752u) && (rc_sum == 0u) &&
             (job->status == 0) && (job->completion == JOB_ID);
  if (pass) {
    u_str("[seed] *** PASS: cluster ran gemm64 dispatch, C == A @ B ***\r\n");
  } else {
    u_str("[seed] *** FAIL ***");
    if (c0_i != 2992u || c255_i != 1976752u) u_str(" [numerics]");
    if (rc_sum != 0u) u_str(" [core-rc]");
    if (job->status != 0) u_str(" [desc.status]");
    if (job->completion != JOB_ID) u_str(" [no-completion]");
    u_str("\r\n");
  }
  uart_write_flush(&__uart_base_addr__);

  // Host return code: 0 on PASS (scratch[2]/SLINK), non-zero otherwise.
  return pass ? 0 : (rc_sum ? (int)rc_sum : 30);
}
