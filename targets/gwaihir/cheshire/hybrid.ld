/* Copyright 2026 ETH Zurich and University of Bologna. */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gwaihir Phase-2 co-sim HYBRID host link script.
 *
 * ROOT CAUSE of the "CVA6 fetches X at entry+4" blocker: the L2-SPM aperture
 * (0x70000000) is in the Cheshire AxiExtRegion [0x2000_0000,0x8000_0000), which
 * is NOT one of the CVA6's ExecuteRegions (see cheshire_pkg::gen_cva6_cfg:
 * ExecuteRegionAddrBase = {ExtRom, BootRom, SPM(0x10000000), SPM_UNC(0x14000000),
 * LlcOut(0x80000000), ExtCI(empty), L1_ISPM}). The CVA6 PMA refuses to fetch
 * instructions outside an execute region -> the icache returns X. It is NOT a
 * cache-coherency problem: the backdoor write lands fine and loads/stores to
 * 0x70000000 work (the firmware reads the buffers). Only *instruction fetch*
 * from 0x70000000 is illegal.
 *
 * FIX: place the host's executable code (.text, 118.5 KiB) into the Cheshire
 * internal SPM (0x10000000) -- an ExecuteRegion + CachedRegion, real coherent
 * SRAM inside the SoC (HW SizeSpm = LlcSetAssoc*LlcNumLines*LlcNumBlocks*
 * AxiDataWidth/8 = 8*256*8*8 = 128 KiB; the conservative cheshire_addrs.ldh
 * declares only 64 KiB but the HW + CVA6 execute/cached region is 128 KiB).
 * Everything else (.misc rodata/data, .bss incl. the VM arena, stack) stays in
 * the L2-SPM (0x70014000), which the CVA6 can load/store from and which the
 * cluster firmware shares. Cross-region absolute references are fine (medany).
 */

INCLUDE common.ldh

MEMORY {
  /* Cheshire internal SPM: CVA6-executable + cached + coherent. 128 KiB HW. */
  chsspm (rwx) : ORIGIN = 0x10000000, LENGTH = 0x00020000
  /* L2-SPM host-data window: ALL host private data (rodata/data, .bss, the host
   * VM bump arena, stack). MUST NOT overlap the QCS shared region the cluster
   * reads -- the old ORIGIN 0x70014000 sat inside the device arena (bindings
   * A/B/C land at 0x70021000+), corrupting the descriptor/bindings so the
   * cluster never ran. L2-SPM (2 MiB) partition, provably disjoint:
   *   [0x70010000,0x70011000) QCS descriptor + dbg block
   *   [0x70011000,0x70040000) QCS device arena (cmd stream + A/B/C bindings)
   *   [0x70040000,0x701ff000) host private data + stack   <-- this window
   *   [0x701ff000,0x70200000) return_code_array (top page)
   * LENGTH = 0x701ff000 - 0x70040000 = 0x001BF000 (~1.75 MiB). */
  l2data (rw)  : ORIGIN = 0x70040000, LENGTH = 0x001BF000
}

SECTIONS {
  /* Stack at the very top of the L2-SPM host-data window, growing down. */
  __stack_pointer$ = ORIGIN(l2data) + LENGTH(l2data) - 16;

  /* Executable code -> Cheshire SPM (the only change that matters). */
  .text : {
    *(.text._start)
    *(.text)
    *(.text.*)
    . = ALIGN(8);
  } > chsspm

  /* rodata/data + GOT -> L2-SPM data window. */
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
  } > l2data

  .bss : ALIGN(16) {
    __bss_start = .;
    *(.bss)
    *(.bss.*)
    *(.sbss)
    *(.sbss.*)
    __bss_end = .;
  } > l2data

  .bulk : ALIGN(16) {
    *(.bulk)
    *(.bulk.*)
  } > l2data

  /* Host VM bump arena -> L2-SPM (back to normal .bss-style storage). NOLOAD so
   * it is not in the ELF image (no JTAG preload, no crt0 zeroing of a big
   * region under the slow sim); the bump allocator zeroes only what it hands out
   * on CALLOC. With the bloat fix the arena is small and fits easily in l2data. */
  .arena (NOLOAD) : ALIGN(64) {
    __arena_start = .;
    *(.arena)
    *(.arena.*)
    __arena_end = .;
  } > l2data
}
