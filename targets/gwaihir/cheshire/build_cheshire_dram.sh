#!/usr/bin/env bash
# No-stubs DRAM host build (host-device split): the IREE EmitC host VM runner as a
# Cheshire/CVA6 baremetal binary running entirely from external DRAM, linked against
# the REAL newlib libc/libm + libgcc -- no hand-stubbed libc/libm, no
# --allow-multiple-definition. Output: quidditch_gemm.dram.elf (tb PRELMODE=4).
#
# RV defaults to gcc-13.2.0 on purpose: its rv64/lp64d libgcc/libc/libm have ZERO
# absolute R_RISCV_HI20 relocs, so they link cleanly with -mcmodel=medany at the DRAM
# base 0x80000000 -- gcc-12.2.0's libgcc has 130 and truncates. Do not lower it.
#
# Kept shims (genuinely unprovided, not workarounds): pthread_stubs.o (newlib has no
# pthread), iree_io_stubs.o (IREE-internal file-handle symbols), cheshire_bridge.c
# (_write->UART, _sbrk over a static heap).
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: GW=<..> IREE_SRC=<..> IREE_BUILD=<..> EMITC=<..> [RV=<bin>] [BUILD_DIR=<dir>] [HOST_LD=<ld>] ${0##*/}
  GW         gwaihir build tree with .bender (cheshire checkout) + .generated   [required]
  IREE_SRC   IREE source tree (headers)                                          [default: \$QUIDDITCH_ROOT/iree]
  IREE_BUILD IREE rv64 baremetal build tree (the vm/hal/base archives)           [required]
  EMITC      dir with the generated gemm_square_emitc.c + prebuilt *_stubs.o     [required]
  RV         riscv64 gcc bin dir   [default: gcc-13.2.0 -- see the reloc note above; do not lower]
  BUILD_DIR  .o/.elf output dir     [default: <script dir>/build]
  HOST_LD    link script           [default: dram_host.ld beside this script]
EOF
  exit 2
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # this repo's cheshire host sources
TARGET_DIR="$(dirname "$HERE")"                         # targets/gwaihir (transport/, hal/)
NIMBUS_ROOT="$(cd "$TARGET_DIR/../.." && pwd)"

RV="${RV:-/usr/pack/riscv-1.0-kgf/riscv64-gcc-13.2.0/bin}"
PFX="$RV/riscv64-unknown-elf-"
GCC="${PFX}gcc"
[ -x "$GCC" ] || { echo "no riscv gcc at '$GCC' -- set RV" >&2; exit 1; }

: "${GW:?set GW to the gwaihir build tree (has .bender + .generated)}" || usage
IREE_SRC="${IREE_SRC:-${QUIDDITCH_ROOT:-$NIMBUS_ROOT/quidditch}/iree}"
: "${IREE_BUILD:?set IREE_BUILD to the IREE rv64 baremetal build tree}" || usage
: "${EMITC:?set EMITC to the generated EmitC dir (gemm_square_emitc.c + *_stubs.o)}" || usage
[ -d "$IREE_SRC/runtime/src" ] || { echo "IREE_SRC='$IREE_SRC' has no runtime/src" >&2; exit 1; }

CHS=$(ls -d "$GW"/.bender/git/checkouts/cheshire-*/ 2>/dev/null | head -1)
GW_GEN="$GW/.generated"
[ -n "$CHS" ] && [ -d "${CHS}sw" ] && [ -d "$GW_GEN" ] \
  || { echo "GW='$GW' has no cheshire checkout or .generated" >&2; exit 1; }
CHS_SW="${CHS}sw"
LIBDIR="$IREE_BUILD/runtime/src/iree"

BUILD_DIR="${BUILD_DIR:-$HERE/build}"
mkdir -p "$BUILD_DIR"
HOST_LD="${HOST_LD:-$HERE/dram_host.ld}"
OUT="$BUILD_DIR/quidditch_gemm.dram.elf"

# Match Cheshire's march/mabi + medany/explicit-relocs (cheshire/sw/sw.mk DRAM build).
ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)
DEFS=(-DIREE_PLATFORM_GENERIC=1 -DIREE_FILE_IO_ENABLE=0 "-DIREE_TIME_NOW_FN={ return 0; }"
 -DIREE_DEVICE_SIZE_T=uint64_t -DPRIdsz=PRIu64)
INC=(-I"$IREE_SRC/runtime/src" -I"$IREE_BUILD/runtime/src"
 -I"$IREE_SRC/third_party/flatcc/include" -I"$IREE_SRC/third_party/printf/src"
 -I"$HERE" -I"$TARGET_DIR/transport" -I"$TARGET_DIR/hal/cluster" -I"$EMITC" -I"$GW_GEN"
 -I"$CHS_SW/include" -I"$CHS_SW/deps/printf")
CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${DEFS[@]}" "${INC[@]}")

o() { echo "$BUILD_DIR/$1"; }
echo "== compiling DRAM host (IREE VM), gcc $($GCC -dumpversion) =="
$GCC "${CFLAGS[@]}" -c "$HERE/cheshire_bridge.c"                          -o "$(o cheshire_bridge.o)"
$GCC "${CFLAGS[@]}" -c "$HERE/shared_region_cheshire.c"                  -o "$(o shared_region.o)"
$GCC "${CFLAGS[@]}" -c "$HERE/quidditch_gemm_main.c"                     -o "$(o quidditch_gemm_main.o)"
$GCC "${CFLAGS[@]}" -c "$EMITC/gemm_square_emitc.c"                      -o "$(o gemm_square_emitc.o)"
$GCC "${CFLAGS[@]}" -c "$TARGET_DIR/transport/cluster_command_stream.c"  -o "$(o cluster_command_stream.o)"
$GCC "${CFLAGS[@]}" -c "$TARGET_DIR/hal/cluster/cluster_allocator.c"      -o "$(o cluster_allocator.o)"
$GCC "${CFLAGS[@]}" -c "$TARGET_DIR/hal/cluster/cluster_command_buffer.c" -o "$(o cluster_command_buffer.o)"
$GCC "${CFLAGS[@]}" -c "$TARGET_DIR/hal/cluster/cluster_device.c"         -o "$(o cluster_device.o)"
MODHAL="$IREE_SRC/runtime/src/iree/modules/hal"
$GCC "${CFLAGS[@]}" -c "$MODHAL/module.c"                    -o "$(o modhal_module.o)"
$GCC "${CFLAGS[@]}" -c "$MODHAL/types.c"                     -o "$(o modhal_types.o)"
$GCC "${CFLAGS[@]}" -c "$MODHAL/debugging.c"                 -o "$(o modhal_debugging.o)"
$GCC "${CFLAGS[@]}" -c "$MODHAL/utils/buffer_diagnostics.c" -o "$(o modhal_buffer_diag.o)"

STUBS="$EMITC/pthread_stubs.o $EMITC/iree_io_stubs.o"
CLUSTER_OBJS="$(o cluster_command_stream.o) $(o cluster_allocator.o) $(o cluster_command_buffer.o) $(o cluster_device.o) $(o modhal_module.o) $(o modhal_types.o) $(o modhal_debugging.o) $(o modhal_buffer_diag.o)"
IREE_LIBS="\
 $LIBDIR/vm/libiree_vm_impl.a \
 $LIBDIR/hal/libiree_hal_hal.a \
 $LIBDIR/hal/utils/libiree_hal_utils_platform_topology.a \
 $LIBDIR/base/libiree_base_base.a \
 $LIBDIR/base/threading/libiree_base_threading_threading.a \
 $LIBDIR/base/internal/libiree_base_internal_time.a \
 $LIBDIR/base/internal/libiree_base_internal_arena.a \
 $LIBDIR/base/internal/libiree_base_internal_atomic_slist.a"

echo "== linking $OUT (crt0 + ${HOST_LD##*/} + REAL newlib/libgcc) =="
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections \
  -T"$HOST_LD" -Wl,-L"$CHS_SW/link" -Wl,-L"$HERE" \
  "$CHS_SW/lib/crt0.o" \
  "$(o cheshire_bridge.o)" "$(o shared_region.o)" "$(o quidditch_gemm_main.o)" \
  "$(o gemm_square_emitc.o)" \
  $CLUSTER_OBJS \
  $STUBS \
  -Wl,--start-group $IREE_LIBS -lc -lm -lgcc "$CHS_SW/lib/libcheshire.a" -Wl,--end-group \
  -o "$OUT"

echo "== result =="
${PFX}size "$OUT"
echo "== absolute R_RISCV_HI20 count (DRAM reloc-safety; must be 0) =="
${PFX}objdump -dr "$OUT" 2>/dev/null | grep -c R_RISCV_HI20 || true
${PFX}readelf -h "$OUT" | grep -E "Entry"
echo "ELF: $OUT"
