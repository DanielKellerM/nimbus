// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Slice-2c test: drive the cluster HAL device end to end.
//   - create a shared region + cluster device
//   - allocate two device buffers via the device's allocator
//   - record FILL(buf0) + COPY(buf0 -> buf1) into a device command buffer
//   - spawn a stub "cluster" thread that waits on the doorbell, replays the
//     QCS stream against the shared buffers, and signals completion
//   - submit via iree_hal_device_queue_execute (real signal semaphore) and
//     verify buf1 holds buf0's filled pattern after completion.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/base/allocator.h"
#include "iree/hal/allocator.h"
#include "iree/hal/buffer.h"
#include "iree/hal/command_buffer.h"
#include "iree/hal/device.h"
#include "iree/hal/semaphore.h"
#include "runtime/host/hal/cluster/cluster_device.h"
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

#define CHECK(cond)                                            \
  do {                                                         \
    if (!(cond)) {                                             \
      fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, \
              #cond);                                          \
      return 1;                                                \
    }                                                          \
  } while (0)

// libc malloc/free-backed allocator (host IREE was not built with
// IREE_ALLOCATOR_SYSTEM_CTL); same pattern as the sibling tests.
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
  params.type =
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
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

//===----------------------------------------------------------------------===//
// Stub cluster thread
//===----------------------------------------------------------------------===//

typedef struct stub_cluster_args_t {
  qcs_shared_region_t* region;
  int32_t replay_status;  // status the parser ultimately reports
} stub_cluster_args_t;

// Replays a FILL/COPY-only QCS stream against the shared buffers and returns 0
// on success or -1 if anything is malformed.
static int32_t stub_replay_stream(qcs_shared_region_t* region,
                                  const qcs_job_descriptor_t* job) {
  if (job->magic != QCS_MAGIC || job->version != QCS_VERSION) return -1;
  qcs_reader_t reader;
  qcs_reader_init(&reader, qcs_pa_to_ptr(region, job->cmd_stream_ptr),
                  (uint32_t)job->cmd_stream_len);
  const qcs_record_header_t* hdr = NULL;
  uint32_t seen = 0;
  while ((hdr = qcs_reader_next(&reader)) != NULL) {
    switch (hdr->type) {
      case QCS_CMD_FILL: {
        const qcs_fill_t* fill = (const qcs_fill_t*)hdr;
        uint8_t* dst = (uint8_t*)qcs_pa_to_ptr(region, fill->dst_ptr);
        if (fill->pattern_length == 0 ||
            (fill->length % fill->pattern_length) != 0) {
          return -1;
        }
        uint8_t unit[8];
        memcpy(unit, &fill->pattern, fill->pattern_length);
        for (uint64_t i = 0; i < fill->length; ++i) {
          dst[i] = unit[i % fill->pattern_length];
        }
        break;
      }
      case QCS_CMD_COPY: {
        const qcs_copy_t* copy = (const qcs_copy_t*)hdr;
        uint8_t* dst = (uint8_t*)qcs_pa_to_ptr(region, copy->dst_ptr);
        const uint8_t* src =
            (const uint8_t*)qcs_pa_to_ptr(region, copy->src_ptr);
        memmove(dst, src, copy->length);
        break;
      }
      default:
        return -1;  // this test only emits FILL + COPY.
    }
    ++seen;
  }
  if (seen != job->record_count) return -1;
  return 0;
}

static void* stub_cluster_main(void* arg) {
  stub_cluster_args_t* args = (stub_cluster_args_t*)arg;
  qcs_job_descriptor_t* job = qcs_shared_job(args->region);
  uint32_t job_id = qcs_doorbell_wait(job);
  int32_t status = stub_replay_stream(args->region, job);
  args->replay_status = status;
  qcs_doorbell_complete(job, job_id, status);
  return NULL;
}

int main(void) {
  const char* path = "/tmp/qcs_cluster_device_test.bin";
  const uint64_t kRegionSize = 1u << 20;  // 1 MiB

  qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, path, kRegionSize) != 0) {
    perror("qcs_shared_region_create");
    return 1;
  }

  iree_allocator_t host_allocator = test_libc_allocator();

  iree_hal_device_t* device = NULL;
  CHECK_OK(iree_hal_cluster_device_create(IREE_SV("cluster"), &region,
                                          host_allocator, &device));
  CHECK(device != NULL);

  iree_hal_allocator_t* allocator = iree_hal_device_allocator(device);
  CHECK(allocator != NULL);

  // Two device buffers.
  const iree_device_size_t kBufSize = 256;
  iree_hal_buffer_t* buf0 = NULL;
  iree_hal_buffer_t* buf1 = NULL;
  uint64_t pa0 = 0, pa1 = 0;
  CHECK_OK(alloc_buf_and_pa(allocator, &region, kBufSize, &buf0, &pa0));
  CHECK_OK(alloc_buf_and_pa(allocator, &region, kBufSize, &buf1, &pa1));
  fprintf(stderr, "pa0=%llu pa1=%llu\n", (unsigned long long)pa0,
          (unsigned long long)pa1);

  // Pre-poison buf1 so a missing copy would be detected.
  {
    iree_hal_buffer_mapping_t m;
    CHECK_OK(iree_hal_buffer_map_range(buf1, IREE_HAL_MAPPING_MODE_SCOPED,
                                       IREE_HAL_MEMORY_ACCESS_WRITE, 0,
                                       IREE_HAL_WHOLE_BUFFER, &m));
    memset(m.contents.data, 0xAB, kBufSize);
    iree_hal_buffer_unmap_range(&m);
  }

  // Record a command buffer via the device (its QCS stream comes from the
  // shared arena, so cmd_stream_ptr will be a real device-PA).
  iree_hal_command_buffer_t* cb = NULL;
  CHECK_OK(iree_hal_command_buffer_create(
      device, IREE_HAL_COMMAND_BUFFER_MODE_ONE_SHOT,
      IREE_HAL_COMMAND_CATEGORY_ANY, IREE_HAL_QUEUE_AFFINITY_ANY,
      /*binding_capacity=*/0, &cb));
  CHECK(cb != NULL);

  const uint32_t kFillPattern = 0xDEADBEEFu;
  const iree_device_size_t kFillLength = 128;
  const iree_device_size_t kCopyLength = 128;

  CHECK_OK(iree_hal_command_buffer_begin(cb));
  {
    iree_hal_buffer_ref_t target =
        iree_hal_make_buffer_ref(buf0, /*offset=*/0, kFillLength);
    CHECK_OK(iree_hal_command_buffer_fill_buffer(
        cb, target, &kFillPattern, sizeof(kFillPattern),
        IREE_HAL_FILL_FLAG_NONE));
  }
  {
    iree_hal_buffer_ref_t source =
        iree_hal_make_buffer_ref(buf0, /*offset=*/0, kCopyLength);
    iree_hal_buffer_ref_t target =
        iree_hal_make_buffer_ref(buf1, /*offset=*/0, kCopyLength);
    CHECK_OK(iree_hal_command_buffer_copy_buffer(cb, source, target,
                                                 IREE_HAL_COPY_FLAG_NONE));
  }
  CHECK_OK(iree_hal_command_buffer_end(cb));

  // Spawn the stub cluster thread (waits on the doorbell).
  stub_cluster_args_t cluster_args = {.region = &region, .replay_status = 0};
  pthread_t cluster_thread;
  if (pthread_create(&cluster_thread, NULL, stub_cluster_main, &cluster_args) !=
      0) {
    fprintf(stderr, "FAIL: pthread_create\n");
    return 1;
  }

  // Submit via the real queue_execute path with a signal semaphore.
  iree_hal_semaphore_t* signal_semaphore = NULL;
  CHECK_OK(iree_hal_semaphore_create(
      device, IREE_HAL_QUEUE_AFFINITY_ANY, /*initial_value=*/0ull,
      IREE_HAL_SEMAPHORE_FLAG_NONE, &signal_semaphore));

  uint64_t signal_value = 1;
  iree_hal_semaphore_list_t signal_list = {
      .count = 1,
      .semaphores = &signal_semaphore,
      .payload_values = &signal_value,
  };
  CHECK_OK(iree_hal_device_queue_execute(
      device, IREE_HAL_QUEUE_AFFINITY_ANY, iree_hal_semaphore_list_empty(),
      signal_list, cb, iree_hal_buffer_binding_table_empty(),
      IREE_HAL_EXECUTE_FLAG_NONE));

  // The cluster completed before queue_execute returned (synchronous), so the
  // signal semaphore must already be at the requested value.
  uint64_t observed = 0;
  CHECK_OK(iree_hal_semaphore_query(signal_semaphore, &observed));
  CHECK(observed == signal_value);

  // Join the cluster thread and check its replay succeeded.
  pthread_join(cluster_thread, NULL);
  CHECK(cluster_args.replay_status == 0);

  // Verify buf1 now holds buf0's filled pattern.
  {
    iree_hal_buffer_mapping_t m;
    CHECK_OK(iree_hal_buffer_map_range(buf1, IREE_HAL_MAPPING_MODE_SCOPED,
                                       IREE_HAL_MEMORY_ACCESS_READ, 0,
                                       IREE_HAL_WHOLE_BUFFER, &m));
    const uint8_t* data = m.contents.data;
    for (iree_device_size_t i = 0; i < kCopyLength; ++i) {
      uint8_t expected = ((const uint8_t*)&kFillPattern)[i % 4];
      if (data[i] != expected) {
        fprintf(stderr, "FAIL: buf1[%llu]=0x%02x expected 0x%02x\n",
                (unsigned long long)i, data[i], expected);
        iree_hal_buffer_unmap_range(&m);
        return 1;
      }
    }
    iree_hal_buffer_unmap_range(&m);
  }

  iree_hal_semaphore_release(signal_semaphore);
  iree_hal_command_buffer_release(cb);
  iree_hal_buffer_release(buf0);
  iree_hal_buffer_release(buf1);
  iree_hal_device_release(device);
  qcs_shared_region_close(&region);

  printf("PASS\n");
  return 0;
}
