// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#define _POSIX_C_SOURCE 200809L  // ftruncate, sched_yield, mmap (dev-box only)

#include "shared_region.h"

#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int qcs_map_fd(qcs_shared_region_t* region, int fd, uint64_t size) {
  void* base = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    0);
  if (base == MAP_FAILED) {
    close(fd);
    return -1;
  }
  region->base = base;
  region->size = size;
  region->fd = fd;
  region->desc_offset = 0;  // dev-box: no firmware below; descriptor at PA 0.
  return 0;
}

int qcs_shared_region_create(qcs_shared_region_t* region, const char* path,
                             uint64_t size) {
  region->base = NULL;
  region->size = 0;
  region->fd = -1;
  region->desc_offset = 0;
  if (size < QCS_SHARED_ARENA_OFFSET) return -1;
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return -1;
  }
  if (qcs_map_fd(region, fd, size) != 0) return -1;
  // Zero the job-descriptor page so doorbell/completion start clear.
  memset(region->base, 0, QCS_SHARED_ARENA_OFFSET);
  return 0;
}

int qcs_shared_region_open(qcs_shared_region_t* region, const char* path) {
  region->base = NULL;
  region->size = 0;
  region->fd = -1;
  int fd = open(path, O_RDWR);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < QCS_SHARED_ARENA_OFFSET) {
    close(fd);
    return -1;
  }
  return qcs_map_fd(region, fd, (uint64_t)st.st_size);
}

void qcs_shared_region_close(qcs_shared_region_t* region) {
  if (region->base) munmap(region->base, (size_t)region->size);
  if (region->fd >= 0) close(region->fd);
  region->base = NULL;
  region->size = 0;
  region->fd = -1;
  region->desc_offset = 0;
}

uint64_t qcs_shared_alloc(qcs_shared_region_t* region, uint64_t* bump,
                          uint64_t bytes, uint64_t align) {
  // Reject an invalid alignment rather than wrap the mask (align==0 would force
  // start to 0 and hand back the job-descriptor page).
  if (align == 0u || (align & (align - 1u)) != 0u) return 0;
  uint64_t start = (*bump + (align - 1u)) & ~(align - 1u);
  // Overflow-safe end check: bytes may be hostile, so test against the room left
  // rather than computing start+bytes (which could wrap).
  if (start > region->size || bytes > region->size - start) return 0;
  *bump = start + bytes;
  return start;
}

// doorbell/completion are plain uint32_t in the portable ABI struct, so use the __atomic builtins (lock-free, explicit ordering) rather than _Atomic type-punning.
void qcs_doorbell_ring(qcs_job_descriptor_t* job, uint32_t job_id) {
  __atomic_store_n(&job->doorbell, job_id, __ATOMIC_RELEASE);
}

uint32_t qcs_doorbell_wait(qcs_job_descriptor_t* job) {
  for (;;) {
    uint32_t db = __atomic_load_n(&job->doorbell, __ATOMIC_ACQUIRE);
    if (db != 0u) return db;
    sched_yield();
  }
}

void qcs_doorbell_complete(qcs_job_descriptor_t* job, uint32_t job_id,
                           int32_t status) {
  // Clear the doorbell (ABI: "cluster clears it") so a later wait can't re-fire on this job; publish status before the completion release-store.
  __atomic_store_n(&job->doorbell, 0u, __ATOMIC_RELAXED);
  job->status = status;
  __atomic_store_n(&job->completion, job_id, __ATOMIC_RELEASE);
}

int32_t qcs_doorbell_wait_completion(qcs_job_descriptor_t* job,
                                     uint32_t job_id) {
  for (;;) {
    uint32_t done = __atomic_load_n(&job->completion, __ATOMIC_ACQUIRE);
    if (done == job_id) return job->status;
    sched_yield();
  }
}
