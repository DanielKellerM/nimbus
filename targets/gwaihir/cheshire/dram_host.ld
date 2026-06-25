/* Copyright 2026 ETH Zurich and University of Bologna. */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gwaihir Phase-2 co-sim PRINCIPLED DRAM host link script.
 *
 * Places the ENTIRE host (.text/.misc/.bss/.arena/stack) in external DRAM at
 * 0x80000000. Rationale (see hybrid.ld's root-cause note): LlcOut(0x80000000)
 * IS one of the CVA6 ExecuteRegions AND a CachedRegion (cheshire_pkg
 * gen_cva6_cfg). So the CVA6 fetches + runs the host from DRAM directly, cached
 * through the LLC -- no SPM crutch, no JTAG text load. The DRAM is now a real
 * backing store: the gwaihir LLC master port is backed by axi_sim_mem
 * (i_llc_sim_mem, see hw/cheshire_tile.sv), not an axi_err_slv. The host ELF is
 * backdoor-loaded into the sim_mem (tb dram_host_elf_preload, PRELMODE=4); the
 * earlier "DRAM bus error" was the JTAG *write path* to DRAM (now bypassed),
 * not a fetch/cache problem.
 *
 * The QCS shared region + A/B/C device buffers do NOT live here -- the cluster
 * allocator carves those out of the L2-SPM (0x70000000) at runtime; only the
 * firmware is preloaded there. The host's VM bookkeeping bump arena (g_arena,
 * .arena) is small (sub-MB after the command-buffer bloat fix) and lives in
 * DRAM .bss.
 *
 * DRAM region: 0x80000000, 8 MiB (cheshire_addrs.ldh). Host text ~120 KiB +
 * data + 1 MiB arena + stack fits comfortably.
 */

INCLUDE common.ldh

SECTIONS {
  /* Stack at the top of DRAM, growing down (8 MiB region). */
  __stack_pointer$ = ORIGIN(dram) + LENGTH(dram) - 16;

  .text : {
    *(.text._start)
    *(.text)
    *(.text.*)
    . = ALIGN(8);
  } > dram

  .misc : ALIGN(16) {
    *(.rodata)
    *(.rodata.*)
    *(.data)
    *(.data.*)
    *(.srodata)
    *(.srodata.*)
    *(.sdata)
    *(.sdata.*)
    *(.got)
    *(.got.*)
    *(.got.plt)
  } > dram

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(.sbss)
    *(.sbss.*)
    __bss_end = .;
  } > dram

  .bulk : ALIGN(16) {
    *(.bulk)
    *(.bulk.*)
  } > dram

  /* Host VM bump arena. NOLOAD: not in the ELF image (no preload, no crt0
   * zeroing of a big region under the slow sim). The bump allocator zeroes only
   * what it hands out on CALLOC. */
  .arena (NOLOAD) : ALIGN(64) {
    __arena_start = .;
    *(.arena)
    *(.arena.*)
    __arena_end = .;
  } > dram
}
