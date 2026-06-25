// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "cluster_command_stream.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint64_t qcs_align_up_u64(uint64_t x, uint64_t a) {
  return (x + (a - 1u)) & ~(a - 1u);
}

// Trailing-payload offset of a DISPATCH record's binding array. Computed in 64
// bits so a hostile/huge constant_count cannot wrap. Shared by the writer, the
// reader's validation, and the accessor so they can never disagree.
static uint64_t qcs_dispatch_bindings_offset(uint32_t constant_count) {
  return qcs_align_up_u64((uint64_t)sizeof(qcs_dispatch_t) +
                              (uint64_t)constant_count * 4u,
                          8u);
}

// Full (padded) size a DISPATCH record with these counts occupies.
static uint64_t qcs_dispatch_total_size(uint32_t constant_count,
                                        uint32_t binding_count) {
  uint64_t end = qcs_dispatch_bindings_offset(constant_count) +
                 (uint64_t)binding_count * (uint64_t)sizeof(qcs_binding_t);
  return qcs_align_up_u64(end, QCS_RECORD_ALIGN);
}

void qcs_writer_init(qcs_writer_t* writer, void* buffer, uint32_t capacity) {
  assert(((uintptr_t)buffer % QCS_RECORD_ALIGN) == 0 &&
         "command-stream buffer must be 8-byte aligned");
  writer->data = (uint8_t*)buffer;
  writer->capacity = capacity;
  writer->size = 0;
  writer->record_count = 0;
  writer->overflowed = 0;
}

// Reserves `bytes` at the current offset. `bytes` is pre-validated to fit u32.
static uint8_t* qcs_writer_reserve(qcs_writer_t* writer, uint32_t bytes) {
  if (writer->overflowed || bytes > writer->capacity - writer->size) {
    writer->overflowed = 1;
    return NULL;
  }
  uint8_t* p = writer->data + writer->size;
  writer->size += bytes;
  ++writer->record_count;
  return p;
}

// Shared by the direct and indirect dispatch writers (flags/workgroup_count_ptr distinguish them).
static int qcs_write_dispatch_common(
    qcs_writer_t* writer, uint32_t executable_id, uint32_t export_ordinal,
    uint32_t flags, const uint32_t workgroup_count[3],
    uint64_t workgroup_count_ptr, const uint32_t workgroup_size[3],
    uint32_t dynamic_local_memory, uint32_t constant_count,
    const uint32_t* constants, uint32_t binding_count,
    const qcs_binding_t* bindings) {
  uint64_t bindings_off = qcs_dispatch_bindings_offset(constant_count);
  uint64_t total64 = qcs_dispatch_total_size(constant_count, binding_count);
  if (total64 > UINT32_MAX) {
    writer->overflowed = 1;
    return -1;
  }
  uint32_t total = (uint32_t)total64;

  uint8_t* base = qcs_writer_reserve(writer, total);
  if (!base) return -1;
  memset(base, 0, total);

  qcs_dispatch_t* d = (qcs_dispatch_t*)base;
  d->header.type = QCS_CMD_DISPATCH;
  d->header.size = total;
  d->executable_id = executable_id;
  d->export_ordinal = export_ordinal;
  d->flags = flags;
  for (int i = 0; i < 3; ++i) {
    d->workgroup_count[i] = workgroup_count ? workgroup_count[i] : 1u;
    d->workgroup_size[i] = workgroup_size ? workgroup_size[i] : 1u;
  }
  d->workgroup_count_ptr = workgroup_count_ptr;
  d->dynamic_local_memory = dynamic_local_memory;
  d->constant_count = constant_count;
  d->binding_count = binding_count;
  if (constant_count) {
    memcpy(base + sizeof(qcs_dispatch_t), constants, constant_count * 4u);
  }
  if (binding_count) {
    memcpy(base + bindings_off, bindings,
           (size_t)binding_count * sizeof(qcs_binding_t));
  }
  return 0;
}

int qcs_write_dispatch(qcs_writer_t* writer, uint32_t executable_id,
                       uint32_t export_ordinal,
                       const uint32_t workgroup_count[3],
                       const uint32_t workgroup_size[3],
                       uint32_t dynamic_local_memory, uint32_t constant_count,
                       const uint32_t* constants, uint32_t binding_count,
                       const qcs_binding_t* bindings) {
  return qcs_write_dispatch_common(writer, executable_id, export_ordinal,
                                   /*flags=*/0u, workgroup_count,
                                   /*workgroup_count_ptr=*/0u, workgroup_size,
                                   dynamic_local_memory, constant_count,
                                   constants, binding_count, bindings);
}

int qcs_write_dispatch_indirect(qcs_writer_t* writer, uint32_t executable_id,
                                uint32_t export_ordinal,
                                uint64_t workgroup_count_ptr,
                                const uint32_t workgroup_size[3],
                                uint32_t dynamic_local_memory,
                                uint32_t constant_count,
                                const uint32_t* constants,
                                uint32_t binding_count,
                                const qcs_binding_t* bindings) {
  return qcs_write_dispatch_common(
      writer, executable_id, export_ordinal, QCS_DISPATCH_FLAG_INDIRECT,
      /*workgroup_count=*/NULL, workgroup_count_ptr, workgroup_size,
      dynamic_local_memory, constant_count, constants, binding_count, bindings);
}

int qcs_write_copy(qcs_writer_t* writer, uint64_t src_ptr, uint64_t dst_ptr,
                   uint64_t length) {
  uint32_t total =
      (uint32_t)qcs_align_up_u64(sizeof(qcs_copy_t), QCS_RECORD_ALIGN);
  qcs_copy_t* c = (qcs_copy_t*)qcs_writer_reserve(writer, total);
  if (!c) return -1;
  memset(c, 0, total);
  c->header.type = QCS_CMD_COPY;
  c->header.size = total;
  c->src_ptr = src_ptr;
  c->dst_ptr = dst_ptr;
  c->length = length;
  return 0;
}

// Shared by the FILL writer and the reader so they can never disagree on what
// a valid fill is: pattern_length in {1,2,4,8} and length a multiple of it.
static int qcs_fill_valid(uint64_t length, uint32_t pattern_length) {
  return (pattern_length == 1u || pattern_length == 2u ||
          pattern_length == 4u || pattern_length == 8u) &&
         (length % pattern_length) == 0u;
}

int qcs_write_fill(qcs_writer_t* writer, uint64_t dst_ptr, uint64_t length,
                   uint64_t pattern, uint32_t pattern_length) {
  if (!qcs_fill_valid(length, pattern_length)) {
    writer->overflowed = 1;
    return -1;
  }
  uint32_t total =
      (uint32_t)qcs_align_up_u64(sizeof(qcs_fill_t), QCS_RECORD_ALIGN);
  qcs_fill_t* f = (qcs_fill_t*)qcs_writer_reserve(writer, total);
  if (!f) return -1;
  memset(f, 0, total);
  f->header.type = QCS_CMD_FILL;
  f->header.size = total;
  f->dst_ptr = dst_ptr;
  f->length = length;
  f->pattern = pattern;
  f->pattern_length = pattern_length;
  return 0;
}

// Full (padded) size an UPDATE record with `length` inline bytes occupies.
// Computed in 64 bits so a hostile/huge length cannot wrap.
static uint64_t qcs_update_total_size(uint64_t length) {
  return qcs_align_up_u64((uint64_t)sizeof(qcs_update_t) + length,
                          QCS_RECORD_ALIGN);
}

int qcs_write_update(qcs_writer_t* writer, uint64_t dst_ptr, const void* data,
                     uint64_t length) {
  uint64_t total64 = qcs_update_total_size(length);
  if (total64 > UINT32_MAX) {
    writer->overflowed = 1;
    return -1;
  }
  uint32_t total = (uint32_t)total64;
  uint8_t* base = qcs_writer_reserve(writer, total);
  if (!base) return -1;
  memset(base, 0, total);
  qcs_update_t* u = (qcs_update_t*)base;
  u->header.type = QCS_CMD_UPDATE;
  u->header.size = total;
  u->dst_ptr = dst_ptr;
  u->length = length;
  if (length) {
    memcpy(base + sizeof(qcs_update_t), data, (size_t)length);
  }
  return 0;
}

void qcs_reader_init(qcs_reader_t* reader, const void* buffer, uint32_t size) {
  assert(((uintptr_t)buffer % QCS_RECORD_ALIGN) == 0 &&
         "command-stream buffer must be 8-byte aligned");
  reader->data = (const uint8_t*)buffer;
  reader->size = size;
  reader->offset = 0;
}

const qcs_record_header_t* qcs_reader_next(qcs_reader_t* reader) {
  if (reader->offset + (uint32_t)sizeof(qcs_record_header_t) > reader->size) {
    return NULL;
  }
  const qcs_record_header_t* h =
      (const qcs_record_header_t*)(reader->data + reader->offset);
  // Header bounds: aligned, covers at least the header, fits in the buffer.
  if (h->size < sizeof(qcs_record_header_t) ||
      (h->size % QCS_RECORD_ALIGN) != 0 ||
      h->size > reader->size - reader->offset) {
    return NULL;
  }
  // Type-specific payload validation. The cluster parses untrusted host data, so
  // a record's declared counts must be re-checked against its size before any
  // accessor dereferences the payload (BUG-1). Unknown types are rejected so the
  // cluster never replays something it does not understand.
  switch (h->type) {
    case QCS_CMD_DISPATCH: {
      if (h->size < sizeof(qcs_dispatch_t)) return NULL;
      const qcs_dispatch_t* d = (const qcs_dispatch_t*)h;
      if (qcs_dispatch_total_size(d->constant_count, d->binding_count) >
          h->size) {
        return NULL;
      }
      break;
    }
    case QCS_CMD_COPY:
      if (h->size < sizeof(qcs_copy_t)) return NULL;
      break;
    case QCS_CMD_FILL: {
      if (h->size < sizeof(qcs_fill_t)) return NULL;
      const qcs_fill_t* f = (const qcs_fill_t*)h;
      if (!qcs_fill_valid(f->length, f->pattern_length)) return NULL;
      break;
    }
    case QCS_CMD_UPDATE: {
      if (h->size < sizeof(qcs_update_t)) return NULL;
      const qcs_update_t* u = (const qcs_update_t*)h;
      // The inline payload must fit in the record. Guard the u64 length before
      // the size computation so a hostile huge length cannot wrap.
      if (u->length > h->size || qcs_update_total_size(u->length) > h->size) {
        return NULL;
      }
      break;
    }
    default:
      return NULL;
  }
  reader->offset += h->size;
  return h;
}

const uint32_t* qcs_dispatch_constants(const qcs_dispatch_t* dispatch) {
  return (const uint32_t*)((const uint8_t*)dispatch + sizeof(qcs_dispatch_t));
}

const qcs_binding_t* qcs_dispatch_bindings(const qcs_dispatch_t* dispatch) {
  return (const qcs_binding_t*)((const uint8_t*)dispatch +
                                qcs_dispatch_bindings_offset(
                                    dispatch->constant_count));
}

const void* qcs_update_data(const qcs_update_t* update) {
  return (const uint8_t*)update + sizeof(qcs_update_t);
}
