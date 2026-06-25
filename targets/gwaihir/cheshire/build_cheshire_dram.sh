#!/usr/bin/env bash
# PRINCIPLED no-stubs DRAM host build (host-device split Phase 2).
#
# Builds the IREE EmitC host VM runner as a Cheshire/CVA6 baremetal SW binary
# that runs ENTIRELY from external DRAM (0x80000000), linked against the REAL
# IIS newlib libc/libm + libgcc -- NO hand-stubbed libc/libm, NO
# --allow-multiple-definition.
#
# Toolchain: riscv64-gcc-13.2.0. Its rv64imafdc/lp64d libgcc/libc/libm have ZERO
# absolute R_RISCV_HI20 relocs (verified: libgcc 0, libc 0, libm 0; the 12.2.0
# libgcc has 130 -> truncate at the DRAM 0x80000000 base). So they link cleanly
# with -mcmodel=medany at DRAM, exactly as cheshire/sw/sw.mk builds DRAM SW.
#
# Kept shims (genuinely unprovided by newlib/libcheshire, NOT workarounds):
#   * pthread_stubs.o    -- newlib has no pthread; single-hart no-op mutexes.
#   * iree_io_stubs.o    -- IREE-internal file-handle symbols, never in any lib.
#   * cheshire_bridge.c  -- _write -> Cheshire _putchar(UART); _sbrk over a small
#                           static heap (newlib needs it; libcheshire has none).
# DROPPED (now real): libc_stubs.o (memcpy/memset/malloc/strtod... -> newlib),
#   libm_stubs.o (__trunctfdf2/frexp/scalbn/... -> libgcc/libm, zero-HI20 in 13.2),
#   syscalls.o (spike HTIF), --allow-multiple-definition.
#
# Output: quidditch_gemm.dram.elf, loaded by the gwaihir sim via the backdoor
# sim_mem path (tb dram_host_elf_preload, PRELMODE=4).
set -euo pipefail

# REAL IIS toolchain: 13.2.0 (zero absolute HI20 in libgcc/libc/libm).
RV=/usr/pack/riscv-1.0-kgf/riscv64-gcc-13.2.0/bin
PFX=$RV/riscv64-unknown-elf-
GCC=${PFX}gcc

IREE_SRC=/home/dankeller/Projects/Quidditch/iree
IREE_BUILD=/scratch/dankeller/snitch-compiler/iree-rv64-baremetal/build
LIBDIR=$IREE_BUILD/runtime/src/iree

PARENT=/scratch/dankeller/snitch-compiler/iree-rv64-host
EMITC=$PARENT/emitc
HERE=$PARENT/cheshire
cd "$HERE"

# gwaihir + cheshire trees (READ-only; from the scratch gwaihir checkout).
GW=/scratch/dankeller/snitch-compiler/gwaihir-phase2/gwaihir
CHS=$GW/.bender/git/checkouts/cheshire-09830518097f85c0
GW_GEN=$GW/.generated

CHS_SW=$CHS/sw
CHS_CRT0=$CHS_SW/lib/crt0.o
CHS_LIB=$CHS_SW/lib/libcheshire.a
CHS_LD_DIR=$CHS_SW/link

HOST_LD=${HOST_LD:-$HERE/dram_host.ld}

# Match Cheshire's march/mabi (newlib multilib + libcheshire ABI-compatible),
# medany + explicit-relocs (the way cheshire/sw/sw.mk:27 builds DRAM SW).
ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)

DEFS=(-DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 "-DIREE_TIME_NOW_FN={ return 0; }"
 -DIREE_DEVICE_SIZE_T=uint64_t -DPRIdsz=PRIu64)

INC=(-I$IREE_SRC/runtime/src -I$IREE_BUILD/runtime/src
 -I$IREE_SRC/third_party/flatcc/include -I$IREE_SRC/third_party/printf/src
 -I$PARENT -I$PARENT/transport -I$EMITC -I$HERE
 -I$GW_GEN
 -I$CHS_SW/include -I$CHS_SW/deps/printf)

CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${DEFS[@]}" "${INC[@]}")

echo "== [13.2.0] compiling cheshire bridge + HW shared region + host main =="
$GCC "${CFLAGS[@]}" -c $HERE/cheshire_bridge.c        -o cheshire_bridge.o
$GCC "${CFLAGS[@]}" -c $HERE/shared_region_cheshire.c -o shared_region.o
$GCC "${CFLAGS[@]}" -c $HERE/quidditch_gemm_main.c    -o quidditch_gemm_main.o

echo "== compiling EmitC native module =="
$GCC "${CFLAGS[@]}" -c $EMITC/gemm_square_emitc.c -o gemm_square_emitc.o

echo "== compiling cluster HAL + command stream (host side) =="
$GCC "${CFLAGS[@]}" -c $PARENT/transport/cluster_command_stream.c -o cluster_command_stream.o
$GCC "${CFLAGS[@]}" -c $PARENT/hal/cluster/cluster_allocator.c      -o cluster_allocator.o
$GCC "${CFLAGS[@]}" -c $PARENT/hal/cluster/cluster_command_buffer.c -o cluster_command_buffer.o
$GCC "${CFLAGS[@]}" -c $PARENT/hal/cluster/cluster_device.c         -o cluster_device.o

echo "== compiling IREE HAL module =="
MODHAL=$IREE_SRC/runtime/src/iree/modules/hal
$GCC "${CFLAGS[@]}" -c $MODHAL/module.c                 -o modhal_module.o
$GCC "${CFLAGS[@]}" -c $MODHAL/types.c                  -o modhal_types.o
$GCC "${CFLAGS[@]}" -c $MODHAL/debugging.c              -o modhal_debugging.o
$GCC "${CFLAGS[@]}" -c $MODHAL/utils/buffer_diagnostics.c -o modhal_buffer_diag.o

# Only genuinely-unprovided shims: pthread no-op mutexes (newlib has no pthread)
# and IREE-internal io file-handle symbols (never in any lib). NO libc/libm stubs.
STUBS="$EMITC/pthread_stubs.o $EMITC/iree_io_stubs.o"

CLUSTER_OBJS="cluster_command_stream.o cluster_allocator.o \
 cluster_command_buffer.o cluster_device.o \
 modhal_module.o modhal_types.o modhal_debugging.o modhal_buffer_diag.o"

IREE_LIBS="\
 $LIBDIR/vm/libiree_vm_impl.a \
 $LIBDIR/hal/libiree_hal_hal.a \
 $LIBDIR/hal/utils/libiree_hal_utils_platform_topology.a \
 $LIBDIR/base/libiree_base_base.a \
 $LIBDIR/base/threading/libiree_base_threading_threading.a \
 $LIBDIR/base/internal/libiree_base_internal_time.a \
 $LIBDIR/base/internal/libiree_base_internal_arena.a \
 $LIBDIR/base/internal/libiree_base_internal_atomic_slist.a"

echo "== linking DRAM ELF (cheshire crt0 + dram_host.ld + REAL newlib/libgcc) =="
# NO --allow-multiple-definition. Real newlib -lc provides memcpy/memset/malloc/
# strtod; libcheshire is an archive pulled only for the UART/clint members it
# uniquely defines (its memcpy/memset members are not pulled because newlib
# resolves those first). Real -lm/-lgcc provide __trunctfdf2/frexp/scalbn/pow...
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections \
  -T$HOST_LD -Wl,-L$CHS_LD_DIR -Wl,-L$HERE \
  $CHS_CRT0 \
  cheshire_bridge.o shared_region.o quidditch_gemm_main.o \
  gemm_square_emitc.o \
  $CLUSTER_OBJS \
  $STUBS \
  -Wl,--start-group $IREE_LIBS -lc -lm -lgcc $CHS_LIB -Wl,--end-group \
  -o quidditch_gemm.dram.elf

echo "== result =="
${PFX}size quidditch_gemm.dram.elf
file quidditch_gemm.dram.elf
echo "== verify ZERO absolute R_RISCV_HI20 in final ELF (DRAM reloc-safety) =="
${PFX}objdump -dr quidditch_gemm.dram.elf 2>/dev/null | grep -c R_RISCV_HI20 || true
echo "== entry =="
${PFX}readelf -h quidditch_gemm.dram.elf | grep -E "Entry"
echo "ELF: $HERE/quidditch_gemm.dram.elf"
