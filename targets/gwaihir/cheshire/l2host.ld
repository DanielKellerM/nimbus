/* Copyright 2026 ETH Zurich and University of Bologna. */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Cheshire host link script for the gwaihir Phase-2 co-sim. The host ELF must
 * live in a JTAG-writable memory: in the gwaihir sim, DRAM (0x80000000) is NOT
 * writable over the JTAG system bus (jtag_elf_run -> "System bus error"), but
 * the L2 SPM (0x70000000, 2 MiB) IS (it is the same aperture the firmware is
 * backdoor-preloaded into). So we place the host in L2-SPM tile 1
 * (0x70100000, 1 MiB), keeping tile 0 (0x70000000..0x70100000) for the
 * qcs_replay firmware (0x70000000..~0x7000e000), the QCS job descriptor
 * (0x70010000), and the A/B/C device buffers (0x70011000+).
 */

INCLUDE common.ldh

MEMORY {
  /* L2-SPM tile 0, above the firmware (..0x7000e000), the QCS descriptor
   * (0x70010000) and the A/B/C device buffers (0x70011000..0x70013000):
   * host code + data + bookkeeping arena + stack live in [0x70014000,
   * 0x70100000) (944 KiB). Using tile 0 (the same region the qcs_replay
   * firmware is backdoor-loaded into and executes from) so the CVA6 fetch
   * path matches a proven-readable L2-SPM region. */
  l2host (rwx) : ORIGIN = 0x70014000, LENGTH = 0x000EC000
}

SECTIONS {
  /* Stack at the very top of tile 1, growing down. */
  __stack_pointer$ = ORIGIN(l2host) + LENGTH(l2host) - 16;

  .text : {
    *(.text._start)
    *(.text)
    *(.text.*)
  } > l2host

  .misc : ALIGN(16) {
    *(.rodata)
    *(.rodata.*)
    *(.data)
    *(.data.*)
    *(.srodata)
    *(.srodata.*)
    *(.sdata)
    *(.sdata.*)
  } > l2host

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(.sbss)
    *(.sbss.*)
    __bss_end = .;
  } > l2host

  .bulk : ALIGN(16) {
    *(.bulk)
    *(.bulk.*)
  } > l2host
}
