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
#include "iree/hal/command_buffer.h"
#include "runtime/host/hal/cluster/cluster_allocator.h"
#include "runtime/host/hal/cluster/cluster_command_buffer.h"
#include "runtime/host/transport/cluster_command_stream.h"
#include "runtime/host/transport/shared_region.h"

#define CHECK_OK(expr)                                                 \
  do {                                                                 \
    iree_status_t _st = (expr);                                        \
    if (!iree_status_is_ok(_st)) {                                     \
      char _buf[512];                                                  \
      iree_host_size_t _n = 0;                                         \
      iree_status_format(_st, sizeof(_buf), _buf, &_n);                \
      fprintf(stderr, "FAIL: %s:%d: %s -> %.*s\n", __FILE__, __LINE__, \
              #expr, (int)_n, _buf);                                   \
      iree_status_free(_st);                                           \
      return 1;                                                        \
    }                                                                  \
  } while (0)

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__,   \
              #cond);                                            \
      return 1;                                                  \
    }                                                            \
  } while (0)

// libc malloc/free-backed allocator (host IREE was not built with
// IREE_ALLOCATOR_SYSTEM_CTL); same pattern as test_cluster_allocator.c.
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

// Allocate a device buffer and recover its device-PA independently.
static iree_status_t alloc_buf_and_pa(iree_hal_allocator_t* allocator,
                                      qcs_shared_region_t* region,
                                      iree_device_size_t size,
                                      iree_hal_buffer_t** out_buffer,
                                      uint64_t* out_pa) {
  iree_hal_buffer_params_t params = {0};
  params.type = IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
                IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  params.usage = IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
  params.access = IREE_HAL_MEMORY_ACCESS_ALL;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(allocator, params,
                                                          size, out_buffer));
  iree_hal_buffer_mapping_t mapping;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_range(
      *out_buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_READ, 0,
      IREE_HAL_WHOLE_BUFFER, &mapping));
  *out_pa = qcs_ptr_to_pa(region, mapping.contents.data);
  iree_hal_buffer_unmap_range(&mapping);
  return iree_ok_status();
}

int main(void) {
  const char* path = "/tmp/qcs_cluster_command_buffer_test.bin";
  const uint64_t kRegionSize = 1u << 20;  // 1 MiB

  qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, path, kRegionSize) != 0) {
    perror("qcs_shared_region_create");
    return 1;
  }

  iree_allocator_t host_allocator = test_libc_allocator();
  iree_hal_allocator_t* allocator = NULL;
  CHECK_OK(iree_hal_cluster_allocator_create(&region, host_allocator,
                                             &allocator));

  // Three device buffers.
  const iree_device_size_t kBuf0Size = 256;
  const iree_device_size_t kBuf1Size = 256;
  const iree_device_size_t kBuf2Size = 256;
  iree_hal_buffer_t* buf0 = NULL;
  iree_hal_buffer_t* buf1 = NULL;
  iree_hal_buffer_t* buf2 = NULL;
  uint64_t pa0 = 0, pa1 = 0, pa2 = 0;
  CHECK_OK(alloc_buf_and_pa(allocator, &region, kBuf0Size, &buf0, &pa0));
  CHECK_OK(alloc_buf_and_pa(allocator, &region, kBuf1Size, &buf1, &pa1));
  CHECK_OK(alloc_buf_and_pa(allocator, &region, kBuf2Size, &buf2, &pa2));

  fprintf(stderr, "pa0=%llu pa1=%llu pa2=%llu\n", (unsigned long long)pa0,
          (unsigned long long)pa1, (unsigned long long)pa2);

  // QCS stream scratch (8-byte aligned host buffer).
  const uint32_t kStreamCapacity = 4096;
  _Alignas(8) static uint8_t stream[4096];

  iree_hal_command_buffer_t* cb = NULL;
  CHECK_OK(iree_hal_cluster_command_buffer_create(
      &region, allocator, stream, kStreamCapacity,
      IREE_HAL_COMMAND_BUFFER_MODE_ONE_SHOT,
      IREE_HAL_COMMAND_CATEGORY_ANY, IREE_HAL_QUEUE_AFFINITY_ANY,
      /*binding_capacity=*/0, host_allocator, &cb));
  CHECK(cb != NULL);
  CHECK(iree_hal_cluster_command_buffer_isa(cb));

  // Record: fill(buf0), copy(buf0->buf1), update(inline->buf2),
  // dispatch(buf1, buf2 + 2 constants).
  const uint32_t kFillPattern = 0xDEADBEEFu;
  const iree_device_size_t kFillLength = 128;
  const iree_device_size_t kCopyLength = 64;
  const uint8_t kUpdateBytes[] = {0x11, 0x22, 0x33, 0x44,
                                  0x55, 0x66, 0x77, 0x88};
  const uint32_t kConstants[2] = {0xCAFEF00Du, 0x12345678u};

  CHECK_OK(iree_hal_command_buffer_begin(cb));

  // FILL on buf0.
  {
    iree_hal_buffer_ref_t target =
        iree_hal_make_buffer_ref(buf0, /*offset=*/0, kFillLength);
    CHECK_OK(iree_hal_command_buffer_fill_buffer(
        cb, target, &kFillPattern, sizeof(kFillPattern),
        IREE_HAL_FILL_FLAG_NONE));
  }

  // COPY buf0 -> buf1.
  {
    iree_hal_buffer_ref_t source =
        iree_hal_make_buffer_ref(buf0, /*offset=*/0, kCopyLength);
    iree_hal_buffer_ref_t target =
        iree_hal_make_buffer_ref(buf1, /*offset=*/0, kCopyLength);
    CHECK_OK(iree_hal_command_buffer_copy_buffer(cb, source, target,
                                                 IREE_HAL_COPY_FLAG_NONE));
  }

  // UPDATE inline bytes -> buf2.
  {
    iree_hal_buffer_ref_t target =
        iree_hal_make_buffer_ref(buf2, /*offset=*/0, sizeof(kUpdateBytes));
    CHECK_OK(iree_hal_command_buffer_update_buffer(
        cb, kUpdateBytes, /*source_offset=*/0, target,
        IREE_HAL_UPDATE_FLAG_NONE));
  }

  // DISPATCH with 2 bindings (buf1, buf2) + 2 constants.
  iree_hal_executable_t* fake_executable = (iree_hal_executable_t*)0x1000;
  const iree_device_size_t kBind1Len = 96;
  const iree_device_size_t kBind2Len = 128;
  {
    iree_hal_buffer_ref_t binding_values[2] = {
        iree_hal_make_buffer_ref(buf1, /*offset=*/0, kBind1Len),
        iree_hal_make_buffer_ref(buf2, /*offset=*/0, kBind2Len),
    };
    iree_hal_buffer_ref_list_t bindings = {2, binding_values};
    iree_hal_dispatch_config_t config =
        iree_hal_make_static_dispatch_config(4, 5, 6);
    config.workgroup_size[0] = 8;
    config.workgroup_size[1] = 1;
    config.workgroup_size[2] = 1;
    iree_const_byte_span_t constants = iree_make_const_byte_span(
        kConstants, sizeof(kConstants));
    CHECK_OK(iree_hal_command_buffer_dispatch(
        cb, fake_executable, /*export_ordinal=*/3, config, constants, bindings,
        IREE_HAL_DISPATCH_FLAG_NONE));
  }

  CHECK_OK(iree_hal_command_buffer_end(cb));

  // Reusable command buffers defer serialization to submission: resolve the
  // (here all-direct) refs against an empty binding table and emit the stream.
  CHECK_OK(iree_hal_cluster_command_buffer_emit(
      cb, iree_hal_buffer_binding_table_empty()));

  uint32_t stream_size = iree_hal_cluster_command_buffer_size(cb);
  uint32_t record_count = iree_hal_cluster_command_buffer_record_count(cb);
  fprintf(stderr, "stream_size=%u record_count=%u\n", stream_size,
          record_count);
  CHECK(record_count == 4);
  CHECK(stream_size > 0);
  CHECK(stream_size <= kStreamCapacity);

  // Parse and validate the emitted stream.
  qcs_reader_t reader;
  qcs_reader_init(&reader, stream, stream_size);

  // Record 0: FILL.
  {
    const qcs_record_header_t* hdr = qcs_reader_next(&reader);
    CHECK(hdr != NULL);
    CHECK(hdr->type == QCS_CMD_FILL);
    const qcs_fill_t* fill = (const qcs_fill_t*)hdr;
    CHECK(fill->dst_ptr == pa0);
    CHECK(fill->length == (uint64_t)kFillLength);
    CHECK(fill->pattern_length == sizeof(kFillPattern));
    CHECK((uint32_t)(fill->pattern & 0xFFFFFFFFu) == kFillPattern);
  }

  // Record 1: COPY.
  {
    const qcs_record_header_t* hdr = qcs_reader_next(&reader);
    CHECK(hdr != NULL);
    CHECK(hdr->type == QCS_CMD_COPY);
    const qcs_copy_t* copy = (const qcs_copy_t*)hdr;
    CHECK(copy->src_ptr == pa0);
    CHECK(copy->dst_ptr == pa1);
    CHECK(copy->length == (uint64_t)kCopyLength);
  }

  // Record 2: UPDATE.
  {
    const qcs_record_header_t* hdr = qcs_reader_next(&reader);
    CHECK(hdr != NULL);
    CHECK(hdr->type == QCS_CMD_UPDATE);
    const qcs_update_t* update = (const qcs_update_t*)hdr;
    CHECK(update->dst_ptr == pa2);
    CHECK(update->length == sizeof(kUpdateBytes));
    const uint8_t* data = (const uint8_t*)qcs_update_data(update);
    CHECK(memcmp(data, kUpdateBytes, sizeof(kUpdateBytes)) == 0);
  }

  // Record 3: DISPATCH.
  {
    const qcs_record_header_t* hdr = qcs_reader_next(&reader);
    CHECK(hdr != NULL);
    CHECK(hdr->type == QCS_CMD_DISPATCH);
    const qcs_dispatch_t* dispatch = (const qcs_dispatch_t*)hdr;
    CHECK(dispatch->executable_id == 0);  // first (only) executable
    CHECK(dispatch->export_ordinal == 3);
    CHECK((dispatch->flags & QCS_DISPATCH_FLAG_INDIRECT) == 0);
    CHECK(dispatch->workgroup_count[0] == 4);
    CHECK(dispatch->workgroup_count[1] == 5);
    CHECK(dispatch->workgroup_count[2] == 6);
    CHECK(dispatch->workgroup_size[0] == 8);
    CHECK(dispatch->constant_count == 2);
    CHECK(dispatch->binding_count == 2);

    const uint32_t* constants = qcs_dispatch_constants(dispatch);
    CHECK(constants[0] == kConstants[0]);
    CHECK(constants[1] == kConstants[1]);

    const qcs_binding_t* bindings = qcs_dispatch_bindings(dispatch);
    CHECK(bindings[0].device_ptr == pa1);
    CHECK(bindings[0].length == (uint64_t)kBind1Len);
    CHECK(bindings[1].device_ptr == pa2);
    CHECK(bindings[1].length == (uint64_t)kBind2Len);
  }

  // End of stream.
  CHECK(qcs_reader_next(&reader) == NULL);

  iree_hal_command_buffer_release(cb);
  iree_hal_buffer_release(buf0);
  iree_hal_buffer_release(buf1);
  iree_hal_buffer_release(buf2);
  iree_hal_allocator_release(allocator);
  qcs_shared_region_close(&region);

  printf("PASS\n");
  return 0;
}
