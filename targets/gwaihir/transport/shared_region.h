// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-1 stand-in for the AXI-shared region (L2 SPM / DRAM) both the CVA6 host
// and the Snitch cluster reach. On the dev box it is an mmap'd file shared by
// two processes; in the real split it becomes the physical shared aperture.
//
// The "device-physical" addresses the command-stream ABI carries are modelled
// here as OFFSETS into this region (device-PA 0 == region base), so each process
// translates between its own mmap VA and the shared PA. Fixed layout:
//   [0 .. QCS_SHARED_ARENA_OFFSET)   the job descriptor (first page)
//   [QCS_SHARED_ARENA_OFFSET .. end) bump arena: command stream + device buffers
//
// POSIX/dev-box only (mmap, atomics): unlike cluster_command_stream.h this is
// host-side Phase-1 plumbing, not firmware that must compile for rv32.

#ifndef QUIDDITCH_HOST_TRANSPORT_SHARED_REGION_H_
#define QUIDDITCH_HOST_TRANSPORT_SHARED_REGION_H_

#include <stdint.h>

#include "cluster_command_stream.h"

#ifdef __cplusplus
extern "C" {
#endif

// The job descriptor occupies the first page; the bump arena starts here.
#define QCS_SHARED_ARENA_OFFSET 4096u

typedef struct qcs_shared_region_t {
  void* base;     // this process's mmap VA of device-PA 0
  uint64_t size;  // total region bytes
  int fd;         // backing file descriptor (-1 if none)
} qcs_shared_region_t;

// Create (host): truncate `path` to `size` and map it shared. Zeroes the job
// descriptor page. Returns 0 on success, -1 on error (errno set).
int qcs_shared_region_create(qcs_shared_region_t* region, const char* path,
                             uint64_t size);

// Open (cluster): map an existing region created by the host. Returns 0/-1.
int qcs_shared_region_open(qcs_shared_region_t* region, const char* path);

// Unmap (and close the fd). Does not unlink the backing file.
void qcs_shared_region_close(qcs_shared_region_t* region);

// device-PA (offset) <-> this-process VA.
static inline void* qcs_pa_to_ptr(const qcs_shared_region_t* region,
                                  uint64_t pa) {
  return (uint8_t*)region->base + pa;
}
static inline uint64_t qcs_ptr_to_pa(const qcs_shared_region_t* region,
                                     const void* ptr) {
  return (uint64_t)((const uint8_t*)ptr - (const uint8_t*)region->base);
}

// The job descriptor lives at device-PA 0.
static inline qcs_job_descriptor_t* qcs_shared_job(
    const qcs_shared_region_t* region) {
  return (qcs_job_descriptor_t*)region->base;
}

// Host-side bump allocator over the arena. `*bump` is the next free device-PA
// (initialise to QCS_SHARED_ARENA_OFFSET). Returns the allocation's device-PA
// (>= QCS_SHARED_ARENA_OFFSET, aligned to `align`), or 0 if it would overflow
// the region. align must be a non-zero power of two.
uint64_t qcs_shared_alloc(qcs_shared_region_t* region, uint64_t* bump,
                          uint64_t bytes, uint64_t align);

//===----------------------------------------------------------------------===//
// Doorbell / completion handshake (Phase-1 polling; no IRQ yet).
//===----------------------------------------------------------------------===//
// Ordering matches the ABI: the cluster writes status -> release fence ->
// completion; the host acquire-loads completion then reads status. The doorbell
// and completion words are accessed atomically across the two processes.

// Host: publish a submitted job id into the doorbell (release).
void qcs_doorbell_ring(qcs_job_descriptor_t* job, uint32_t job_id);

// Cluster: spin until the doorbell carries a job id (acquire); returns it.
uint32_t qcs_doorbell_wait(qcs_job_descriptor_t* job);

// Cluster: publish the result (status first, then completion w/ release fence).
void qcs_doorbell_complete(qcs_job_descriptor_t* job, uint32_t job_id,
                           int32_t status);

// Host: spin until `completion == job_id` (acquire); returns job->status.
int32_t qcs_doorbell_wait_completion(qcs_job_descriptor_t* job,
                                     uint32_t job_id);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_TRANSPORT_SHARED_REGION_H_
