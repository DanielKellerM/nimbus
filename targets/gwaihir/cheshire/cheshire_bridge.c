// Copyright 2026 ETH Zurich and University of Bologna.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Thin bridge from the spike-era IREE host glue (hostio.h talks to _write, IREE
// pulls _sbrk for any stray malloc) to the Cheshire baremetal SW environment.
// Cheshire provides _putchar (UART), printf, _start, _exit (crt0). We supply:
//   * _write  -> loops over Cheshire _putchar so ALL of hostio.h's debug
//                tracing comes out on the gwaihir UART console (no HTIF, which
//                would wedge the console).
//   * _sbrk   -> bump allocator over a static heap window (Cheshire's spm.ld
//                provides no heap/end symbol for newlib malloc).
// pthread_mutex_* and iree_io_file_handle_* are still provided by the kept
// spike stub objects (pthread_stubs.o, iree_io_stubs.o).

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "regs/cheshire.h"
#include "dif/clint.h"
#include "dif/uart.h"
#include "params.h"
#include "util.h"

// Cheshire UART char output (libcheshire.a / sw/lib/dif/uart.c).
extern void _putchar(char c);

// Bring up the gwaihir/Cheshire UART so _putchar (and thus _write/hostio) emit
// on the console. Mirrors sw/tests/helloworld.c. Must be called before any
// printing.
void cheshire_console_init(void) {
  uint32_t rtc_freq = CHS_REGS->rtc_freq.f.ref_freq;
  uint64_t reset_freq = clint_get_core_freq(rtc_freq, 2500);
  uart_init(&__uart_base_addr__, reset_freq, __BOOT_BAUDRATE);
}

// Console bridge: hostio.h calls _write(1, ptr, len); route to the UART.
ssize_t _write(int file, const void* ptr, size_t len) {
  (void)file;
  const char* p = (const char*)ptr;
  for (size_t i = 0; i < len; ++i) {
    char c = p[i];
    if (c == '\n') _putchar('\r');  // CR+LF for terminals
    _putchar(c);
  }
  return (ssize_t)len;
}

// Minimal heap for any newlib malloc the IREE libs may still reference. The
// IREE host VM runs off its own static bump arena (main), so this is only a
// safety net; 256 KiB lives in .bss (zeroed by crt0).
#ifndef CHESHIRE_HEAP_BYTES
#define CHESHIRE_HEAP_BYTES (64u * 1024u)
#endif
static uint8_t g_heap[CHESHIRE_HEAP_BYTES] __attribute__((aligned(16)));
static size_t g_heap_off = 0;

void* _sbrk(ptrdiff_t incr) {
  if (incr < 0) return (void*)-1;
  size_t need = (size_t)incr;
  if (g_heap_off + need > CHESHIRE_HEAP_BYTES) return (void*)-1;
  void* base = &g_heap[g_heap_off];
  g_heap_off += need;
  return base;
}
