// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Gate-(b) bring-up host for the QCS replayer (no-job smoke test).
//
// This is a minimal variant of simple_offload.c (the proven offload oracle).
// It performs the SAME boot/wake protocol -- seed every cluster's boot scratch
// regs (entry point + return_code_array slot), wake cluster 0, then poll the
// return_code_array until all cores of all clusters report done -- and ADDS one
// step: it zeroes the QCS job-descriptor page in L2 SPM before waking.
//
// WHY: in the real flow the host writes a valid qcs_job_descriptor_t into L2 SPM
// before the doorbell, so that region is initialized. In this no-job smoke test
// nothing writes the descriptor, and uninitialized L2 SPM reads back as X in
// RTL sim. Cluster 0's cores load job->magic at L2_SPM_BASE+0x10000, get X, and
// the X propagates into a register write -> the snitch RegWriteKnown assertion
// fires on every cluster-0 core (the prior gate-(b) wedge). Zeroing the
// descriptor page makes job->magic == 0 != QCS_MAGIC, so the cluster firmware's
// no-job guard returns 0 cleanly (rc 0) exactly like simple.elf.
//
// We deliberately do NOT modify simple_offload.c (the oracle); this is a
// separate, equally byte-aligned host.

#include <stdint.h>
#include "gw_addrmap.h"
#include "gw_raw_addrmap.h"

#include "snitch_cluster_cfg.h"

// Return code array placed at the last 4K page of the L2 SPM region.
// This address scales automatically with the number and size of memory tiles.
#define RETURN_CODE_ADDR \
  (GW_L2_SPM_BASE_ADDR(0) + GW_L2_SPM_TOTAL_SIZE - 0x1000)

// QCS job descriptor page: the cluster firmware reads the descriptor at this
// fixed offset into the L2-SPM aperture (QCS_JOB_DESCRIPTOR_OFFSET == 0x10000,
// see sw/snitch/apps/qcs_replay/src/cluster_command_stream.h). The descriptor
// struct (qcs_job_descriptor_t) is 56 bytes; we zero a full 4K page so the
// whole descriptor + any slack the firmware might touch reads back as 0.
#define QCS_JOB_DESCRIPTOR_OFFSET 0x10000u
#define QCS_DESCRIPTOR_ADDR (GW_L2_SPM_BASE_ADDR(0) + QCS_JOB_DESCRIPTOR_OFFSET)
#define QCS_DESCRIPTOR_ZERO_BYTES 0x1000u  // one page, >> sizeof(descriptor)

// This needs to be in a region which is not cached
volatile uint32_t (*return_code_array)[CFG_CLUSTER_NR_CORES] = (uint32_t (*)[CFG_CLUSTER_NR_CORES])RETURN_CODE_ADDR;

int main() {

  // Zero the QCS job-descriptor page BEFORE waking the cluster, so the cluster
  // firmware reads a defined (all-zero) descriptor: magic 0 != QCS_MAGIC ->
  // no-job guard -> clean return. Word-wise to keep it simple and aligned.
  volatile uint32_t *desc = (volatile uint32_t *)QCS_DESCRIPTOR_ADDR;
  for (uint32_t off = 0; off < QCS_DESCRIPTOR_ZERO_BYTES / sizeof(uint32_t); off++) {
    desc[off] = 0u;
  }

  // Write entry point to scratch register 1
  // and return code address to scratch register 0
  // Initalize return address loaction before offloading.
  for (int i = 0; i < SNRT_CLUSTER_NUM; i++) {
    *(volatile uint64_t *)&(gwaihir_addrmap.cluster[i].peripheral_reg.scratch[1].w) = (uintptr_t)&gwaihir_addrmap.l2_spm;
    *(volatile uint64_t *)&(gwaihir_addrmap.cluster[i].peripheral_reg.scratch[0].w) = (uintptr_t)&return_code_array[i];
    for (int j = 0; j < CFG_CLUSTER_NR_CORES; j++) {
      return_code_array[i][j] = 0;
    }
  }

  // Start all cores in Cluster 0, which will wake up all other clusters
  *(volatile uint64_t *)&(gwaihir_addrmap.cluster[0].peripheral_reg.cl_clint_set.w) = (1 << CFG_CLUSTER_NR_CORES) - 1;

  // Wait until all cores have finished
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

  // Sum up the return codes
  uint32_t sum = 0;
  for (int i = 0; i < SNRT_CLUSTER_NUM; i++) {
    for (int j = 0; j < CFG_CLUSTER_NR_CORES; j++) {
      sum += (return_code_array[i][j] >> 1);
    }
  }

  return sum;
}
