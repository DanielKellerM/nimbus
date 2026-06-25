// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Phase-0 proof (no IREE, no RTL): the host serializes a job into a shared
// buffer; the "cluster" parses and replays it, asserting it observes exactly
// what the host wrote. Proves the command-stream ABI + the serialize/replay
// contract — the riskiest unknown of the host-device split — on the dev box.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cluster_command_stream.h"

int main(void) {
  // Stand-in for the AXI-shared region both sides reach (8-aligned like the
  // real shared aperture).
  static uint8_t shared_dram[4096] __attribute__((aligned(8)));

  // ---- HOST: record a 2-dispatch + 1-copy job ----
  qcs_writer_t w;
  qcs_writer_init(&w, shared_dram, (uint32_t)sizeof(shared_dram));

  const uint32_t wgc0[3] = {2, 1, 1}, wgs0[3] = {8, 1, 1};
  const uint32_t consts0[2] = {16, 16};  // e.g. M, N push-constants
  const qcs_binding_t binds0[3] = {
      {0x80001000ull, 2048}, {0x80002000ull, 2048}, {0x80003000ull, 2048}};
  assert(qcs_write_dispatch(&w, 0, 0, wgc0, wgs0, /*dyn_l1=*/4096, 2, consts0, 3,
                            binds0) == 0);

  const uint32_t wgc1[3] = {1, 1, 1}, wgs1[3] = {8, 1, 1};
  const qcs_binding_t binds1[1] = {{0x80004000ull, 512}};
  assert(qcs_write_dispatch(&w, 1, 2, wgc1, wgs1, 0, 0, NULL, 1, binds1) == 0);

  // FILL: zero-ish init a binding with a 4-byte repeating pattern.
  assert(qcs_write_fill(&w, 0x80005000ull, 1024, 0xDEADBEEFull, 4) == 0);

  // UPDATE: push an inline weight blob the cluster DMAs to a device buffer.
  const uint8_t weights[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  assert(qcs_write_update(&w, 0x80006000ull, weights, sizeof(weights)) == 0);

  // Indirect DISPATCH: workgroup count read from device memory at dispatch
  // time. Carries a constant too, exercising the constants-before-bindings
  // payload on the indirect path.
  const uint32_t consts2[1] = {42};
  const qcs_binding_t binds2[1] = {{0x80008000ull, 256}};
  assert(qcs_write_dispatch_indirect(&w, 2, 0, 0x80007000ull, wgs1, 0, 1,
                                     consts2, 1, binds2) == 0);

  assert(qcs_write_copy(&w, 0x80003000ull, 0x90000000ull, 2048) == 0);
  assert(!w.overflowed);

  qcs_job_descriptor_t job = {0};
  job.magic = QCS_MAGIC;
  job.version = QCS_VERSION;
  job.executable_table_id = 0xABCDEF0123456789ull;
  job.record_count = w.record_count;
  job.cmd_stream_ptr = (uint64_t)(uintptr_t)shared_dram;
  job.cmd_stream_len = w.size;
  job.doorbell = 1;  // submit

  // ---- CLUSTER: pick up the descriptor, parse + replay ----
  assert(job.magic == QCS_MAGIC && job.version == QCS_VERSION);
  qcs_reader_t r;
  qcs_reader_init(&r, (const void*)(uintptr_t)job.cmd_stream_ptr,
                  (uint32_t)job.cmd_stream_len);

  int n_dispatch = 0, n_copy = 0, n_fill = 0, n_update = 0;
  const qcs_record_header_t* h;
  while ((h = qcs_reader_next(&r)) != NULL) {
    if (h->type == QCS_CMD_DISPATCH) {
      const qcs_dispatch_t* d = (const qcs_dispatch_t*)h;
      const uint32_t* c = qcs_dispatch_constants(d);
      const qcs_binding_t* b = qcs_dispatch_bindings(d);
      printf("DISPATCH exec=%u ord=%u wg=[%u,%u,%u] consts=%u bindings=%u%s",
             d->executable_id, d->export_ordinal, d->workgroup_count[0],
             d->workgroup_count[1], d->workgroup_count[2], d->constant_count,
             d->binding_count,
             (d->flags & QCS_DISPATCH_FLAG_INDIRECT) ? " [indirect]" : "");
      if (d->binding_count) {
        printf(" binding0=0x%llx len=%llu",
               (unsigned long long)b[0].device_ptr,
               (unsigned long long)b[0].length);
      }
      printf("\n");
      if (n_dispatch == 0) {
        assert(!(d->flags & QCS_DISPATCH_FLAG_INDIRECT));
        assert(d->executable_id == 0 && d->export_ordinal == 0);
        assert(d->dynamic_local_memory == 4096);
        assert(d->workgroup_count[0] == 2 && d->workgroup_size[0] == 8);
        assert(d->constant_count == 2 && c[0] == 16 && c[1] == 16);
        assert(d->binding_count == 3);
        assert(b[2].device_ptr == 0x80003000ull && b[2].length == 2048);
      } else if (n_dispatch == 1) {
        assert(!(d->flags & QCS_DISPATCH_FLAG_INDIRECT));
        assert(d->executable_id == 1 && d->export_ordinal == 2);
        assert(d->constant_count == 0 && d->binding_count == 1);
        assert(b[0].device_ptr == 0x80004000ull && b[0].length == 512);
      } else {
        // The indirect dispatch: count comes from device memory, not the record.
        assert(d->flags & QCS_DISPATCH_FLAG_INDIRECT);
        assert(d->executable_id == 2 && d->export_ordinal == 0);
        assert(d->workgroup_count_ptr == 0x80007000ull);
        assert(d->constant_count == 1 && c[0] == 42);
        assert(d->binding_count == 1 && b[0].device_ptr == 0x80008000ull);
      }
      ++n_dispatch;
    } else if (h->type == QCS_CMD_COPY) {
      const qcs_copy_t* cp = (const qcs_copy_t*)h;
      printf("COPY 0x%llx -> 0x%llx (%llu bytes)\n",
             (unsigned long long)cp->src_ptr, (unsigned long long)cp->dst_ptr,
             (unsigned long long)cp->length);
      assert(cp->src_ptr == 0x80003000ull && cp->dst_ptr == 0x90000000ull &&
             cp->length == 2048);
      ++n_copy;
    } else if (h->type == QCS_CMD_FILL) {
      const qcs_fill_t* f = (const qcs_fill_t*)h;
      printf("FILL 0x%llx len=%llu pattern=0x%llx/%uB\n",
             (unsigned long long)f->dst_ptr, (unsigned long long)f->length,
             (unsigned long long)f->pattern, f->pattern_length);
      assert(f->dst_ptr == 0x80005000ull && f->length == 1024);
      assert(f->pattern == 0xDEADBEEFull && f->pattern_length == 4);
      ++n_fill;
    } else if (h->type == QCS_CMD_UPDATE) {
      const qcs_update_t* u = (const qcs_update_t*)h;
      const uint8_t* data = (const uint8_t*)qcs_update_data(u);
      printf("UPDATE 0x%llx len=%llu data[0..2]=%u,%u,%u\n",
             (unsigned long long)u->dst_ptr, (unsigned long long)u->length,
             data[0], data[1], data[2]);
      assert(u->dst_ptr == 0x80006000ull && u->length == 12);
      for (uint32_t i = 0; i < u->length; ++i) assert(data[i] == (uint8_t)i);
      ++n_update;
    } else {
      assert(0 && "unknown record type");
    }
  }

  job.completion = 1;
  job.status = 0;

  assert(r.offset == r.size && "stream fully consumed, no trailing garbage");
  assert(n_dispatch == 3 && n_copy == 1 && n_fill == 1 && n_update == 1);
  assert((uint32_t)(n_dispatch + n_copy + n_fill + n_update) ==
         job.record_count);
  printf("OK: %d dispatch (1 indirect) + %d copy + %d fill + %d update records "
         "round-tripped (%u bytes)\n",
         n_dispatch, n_copy, n_fill, n_update, w.size);

  // ---- ADVERSARIAL: the cluster parses untrusted host data ----
  // A record with a valid, in-bounds `size` but lying counts must be rejected
  // (else the accessors would read megabytes past the 64-byte record).
  {
    static uint8_t bad[64] __attribute__((aligned(8)));
    memset(bad, 0, sizeof(bad));
    qcs_dispatch_t* d = (qcs_dispatch_t*)bad;
    d->header.type = QCS_CMD_DISPATCH;
    d->header.size = sizeof(bad);  // valid header, in-bounds, 8-aligned
    d->constant_count = 1000000u;  // lies: payload would be megabytes
    d->binding_count = 1000000u;
    qcs_reader_t rr;
    qcs_reader_init(&rr, bad, sizeof(bad));
    assert(qcs_reader_next(&rr) == NULL && "must reject oversized counts");

    // An unknown record type must stop the parse, not be skipped silently.
    d->header.type = 0x999u;
    d->constant_count = 0;
    d->binding_count = 0;
    qcs_reader_init(&rr, bad, sizeof(bad));
    assert(qcs_reader_next(&rr) == NULL && "must reject unknown type");

    // A truncated record (size past the buffer end) must be rejected.
    d->header.type = QCS_CMD_DISPATCH;
    d->header.size = 4096;  // larger than the 64-byte buffer
    qcs_reader_init(&rr, bad, sizeof(bad));
    assert(qcs_reader_next(&rr) == NULL && "must reject truncated record");
    printf("OK: malformed records (huge counts / unknown type / truncated) "
           "rejected\n");

    // Writer must reject an integer-overflowing record, not silently
    // under-reserve and memcpy past the buffer.
    qcs_writer_t bw;
    qcs_writer_init(&bw, shared_dram, (uint32_t)sizeof(shared_dram));
    assert(qcs_write_dispatch(&bw, 0, 0, wgc1, wgs1, 0, 0, NULL, 0x10000000u,
                              binds0) == -1);
    assert(bw.overflowed);
    printf("OK: writer rejects integer-overflowing record\n");

    // FILL with a non-power-of-two pattern length must be rejected by both the
    // writer and the reader (the cluster would otherwise memset with a bad unit).
    qcs_writer_init(&bw, shared_dram, (uint32_t)sizeof(shared_dram));
    assert(qcs_write_fill(&bw, 0x80005000ull, 1024, 0, 3) == -1);  // bad unit
    assert(qcs_write_fill(&bw, 0x80005000ull, 1023, 0, 4) == -1);  // not multiple
    qcs_fill_t* bf = (qcs_fill_t*)bad;
    memset(bf, 0, sizeof(bad));
    bf->header.type = QCS_CMD_FILL;
    bf->header.size = (uint32_t)sizeof(qcs_fill_t);  // already 8-aligned
    bf->length = 1024;
    bf->pattern_length = 3;  // invalid
    qcs_reader_init(&rr, bad, sizeof(bad));
    assert(qcs_reader_next(&rr) == NULL && "must reject bad fill pattern_length");

    // UPDATE whose inline length runs past the record must be rejected (else the
    // accessor reads past the record into the rest of the stream).
    qcs_update_t* bu = (qcs_update_t*)bad;
    memset(bu, 0, sizeof(bad));
    bu->header.type = QCS_CMD_UPDATE;
    bu->header.size = (uint32_t)sizeof(bad);  // 64-byte record
    bu->length = 4096;  // lies: far more than the record can hold
    qcs_reader_init(&rr, bad, sizeof(bad));
    assert(qcs_reader_next(&rr) == NULL && "must reject oversized update length");
    printf("OK: malformed fill/update records rejected\n");

    // Zero-length UPDATE (data == NULL): the writer's `if (length)` guard must
    // skip the memcpy, and it must round-trip as an empty update.
    qcs_writer_t zw;
    qcs_writer_init(&zw, shared_dram, (uint32_t)sizeof(shared_dram));
    assert(qcs_write_update(&zw, 0x80009000ull, NULL, 0) == 0 && !zw.overflowed);
    qcs_reader_t zr;
    qcs_reader_init(&zr, shared_dram, zw.size);
    const qcs_record_header_t* zh = qcs_reader_next(&zr);
    assert(zh && zh->type == QCS_CMD_UPDATE);
    const qcs_update_t* zu = (const qcs_update_t*)zh;
    assert(zu->dst_ptr == 0x80009000ull && zu->length == 0);
    assert(qcs_reader_next(&zr) == NULL && zr.offset == zr.size);
    printf("OK: zero-length update round-trips\n");
  }
  return 0;
}
