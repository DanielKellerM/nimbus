// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 capstone: a TWO-PROCESS GEMM that runs the host-device split end to
// end on the dev box (no RTL). This is the integration proof that ties the
// three already-built, tested pieces together across two address spaces:
//
//   HOST process (parent): the real IREE VM-style record + submit path on the
//     custom cluster HAL device. It allocates three device buffers A,B,C from
//     the shared arena via the device allocator, records ONE dispatch into a
//     QCS-emitting command buffer (whose stream lives in the arena), and submits
//     it through iree_hal_device_queue_execute, which publishes the QCS stream
//     into the job descriptor, rings the doorbell and spin-waits completion.
//
//   CLUSTER process (child): a separate process with its OWN mmap of the shared
//     region (different base VA -> real device-PA <-> VA translation). It
//     registers a host-C stub GEMM kernel and replays the QCS stream via
//     cluster_replay_stream, which invokes the stub over the workgroup grid
//     through the IREE executable-library ABI.
//
// We fork (like runtime/host/transport/test_shared_region.c) so the two
// processes have DIFFERENT base VAs. The parent creates + keeps the region; the
// child opens its OWN mapping (the inherited mapping is kept alive so the
// child's open() lands at a different base, genuinely exercising PA->VA
// translation).
//
// The host computes C = A @ B (row-major, int32) as the reference and asserts
// the cluster-produced C matches for all M*N elements.

#define _POSIX_C_SOURCE 200809L  // fork, waitpid, unlink (dev-box only)

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "iree/base/api.h"
#include "iree/base/allocator.h"
#include "iree/hal/allocator.h"
#include "iree/hal/buffer.h"
#include "iree/hal/command_buffer.h"
#include "iree/hal/device.h"
#include "iree/hal/local/executable_library.h"
#include "iree/hal/semaphore.h"
#include "runtime/host/firmware/cluster_replay.h"
#include "runtime/host/hal/cluster/cluster_device.h"
#include "runtime/host/transport/cluster_command_stream.h"
#include "runtime/host/transport/shared_region.h"

#define REGION_SIZE (1u << 20)  // 1 MiB device DRAM

// Region backing file; pid-stamped in main() so concurrent runs don't collide.
// Set before fork() so the child inherits the same path.
static char g_region_path[64];

// Small square GEMM: keeps the test about the transport/integration, not tiling.
#define M 8
#define N 8
#define K 8

//===----------------------------------------------------------------------===//
// libc malloc/free-backed allocator. The prebuilt host IREE libs in this
// environment were not configured with IREE_ALLOCATOR_SYSTEM_CTL, so we supply
// our own (copied from the sibling cluster tests).
//===----------------------------------------------------------------------===//
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

//===----------------------------------------------------------------------===//
// CLUSTER process: stub GEMM kernel + replay loop.
//===----------------------------------------------------------------------===//

// Stub GEMM kernel (matches iree_hal_executable_dispatch_v0_t). grid = 1x1x1:
// one invocation computes the whole MxN product. Reads:
//   state->constants = {M, N, K}
//   state->binding_ptrs[0] = A (MxK), [1] = B (KxN), [2] = C (MxN), all int32.
// Computes C = A @ B (row-major). Returns 0 on success, nonzero on bad args.
static int stub_gemm_kernel(
    const iree_hal_executable_environment_v0_t* env,
    const iree_hal_executable_dispatch_state_v0_t* state,
    const iree_hal_executable_workgroup_state_v0_t* wg) {
  (void)env;
  (void)wg;
  if (state->constant_count < 3) return 1;
  if (state->binding_count != 3) return 1;
  const uint32_t m = state->constants[0];
  const uint32_t n = state->constants[1];
  const uint32_t k = state->constants[2];
  const int32_t* a = (const int32_t*)state->binding_ptrs[0];
  const int32_t* b = (const int32_t*)state->binding_ptrs[1];
  int32_t* c = (int32_t*)state->binding_ptrs[2];
  for (uint32_t i = 0; i < m; ++i) {
    for (uint32_t j = 0; j < n; ++j) {
      int32_t acc = 0;
      for (uint32_t kk = 0; kk < k; ++kk) {
        acc += a[i * k + kk] * b[kk * n + j];
      }
      c[i * n + j] = acc;
    }
  }
  return 0;
}

static int cluster_main(void) {
  qcs_shared_region_t region;
  if (qcs_shared_region_open(&region, g_region_path) != 0) return 2;
  qcs_job_descriptor_t* job = qcs_shared_job(&region);

  // Register the stub GEMM under (executable_id=0, export_ordinal=0): the host
  // command buffer assigns id 0 to the first distinct executable pointer.
  cluster_replay_table_t table;
  cluster_replay_table_init(&table);
  if (cluster_replay_register(&table, 0, 0, stub_gemm_kernel) != 0) return 3;

  uint32_t id = qcs_doorbell_wait(job);
  int rc = cluster_replay_stream(&region, job, &table);
  qcs_doorbell_complete(job, id, rc == CLUSTER_REPLAY_OK ? 0 : -1);

  qcs_shared_region_close(&region);
  return rc == CLUSTER_REPLAY_OK ? 0 : 4;  // exit code reflects replay outcome
}

//===----------------------------------------------------------------------===//
// HOST process: record + submit via the cluster HAL device, then verify.
//===----------------------------------------------------------------------===//

// Allocate a device buffer (int32 elements) from the device allocator.
static iree_status_t alloc_i32_buffer(iree_hal_allocator_t* allocator,
                                      iree_host_size_t element_count,
                                      iree_hal_buffer_t** out_buffer) {
  iree_hal_buffer_params_t params = {0};
  params.type =
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
  params.access = IREE_HAL_MEMORY_ACCESS_ALL;
  return iree_hal_allocator_allocate_buffer(
      allocator, params, element_count * sizeof(int32_t), out_buffer);
}

// Seed/zero a buffer with caller-provided int32 values (NULL => zero-fill).
static iree_status_t fill_i32_buffer(iree_hal_buffer_t* buffer,
                                     const int32_t* values,
                                     iree_host_size_t count) {
  iree_hal_buffer_mapping_t m;
  IREE_RETURN_IF_ERROR(iree_hal_buffer_map_range(
      buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_DISCARD_WRITE,
      0, IREE_HAL_WHOLE_BUFFER, &m));
  int32_t* dst = (int32_t*)m.contents.data;
  if (values) {
    memcpy(dst, values, count * sizeof(int32_t));
  } else {
    memset(dst, 0, count * sizeof(int32_t));
  }
  iree_hal_buffer_unmap_range(&m);
  return iree_ok_status();
}

static int host_main(qcs_shared_region_t* region) {
  iree_allocator_t host_allocator = test_libc_allocator();

  iree_hal_device_t* device = NULL;
  iree_status_t status = iree_hal_cluster_device_create(
      IREE_SV("cluster"), region, host_allocator, &device);
  if (!iree_status_is_ok(status)) goto fail;

  iree_hal_allocator_t* allocator = iree_hal_device_allocator(device);
  assert(allocator != NULL);

  // Reference inputs: A (MxK), B (KxN), row-major int32.
  int32_t hostA[M * K];
  int32_t hostB[K * N];
  int32_t refC[M * N];
  for (int i = 0; i < M; ++i)
    // Asymmetric in (i,kk) so an A-transpose / index-swap bug can't false-PASS.
    for (int kk = 0; kk < K; ++kk) hostA[i * K + kk] = 2 * i + 3 * kk + 1;
  for (int kk = 0; kk < K; ++kk)
    for (int j = 0; j < N; ++j) hostB[kk * N + j] = (kk * 2) - j + 3;  // known
  // Host reference C = A @ B.
  for (int i = 0; i < M; ++i) {
    for (int j = 0; j < N; ++j) {
      int32_t acc = 0;
      for (int kk = 0; kk < K; ++kk) acc += hostA[i * K + kk] * hostB[kk * N + j];
      refC[i * N + j] = acc;
    }
  }

  // Allocate the three device buffers from the shared arena.
  iree_hal_buffer_t* bufA = NULL;
  iree_hal_buffer_t* bufB = NULL;
  iree_hal_buffer_t* bufC = NULL;
  if (!iree_status_is_ok(status = alloc_i32_buffer(allocator, M * K, &bufA)))
    goto fail_device;
  if (!iree_status_is_ok(status = alloc_i32_buffer(allocator, K * N, &bufB)))
    goto fail_device;
  if (!iree_status_is_ok(status = alloc_i32_buffer(allocator, M * N, &bufC)))
    goto fail_device;

  // Seed A, B with known values; zero C.
  if (!iree_status_is_ok(status = fill_i32_buffer(bufA, hostA, M * K)))
    goto fail_device;
  if (!iree_status_is_ok(status = fill_i32_buffer(bufB, hostB, K * N)))
    goto fail_device;
  if (!iree_status_is_ok(status = fill_i32_buffer(bufC, NULL, M * N)))
    goto fail_device;

  // Record one dispatch into a device command buffer (QCS stream in the arena).
  iree_hal_command_buffer_t* cb = NULL;
  if (!iree_status_is_ok(status = iree_hal_command_buffer_create(
            device, IREE_HAL_COMMAND_BUFFER_MODE_ONE_SHOT,
            IREE_HAL_COMMAND_CATEGORY_ANY, IREE_HAL_QUEUE_AFFINITY_ANY,
            /*binding_capacity=*/0, &cb)))
    goto fail_device;

  // A non-NULL placeholder executable pointer: the cluster command buffer only
  // uses it as a table key (first distinct pointer -> executable_id 0). The stub
  // is registered cluster-side under (id=0, ordinal=0).
  iree_hal_executable_t* placeholder_executable = (iree_hal_executable_t*)0x1;

  const uint32_t constants[3] = {(uint32_t)M, (uint32_t)N, (uint32_t)K};
  // grid 1x1x1: the stub computes the whole product in one invocation.
  iree_hal_dispatch_config_t config = {0};
  config.workgroup_count[0] = 1;
  config.workgroup_count[1] = 1;
  config.workgroup_count[2] = 1;

  iree_hal_buffer_ref_t binding_values[3] = {
      iree_hal_make_buffer_ref(bufA, 0, M * K * sizeof(int32_t)),
      iree_hal_make_buffer_ref(bufB, 0, K * N * sizeof(int32_t)),
      iree_hal_make_buffer_ref(bufC, 0, M * N * sizeof(int32_t)),
  };
  iree_hal_buffer_ref_list_t bindings = {
      .count = 3,
      .values = binding_values,
  };

  if (!iree_status_is_ok(status = iree_hal_command_buffer_begin(cb)))
    goto fail_cb;
  status = iree_hal_command_buffer_dispatch(
      cb, placeholder_executable, /*export_ordinal=*/0, config,
      iree_make_const_byte_span(constants, sizeof(constants)), bindings,
      IREE_HAL_DISPATCH_FLAG_NONE);
  if (!iree_status_is_ok(status)) goto fail_cb;
  if (!iree_status_is_ok(status = iree_hal_command_buffer_end(cb)))
    goto fail_cb;

  // Create a signal semaphore (provided by the device) and submit.
  iree_hal_semaphore_t* signal_semaphore = NULL;
  if (!iree_status_is_ok(status = iree_hal_semaphore_create(
            device, IREE_HAL_QUEUE_AFFINITY_ANY, /*initial_value=*/0ull,
            IREE_HAL_SEMAPHORE_FLAG_NONE, &signal_semaphore)))
    goto fail_cb;

  uint64_t signal_value = 1;
  iree_hal_semaphore_list_t signal_list = {
      .count = 1,
      .semaphores = &signal_semaphore,
      .payload_values = &signal_value,
  };

  // queue_execute publishes the QCS stream, rings the doorbell, spin-waits the
  // cluster's completion (synchronous), then signals the semaphore.
  status = iree_hal_device_queue_execute(
      device, IREE_HAL_QUEUE_AFFINITY_ANY, iree_hal_semaphore_list_empty(),
      signal_list, cb, iree_hal_buffer_binding_table_empty(),
      IREE_HAL_EXECUTE_FLAG_NONE);
  if (!iree_status_is_ok(status)) goto fail_sem;

  // Wait the semaphore (already satisfied: submission is synchronous).
  if (!iree_status_is_ok(status = iree_hal_semaphore_wait(
            signal_semaphore, signal_value, iree_infinite_timeout(),
            IREE_HAL_WAIT_FLAG_DEFAULT)))
    goto fail_sem;

  // Map C and verify C == A @ B across all M*N elements.
  int verify_failed = 0;
  {
    iree_hal_buffer_mapping_t m;
    status = iree_hal_buffer_map_range(bufC, IREE_HAL_MAPPING_MODE_SCOPED,
                                       IREE_HAL_MEMORY_ACCESS_READ, 0,
                                       IREE_HAL_WHOLE_BUFFER, &m);
    if (!iree_status_is_ok(status)) goto fail_sem;
    const int32_t* gotC = (const int32_t*)m.contents.data;

    printf("Recovered GEMM result C = A @ B (M=%d N=%d K=%d):\n", M, N, K);
    for (int i = 0; i < M; ++i) {
      printf("  ");
      for (int j = 0; j < N; ++j) {
        printf("%6d", gotC[i * N + j]);
        if (gotC[i * N + j] != refC[i * N + j]) verify_failed = 1;
      }
      printf("\n");
    }
    iree_hal_buffer_unmap_range(&m);
  }

  iree_hal_semaphore_release(signal_semaphore);
  iree_hal_command_buffer_release(cb);
  iree_hal_buffer_release(bufA);
  iree_hal_buffer_release(bufB);
  iree_hal_buffer_release(bufC);
  iree_hal_device_release(device);

  // Explicit check, not assert-only: asserts vanish under -DNDEBUG and the whole
  // test rests on this comparison.
  if (verify_failed) {
    fprintf(stderr, "FAIL: cluster-produced C != reference A@B\n");
    return 1;
  }
  printf(
      "OK: host recorded a QCS dispatch, doorbell -> separate cluster process "
      "replayed the stub GEMM across two address spaces; C == A@B verified for "
      "all %d elements\n",
      M * N);
  return 0;

fail_sem:
  iree_hal_semaphore_release(signal_semaphore);
fail_cb:
  iree_hal_command_buffer_release(cb);
fail_device:
  iree_hal_buffer_release(bufA);
  iree_hal_buffer_release(bufB);
  iree_hal_buffer_release(bufC);
  iree_hal_device_release(device);
fail: {
  char buf[512];
  iree_host_size_t n = 0;
  iree_status_format(status, sizeof(buf), buf, &n);
  fprintf(stderr, "FAIL: %.*s\n", (int)n, buf);
  iree_status_free(status);
  return 1;
}
}

int main(void) {
  // pid-stamp the region path (set before fork so the child inherits it) so
  // concurrent runs don't clobber each other's backing file.
  snprintf(g_region_path, sizeof(g_region_path), "/tmp/qcs_phase1_gemm_%d.bin",
           (int)getpid());

  // Create + size the region once before forking so the child can open it; the
  // parent keeps this mapping to drive the job and verify results.
  qcs_shared_region_t region;
  if (qcs_shared_region_create(&region, g_region_path, REGION_SIZE) != 0) {
    perror("qcs_shared_region_create");
    return 1;
  }

  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    // Child = cluster process. The inherited mapping is kept alive (we do not
    // close `region` before opening our own), so the child's own open() lands at
    // a DIFFERENT base VA -> genuine device-PA <-> VA translation.
    _exit(cluster_main());
  }

  int rc = host_main(&region);

  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  int child_ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;
  if (!child_ok) {
    fprintf(stderr, "FAIL: cluster process did not exit cleanly (status=%d)\n",
            wstatus);
    if (rc == 0) rc = 1;
  }

  qcs_shared_region_close(&region);
  unlink(g_region_path);

  if (rc == 0) printf("PASS\n");
  return rc;
}
