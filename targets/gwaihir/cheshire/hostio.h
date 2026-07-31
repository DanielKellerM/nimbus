// Tiny freestanding console output that bypasses newlib stdio/malloc.
// newlib's prebuilt libc.a is medlow (absolute lui), and its _impure_ptr /
// stdio globals live in .sdata at DRAM 0x80000000 -- beyond the signed 32-bit
// lui range -- so pulling in fprintf triggers R_RISCV_HI20 reloc overflows.
// We avoid all of stdio by talking to _write (HTIF) directly.
#ifndef HOSTIO_H_
#define HOSTIO_H_
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
ssize_t _write(int file, const void* ptr, size_t len);

static size_t host_strlen(const char* s) {
  size_t n = 0;
  while (s[n]) ++n;
  return n;
}
static void host_puts(const char* s) { _write(1, s, host_strlen(s)); }

// Minimal unsigned-decimal print.
static void host_putu(unsigned long v) {
  char buf[24];
  int i = 24;
  if (v == 0) buf[--i] = '0';
  while (v) {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  _write(1, &buf[i], (size_t)(24 - i));
}
// Minimal hex print (no 0x prefix; caller adds it).
static void host_puthex64(uint64_t v) {
  char buf[16];
  for (int i = 0; i < 16; ++i) {
    unsigned nyb = (unsigned)((v >> ((15 - i) * 4)) & 0xf);
    buf[i] = (char)(nyb < 10 ? '0' + nyb : 'a' + (nyb - 10));
  }
  // Trim leading zeros but keep at least one digit.
  int start = 0;
  while (start < 15 && buf[start] == '0') ++start;
  _write(1, &buf[start], (size_t)(16 - start));
}
static void host_puthex(uint32_t v) { host_puthex64((uint64_t)v); }

#ifdef __cplusplus
}
#endif
#endif  // HOSTIO_H_
