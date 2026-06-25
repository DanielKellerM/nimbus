// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/base/allocator.h"
#include "iree/hal/allocator.h"
#include "iree/hal/buffer.h"
#include "runtime/host/hal/cluster/cluster_allocator.h"
#include "runtime/host/transport/shared_region.h"

#define CHECK_OK(expr)                                                     \
  do {                                                                     \
    iree_status_t _st = (expr);                                            \
    if (!iree_status_is_ok(_st)) {                                         \
      char _buf[512];                                                      \
      iree_host_size_t _n = 0;                                             \
      iree_status_format(_st, sizeof(_buf), _buf, &_n);                    \
      fprintf(stderr, "FAIL: %s:%d: %s -> %.*s\n", __FILE__, __LINE__,     \
              #expr, (int)_n, _buf);                                       \
      iree_status_free(_st);                                               \
      return 1;                                                            \
    }                                                                      \
  } while (0)

// A minimal libc malloc/free-backed allocator. The prebuilt host IREE libs in
// this environment were not configured with IREE_ALLOCATOR_SYSTEM_CTL, so we
// supply our own rather than rely on iree_allocator_system().
static iree_status_t test_libc_ctl(void* self, iree_allocator_command_t command,
                                   const void* params, void** inout_ptr) {
  (void)self;
  switch (command) {
    case IREE_ALLOCATOR_COMMAND_MALLOC:
    case IREE_ALLOCATOR_COMMAND_CALLOC: {
      const iree_allocator_alloc_params_t* p =
          (const iree_allocator_alloc_params_t*)params;
      void* ptr = (command == IREE_ALLOCATOR_COMMAND_CALLOC)
                      ? calloc(1, p->byte_length)
                      : malloc(p->byte_length);
      if (!ptr) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "libc allocation failed");
      }
      *inout_ptr = ptr;
      return iree_ok_status();
    }
    case IREE_ALLOCATOR_COMMAND_REALLOC: {
      const iree_allocator_alloc_params_t* p =
          (const iree_allocator_alloc_params_t*)params;
      void* ptr = realloc(*inout_ptr, p->byte_length);
      if (!ptr) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "libc reallocation failed");
      }
      *inout_ptr = ptr;
      return iree_ok_status();
    }
    case IREE_ALLOCATOR_COMMAND_FREE:
      free(*inout_ptr);
      return iree_ok_status();
    default:
      return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                              "unsupported allocator command");
  }
}

static iree_allocator_t test_libc_allocator(void) {
  iree_allocator_t v = {NULL, test_libc_ctl};
  return v;
}

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__,       \
              #cond);                                                \
      return 1;                                                      \
    }                                                                \
  } while (0)

int main(void) {
  const char* path = "/tmp/qcs_cluster_allocator_test.bin";
  const uint64_t kRegionSize = 1u << 20;  // 1 MiB
  const iree_device_size_t kBufSize = 256;

  qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, path, kRegionSize) != 0) {
    perror("qcs_shared_region_create");
    return 1;
  }

  iree_allocator_t host_allocator = test_libc_allocator();
  iree_hal_allocator_t* allocator = NULL;
  CHECK_OK(iree_hal_cluster_allocator_create(&region, host_allocator,
                                             &allocator));

  iree_hal_buffer_params_t params = {0};
  params.type = IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
                IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
  params.access = IREE_HAL_MEMORY_ACCESS_ALL;

  iree_hal_buffer_t* buffer = NULL;
  CHECK_OK(iree_hal_allocator_allocate_buffer(allocator, params, kBufSize,
                                              &buffer));
  CHECK(buffer != NULL);

  // Write a known pattern.
  const uint8_t kPattern = 0xA5;
  {
    iree_hal_buffer_mapping_t mapping;
    CHECK_OK(iree_hal_buffer_map_range(
        buffer, IREE_HAL_MAPPING_MODE_SCOPED,
        IREE_HAL_MEMORY_ACCESS_DISCARD_WRITE, 0, IREE_HAL_WHOLE_BUFFER,
        &mapping));
    CHECK(mapping.contents.data_length >= (iree_host_size_t)kBufSize);
    memset(mapping.contents.data, kPattern, (size_t)kBufSize);
    CHECK_OK(iree_hal_buffer_unmap_range(&mapping));
  }

  // Read it back, verify, and recover the device-PA.
  {
    iree_hal_buffer_mapping_t mapping;
    CHECK_OK(iree_hal_buffer_map_range(buffer, IREE_HAL_MAPPING_MODE_SCOPED,
                                       IREE_HAL_MEMORY_ACCESS_READ, 0,
                                       IREE_HAL_WHOLE_BUFFER, &mapping));
    const uint8_t* data = mapping.contents.data;
    for (iree_device_size_t i = 0; i < kBufSize; ++i) {
      CHECK(data[i] == kPattern);
    }

    // Recover the device-PA = mapped_ptr - region.base.
    uint64_t pa = qcs_ptr_to_pa(&region, mapping.contents.data);
    fprintf(stderr, "recovered device-PA = %llu (arena_offset=%u, size=%llu)\n",
            (unsigned long long)pa, (unsigned)QCS_SHARED_ARENA_OFFSET,
            (unsigned long long)region.size);
    CHECK(pa >= QCS_SHARED_ARENA_OFFSET);
    CHECK(pa < region.size);
    // Round-trip the PA -> ptr.
    CHECK(qcs_pa_to_ptr(&region, pa) == (void*)mapping.contents.data);

    CHECK_OK(iree_hal_buffer_unmap_range(&mapping));
  }

  iree_hal_buffer_release(buffer);
  iree_hal_allocator_release(allocator);
  qcs_shared_region_close(&region);

  printf("PASS\n");
  return 0;
}
