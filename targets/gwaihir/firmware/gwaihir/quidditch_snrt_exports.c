// Copyright 2024 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>

#include "snrt.h"

extern "C" uint32_t quidditch_export_snrt_dma_start_1d(void *dst, void *src,
                                                       size_t size)
    asm("snrt_dma_start_1d");
extern "C" uint32_t quidditch_export_snrt_dma_start_1d(void *dst, void *src,
                                                       size_t size) {
  return snrt_dma_start_1d((uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)src,
                           size);
}

extern "C" uint32_t quidditch_export_snrt_dma_start_2d(
    void *dst, void *src, size_t size, size_t dst_stride, size_t src_stride,
    size_t repeat) asm("snrt_dma_start_2d");
extern "C" uint32_t quidditch_export_snrt_dma_start_2d(
    void *dst, void *src, size_t size, size_t dst_stride, size_t src_stride,
    size_t repeat) {
  return snrt_dma_start_2d((uint64_t)(uintptr_t)dst, (uint64_t)(uintptr_t)src,
                           size, dst_stride, src_stride, repeat);
}

extern "C" uint32_t quidditch_export_snrt_cluster_core_idx(void)
    asm("snrt_cluster_core_idx");
extern "C" uint32_t quidditch_export_snrt_cluster_core_idx(void) {
  return snrt_cluster_core_idx();
}

extern "C" void quidditch_export_snrt_cluster_hw_barrier(void)
    asm("snrt_cluster_hw_barrier");
extern "C" void quidditch_export_snrt_cluster_hw_barrier(void) {
  snrt_cluster_hw_barrier();
}

extern "C" int quidditch_export_snrt_is_dm_core(void) asm("snrt_is_dm_core");
extern "C" int quidditch_export_snrt_is_dm_core(void) {
  return snrt_is_dm_core();
}

extern "C" uint32_t quidditch_export_snrt_cluster_compute_core_num(void)
    asm("snrt_cluster_compute_core_num");
extern "C" uint32_t quidditch_export_snrt_cluster_compute_core_num(void) {
  return snrt_cluster_compute_core_num();
}

extern "C" void quidditch_export_snrt_dma_wait_all(void)
    asm("snrt_dma_wait_all");
extern "C" void quidditch_export_snrt_dma_wait_all(void) {
  snrt_dma_wait_all();
}

// Used by _write() (syscalls.c) to index the per-core output buffer.
extern "C" uint32_t quidditch_export_snrt_hartid(void) asm("snrt_hartid");
extern "C" uint32_t quidditch_export_snrt_hartid(void) { return snrt_hartid(); }
