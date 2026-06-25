// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "runtime/host/hal/cluster/cluster_allocator.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "iree/hal/buffer.h"
#include "iree/hal/resource.h"

typedef struct iree_hal_cluster_allocator_t {
  iree_hal_resource_t resource;
  iree_allocator_t host_allocator;
  // Shared region the device buffers are carved out of (borrowed).
  qcs_shared_region_t* region;
  // Next free device-PA in the arena.
  uint64_t bump;
} iree_hal_cluster_allocator_t;

static const iree_hal_allocator_vtable_t iree_hal_cluster_allocator_vtable;

static iree_hal_cluster_allocator_t* iree_hal_cluster_allocator_cast(
    iree_hal_allocator_t* IREE_RESTRICT base_value) {
  return (iree_hal_cluster_allocator_t*)base_value;
}

iree_status_t iree_hal_cluster_allocator_create(
    qcs_shared_region_t* region, iree_allocator_t host_allocator,
    iree_hal_allocator_t** out_allocator) {
  IREE_ASSERT_ARGUMENT(region);
  IREE_ASSERT_ARGUMENT(out_allocator);
  *out_allocator = NULL;

  iree_hal_cluster_allocator_t* allocator = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*allocator),
                                             (void**)&allocator));
  iree_hal_resource_initialize(&iree_hal_cluster_allocator_vtable,
                               &allocator->resource);
  allocator->host_allocator = host_allocator;
  allocator->region = region;
  allocator->bump = QCS_SHARED_ARENA_OFFSET;

  *out_allocator = (iree_hal_allocator_t*)allocator;
  return iree_ok_status();
}

static void iree_hal_cluster_allocator_destroy(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  iree_hal_cluster_allocator_t* allocator =
      iree_hal_cluster_allocator_cast(base_allocator);
  iree_allocator_t host_allocator = allocator->host_allocator;
  iree_allocator_free(host_allocator, allocator);
}

static iree_allocator_t iree_hal_cluster_allocator_host_allocator(
    const iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  iree_hal_cluster_allocator_t* allocator =
      (iree_hal_cluster_allocator_t*)base_allocator;
  return allocator->host_allocator;
}

static iree_status_t iree_hal_cluster_allocator_trim(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator) {
  (void)base_allocator;
  return iree_ok_status();
}

static void iree_hal_cluster_allocator_query_statistics(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_allocator_statistics_t* IREE_RESTRICT out_statistics) {
  (void)base_allocator;
  memset(out_statistics, 0, sizeof(*out_statistics));
}

static iree_status_t iree_hal_cluster_allocator_query_memory_heaps(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_host_size_t capacity,
    iree_hal_allocator_memory_heap_t* IREE_RESTRICT heaps,
    iree_host_size_t* IREE_RESTRICT out_count) {
  (void)base_allocator;
  const iree_host_size_t count = 1;
  if (out_count) *out_count = count;
  if (capacity < count) {
    return iree_status_from_code(IREE_STATUS_OUT_OF_RANGE);
  }
  heaps[0] = (iree_hal_allocator_memory_heap_t){
      .type = IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL |
              IREE_HAL_MEMORY_TYPE_HOST_VISIBLE |
              IREE_HAL_MEMORY_TYPE_HOST_COHERENT,
      .allowed_usage = IREE_HAL_BUFFER_USAGE_TRANSFER |
                       IREE_HAL_BUFFER_USAGE_DISPATCH |
                       IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED |
                       IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT |
                       IREE_HAL_BUFFER_USAGE_MAPPING_ACCESS_RANDOM |
                       IREE_HAL_BUFFER_USAGE_MAPPING_ACCESS_SEQUENTIAL_WRITE,
      .max_allocation_size = ~(iree_device_size_t)0,
      .min_alignment = IREE_HAL_HEAP_BUFFER_ALIGNMENT,
  };
  return iree_ok_status();
}

static iree_hal_buffer_compatibility_t
iree_hal_cluster_allocator_query_buffer_compatibility(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_params_t* IREE_RESTRICT params,
    iree_device_size_t* IREE_RESTRICT allocation_size) {
  (void)base_allocator;
  (void)allocation_size;
  // All buffers are allocatable from the shared arena and are mappable.
  // Only ALLOCATABLE: import/export are stubbed (return UNAVAILABLE), so we must
  // not advertise IMPORTABLE/EXPORTABLE (the contract is "bit set => op works").
  iree_hal_buffer_compatibility_t compatibility =
      IREE_HAL_BUFFER_COMPATIBILITY_ALLOCATABLE;

  // Device-visible buffers can be used on the queue.
  if (iree_all_bits_set(params->type, IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE)) {
    if (iree_any_bit_set(params->usage, IREE_HAL_BUFFER_USAGE_TRANSFER)) {
      compatibility |= IREE_HAL_BUFFER_COMPATIBILITY_QUEUE_TRANSFER;
    }
    if (iree_any_bit_set(params->usage,
                         IREE_HAL_BUFFER_USAGE_DISPATCH_STORAGE)) {
      compatibility |= IREE_HAL_BUFFER_COMPATIBILITY_QUEUE_DISPATCH;
    }
  }

  // Mirror the heap allocator's coercion: always host-visible + mappable.
  params->type |= IREE_HAL_MEMORY_TYPE_HOST_VISIBLE;
  params->type &= ~IREE_HAL_MEMORY_TYPE_OPTIMAL;
  params->usage |= IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED |
                   IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT |
                   IREE_HAL_BUFFER_USAGE_MAPPING_ACCESS_RANDOM |
                   IREE_HAL_BUFFER_USAGE_TRANSFER;

  return compatibility;
}

static iree_status_t iree_hal_cluster_allocator_allocate_buffer(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    const iree_hal_buffer_params_t* IREE_RESTRICT params,
    iree_device_size_t allocation_size,
    iree_hal_buffer_t** IREE_RESTRICT out_buffer) {
  iree_hal_cluster_allocator_t* allocator =
      iree_hal_cluster_allocator_cast(base_allocator);

  // Coerce the params into something the shared arena can serve.
  iree_hal_buffer_params_t compat_params = *params;
  if (!iree_all_bits_set(
          iree_hal_cluster_allocator_query_buffer_compatibility(
              base_allocator, &compat_params, &allocation_size),
          IREE_HAL_BUFFER_COMPATIBILITY_ALLOCATABLE)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "allocator cannot allocate a buffer with the given parameters");
  }

  // Carve a device-PA out of the shared arena.
  uint64_t pa = qcs_shared_alloc(allocator->region, &allocator->bump,
                                 (uint64_t)allocation_size,
                                 IREE_HAL_HEAP_BUFFER_ALIGNMENT);
  if (pa == 0) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "shared arena out of space for %" PRIu64 " bytes",
                            (uint64_t)allocation_size);
  }

  void* ptr = qcs_pa_to_ptr(allocator->region, pa);

  // The bump allocator owns the memory; the wrapping buffer just borrows it.
  iree_hal_buffer_t* buffer = NULL;
  iree_status_t status = iree_hal_heap_buffer_wrap(
      iree_hal_buffer_placement_undefined(), compat_params.type,
      compat_params.access, compat_params.usage, allocation_size,
      iree_make_byte_span(ptr, (iree_host_size_t)allocation_size),
      iree_hal_buffer_release_callback_null(), allocator->host_allocator,
      &buffer);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  *out_buffer = buffer;
  return iree_ok_status();
}

static void iree_hal_cluster_allocator_deallocate_buffer(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT base_buffer) {
  (void)base_allocator;
  // Bump allocator: the arena owns the backing memory and the PA is not
  // reclaimed (Phase-1 jobs are short-lived); only free the buffer wrapper.
  iree_hal_buffer_destroy(base_buffer);
}

static iree_status_t iree_hal_cluster_allocator_import_buffer(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    const iree_hal_buffer_params_t* IREE_RESTRICT params,
    iree_hal_external_buffer_t* IREE_RESTRICT external_buffer,
    iree_hal_buffer_release_callback_t release_callback,
    iree_hal_buffer_t** IREE_RESTRICT out_buffer) {
  (void)params;
  (void)external_buffer;
  (void)release_callback;
  (void)out_buffer;
  (void)base_allocator;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "buffer import not supported by cluster allocator");
}

static iree_status_t iree_hal_cluster_allocator_export_buffer(
    iree_hal_allocator_t* IREE_RESTRICT base_allocator,
    iree_hal_buffer_t* IREE_RESTRICT buffer,
    iree_hal_external_buffer_type_t requested_type,
    iree_hal_external_buffer_flags_t requested_flags,
    iree_hal_external_buffer_t* IREE_RESTRICT out_external_buffer) {
  (void)base_allocator;
  (void)buffer;
  (void)requested_type;
  (void)requested_flags;
  (void)out_external_buffer;
  return iree_make_status(IREE_STATUS_UNAVAILABLE,
                          "buffer export not supported by cluster allocator");
}

static const iree_hal_allocator_vtable_t iree_hal_cluster_allocator_vtable = {
    .destroy = iree_hal_cluster_allocator_destroy,
    .host_allocator = iree_hal_cluster_allocator_host_allocator,
    .trim = iree_hal_cluster_allocator_trim,
    .query_statistics = iree_hal_cluster_allocator_query_statistics,
    .query_memory_heaps = iree_hal_cluster_allocator_query_memory_heaps,
    .query_buffer_compatibility =
        iree_hal_cluster_allocator_query_buffer_compatibility,
    .allocate_buffer = iree_hal_cluster_allocator_allocate_buffer,
    .deallocate_buffer = iree_hal_cluster_allocator_deallocate_buffer,
    .import_buffer = iree_hal_cluster_allocator_import_buffer,
    .export_buffer = iree_hal_cluster_allocator_export_buffer,
    // virtual_memory_* operations intentionally left NULL.
};
