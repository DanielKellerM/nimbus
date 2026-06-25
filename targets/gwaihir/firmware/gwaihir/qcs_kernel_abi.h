// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Minimal, self-contained slice of IREE's executable-library v0 ABI
// (iree/hal/local/executable_library.h) sufficient for the QCS replayer to
// build an iree_hal_executable_dispatch_state_v0_t + workgroup_state and invoke
// an iree_hal_executable_dispatch_v0_t kernel.
//
// The struct layouts here are field-for-field identical to IREE's so a real
// iree+xdsl kernel.o (compiled against the upstream header) links and is called
// with the exact same memory layout. We vendor a minimal copy rather than the
// full upstream header so the rv32 firmware build stays dependency-free (the
// upstream header pulls in iree/base/*, attribute macros, etc.). When the real
// kernel.o is wired in, this can be swapped for the upstream header; the layouts
// match by construction.
//
// SOURCE OF TRUTH: iree/runtime/src/iree/hal/local/executable_library.h
// (mirrored in Quidditch's runtime/runtime/src/Quidditch/executable/).

#ifndef QCS_REPLAY_KERNEL_ABI_H_
#define QCS_REPLAY_KERNEL_ABI_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// iree_hal_processor_v0_t: opaque per-core CPU info. IREE sizes this at 8 u64s.
typedef struct iree_hal_processor_v0_t {
  uint64_t data[8];
} iree_hal_processor_v0_t;

// Import thunk + import function pointer types. The replayer passes a zeroed
// environment (no imports), so the exact callee signature is irrelevant here;
// we only need the field to exist with pointer size/alignment.
typedef int (*iree_hal_executable_import_v0_t)(void* params, void* context,
                                               void* reserved);
typedef int (*iree_hal_executable_import_thunk_v0_t)(
    iree_hal_executable_import_v0_t fn_ptr, void* params, void* context,
    void* reserved);

// Per-dispatch immutable execution environment.
typedef struct iree_hal_executable_environment_v0_t {
  const uint32_t* constants;
  iree_hal_executable_import_thunk_v0_t import_thunk;
  const iree_hal_executable_import_v0_t* import_funcs;
  const void** import_contexts;
  iree_hal_processor_v0_t processor;
} iree_hal_executable_environment_v0_t;

// Per-dispatch state shared by every workgroup in the grid.
typedef struct iree_hal_executable_dispatch_state_v0_t {
  uint32_t workgroup_size_x;
  uint32_t workgroup_size_y;
  uint16_t workgroup_size_z;
  uint16_t constant_count;
  uint32_t workgroup_count_x;
  uint32_t workgroup_count_y;
  uint16_t workgroup_count_z;
  uint8_t max_concurrency;
  uint8_t binding_count;
  const uint32_t* constants;
  void* const* binding_ptrs;
  const size_t* binding_lengths;
} iree_hal_executable_dispatch_state_v0_t;

// Per-workgroup state (one per invocation of the kernel fn).
typedef struct iree_hal_executable_workgroup_state_v0_t {
  uint32_t workgroup_id_x;
  uint32_t workgroup_id_y;
  uint16_t workgroup_id_z;
  uint16_t reserved;
  uint32_t processor_id;
  void* local_memory;
  uint32_t local_memory_size;
} iree_hal_executable_workgroup_state_v0_t;

// The dispatch entry point. A real iree+xdsl export has this exact signature,
// e.g. `int gemm64_dispatch_0(const iree_hal_executable_environment_v0_t*,
// const iree_hal_executable_dispatch_state_v0_t*,
// const iree_hal_executable_workgroup_state_v0_t*)`.
typedef int (*iree_hal_executable_dispatch_v0_t)(
    const iree_hal_executable_environment_v0_t* environment,
    const iree_hal_executable_dispatch_state_v0_t* dispatch_state,
    const iree_hal_executable_workgroup_state_v0_t* workgroup_state);

//===----------------------------------------------------------------------===//
// Static-library query ABI (Quidditch fork of executable_library v0)
//===----------------------------------------------------------------------===//
// Minimal, offset-faithful slice of the export/library tables emitted by the
// Quidditch iree+xdsl static-library path, sufficient to walk the table returned
// by `<name>_library_query()` and recover the per-export dispatch fn pointers.
//
// SOURCE OF TRUTH (layout MUST match these byte-for-byte):
//   iree/runtime/src/iree/hal/local/executable_library.h  (header + sub-tables)
//   runtime/runtime/src/Quidditch/executable/executable_library.h  (the fork's
//       quidditch_executable_{export_table,library}_v0_t, which adds compute_core
//       vs dma_core split + the v0.6 params/occupancy slots).
//
// We only DEREFERENCE `exports.count` and `exports.compute_core_ptrs`; every
// other table member is carried as a same-width pointer/scalar purely to keep
// field offsets identical to the compiler's emission.

typedef uint32_t iree_hal_executable_library_version_t;
typedef uint32_t iree_hal_executable_library_features_t;
typedef uint32_t iree_hal_executable_library_sanitizer_kind_t;

// iree_hal_executable_library_header_t
typedef struct iree_hal_executable_library_header_t {
  iree_hal_executable_library_version_t version;
  const char* name;
  iree_hal_executable_library_features_t features;
  iree_hal_executable_library_sanitizer_kind_t sanitizer;
} iree_hal_executable_library_header_t;

// iree_hal_executable_import_table_v0_t
typedef struct iree_hal_executable_import_table_v0_t {
  uint32_t count;
  const char* const* symbols;
} iree_hal_executable_import_table_v0_t;

// quidditch_executable_export_table_v0_t (struct-of-arrays, fork layout).
// Only `count` + `compute_core_ptrs` are read; the rest preserve offsets.
typedef struct quidditch_executable_export_table_v0_t {
  uint32_t count;
  const iree_hal_executable_dispatch_v0_t* compute_core_ptrs;
  const void* attrs;
  const void* params;
  const void* occupancy;
  const char* const* names;
  const char* const* tags;
  const char* const* parameter_names;
  const void* source_locations;
  const void* stage_locations;
  const iree_hal_executable_dispatch_v0_t* dma_core_ptrs;
} quidditch_executable_export_table_v0_t;

// iree_hal_executable_constant_table_v0_t
typedef struct iree_hal_executable_constant_table_v0_t {
  uint32_t count;
} iree_hal_executable_constant_table_v0_t;

// iree_hal_executable_source_file_table_v0_t
typedef struct iree_hal_executable_source_file_table_v0_t {
  uint32_t count;
  const void* files;
} iree_hal_executable_source_file_table_v0_t;

// quidditch_executable_library_v0_t
typedef struct quidditch_executable_library_v0_t {
  const iree_hal_executable_library_header_t* header;
  iree_hal_executable_import_table_v0_t imports;
  quidditch_executable_export_table_v0_t exports;
  iree_hal_executable_constant_table_v0_t constants;
  iree_hal_executable_source_file_table_v0_t sources;
} quidditch_executable_library_v0_t;

// The library query function signature (matches IREE's
// iree_hal_executable_library_query_fn_t). Returns a pointer to the library
// header pointer (which is the first member of quidditch_executable_library_v0_t,
// so the returned pointer aliases the library struct).
typedef const iree_hal_executable_library_header_t** (
    *iree_hal_executable_library_query_fn_t)(
    iree_hal_executable_library_version_t max_version,
    const iree_hal_executable_environment_v0_t* environment);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QCS_REPLAY_KERNEL_ABI_H_
