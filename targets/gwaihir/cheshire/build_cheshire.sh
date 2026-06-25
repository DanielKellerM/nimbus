#!/usr/bin/env bash
# Build the IREE EmitC host VM runner as a Cheshire/CVA6 baremetal SW binary
# for the gwaihir co-sim (host-device split Phase 2). Reuses the prebuilt IREE
# rv64 objects/libs from the spike build, but links against Cheshire's crt0 +
# linker script + libcheshire (UART console, clint) instead of the spike
# HTIF/syscalls glue. Produces quidditch_gemm.dram.elf loadable by the gwaihir
# sim (PRELMODE=3, CHS_BINARY=...).
set -euo pipefail

RV=/usr/pack/riscv-1.0-kgf/riscv64-gcc-12.2.0/bin
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

# Cheshire SW build vars (mirror sw/sw.mk).
CHS_SW=$CHS/sw
CHS_CRT0=$CHS_SW/lib/crt0.o
CHS_LIB=$CHS_SW/lib/libcheshire.a
CHS_LD_DIR=$CHS_SW/link
# Host links into L2-SPM tile 1 (0x70100000) via our l2host.ld: the gwaihir sim
# cannot JTAG-write DRAM (0x80000000) -- "System bus error" -- but L2 SPM IS
# JTAG-writable. -L$CHS_LD_DIR is still needed for the common.ldh / cheshire_addrs
# the script INCLUDEs.
LINKMODE=${LINKMODE:-l2host}
HOST_LD=${HOST_LD:-$HERE/${LINKMODE}.ld}

# Match Cheshire's march/mabi so newlib multilib + libcheshire are ABI-compatible.
ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
# Keep ffreestanding off (we DO use newlib libc/libm here) but no startfiles
# (Cheshire crt0 provides _start).
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)

DEFS=(-DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 "-DIREE_TIME_NOW_FN={ return 0; }"
 -DIREE_DEVICE_SIZE_T=uint64_t -DPRIdsz=PRIu64)

INC=(-I$IREE_SRC/runtime/src -I$IREE_BUILD/runtime/src
 -I$IREE_SRC/third_party/flatcc/include -I$IREE_SRC/third_party/printf/src
 -I$PARENT -I$PARENT/transport -I$EMITC -I$HERE
 # gwaihir generated addrmap (GW_L2_SPM_BASE_ADDR, cluster scratch/clint regs)
 -I$GW_GEN
 # cheshire SW headers (regs/cheshire.h, dif/uart.h, dif/clint.h, params.h)
 -I$CHS_SW/include -I$CHS_SW/deps/printf)

CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${DEFS[@]}" "${INC[@]}")

echo "== compiling cheshire bridge + HW shared region + host main =="
$GCC "${CFLAGS[@]}" -c $HERE/cheshire_bridge.c        -o cheshire_bridge.o
$GCC "${CFLAGS[@]}" -c $HERE/shared_region_cheshire.c -o shared_region.o
$GCC "${CFLAGS[@]}" -c $HERE/quidditch_gemm_main.c    -o quidditch_gemm_main.o

echo "== compiling EmitC native module (reuse from spike sources) =="
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

# Kept spike stub objects: libc helpers (memcpy/memset/malloc family), pthread
# no-ops, IREE io stubs, AND libm_stubs (medany reimpls of frexp/scalbn/
# __trunctfdf2 -- newlib's medlow versions reloc-truncate against .LC0 at the
# DRAM 0x80000000 base). DROPPED: syscalls.o (HTIF _write/_exit/_sbrk collide
# with cheshire crt0/_putchar and our bridge), spike crt0.o (use cheshire crt0).
STUBS="$EMITC/libc_stubs.o $EMITC/libm_stubs.o $EMITC/pthread_stubs.o $EMITC/iree_io_stubs.o"

# NOTE: shared_region.o (our HW-wired version) is compiled above and listed
# explicitly in the link; do NOT repeat it here.
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

echo "== linking ELF (cheshire crt0 + ${LINKMODE}.ld + libcheshire) =="
# Link order: cheshire crt0 FIRST (provides _start/_exit/_putchar callers),
# our objects, then stubs, then IREE libs in a group, then libcheshire + newlib.
# --allow-multiple-definition: our medany memcpy/memset (libc_stubs.o) precede
# newlib's, so the linker takes ours (medany-safe at DRAM 0x80000000).
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections -Wl,--allow-multiple-definition \
  -T$HOST_LD -Wl,-L$CHS_LD_DIR -Wl,-L$HERE \
  $CHS_CRT0 \
  cheshire_bridge.o shared_region.o quidditch_gemm_main.o \
  gemm_square_emitc.o \
  $CLUSTER_OBJS \
  $STUBS \
  -Wl,--start-group $IREE_LIBS $CHS_LIB -lc -lm -lgcc -Wl,--end-group \
  -o quidditch_gemm.${LINKMODE}.elf

echo "== result =="
${PFX}size quidditch_gemm.${LINKMODE}.elf
file quidditch_gemm.${LINKMODE}.elf
echo "ELF: $HERE/quidditch_gemm.${LINKMODE}.elf"
