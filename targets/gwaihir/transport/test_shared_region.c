// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 proof (no IREE, no RTL): two processes share an mmap'd "device DRAM".
// The host records a QCS stream + device buffers, rings the doorbell; a separate
// cluster process (its OWN mmap, so a different base VA) waits on the doorbell,
// replays the stream against shared device buffers via device-PA <-> VA
// translation, and signals completion. The host then verifies the buffers.
// Proves the shared-region transport + doorbell handshake + PA translation --
// the integration the two-process split rests on -- on the dev box.

#define _POSIX_C_SOURCE 200809L  // fork, waitpid, unlink (dev-box only)

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cluster_command_stream.h"
#include "shared_region.h"

#define REGION_PATH "/tmp/qcs_phase1_shared_region.bin"
#define REGION_SIZE (1u << 20)  // 1 MiB device DRAM
#define BUF_LEN 256u

// Apply one fill: replicate the low pattern_length bytes of `pattern` over the
// buffer (matches what the cluster firmware will do device-side).
static void apply_fill(uint8_t* dst, uint64_t length, uint64_t pattern,
                       uint32_t pattern_length) {
  uint8_t unit[8];
  for (uint32_t i = 0; i < pattern_length; ++i)
    unit[i] = (uint8_t)(pattern >> (8u * i));
  for (uint64_t i = 0; i < length; ++i) dst[i] = unit[i % pattern_length];
}

// ---- CLUSTER process: open the region, replay the job, signal completion ----
static int cluster_main(void) {
  qcs_shared_region_t region;
  if (qcs_shared_region_open(&region, REGION_PATH) != 0) return 2;
  qcs_job_descriptor_t* job = qcs_shared_job(&region);

  uint32_t job_id = qcs_doorbell_wait(job);
  if (job->magic != QCS_MAGIC || job->version != QCS_VERSION) {
    qcs_doorbell_complete(job, job_id, -1);
    qcs_shared_region_close(&region);
    return 0;
  }

  qcs_reader_t r;
  qcs_reader_init(&r, qcs_pa_to_ptr(&region, job->cmd_stream_ptr),
                  (uint32_t)job->cmd_stream_len);
  int32_t status = 0;
  uint32_t seen = 0;
  const qcs_record_header_t* h;
  while ((h = qcs_reader_next(&r)) != NULL) {
    ++seen;
    switch (h->type) {
      case QCS_CMD_COPY: {
        const qcs_copy_t* c = (const qcs_copy_t*)h;
        memcpy(qcs_pa_to_ptr(&region, c->dst_ptr),
               qcs_pa_to_ptr(&region, c->src_ptr), (size_t)c->length);
        break;
      }
      case QCS_CMD_FILL: {
        const qcs_fill_t* f = (const qcs_fill_t*)h;
        apply_fill(qcs_pa_to_ptr(&region, f->dst_ptr), f->length, f->pattern,
                   f->pattern_length);
        break;
      }
      case QCS_CMD_UPDATE: {
        const qcs_update_t* u = (const qcs_update_t*)h;
        memcpy(qcs_pa_to_ptr(&region, u->dst_ptr), qcs_update_data(u),
               (size_t)u->length);
        break;
      }
      case QCS_CMD_DISPATCH:
        // Phase-1 slice: no kernels yet; just count it.
        break;
      default:
        status = -2;
        break;
    }
  }
  if (seen != job->record_count) status = -3;

  qcs_doorbell_complete(job, job_id, status);
  qcs_shared_region_close(&region);
  return 0;
}

// ---- HOST process: lay out device buffers + a QCS stream, submit, verify ----
static int host_main(qcs_shared_region_t* region) {
  uint64_t bump = QCS_SHARED_ARENA_OFFSET;
  uint64_t stream_pa = qcs_shared_alloc(region, &bump, 1024, QCS_RECORD_ALIGN);
  uint64_t src_pa = qcs_shared_alloc(region, &bump, BUF_LEN, 8);
  uint64_t dst_pa = qcs_shared_alloc(region, &bump, BUF_LEN, 8);
  uint64_t fill_pa = qcs_shared_alloc(region, &bump, BUF_LEN, 8);
  uint64_t upd_pa = qcs_shared_alloc(region, &bump, 64, 8);
  assert(stream_pa && src_pa && dst_pa && fill_pa && upd_pa);

  // Seed the source device buffer with a known pattern (host writes via its VA).
  uint8_t* src = (uint8_t*)qcs_pa_to_ptr(region, src_pa);
  for (uint32_t i = 0; i < BUF_LEN; ++i) src[i] = (uint8_t)(i * 7u + 1u);
  const uint8_t weights[16] = {9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};

  // Record the job: COPY src->dst, FILL fill_buf, UPDATE weights->upd, a DISPATCH.
  qcs_writer_t w;
  qcs_writer_init(&w, qcs_pa_to_ptr(region, stream_pa), 1024);
  assert(qcs_write_copy(&w, src_pa, dst_pa, BUF_LEN) == 0);
  assert(qcs_write_fill(&w, fill_pa, BUF_LEN, 0xABu, 1) == 0);
  assert(qcs_write_update(&w, upd_pa, weights, sizeof(weights)) == 0);
  const uint32_t wgc[3] = {1, 1, 1}, wgs[3] = {8, 1, 1};
  const qcs_binding_t b[1] = {{dst_pa, BUF_LEN}};
  assert(qcs_write_dispatch(&w, 0, 0, wgc, wgs, 0, 0, NULL, 1, b) == 0);
  assert(!w.overflowed);

  qcs_job_descriptor_t* job = qcs_shared_job(region);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->record_count = w.record_count;
  job->cmd_stream_ptr = stream_pa;
  job->cmd_stream_len = w.size;

  qcs_doorbell_ring(job, /*job_id=*/1u);
  int32_t status = qcs_doorbell_wait_completion(job, /*job_id=*/1u);
  assert(status == 0 && "cluster reported success");

  // Verify the cluster's device-side effects landed in shared memory.
  const uint8_t* dst = (const uint8_t*)qcs_pa_to_ptr(region, dst_pa);
  const uint8_t* fill = (const uint8_t*)qcs_pa_to_ptr(region, fill_pa);
  const uint8_t* upd = (const uint8_t*)qcs_pa_to_ptr(region, upd_pa);
  for (uint32_t i = 0; i < BUF_LEN; ++i) {
    assert(dst[i] == (uint8_t)(i * 7u + 1u) && "COPY moved the bytes");
    assert(fill[i] == 0xABu && "FILL set the pattern");
  }
  for (uint32_t i = 0; i < sizeof(weights); ++i)
    assert(upd[i] == weights[i] && "UPDATE pushed the inline blob");

  printf("OK: %u-record job (copy+fill+update+dispatch) round-tripped through "
         "shared mmap across two processes; device buffers verified\n",
         w.record_count);
  return 0;
}

int main(void) {
  // Create + size the region once before forking so the child can open it;
  // the parent keeps this mapping to drive the job and verify results.
  qcs_shared_region_t region;
  assert(qcs_shared_region_create(&region, REGION_PATH, REGION_SIZE) == 0);

  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) {
    // Keep the inherited mapping alive: it occupies the parent's base VA so the child's own open() lands at a different base, genuinely exercising PA->VA translation.
    _exit(cluster_main());
  }
  int rc = host_main(&region);
  int wstatus = 0;
  waitpid(pid, &wstatus, 0);
  assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 &&
         "cluster process exited cleanly");

  qcs_shared_region_close(&region);
  unlink(REGION_PATH);
  return rc;
}
