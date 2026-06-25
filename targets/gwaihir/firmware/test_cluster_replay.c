// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 slice-3 proof (no IREE link, no RTL): the host serializes a job into
// the shared region; the cluster-side replayer parses and replays it, invoking
// a host-C STUB kernel through the IREE executable-library ABI for DISPATCH and
// performing COPY/FILL in place. Also exercises the adversarial path: an
// out-of-region device-PA must be rejected (error, no segfault) under ASan.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cluster_replay.h"

#define N 64

//===----------------------------------------------------------------------===//
// Stub kernel: C[wg.x] = A[wg.x] + B[wg.x], one element per workgroup.
// bindings[0]=A (in), bindings[1]=B (in), bindings[2]=C (out).
//===----------------------------------------------------------------------===//
static int stub_add_kernel(
    const iree_hal_executable_environment_v0_t* env,
    const iree_hal_executable_dispatch_state_v0_t* state,
    const iree_hal_executable_workgroup_state_v0_t* wg) {
  (void)env;
  if (state->binding_count != 3) return 1;
  const int32_t* a = (const int32_t*)state->binding_ptrs[0];
  const int32_t* b = (const int32_t*)state->binding_ptrs[1];
  int32_t* c = (int32_t*)state->binding_ptrs[2];
  uint32_t i = wg->workgroup_id_x;
  if (i >= N) return 1;
  c[i] = a[i] + b[i];
  return 0;
}

int main(void) {
  const char* path = "/tmp/qcs_replay_region.bin";
  qcs_shared_region_t region;
  // 64 KiB region: descriptor page + command stream + device buffers.
  if (qcs_shared_region_create(&region, path, 64u * 1024u) != 0) {
    fprintf(stderr, "FAIL: could not create shared region\n");
    return 1;
  }

  uint64_t bump = QCS_SHARED_ARENA_OFFSET;

  // Bump-allocate the command stream buffer and three device buffers A,B,C.
  uint64_t stream_pa = qcs_shared_alloc(&region, &bump, 4096, 8);
  uint64_t a_pa = qcs_shared_alloc(&region, &bump, N * sizeof(int32_t), 64);
  uint64_t b_pa = qcs_shared_alloc(&region, &bump, N * sizeof(int32_t), 64);
  uint64_t c_pa = qcs_shared_alloc(&region, &bump, N * sizeof(int32_t), 64);
  uint64_t fill_pa = qcs_shared_alloc(&region, &bump, N * sizeof(int32_t), 64);
  uint64_t copy_pa = qcs_shared_alloc(&region, &bump, N * sizeof(int32_t), 64);
  assert(stream_pa && a_pa && b_pa && c_pa && fill_pa && copy_pa);

  // Seed A and B with known values (write through the cluster VA).
  int32_t* A = (int32_t*)qcs_pa_to_ptr(&region, a_pa);
  int32_t* B = (int32_t*)qcs_pa_to_ptr(&region, b_pa);
  int32_t* C = (int32_t*)qcs_pa_to_ptr(&region, c_pa);
  for (int i = 0; i < N; ++i) {
    A[i] = i;
    B[i] = 100 + 2 * i;
    C[i] = -1;
  }

  //===--------------------------------------------------------------------===//
  // HOST: record a job: DISPATCH(add) + COPY + FILL.
  //===--------------------------------------------------------------------===//
  qcs_writer_t w;
  qcs_writer_init(&w, qcs_pa_to_ptr(&region, stream_pa), 4096);

  const uint32_t wgc[3] = {N, 1, 1};
  const uint32_t wgs[3] = {1, 1, 1};
  const qcs_binding_t binds[3] = {
      {a_pa, N * sizeof(int32_t)},
      {b_pa, N * sizeof(int32_t)},
      {c_pa, N * sizeof(int32_t)},
  };
  assert(qcs_write_dispatch(&w, /*exec=*/0, /*ord=*/0, wgc, wgs,
                            /*dyn_l1=*/0, /*constant_count=*/0, NULL,
                            /*binding_count=*/3, binds) == 0);

  // COPY: copy A -> copy_pa.
  assert(qcs_write_copy(&w, a_pa, copy_pa, N * sizeof(int32_t)) == 0);

  // FILL: fill fill_pa with a 4-byte repeating pattern.
  const uint32_t fill_pattern = 0xABCD1234u;
  assert(qcs_write_fill(&w, fill_pa, N * sizeof(int32_t), fill_pattern, 4) == 0);

  // Fill in the job descriptor at device-PA 0.
  qcs_job_descriptor_t* job = qcs_shared_job(&region);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->record_count = w.record_count;
  job->cmd_stream_ptr = stream_pa;
  job->cmd_stream_len = w.size;

  //===--------------------------------------------------------------------===//
  // CLUSTER: register the stub kernel and replay.
  //===--------------------------------------------------------------------===//
  cluster_replay_table_t table;
  cluster_replay_table_init(&table);
  assert(cluster_replay_register(&table, 0, 0, stub_add_kernel) == 0);

  int rc = cluster_replay_stream(&region, job, &table);
  if (rc != CLUSTER_REPLAY_OK) {
    fprintf(stderr, "FAIL: replay returned %d\n", rc);
    return 1;
  }

  // Verify DISPATCH: C[i] == A[i] + B[i].
  for (int i = 0; i < N; ++i) {
    if (C[i] != A[i] + B[i]) {
      fprintf(stderr, "FAIL: C[%d]=%d expected %d\n", i, C[i], A[i] + B[i]);
      return 1;
    }
  }

  // Verify COPY: copy_pa == A.
  int32_t* CP = (int32_t*)qcs_pa_to_ptr(&region, copy_pa);
  for (int i = 0; i < N; ++i) {
    if (CP[i] != A[i]) {
      fprintf(stderr, "FAIL: copy[%d]=%d expected %d\n", i, CP[i], A[i]);
      return 1;
    }
  }

  // Verify FILL: every 4-byte word == fill_pattern.
  uint32_t* FL = (uint32_t*)qcs_pa_to_ptr(&region, fill_pa);
  for (int i = 0; i < N; ++i) {
    if (FL[i] != fill_pattern) {
      fprintf(stderr, "FAIL: fill[%d]=0x%x expected 0x%x\n", i, FL[i],
              fill_pattern);
      return 1;
    }
  }
  printf("OK: DISPATCH + COPY + FILL replayed correctly\n");

  //===--------------------------------------------------------------------===//
  // ADVERSARIAL: a COPY whose dst device-PA is out of region must be rejected
  // (error, no segfault under ASan).
  //===--------------------------------------------------------------------===//
  {
    qcs_writer_t aw;
    qcs_writer_init(&aw, qcs_pa_to_ptr(&region, stream_pa), 4096);
    // dst far past the end of the region.
    uint64_t bad_dst = region.size + 0x1000ull;
    assert(qcs_write_copy(&aw, a_pa, bad_dst, N * sizeof(int32_t)) == 0);
    job->record_count = aw.record_count;
    job->cmd_stream_len = aw.size;
    int arc = cluster_replay_stream(&region, job, &table);
    if (arc != CLUSTER_REPLAY_ERR_BOUNDS) {
      fprintf(stderr, "FAIL: bad COPY not rejected (rc=%d)\n", arc);
      return 1;
    }
    printf("OK: out-of-region COPY rejected (rc=%d)\n", arc);
  }

  //===--------------------------------------------------------------------===//
  // ADVERSARIAL: a DISPATCH with an out-of-region binding must be rejected.
  //===--------------------------------------------------------------------===//
  {
    qcs_writer_t aw;
    qcs_writer_init(&aw, qcs_pa_to_ptr(&region, stream_pa), 4096);
    const qcs_binding_t bad_binds[3] = {
        {a_pa, N * sizeof(int32_t)},
        {b_pa, N * sizeof(int32_t)},
        {region.size - 16, N * sizeof(int32_t)},  // length escapes the region
    };
    assert(qcs_write_dispatch(&aw, 0, 0, wgc, wgs, 0, 0, NULL, 3,
                              bad_binds) == 0);
    job->record_count = aw.record_count;
    job->cmd_stream_len = aw.size;
    int arc = cluster_replay_stream(&region, job, &table);
    if (arc != CLUSTER_REPLAY_ERR_BOUNDS) {
      fprintf(stderr, "FAIL: bad DISPATCH binding not rejected (rc=%d)\n", arc);
      return 1;
    }
    printf("OK: out-of-region DISPATCH binding rejected (rc=%d)\n", arc);
  }

  //===--------------------------------------------------------------------===//
  // ADVERSARIAL: untrusted geometry. A grid past the cap and a z-dim past the
  // ABI's uint16 limit must be rejected, not run / silently truncated.
  //===--------------------------------------------------------------------===//
  {
    qcs_writer_t aw;
    qcs_writer_init(&aw, qcs_pa_to_ptr(&region, stream_pa), 4096);
    const uint32_t huge_wgc[3] = {1u << 25, 1, 1};  // > CLUSTER_REPLAY_MAX_WORKGROUPS
    assert(qcs_write_dispatch(&aw, 0, 0, huge_wgc, wgs, 0, 0, NULL, 0, NULL) ==
           0);
    job->record_count = aw.record_count;
    job->cmd_stream_len = aw.size;
    int arc = cluster_replay_stream(&region, job, &table);
    if (arc != CLUSTER_REPLAY_ERR_BAD_GRID) {
      fprintf(stderr, "FAIL: oversized grid not rejected (rc=%d)\n", arc);
      return 1;
    }
    printf("OK: oversized workgroup grid rejected (rc=%d)\n", arc);

    qcs_writer_init(&aw, qcs_pa_to_ptr(&region, stream_pa), 4096);
    const uint32_t deep_wgc[3] = {1, 1, 70000};  // z > UINT16_MAX
    assert(qcs_write_dispatch(&aw, 0, 0, deep_wgc, wgs, 0, 0, NULL, 0, NULL) ==
           0);
    job->record_count = aw.record_count;
    job->cmd_stream_len = aw.size;
    arc = cluster_replay_stream(&region, job, &table);
    if (arc != CLUSTER_REPLAY_ERR_BAD_DISPATCH) {
      fprintf(stderr, "FAIL: oversized z-dim not rejected (rc=%d)\n", arc);
      return 1;
    }
    printf("OK: oversized z-dim rejected (rc=%d)\n", arc);
  }

  qcs_shared_region_close(&region);
  remove(path);
  printf("PASS\n");
  return 0;
}
