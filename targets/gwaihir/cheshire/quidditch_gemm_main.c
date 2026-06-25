// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// gwaihir host-device split, Phase 2 -- FINAL co-sim host.
//
// The IREE host VM (native EmitC, running on the Cheshire/CVA6) records a QCS
// GEMM offload into the L2 SPM, rings the cluster doorbell (cl_clint_set), the
// snitch_cluster firmware (qcs_replay.elf, preloaded at the cluster entry)
// replays it running the real iree+xdsl gemm kernel, writes C + completion back
// into the L2-SPM descriptor, and the host verifies C == A @ B.
//
// This is the Cheshire-SW build of the spike main_emitc.c: same VM/HAL/EmitC
// setup, but (1) the cluster HAL points at the gwaihir L2-SPM HW aperture, (2)
// the doorbell is the real cl_clint_set wake + completion poll
// (shared_region_cheshire.c), and (3) after the invoke the host reads C out of
// the L2-SPM output buffer-view and checks it against A @ B, returning a
// PASS/FAIL code the Cheshire JTAG harness reports via scratch[2].

#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "iree/hal/device_group.h"
#include "iree/modules/hal/module.h"
#include "iree/modules/hal/types.h"
#include "iree/vm/api.h"

#include "gemm_square_emitc.h"
#include "hostio.h"
#include "runtime/host/hal/cluster/cluster_device.h"
#include "runtime/host/transport/cluster_command_stream.h"
#include "runtime/host/transport/shared_region.h"

//===----------------------------------------------------------------------===//
// Host VM bookkeeping arena (NOT the L2-SPM shared region).
//===----------------------------------------------------------------------===//
// The IREE VM/HAL/list/command-buffer bookkeeping allocates from this static
// bump arena so the VM never depends on the newlib heap. The A/B/C DEVICE
// buffers are NOT here -- the cluster allocator carves those out of the L2-SPM
// shared region (see cluster_allocator.c). Linked into DRAM (.bss) via dram.ld.
// Sized to fit L2-SPM tile 1 (1 MiB) alongside the host text/data/stack. IREE
// VM/HAL bookkeeping for a single dispatch is well under this.
// The earlier 64 MiB DRAM arena was a workaround for the command-buffer BLOAT
// (a per-dispatch ~16.8 MiB cluster_command_buffer_t: records[256] each holding
// a 64 KiB inline update_data) which exhausted the arena at status 8 =
// RESOURCE_EXHAUSTED in context_create_with_modules. With that fix (records[64],
// update bytes heap-allocated on demand) the command buffer is ~tens of KiB and
// the real arena need is sub-MB. So the arena goes BACK into L2-SPM as ordinary
// .bss (.arena NOLOAD in l2data; see hybrid.ld) -- DRAM is non-functional on
// this sim. 1 MiB is a comfortable margin over the measured spike HWM and fits
// l2data (~1.95 MiB) alongside host text/data/stack. NOLOAD keeps crt0 from
// zeroing it under the slow sim; the bump allocator zeroes CALLOC bytes.
#define HOST_ARENA_BYTES (1 * 1024 * 1024)
static uint8_t g_arena[HOST_ARENA_BYTES]
    __attribute__((aligned(64), section(".arena")));
static iree_host_size_t g_arena_off = 0;
static iree_host_size_t g_arena_hwm = 0;  // high-water mark (peak bytes used)

static iree_status_t host_arena_ctl(void* self, iree_allocator_command_t command,
                                    const void* params, void** inout_ptr) {
  switch (command) {
    case IREE_ALLOCATOR_COMMAND_MALLOC:
    case IREE_ALLOCATOR_COMMAND_CALLOC:
    case IREE_ALLOCATOR_COMMAND_REALLOC: {
      const iree_allocator_alloc_params_t* p =
          (const iree_allocator_alloc_params_t*)params;
      iree_host_size_t size = p->byte_length;
      iree_host_size_t aligned = (g_arena_off + 63u) & ~(iree_host_size_t)63u;
      if (aligned + size > HOST_ARENA_BYTES) {
        host_puts("[host] ARENA EXHAUSTED: need 0x");
        host_puthex64((uint64_t)(aligned + size));
        host_puts(" have 0x");
        host_puthex64((uint64_t)HOST_ARENA_BYTES);
        host_puts(" (hwm 0x");
        host_puthex64((uint64_t)g_arena_hwm);
        host_puts(")\n");
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "host arena exhausted");
      }
      void* ptr = &g_arena[aligned];
      g_arena_off = aligned + size;
      if (g_arena_off > g_arena_hwm) g_arena_hwm = g_arena_off;
      if (command == IREE_ALLOCATOR_COMMAND_CALLOC) {
        memset(ptr, 0, size);
      } else if (command == IREE_ALLOCATOR_COMMAND_REALLOC && *inout_ptr) {
        memcpy(ptr, *inout_ptr, size);
      }
      *inout_ptr = ptr;
      return iree_ok_status();
    }
    case IREE_ALLOCATOR_COMMAND_FREE:
      return iree_ok_status();
    default:
      return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "unsupported allocator command");
  }
}

static iree_allocator_t host_allocator(void) {
  iree_allocator_t v = {NULL, host_arena_ctl};
  return v;
}

static void host_print_status(iree_status_t st) {
  static char sbuf[1024];
  iree_host_size_t n = 0;
  if (iree_status_format(st, sizeof(sbuf), sbuf, &n)) {
    _write(1, sbuf, n > sizeof(sbuf) ? sizeof(sbuf) : n);
    _write(1, "\n", 1);
  }
}

#define CHECK(st, msg)                                                       \
  do {                                                                       \
    if (!iree_status_is_ok(st)) {                                            \
      host_puts("[host] FAIL: " msg " (status code ");                       \
      host_putu((unsigned long)iree_status_code(st));                        \
      host_puts(")\n");                                                      \
      host_print_status(st);                                                 \
      iree_status_ignore(st);                                                \
      return 10;                                                             \
    }                                                                        \
  } while (0)

#define N 16

// Reference A,B fill (deterministic) and the golden C = A @ B (row-major f64).
static void fill_inputs(double* a, double* b) {
  for (int i = 0; i < N * N; ++i) {
    a[i] = (double)((1) * (i + 1));  // arg0 pattern (same as spike)
    b[i] = (double)((2) * (i + 1));  // arg1 pattern
  }
}
// The kernel (runtime/samples/gemm_square/gemm_square.mlir) is matmul_transpose_b:
//   C[i,j] = sum_k A[i,k] * B[j,k]   (NOT plain matmul a[i,k]*b[k,j]).
// Both A and B are row-major 16x16xf64. Compute the golden accordingly.
static void golden_gemm(const double* a, const double* b, double* c) {
  for (int i = 0; i < N; ++i)
    for (int j = 0; j < N; ++j) {
      double acc = 0.0;
      for (int k = 0; k < N; ++k) acc += a[i * N + k] * b[j * N + k];
      c[i * N + j] = acc;
    }
}

extern void cheshire_console_init(void);

int main(void) {
  cheshire_console_init();  // bring up the UART before any printing
  host_puts("\n[host] === gwaihir Phase-2 co-sim: IREE host -> QCS -> cluster gemm ===\n");

  // Arena sanity probe: the host VM arena (g_arena) now lives in L2-SPM
  // (.arena NOLOAD in l2data; DRAM is non-functional on this sim). Confirm CVA6
  // load/store to both ends of the arena works before the VM relies on it.
  {
    volatile uint64_t* lo = (volatile uint64_t*)&g_arena[0];
    volatile uint64_t* hi =
        (volatile uint64_t*)&g_arena[HOST_ARENA_BYTES - 8];
    *lo = 0xC0FFEE11D15EA5EDull;
    *hi = 0x0123456789ABCDEFull;
    __asm__ volatile("fence" ::: "memory");
    host_puts("[host] arena probe: arena @ 0x");
    host_puthex64((uint64_t)(uintptr_t)&g_arena[0]);
    host_puts(" size 0x"); host_puthex64((uint64_t)HOST_ARENA_BYTES);
    host_puts(" lo=0x"); host_puthex64(*lo);
    host_puts(" hi=0x"); host_puthex64(*hi);
    if (*lo == 0xC0FFEE11D15EA5EDull && *hi == 0x0123456789ABCDEFull) {
      host_puts(" OK\n");
    } else {
      host_puts(" FAIL (arena not writable!)\n");
      return 4;
    }
    *lo = 0; *hi = 0;  // clear sentinels so the arena starts clean
  }

  iree_allocator_t allocator = host_allocator();

  iree_vm_instance_t* instance = NULL;
  iree_status_t status = iree_vm_instance_create(
      IREE_VM_TYPE_CAPACITY_DEFAULT, allocator, &instance);
  CHECK(status, "instance_create");
  host_puts("[host] VM instance created\n");

  status = iree_hal_module_register_all_types(instance);
  CHECK(status, "hal_module_register_all_types");
  host_puts("[host] HAL types registered\n");

  // Cluster HAL device over the gwaihir L2-SPM aperture (HW-wired region).
  static qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, NULL, 0) != 0) {
    host_puts("[host] FAIL: shared_region_create\n");
    return 3;
  }
  host_puts("[host] L2-SPM shared region wired (base=0x"); host_puthex64((uint64_t)(uintptr_t)region.base);
  host_puts(" size=0x"); host_puthex64(region.size); host_puts(")\n");

  iree_hal_device_t* device = NULL;
  status = iree_hal_cluster_device_create(IREE_SV("quidditch_device"), &region,
                                          allocator, &device);
  CHECK(status, "cluster_device_create");
  host_puts("[host] cluster HAL device created\n");

  iree_hal_device_group_t* device_group = NULL;
  status = iree_hal_device_group_create_from_device(device, allocator,
                                                    &device_group);
  CHECK(status, "device_group_create_from_device");

  iree_vm_module_t* hal_module = NULL;
  status = iree_hal_module_create(
      instance, iree_hal_module_device_policy_default(), device_group,
      IREE_HAL_MODULE_FLAG_SYNCHRONOUS, iree_hal_module_debug_sink_null(),
      allocator, &hal_module);
  CHECK(status, "hal_module_create");
  host_puts("[host] HAL module created\n");

  iree_vm_module_t* gemm_module = NULL;
  status = gemm_square_create(instance, allocator, &gemm_module);
  CHECK(status, "gemm_square_create (EmitC native module)");
  host_puts("[host] EmitC native module created\n");

  iree_vm_module_t* modules[2] = {hal_module, gemm_module};
  iree_vm_context_t* context = NULL;
  status = iree_vm_context_create_with_modules(
      instance, IREE_VM_CONTEXT_FLAG_NONE, IREE_ARRAYSIZE(modules), modules,
      allocator, &context);
  CHECK(status, "context_create_with_modules");
  host_puts("[host] *** context created: HAL imports RESOLVED *** (arena hwm 0x");
  host_puthex64((uint64_t)g_arena_hwm); host_puts(")\n");

  iree_vm_function_t function;
  status = iree_vm_context_resolve_function(
      context, IREE_SV("gemm_square.gemm64"), &function);
  CHECK(status, "resolve gemm_square.gemm64");
  host_puts("[host] resolved export gemm_square.gemm64\n");

  // Golden reference computed on the host.
  static double A[N * N], B[N * N], C_gold[N * N];
  fill_inputs(A, B);
  golden_gemm(A, B, C_gold);

  iree_hal_allocator_t* dev_alloc = iree_hal_device_allocator(device);
  iree_hal_buffer_params_t bparams = {0};
  bparams.type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL;
  bparams.usage = IREE_HAL_BUFFER_USAGE_DEFAULT | IREE_HAL_BUFFER_USAGE_TRANSFER |
                  IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE |
                  IREE_HAL_BUFFER_USAGE_MAPPING;
  bparams.access = IREE_HAL_MEMORY_ACCESS_ALL;
  const iree_hal_dim_t shape[2] = {N, N};
  const iree_device_size_t bytes = N * N * sizeof(double);

  iree_vm_list_t* inputs = NULL;
  status = iree_vm_list_create(iree_vm_make_undefined_type_def(), 2, allocator,
                               &inputs);
  CHECK(status, "vm_list_create inputs");

  for (int arg = 0; arg < 2; ++arg) {
    iree_hal_buffer_t* buffer = NULL;
    status = iree_hal_allocator_allocate_buffer(dev_alloc, bparams, bytes,
                                                &buffer);
    CHECK(status, "allocate_buffer input");
    iree_hal_buffer_mapping_t map = {{0}};
    status = iree_hal_buffer_map_range(buffer, IREE_HAL_MAPPING_MODE_SCOPED,
                                       IREE_HAL_MEMORY_ACCESS_DISCARD_WRITE, 0,
                                       IREE_HAL_WHOLE_BUFFER, &map);
    CHECK(status, "map input");
    double* data = (double*)map.contents.data;
    const double* src = (arg == 0) ? A : B;
    for (int i = 0; i < N * N; ++i) data[i] = src[i];
    iree_hal_buffer_unmap_range(&map);

    iree_hal_buffer_view_t* bv = NULL;
    status = iree_hal_buffer_view_create(
        buffer, 2, shape, IREE_HAL_ELEMENT_TYPE_FLOAT_64,
        IREE_HAL_ENCODING_TYPE_DENSE_ROW_MAJOR, allocator, &bv);
    iree_hal_buffer_release(buffer);
    CHECK(status, "buffer_view_create input");
    iree_vm_ref_t ref = iree_hal_buffer_view_move_ref(bv);
    status = iree_vm_list_push_ref_move(inputs, &ref);
    CHECK(status, "list_push input");
  }
  host_puts("[host] inputs A,B written into L2 SPM (2x tensor<16x16xf64>)\n");

  iree_vm_list_t* outputs = NULL;
  status = iree_vm_list_create(iree_vm_make_undefined_type_def(), 1, allocator,
                               &outputs);
  CHECK(status, "vm_list_create outputs");

  host_puts("[host] invoking gemm_square.gemm64 (records QCS, rings cl_clint_set, waits) ...\n");
  status = iree_vm_invoke(context, function, IREE_VM_INVOCATION_FLAG_NONE,
                          /*policy=*/NULL, inputs, outputs, allocator);
  if (iree_status_is_ok(status)) {
    host_puts("[host] *** invoke RETURNED OK (cluster completed) *** (arena hwm 0x");
    host_puthex64((uint64_t)g_arena_hwm); host_puts(")\n");
  } else {
    host_puts("[host] FAIL: invoke returned non-OK, status code = ");
    host_putu((unsigned long)iree_status_code(status));
    host_puts("\n");
    host_print_status(status);
    iree_status_ignore(status);
    return 11;
  }

  // Read C out of the output buffer-view (which lives in L2 SPM) and verify.
  iree_hal_buffer_view_t* out_bv =
      (iree_hal_buffer_view_t*)iree_vm_list_get_ref_deref(
          outputs, 0, iree_hal_buffer_view_type());
  if (!out_bv) {
    host_puts("[host] FAIL: no output buffer-view returned\n");
    return 12;
  }
  iree_hal_buffer_t* out_buf = iree_hal_buffer_view_buffer(out_bv);
  iree_hal_buffer_mapping_t omap = {{0}};
  status = iree_hal_buffer_map_range(out_buf, IREE_HAL_MAPPING_MODE_SCOPED,
                                     IREE_HAL_MEMORY_ACCESS_READ, 0,
                                     IREE_HAL_WHOLE_BUFFER, &omap);
  CHECK(status, "map output C");
  const double* Cdev = (const double*)omap.contents.data;

  // Compare C (device) against the golden A@B.
  int mismatches = 0;
  for (int i = 0; i < N * N; ++i) {
    double diff = Cdev[i] - C_gold[i];
    if (diff < 0) diff = -diff;
    // f64 exact-ish: tolerate tiny rounding.
    double tol = 1e-6 * (C_gold[i] < 0 ? -C_gold[i] : C_gold[i]) + 1e-9;
    if (diff > tol) {
      if (mismatches < 4) {
        host_puts("[host]   C["); host_putu(i); host_puts("] dev/gold mismatch\n");
      }
      ++mismatches;
    }
  }
  host_puts("[host] C[0]=("); host_putu((unsigned long)(long)Cdev[0]);
  host_puts(") gold C[0]=("); host_putu((unsigned long)(long)C_gold[0]); host_puts(")\n");
  host_puts("[host] C[255]=("); host_putu((unsigned long)(long)Cdev[N*N-1]);
  host_puts(") gold C[255]=("); host_putu((unsigned long)(long)C_gold[N*N-1]); host_puts(")\n");
  iree_hal_buffer_unmap_range(&omap);

  if (mismatches == 0) {
    host_puts("[host] *** PASS: C == A @ B  (gwaihir end-to-end gemm offload) ***\n");
    return 0;
  }
  host_puts("[host] *** FAIL: C != A @ B, mismatches = ");
  host_putu((unsigned long)mismatches); host_puts(" ***\n");
  return 20;
}
