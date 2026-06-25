// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Host<->Snitch-cluster job ABI for the Cheshire/Carfield host-device split.
//
// In the split deployment the IREE VM/HAL runs on the CVA6 host; the Snitch
// cluster is a pure accelerator. The host serializes a recorded command buffer
// into a flat command stream in shared memory (DRAM/L2 SPM the cluster iDMA can
// reach), publishes a job descriptor, and rings a doorbell. The cluster DM core
// parses the stream and replays it onto the 8 compute cores via the existing
// dispatch fan-out, then signals completion.
//
// This header is the contract between the two sides and is deliberately
// dependency-free (only <stdint.h>) so it compiles for both the rv64 Linux host
// and the rv32 Snitch firmware. All multi-byte fields are little-endian; both
// sides are RISC-V LE. Binding addresses are DEVICE-physical (the host
// translates host-VA -> device-PA before writing them).

#ifndef QUIDDITCH_HOST_TRANSPORT_CLUSTER_COMMAND_STREAM_H_
#define QUIDDITCH_HOST_TRANSPORT_CLUSTER_COMMAND_STREAM_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 'QCS1' — bump the version when the record layout changes.
#define QCS_MAGIC 0x31534351u
#define QCS_VERSION 2u  // v2: + FILL/UPDATE records, indirect dispatch

// Every record is aligned to this many bytes within the stream.
#define QCS_RECORD_ALIGN 8u

// Offset of the job descriptor within the shared region (e.g. L2 SPM). Must be
// above the cluster firmware's image, which links at the region base — placing
// the descriptor at offset 0 collides with the firmware .text.
#define QCS_JOB_DESCRIPTOR_OFFSET 0x10000u

typedef enum qcs_cmd_type_e {
  QCS_CMD_DISPATCH = 1,  // replay a dispatch onto the compute cores
  QCS_CMD_COPY = 2,      // device-side iDMA copy (PA -> PA)
  QCS_CMD_FILL = 3,      // memset a binding (fill_buffer): zero-init / bias
  QCS_CMD_UPDATE = 4,    // push an inline host data blob into a device buffer
} qcs_cmd_type_t;

// qcs_dispatch_t.flags bits.
// INDIRECT: the workgroup count is read from device memory at
// `workgroup_count_ptr` (3x uint32) at dispatch time; workgroup_count[] is then
// the host's fallback estimate and may be ignored by the cluster.
#define QCS_DISPATCH_FLAG_INDIRECT 1u

// Common header at the start of every record. `size` is the whole record
// including this header and any trailing variable-length payload, padded up to
// QCS_RECORD_ALIGN, so a reader can advance by it without knowing the type.
typedef struct qcs_record_header_t {
  uint32_t type;  // qcs_cmd_type_t
  uint32_t size;  // total record bytes (header + payload + padding)
} qcs_record_header_t;

// DISPATCH record. Trailing payload (in order, each naturally aligned):
//   uint32_t constants[constant_count];
//   qcs_binding_t bindings[binding_count];  (re-aligned to 8)
typedef struct qcs_dispatch_t {
  qcs_record_header_t header;
  uint32_t executable_id;      // index into the host-loaded executable table
  uint32_t export_ordinal;     // entry point within the executable
  uint32_t flags;              // QCS_DISPATCH_FLAG_* bits
  uint32_t workgroup_count[3];
  uint32_t workgroup_size[3];  // cluster may ignore in favour of the export's
  uint32_t dynamic_local_memory;  // bytes, added on top of the export's static L1
  uint64_t workgroup_count_ptr;   // device-PA of 3x uint32; valid iff flags&INDIRECT
  uint32_t constant_count;
  uint32_t binding_count;
} qcs_dispatch_t;

// One binding. `device_ptr` is the device-PHYSICAL address with IREE's
// buffer+offset already folded in by the host; `length` is the binding length
// (post-offset). The host owns the host-VA -> device-PA translation.
typedef struct qcs_binding_t {
  uint64_t device_ptr;  // device-physical, iDMA-reachable, = buffer_base + offset
  uint64_t length;
} qcs_binding_t;

// COPY record: an iDMA transfer the cluster performs device-side.
typedef struct qcs_copy_t {
  qcs_record_header_t header;
  uint64_t src_ptr;  // device-physical
  uint64_t dst_ptr;  // device-physical
  uint64_t length;
} qcs_copy_t;

// FILL record: memset `length` bytes at `dst_ptr` with a repeating pattern
// (IREE fill_buffer). The low `pattern_length` bytes of `pattern` are the unit,
// repeated; `length` must be a multiple of `pattern_length`.
typedef struct qcs_fill_t {
  qcs_record_header_t header;
  uint64_t dst_ptr;         // device-physical
  uint64_t length;          // bytes to fill
  uint64_t pattern;         // pattern unit in the low pattern_length bytes (LE)
  uint32_t pattern_length;  // 1, 2, 4, or 8
  uint32_t reserved;
} qcs_fill_t;

// UPDATE record: the host pushes an inline data blob (weights/parameters) the
// cluster DMAs to `dst_ptr`. Trailing payload: uint8_t data[length], padded to
// QCS_RECORD_ALIGN.
typedef struct qcs_update_t {
  qcs_record_header_t header;
  uint64_t dst_ptr;  // device-physical
  uint64_t length;   // inline payload bytes (trailing)
} qcs_update_t;

// Job descriptor — lives at a fixed shared address known to both sides.
// `doorbell` and `completion` are the host->cluster and cluster->host signals;
// on Carfield these map to the mailbox / CLINT msip and the PLIC completion IRQ,
// and are polled in the Phase-0 (no-IRQ) bring-up.
// The cluster publishes results with: write status -> fence -> write completion;
// the host polls `completion` (Phase 0) or waits on the completion IRQ (Phase 2).
typedef struct qcs_job_descriptor_t {
  uint32_t magic;          // QCS_MAGIC
  uint32_t version;        // QCS_VERSION (hard gate)
  uint32_t feature_flags;  // additive-feature negotiation without a version bump
  uint32_t doorbell;       // host writes job id (!=0) to submit; cluster clears it
  uint32_t completion;     // cluster writes job id when the whole stream is done
  int32_t status;          // cluster writes 0 on success, else an error code
  uint32_t record_count;   // records in the stream (lets the cluster pre-validate)
  uint32_t reserved;
  // Identity/hash of the kernel set the host compiled against; the cluster
  // rejects a job whose executables it does not have loaded.
  uint64_t executable_table_id;
  uint64_t cmd_stream_ptr;  // device-physical address of the command stream (8-aligned)
  uint64_t cmd_stream_len;  // bytes
} qcs_job_descriptor_t;

//===----------------------------------------------------------------------===//
// Writer (host side): append records to a caller-provided buffer.
//===----------------------------------------------------------------------===//

typedef struct qcs_writer_t {
  uint8_t* data;
  uint32_t capacity;
  uint32_t size;          // bytes written so far
  uint32_t record_count;  // records written so far
  int32_t overflowed;     // set on any failed append (overflow or bad argument)
} qcs_writer_t;

// `buffer` must be 8-byte aligned (records hold 64-bit fields the cluster loads
// in place; a misaligned base traps on rv32).
void qcs_writer_init(qcs_writer_t* writer, void* buffer, uint32_t capacity);

// Appends a DISPATCH record. `constants`/`bindings` may be NULL iff their count
// is 0. Returns 0 on success, -1 on overflow (and sets writer->overflowed).
int qcs_write_dispatch(qcs_writer_t* writer, uint32_t executable_id,
                       uint32_t export_ordinal, const uint32_t workgroup_count[3],
                       const uint32_t workgroup_size[3],
                       uint32_t dynamic_local_memory, uint32_t constant_count,
                       const uint32_t* constants, uint32_t binding_count,
                       const qcs_binding_t* bindings);

// Appends an indirect DISPATCH (workgroup count read from device memory at
// `workgroup_count_ptr`). `workgroup_size` may be NULL (defaults to 1s).
// Returns 0 on success, -1 on overflow.
int qcs_write_dispatch_indirect(qcs_writer_t* writer, uint32_t executable_id,
                                uint32_t export_ordinal,
                                uint64_t workgroup_count_ptr,
                                const uint32_t workgroup_size[3],
                                uint32_t dynamic_local_memory,
                                uint32_t constant_count,
                                const uint32_t* constants, uint32_t binding_count,
                                const qcs_binding_t* bindings);

// Appends a COPY record. Returns 0 on success, -1 on overflow.
int qcs_write_copy(qcs_writer_t* writer, uint64_t src_ptr, uint64_t dst_ptr,
                   uint64_t length);

// Appends a FILL record. `pattern_length` must be 1, 2, 4, or 8 and `length` a
// multiple of it. Returns 0 on success, -1 on overflow or bad pattern_length.
int qcs_write_fill(qcs_writer_t* writer, uint64_t dst_ptr, uint64_t length,
                   uint64_t pattern, uint32_t pattern_length);

// Appends an UPDATE record copying `length` bytes from `data` inline into the
// stream. `data` may be NULL iff `length` is 0. Returns 0 on success, -1 on
// overflow.
int qcs_write_update(qcs_writer_t* writer, uint64_t dst_ptr, const void* data,
                     uint64_t length);

//===----------------------------------------------------------------------===//
// Reader (cluster side): iterate records in place, no allocation.
//===----------------------------------------------------------------------===//

typedef struct qcs_reader_t {
  const uint8_t* data;
  uint32_t size;
  uint32_t offset;
} qcs_reader_t;

// `buffer` must be 8-byte aligned (see qcs_writer_init).
void qcs_reader_init(qcs_reader_t* reader, const void* buffer, uint32_t size);

// Returns the next record header (cast to the concrete type by `->type`), or
// NULL at end of stream / on a malformed, truncated, or unknown-type record.
// The cluster parses untrusted host-written data, so this re-validates each
// record's payload (counts vs. size), not just its header — after it returns a
// DISPATCH record, the accessors below are guaranteed in-bounds.
const qcs_record_header_t* qcs_reader_next(qcs_reader_t* reader);

// Accessors for a DISPATCH record's trailing payload. Only valid on a record
// returned by qcs_reader_next (which has validated the counts against size).
const uint32_t* qcs_dispatch_constants(const qcs_dispatch_t* dispatch);
const qcs_binding_t* qcs_dispatch_bindings(const qcs_dispatch_t* dispatch);

// Inline payload of an UPDATE record. Only valid on a record returned by
// qcs_reader_next (which has validated `length` against the record size).
const void* qcs_update_data(const qcs_update_t* update);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // QUIDDITCH_HOST_TRANSPORT_CLUSTER_COMMAND_STREAM_H_
