// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Minimal bare-metal CVA6 wake probe -- isolates the Snitch-cluster doorbell.
//
// PURPOSE. Capture the DEFINITIVE failing-case trace behind the shipped 32-bit
// wake fix (shared_region_cheshire.c qcs_doorbell_ring): a SINGLE 64-bit store
// to cl_clint_set does NOT wake the cluster because cl_clint_set is write-enabled
// only in the UPPER 32-bit lane of its 64-bit reg bus (reg2hw.cl_clint_set.
// wr_biten = 0xffffffff<<32). The naturally-lower-lane 64-bit store misses that
// lane, so cl_clint_q stays 0. This probe drives exactly that store and spins.
//
// WHY A SEPARATE PROBE. The full CVA6 host cannot reach the doorbell on the debug
// simv: under PRELMODE=3 the host ELF is JTAG-loaded WITHOUT the autonomous
// Cheshire boot, so CHS_REGS->rtc_freq is garbage, uart_init picks a bad divisor,
// and _putchar spins forever on the UART LSR before any doorbell store. This probe
// touches ONLY peripheral MMIO (cluster scratch + cl_clint_set, in the uncached
// 0x2000_0000 Ext region -- no cached L2/DRAM, no CMO path) and NO UART / newlib /
// region / dispatch, so it reaches the 64-bit store in microseconds.
//
// WHAT THE TRACE PROVES (dump scoped to i_snitch_cluster_peripheral, wake_iface.py):
//   * reg2hw.cl_clint_set.req_is_wr pulses and .wr_data = msip mask arrives, yet
//     cl_clint_q (and cl_clint_o, the msip to the cores) STAYS 0 -> the 64-bit
//     store is swallowed. That is the failing case, definitively.
//   * Build with -DWAKE_POSITIVE_CONTROL to append the shipped 32-bit store after
//     a delay: the SAME trace then shows cl_clint_q -> mask and cl_clint_o assert
//     -- an in-run A/B that pins width, not address, as the discriminator.
//
// Running this is OPTIONAL: the mechanism is already evidenced by the working
// mode-5 VPD trace that pinned wr_biten. This probe is the clean confirmation.

#include <stdint.h>

// gwaihir generated address map -- the SAME macros the host doorbell uses; no
// hardcoded peripheral addresses.
#include "gw_raw_addrmap.h"

// Cluster 0 boot/doorbell peripheral regs (uncached Ext MMIO), via the generated
// macros exactly as shared_region_cheshire.c derives CL_SCRATCH / CL_CLINT_SET.
#define CL_SCRATCH(cl, idx) \
  ((volatile uint64_t*)(uintptr_t)GW_CLUSTER_PERIPHERAL_REG_SCRATCH_BASE_ADDR((cl), (idx)))
#define CL_CLINT_SET(cl) \
  ((volatile uint64_t*)(uintptr_t)GW_CLUSTER_PERIPHERAL_REG_CL_CLINT_SET_BASE_ADDR((cl)))

#ifndef QCS_CLUSTER_NR_CORES
#define QCS_CLUSTER_NR_CORES 9  // 8 compute + 1 DM core -> msip mask (1<<9)-1
#endif

// Short spacer so the failing 64-bit store and the positive-control 32-bit store
// land at clearly separated sim times in the peripheral trace. MMIO-only, no UART.
static void spin_delay(volatile uint32_t n) {
  while (n--) __asm__ volatile("nop");
}

int main(void) {
  const uint32_t msip_mask = (uint32_t)((1u << QCS_CLUSTER_NR_CORES) - 1u);

  // Faithful doorbell preamble: publish the boot scratch regs for cluster 0 (entry
  // = L2-SPM base, rc slot = 0). Incidental to the wake; kept so the peripheral
  // trace also shows the scratch bus activity that precedes the msip store.
  CL_SCRATCH(0, 1)[0] = (uint64_t)(uintptr_t)GW_L2_SPM_BASE_ADDR(0);  // scratch[1] = entry
  CL_SCRATCH(0, 0)[0] = 0u;                                           // scratch[0] = &rc
  __asm__ volatile("fence" ::: "memory");

  // *** FAILING CASE ***  single 64-bit store to cl_clint_set (a single `sd`).
  // msip_mask lands in bus data[31:0]; wr_biten gates only data[63:32] -> the
  // write is swallowed. Trace: cl_clint_q stays 0.
  *CL_CLINT_SET(0) = (uint64_t)msip_mask;
  __asm__ volatile("fence" ::: "memory");

#ifdef WAKE_POSITIVE_CONTROL
  // *** POSITIVE CONTROL ***  the shipped 32-bit store (a single `sw`) after a gap.
  // Trace: cl_clint_q -> msip_mask, cl_clint_o asserts -> width, not address, is
  // the discriminator. (Wakes the cluster; harmless -- no firmware need be loaded.)
  spin_delay(64);
  *(volatile uint32_t*)CL_CLINT_SET(0) = msip_mask;
  __asm__ volatile("fence" ::: "memory");
#else
  (void)spin_delay;
#endif

  // Park forever so the run stays in the peripheral window; never returns to crt0
  // (_exit / HTIF), never touches UART.
  for (;;) __asm__ volatile("wfi");
  return 0;
}
