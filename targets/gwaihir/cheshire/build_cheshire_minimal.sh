#!/usr/bin/env bash
# MINIMAL host build (the "size fix"): the IREE host VM is excised. Links only
# the QCS transport + Cheshire bridge + the hand-written dispatch driver, so the
# host .text drops from 137.6 KiB (VM, DRAM-resident) to ~10-15 KiB, resident in
# the 128 KiB Cheshire SPM via hybrid.ld (text->SPM, data->L2-SPM).
#
# Compare build_cheshire_dram.sh: this DROPS gemm_square_emitc.o, the whole
# cluster HAL (cluster_allocator/command_buffer/device.o), modhal_*.o, the
# pthread/io stubs, and the entire $IREE_LIBS group (vm/hal/base/threading/...).
# Kept: cheshire_bridge.o (_write/_sbrk), shared_region.o (HW L2-SPM region +
# doorbell), cluster_command_stream.o (qcs_writer). Newlib -lc for memset only.
#
# Output: quidditch_gemm_min.elf. Default link is hybrid.ld (text->Cheshire SPM
# 0x10000000, an executable+cached region; data/bss->L2-SPM 0x70040000). Set
# HOST_LD=dram_host.ld to A/B the DRAM path.
set -euo pipefail

RV=/usr/pack/riscv-1.0-kgf/riscv64-gcc-13.2.0/bin
PFX=$RV/riscv64-unknown-elf-
GCC=${PFX}gcc

IREE_SRC=/home/dankeller/Projects/Quidditch/iree
PARENT=/scratch/dankeller/snitch-compiler/iree-rv64-host
HERE=$PARENT/cheshire
cd "$HERE"

GW=${GW:-$(dirname "$IREE_SRC")/.gwaihir}
CHS=$GW/.bender/git/checkouts/cheshire-09830518097f85c0
GW_GEN=$GW/.generated

CHS_SW=$CHS/sw
CHS_CRT0=$CHS_SW/lib/crt0.o
CHS_LIB=$CHS_SW/lib/libcheshire.a
CHS_LD_DIR=$CHS_SW/link

HOST_LD=${HOST_LD:-$HERE/hybrid.ld}

ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)

# No IREE VM/HAL: only the transport headers + Cheshire includes are needed.
INC=(-I$PARENT -I$PARENT/transport -I$HERE -I$GW_GEN
     -I$CHS_SW/include -I$CHS_SW/deps/printf)

CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${INC[@]}")

echo "== [13.2.0] compiling minimal host (no IREE) =="
$GCC "${CFLAGS[@]}" -c $HERE/cheshire_bridge.c               -o cheshire_bridge.o
$GCC "${CFLAGS[@]}" -c $HERE/shared_region_cheshire.c        -o shared_region.o
$GCC "${CFLAGS[@]}" -c $PARENT/transport/cluster_command_stream.c -o cluster_command_stream.o
$GCC "${CFLAGS[@]}" -c $HERE/quidditch_gemm_minimal_main.c   -o quidditch_gemm_minimal_main.o

echo "== linking minimal ELF (crt0 + $HOST_LD + newlib) =="
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections \
  -T$HOST_LD -Wl,-L$CHS_LD_DIR -Wl,-L$HERE \
  $CHS_CRT0 \
  cheshire_bridge.o shared_region.o cluster_command_stream.o \
  quidditch_gemm_minimal_main.o \
  -Wl,--start-group -lc -lgcc $CHS_LIB -Wl,--end-group \
  -o quidditch_gemm_min.elf

echo "== result =="
${PFX}size quidditch_gemm_min.elf
${PFX}readelf -h quidditch_gemm_min.elf | grep -E "Entry"
echo "== .text/.bss placement (must be chsspm 0x10000000 / l2data 0x70040000) =="
${PFX}readelf -S quidditch_gemm_min.elf | grep -E "\.text|\.bss|\.misc|Name" | head
echo "ELF: $HERE/quidditch_gemm_min.elf"
