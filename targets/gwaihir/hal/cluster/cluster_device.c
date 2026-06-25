// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "runtime/host/hal/cluster/cluster_device.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/hal/api.h"
#include "runtime/host/hal/cluster/cluster_allocator.h"
#include "runtime/host/hal/cluster/cluster_command_buffer.h"
#include "runtime/host/transport/cluster_command_stream.h"
#include "runtime/host/transport/shared_region.h"

//===----------------------------------------------------------------------===//
// Trivial synchronous host semaphore
//===----------------------------------------------------------------------===//
// queue_execute is fully synchronous (we block on the cluster's completion
// before returning), so semaphores only need to record a monotonic value and
// satisfy waits whose target has already been signaled. The host IREE build
// ships neither iree/hal/utils/semaphore_base nor the local_sync driver, so we
// implement a self-contained semaphore that only relies on the inline
// iree_hal_resource_* helpers and the public semaphore vtable (which
// iree_hal_semaphore_{signal,wait,query} dispatch through directly).

typedef struct iree_hal_cluster_semaphore_t {
  iree_hal_resource_t resource;  // must be at offset 0
  iree_allocator_t host_allocator;
  pthread_mutex_t mutex;
  uint64_t value;
  iree_status_t failure;  // sticky failure status (owned)
} iree_hal_cluster_semaphore_t;

static const iree_hal_semaphore_vtable_t iree_hal_cluster_semaphore_vtable;

static iree_hal_cluster_semaphore_t* iree_hal_cluster_semaphore_cast(
    iree_hal_semaphore_t* base_value) {
  return (iree_hal_cluster_semaphore_t*)base_value;
}

static iree_status_t iree_hal_cluster_semaphore_create(
    uint64_t initial_value, iree_allocator_t host_allocator,
    iree_hal_semaphore_t** out_semaphore) {
  *out_semaphore = NULL;
  iree_hal_cluster_semaphore_t* semaphore = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*semaphore),
                                             (void**)&semaphore));
  iree_hal_resource_initialize(&iree_hal_cluster_semaphore_vtable,
                               &semaphore->resource);
  semaphore->host_allocator = host_allocator;
  pthread_mutex_init(&semaphore->mutex, NULL);
  semaphore->value = initial_value;
  semaphore->failure = iree_ok_status();
  *out_semaphore = (iree_hal_semaphore_t*)semaphore;
  return iree_ok_status();
}

static void iree_hal_cluster_semaphore_destroy(
    iree_hal_semaphore_t* base_semaphore) {
  iree_hal_cluster_semaphore_t* semaphore =
      iree_hal_cluster_semaphore_cast(base_semaphore);
  iree_allocator_t host_allocator = semaphore->host_allocator;
  iree_status_ignore(semaphore->failure);
  pthread_mutex_destroy(&semaphore->mutex);
  iree_allocator_free(host_allocator, semaphore);
}

static iree_status_t iree_hal_cluster_semaphore_query(
    iree_hal_semaphore_t* base_semaphore, uint64_t* out_value) {
  iree_hal_cluster_semaphore_t* semaphore =
      iree_hal_cluster_semaphore_cast(base_semaphore);
  pthread_mutex_lock(&semaphore->mutex);
  *out_value = semaphore->value;
  iree_status_t status = iree_status_clone(semaphore->failure);
  pthread_mutex_unlock(&semaphore->mutex);
  return status;
}

static iree_status_t iree_hal_cluster_semaphore_signal(
    iree_hal_semaphore_t* base_semaphore, uint64_t new_value) {
  iree_hal_cluster_semaphore_t* semaphore =
      iree_hal_cluster_semaphore_cast(base_semaphore);
  pthread_mutex_lock(&semaphore->mutex);
  if (new_value > semaphore->value) {
    semaphore->value = new_value;
  }
  pthread_mutex_unlock(&semaphore->mutex);
  return iree_ok_status();
}

static void iree_hal_cluster_semaphore_fail(
    iree_hal_semaphore_t* base_semaphore, iree_status_t status) {
  iree_hal_cluster_semaphore_t* semaphore =
      iree_hal_cluster_semaphore_cast(base_semaphore);
  pthread_mutex_lock(&semaphore->mutex);
  if (iree_status_is_ok(semaphore->failure)) {
    semaphore->failure = status;
  } else {
    iree_status_ignore(status);
  }
  pthread_mutex_unlock(&semaphore->mutex);
}

static iree_status_t iree_hal_cluster_semaphore_wait(
    iree_hal_semaphore_t* base_semaphore, uint64_t value,
    iree_timeout_t timeout, iree_hal_wait_flags_t flags) {
  iree_hal_cluster_semaphore_t* semaphore =
      iree_hal_cluster_semaphore_cast(base_semaphore);
  // Synchronous backend: all signaling work has already completed by the time a
  // wait is issued. If the value has been reached, succeed; otherwise this is a
  // deadlock we cannot resolve (no async producers), so report deadline.
  pthread_mutex_lock(&semaphore->mutex);
  iree_status_t status = iree_ok_status();
  if (!iree_status_is_ok(semaphore->failure)) {
    status = iree_status_clone(semaphore->failure);
  } else if (semaphore->value < value) {
    status = iree_status_from_code(IREE_STATUS_DEADLINE_EXCEEDED);
  }
  pthread_mutex_unlock(&semaphore->mutex);
  return status;
}

static iree_status_t iree_hal_cluster_semaphore_import_timepoint(
    iree_hal_semaphore_t* base_semaphore, uint64_t value,
    iree_hal_queue_affinity_t queue_affinity,
    iree_hal_external_timepoint_t external_timepoint) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "timepoints not supported on the cluster device");
}

static iree_status_t iree_hal_cluster_semaphore_export_timepoint(
    iree_hal_semaphore_t* base_semaphore, uint64_t value,
    iree_hal_queue_affinity_t queue_affinity,
    iree_hal_external_timepoint_type_t requested_type,
    iree_hal_external_timepoint_flags_t requested_flags,
    iree_hal_external_timepoint_t* IREE_RESTRICT out_external_timepoint) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "timepoints not supported on the cluster device");
}

static const iree_hal_semaphore_vtable_t iree_hal_cluster_semaphore_vtable = {
    .destroy = iree_hal_cluster_semaphore_destroy,
    .query = iree_hal_cluster_semaphore_query,
    .signal = iree_hal_cluster_semaphore_signal,
    .fail = iree_hal_cluster_semaphore_fail,
    .wait = iree_hal_cluster_semaphore_wait,
    .import_timepoint = iree_hal_cluster_semaphore_import_timepoint,
    .export_timepoint = iree_hal_cluster_semaphore_export_timepoint,
};

//===----------------------------------------------------------------------===//
// iree_hal_cluster_device_t
//===----------------------------------------------------------------------===//

// Each command buffer's QCS stream is allocated from the shared arena; the
// command buffer exposes its own stream device-PA (..._stream_pa), so the
// device needs no command-buffer -> PA table.
#define IREE_HAL_CLUSTER_STREAM_CAPACITY (64u * 1024u)

typedef struct iree_hal_cluster_device_t {
  iree_hal_resource_t resource;
  iree_string_view_t identifier;

  iree_allocator_t host_allocator;
  iree_hal_allocator_t* device_allocator;  // slice-2a cluster allocator (owned)

  qcs_shared_region_t* region;  // borrowed

  iree_hal_device_topology_info_t topology_info;

  // Monotonic job id rung into the doorbell (must be != 0 to submit).
  uint32_t next_job_id;

  // Trailing identifier string storage.
  char identifier_storage[];
} iree_hal_cluster_device_t;

static const iree_hal_device_vtable_t iree_hal_cluster_device_vtable;

static iree_hal_cluster_device_t* iree_hal_cluster_device_cast(
    iree_hal_device_t* base_value) {
  IREE_HAL_ASSERT_TYPE(base_value, &iree_hal_cluster_device_vtable);
  return (iree_hal_cluster_device_t*)base_value;
}

iree_status_t iree_hal_cluster_device_create(iree_string_view_t id,
                                             qcs_shared_region_t* region,
                                             iree_allocator_t host_allocator,
                                             iree_hal_device_t** out_device) {
  IREE_ASSERT_ARGUMENT(region);
  IREE_ASSERT_ARGUMENT(out_device);
  *out_device = NULL;

  iree_hal_allocator_t* device_allocator = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_cluster_allocator_create(region, host_allocator,
                                                         &device_allocator));

  iree_hal_cluster_device_t* device = NULL;
  iree_host_size_t total_size = sizeof(*device) + id.size + 1;
  iree_status_t status =
      iree_allocator_malloc(host_allocator, total_size, (void**)&device);
  if (iree_status_is_ok(status)) {
    memset(device, 0, total_size);
    iree_hal_resource_initialize(&iree_hal_cluster_device_vtable,
                                 &device->resource);
    iree_string_view_append_to_buffer(id, &device->identifier,
                                       device->identifier_storage);
    device->host_allocator = host_allocator;
    device->device_allocator = device_allocator;  // takes the reference
    device->region = region;
    device->next_job_id = 1;
    *out_device = (iree_hal_device_t*)device;
  } else {
    iree_hal_allocator_release(device_allocator);
  }
  return status;
}

static void iree_hal_cluster_device_destroy(iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  iree_allocator_t host_allocator = device->host_allocator;
  iree_hal_allocator_release(device->device_allocator);
  iree_allocator_free(host_allocator, device);
}

static iree_string_view_t iree_hal_cluster_device_id(
    iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return device->identifier;
}

static iree_allocator_t iree_hal_cluster_device_host_allocator(
    iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return device->host_allocator;
}

static iree_hal_allocator_t* iree_hal_cluster_device_allocator(
    iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return device->device_allocator;
}

static void iree_hal_cluster_device_replace_device_allocator(
    iree_hal_device_t* base_device, iree_hal_allocator_t* new_allocator) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  iree_hal_allocator_retain(new_allocator);
  iree_hal_allocator_release(device->device_allocator);
  device->device_allocator = new_allocator;
}

static void iree_hal_cluster_device_replace_channel_provider(
    iree_hal_device_t* base_device, iree_hal_channel_provider_t* new_provider) {
  // No collectives on the cluster device; ignore.
  (void)base_device;
  (void)new_provider;
}

static iree_status_t iree_hal_cluster_device_trim(
    iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return iree_hal_allocator_trim(device->device_allocator);
}

static iree_status_t iree_hal_cluster_device_query_i64(
    iree_hal_device_t* base_device, iree_string_view_t category,
    iree_string_view_t key, int64_t* out_value) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  *out_value = 0;
  if (iree_string_view_equal(category, IREE_SV("hal.device.id"))) {
    *out_value =
        iree_string_view_match_pattern(device->identifier, key) ? 1 : 0;
    return iree_ok_status();
  }
  if (iree_string_view_equal(category, IREE_SV("hal.device")) &&
      iree_string_view_equal(key, IREE_SV("concurrency"))) {
    *out_value = 1;  // synchronous single-queue device.
    return iree_ok_status();
  }
  return iree_make_status(
      IREE_STATUS_NOT_FOUND,
      "unknown device configuration key value '%.*s :: %.*s'",
      (int)category.size, category.data, (int)key.size, key.data);
}

static iree_status_t iree_hal_cluster_device_query_capabilities(
    iree_hal_device_t* base_device,
    iree_hal_device_capabilities_t* out_capabilities) {
  (void)base_device;
  memset(out_capabilities, 0, sizeof(*out_capabilities));
  return iree_ok_status();
}

static const iree_hal_device_topology_info_t*
iree_hal_cluster_device_topology_info(iree_hal_device_t* base_device) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return &device->topology_info;
}

static iree_status_t iree_hal_cluster_device_refine_topology_edge(
    iree_hal_device_t* src_device, iree_hal_device_t* dst_device,
    iree_hal_topology_edge_t* edge) {
  (void)src_device;
  (void)dst_device;
  (void)edge;
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_device_assign_topology_info(
    iree_hal_device_t* base_device,
    const iree_hal_device_topology_info_t* topology_info) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  device->topology_info = *topology_info;
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_device_create_channel(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    iree_hal_channel_params_t params, iree_hal_channel_t** out_channel) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "collectives not implemented on the cluster device");
}

static iree_status_t iree_hal_cluster_device_create_command_buffer(
    iree_hal_device_t* base_device, iree_hal_command_buffer_mode_t mode,
    iree_hal_command_category_t command_categories,
    iree_hal_queue_affinity_t queue_affinity, iree_host_size_t binding_capacity,
    iree_hal_command_buffer_t** out_command_buffer) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  *out_command_buffer = NULL;

  // Allocate the QCS stream buffer from the shared arena so its address is a
  // real device-PA the cluster can DMA-read. The slice-2a allocator owns the
  // live shared bump pointer, so rather than carving the arena directly (which
  // would collide with allocator buffers) we allocate the stream as an ordinary
  // device buffer through that allocator, then recover its device-PA by mapping
  // it. The backing memory lives in the bump arena (no free) for the region's
  // lifetime, so it stays valid for the command buffer that records into it.
  iree_hal_buffer_params_t params = {0};
  params.type =
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_VISIBLE;
  params.usage =
      IREE_HAL_BUFFER_USAGE_TRANSFER | IREE_HAL_BUFFER_USAGE_DISPATCH;
  params.access = IREE_HAL_MEMORY_ACCESS_ALL;
  iree_hal_buffer_t* stream_buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      device->device_allocator, params, IREE_HAL_CLUSTER_STREAM_CAPACITY,
      &stream_buffer));

  iree_hal_buffer_mapping_t mapping = {{0}};
  iree_status_t status = iree_hal_buffer_map_range(
      stream_buffer, IREE_HAL_MAPPING_MODE_SCOPED, IREE_HAL_MEMORY_ACCESS_ANY,
      0, IREE_HAL_WHOLE_BUFFER, &mapping);
  if (!iree_status_is_ok(status)) {
    iree_hal_buffer_release(stream_buffer);
    return status;
  }
  void* stream_ptr = mapping.contents.data;
  iree_hal_buffer_unmap_range(&mapping);

  iree_hal_command_buffer_t* command_buffer = NULL;
  status = iree_hal_cluster_command_buffer_create(
      device->region, device->device_allocator, stream_ptr,
      IREE_HAL_CLUSTER_STREAM_CAPACITY, mode, command_categories,
      queue_affinity, binding_capacity, device->host_allocator,
      &command_buffer);
  if (!iree_status_is_ok(status)) {
    iree_hal_buffer_release(stream_buffer);
    return status;
  }

  // The stream buffer lives in the shared arena (bump allocator: no free), so
  // we intentionally drop our reference here; the backing memory persists for
  // the region's lifetime and the command buffer holds the host pointer.
  iree_hal_buffer_release(stream_buffer);

  *out_command_buffer = command_buffer;
  return iree_ok_status();
}

iree_status_t iree_hal_cluster_device_submit(
    iree_hal_device_t* base_device,
    iree_hal_command_buffer_t* command_buffer) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);

  if (!command_buffer ||
      !iree_hal_cluster_command_buffer_isa(command_buffer)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "expected a cluster command buffer");
  }

  uint64_t stream_pa =
      iree_hal_cluster_command_buffer_stream_pa(command_buffer);

  uint32_t stream_size =
      iree_hal_cluster_command_buffer_size(command_buffer);
  uint32_t record_count =
      iree_hal_cluster_command_buffer_record_count(command_buffer);

  // Publish the job descriptor in the shared region.
  qcs_job_descriptor_t* job = qcs_shared_job(device->region);
  job->magic = QCS_MAGIC;
  job->version = QCS_VERSION;
  job->feature_flags = 0;
  job->status = 0;
  job->record_count = record_count;
  job->reserved = 0;
  job->executable_table_id = 0;
  job->cmd_stream_ptr = stream_pa;
  job->cmd_stream_len = stream_size;

  uint32_t job_id = device->next_job_id++;
  if (device->next_job_id == 0) device->next_job_id = 1;  // never use id 0.

  // Ring the doorbell and wait for the cluster to finish (synchronous).
  qcs_doorbell_ring(job, job_id);
  int32_t cluster_status = qcs_doorbell_wait_completion(job, job_id);
  if (cluster_status != 0) {
    return iree_make_status(IREE_STATUS_INTERNAL,
                            "cluster reported job failure (status=%d)",
                            (int)cluster_status);
  }
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_device_create_event(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    iree_hal_event_flags_t flags, iree_hal_event_t** out_event) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "events not implemented on the cluster device");
}

static iree_status_t iree_hal_cluster_device_create_executable_cache(
    iree_hal_device_t* base_device, iree_string_view_t identifier,
    iree_loop_t loop, iree_hal_executable_cache_t** out_executable_cache) {
  return iree_make_status(
      IREE_STATUS_UNIMPLEMENTED,
      "executable cache not implemented on the cluster device (Phase-1)");
}

static iree_status_t iree_hal_cluster_device_import_file(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    iree_hal_memory_access_t access, iree_io_file_handle_t* handle,
    iree_hal_external_file_flags_t flags, iree_hal_file_t** out_file) {
  return iree_make_status(IREE_STATUS_UNIMPLEMENTED,
                          "file import not implemented on the cluster device");
}

static iree_status_t iree_hal_cluster_device_create_semaphore(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    uint64_t initial_value, iree_hal_semaphore_flags_t flags,
    iree_hal_semaphore_t** out_semaphore) {
  iree_hal_cluster_device_t* device = iree_hal_cluster_device_cast(base_device);
  return iree_hal_cluster_semaphore_create(initial_value, device->host_allocator,
                                           out_semaphore);
}

static iree_hal_semaphore_compatibility_t
iree_hal_cluster_device_query_semaphore_compatibility(
    iree_hal_device_t* base_device, iree_hal_semaphore_t* semaphore) {
  (void)base_device;
  (void)semaphore;
  return IREE_HAL_SEMAPHORE_COMPATIBILITY_HOST_ONLY;
}

static iree_status_t iree_hal_cluster_device_queue_alloca(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_allocator_pool_t pool, iree_hal_buffer_params_t params,
    iree_device_size_t allocation_size, iree_hal_alloca_flags_t flags,
    iree_hal_buffer_t** IREE_RESTRICT out_buffer) {
  // Synchronous: wait, allocate from the device allocator, then signal.
  IREE_RETURN_IF_ERROR(iree_hal_semaphore_list_wait(
      wait_semaphore_list, iree_infinite_timeout(), IREE_HAL_WAIT_FLAG_DEFAULT));
  IREE_RETURN_IF_ERROR(iree_hal_allocator_allocate_buffer(
      iree_hal_device_allocator(base_device), params, allocation_size,
      out_buffer));
  return iree_hal_semaphore_list_signal(signal_semaphore_list);
}

static iree_status_t iree_hal_cluster_device_queue_dealloca(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_buffer_t* buffer, iree_hal_dealloca_flags_t flags) {
  IREE_RETURN_IF_ERROR(iree_hal_semaphore_list_wait(
      wait_semaphore_list, iree_infinite_timeout(), IREE_HAL_WAIT_FLAG_DEFAULT));
  return iree_hal_semaphore_list_signal(signal_semaphore_list);
}

static iree_status_t iree_hal_cluster_device_queue_unimplemented(void) {
  return iree_make_status(
      IREE_STATUS_UNIMPLEMENTED,
      "queue operation not implemented on the cluster device (Phase-1)");
}

static iree_status_t iree_hal_cluster_device_queue_fill(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length, const void* pattern,
    iree_host_size_t pattern_length, iree_hal_fill_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_update(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    const void* source_buffer, iree_host_size_t source_offset,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length, iree_hal_update_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_copy(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_buffer_t* source_buffer, iree_device_size_t source_offset,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length, iree_hal_copy_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_read(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_file_t* source_file, uint64_t source_offset,
    iree_hal_buffer_t* target_buffer, iree_device_size_t target_offset,
    iree_device_size_t length, iree_hal_read_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_write(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_buffer_t* source_buffer, iree_device_size_t source_offset,
    iree_hal_file_t* target_file, uint64_t target_offset,
    iree_device_size_t length, iree_hal_write_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_host_call(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_host_call_t call, const uint64_t args[4],
    iree_hal_host_call_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_dispatch(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_executable_t* executable,
    iree_hal_executable_export_ordinal_t export_ordinal,
    const iree_hal_dispatch_config_t config, iree_const_byte_span_t constants,
    const iree_hal_buffer_ref_list_t bindings,
    iree_hal_dispatch_flags_t flags) {
  return iree_hal_cluster_device_queue_unimplemented();
}

static iree_status_t iree_hal_cluster_device_queue_execute(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_command_buffer_t* command_buffer,
    iree_hal_buffer_binding_table_t binding_table,
    iree_hal_execute_flags_t flags) {
  // Synchronous submission (concurrency == 1): wait for dependencies, publish
  // the recorded QCS stream + ring the doorbell + block for completion, then
  // satisfy the signal semaphores. Any cluster failure is propagated to the
  // signal semaphores so dependents observe the abort.
  IREE_RETURN_IF_ERROR(iree_hal_semaphore_list_wait(
      wait_semaphore_list, iree_infinite_timeout(), IREE_HAL_WAIT_FLAG_DEFAULT));

  // A barrier-only submission (no command buffer) is a valid no-op.
  if (command_buffer) {
    iree_status_t submit_status =
        iree_hal_cluster_device_submit(base_device, command_buffer);
    if (!iree_status_is_ok(submit_status)) {
      iree_hal_semaphore_list_fail(signal_semaphore_list,
                                   iree_status_clone(submit_status));
      return submit_status;
    }
  }

  return iree_hal_semaphore_list_signal(signal_semaphore_list);
}

static iree_status_t iree_hal_cluster_device_queue_flush(
    iree_hal_device_t* base_device, iree_hal_queue_affinity_t queue_affinity) {
  return iree_ok_status();  // submissions flush eagerly.
}

static iree_status_t iree_hal_cluster_device_wait_semaphores(
    iree_hal_device_t* base_device, iree_hal_wait_mode_t wait_mode,
    const iree_hal_semaphore_list_t semaphore_list, iree_timeout_t timeout,
    iree_hal_wait_flags_t flags) {
  (void)base_device;
  (void)wait_mode;
  return iree_hal_semaphore_list_wait(semaphore_list, timeout, flags);
}

static iree_status_t iree_hal_cluster_device_profiling_begin(
    iree_hal_device_t* base_device,
    const iree_hal_device_profiling_options_t* options) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_device_profiling_flush(
    iree_hal_device_t* base_device) {
  return iree_ok_status();
}

static iree_status_t iree_hal_cluster_device_profiling_end(
    iree_hal_device_t* base_device) {
  return iree_ok_status();
}

static const iree_hal_device_vtable_t iree_hal_cluster_device_vtable = {
    .destroy = iree_hal_cluster_device_destroy,
    .id = iree_hal_cluster_device_id,
    .host_allocator = iree_hal_cluster_device_host_allocator,
    .device_allocator = iree_hal_cluster_device_allocator,
    .replace_device_allocator = iree_hal_cluster_device_replace_device_allocator,
    .replace_channel_provider =
        iree_hal_cluster_device_replace_channel_provider,
    .trim = iree_hal_cluster_device_trim,
    .query_i64 = iree_hal_cluster_device_query_i64,
    .query_capabilities = iree_hal_cluster_device_query_capabilities,
    .topology_info = iree_hal_cluster_device_topology_info,
    .refine_topology_edge = iree_hal_cluster_device_refine_topology_edge,
    .assign_topology_info = iree_hal_cluster_device_assign_topology_info,
    .create_channel = iree_hal_cluster_device_create_channel,
    .create_command_buffer = iree_hal_cluster_device_create_command_buffer,
    .create_event = iree_hal_cluster_device_create_event,
    .create_executable_cache = iree_hal_cluster_device_create_executable_cache,
    .import_file = iree_hal_cluster_device_import_file,
    .create_semaphore = iree_hal_cluster_device_create_semaphore,
    .query_semaphore_compatibility =
        iree_hal_cluster_device_query_semaphore_compatibility,
    .queue_alloca = iree_hal_cluster_device_queue_alloca,
    .queue_dealloca = iree_hal_cluster_device_queue_dealloca,
    .queue_fill = iree_hal_cluster_device_queue_fill,
    .queue_update = iree_hal_cluster_device_queue_update,
    .queue_copy = iree_hal_cluster_device_queue_copy,
    .queue_read = iree_hal_cluster_device_queue_read,
    .queue_write = iree_hal_cluster_device_queue_write,
    .queue_host_call = iree_hal_cluster_device_queue_host_call,
    .queue_dispatch = iree_hal_cluster_device_queue_dispatch,
    .queue_execute = iree_hal_cluster_device_queue_execute,
    .queue_flush = iree_hal_cluster_device_queue_flush,
    .wait_semaphores = iree_hal_cluster_device_wait_semaphores,
    .profiling_begin = iree_hal_cluster_device_profiling_begin,
    .profiling_flush = iree_hal_cluster_device_profiling_flush,
    .profiling_end = iree_hal_cluster_device_profiling_end,
};
