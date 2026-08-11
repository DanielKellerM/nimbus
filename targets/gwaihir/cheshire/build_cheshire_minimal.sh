#!/usr/bin/env bash
# Minimal QCS host build (the "size fix"): links only the QCS transport + Cheshire
# bridge + the hand-written dispatch driver -- no IREE VM/HAL. text -> Cheshire SPM,
# data/bss -> L2-SPM via hybrid.ld (generated here from the SoC addrmap). Output:
# quidditch_gemm_min.elf. Set HOST_LD=<ld> to A/B a different link script.
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: GW=<gwaihir build tree> [RV=<bin>] [BUILD_DIR=<dir>] [HOST_LD=<ld>] ${0##*/}
  GW         gwaihir build tree with .bender (cheshire checkout) + .generated (addrmap)   [required]
  RV         riscv64 gcc bin dir   [default: \$CHS_SW_GCC_BINROOT, else riscv64-unknown-elf-gcc on PATH; use gcc-12.2.0 to match libcheshire's LTO]
  BUILD_DIR  .o/.elf output dir     [default: <script dir>/build]
  HOST_LD    link script           [default: the hybrid.ld generated from the addrmap]
EOF
  exit 2
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # this repo's cheshire host sources
TARGET_DIR="$(dirname "$HERE")"                         # targets/gwaihir (transport/ lives here)

RV="${RV:-${CHS_SW_GCC_BINROOT:-$(command -v riscv64-unknown-elf-gcc 2>/dev/null | sed 's,/[^/]*$,,')}}"
PFX="$RV/riscv64-unknown-elf-"
GCC="${PFX}gcc"
[ -x "$GCC" ] || { echo "no riscv gcc at '$GCC' -- set RV" >&2; exit 1; }

[ -n "${GW:-}" ] || usage
CHS=$(ls -d "$GW"/.bender/git/checkouts/cheshire-*/ 2>/dev/null | head -1)
GW_GEN="$GW/.generated"
[ -n "$CHS" ] && [ -d "${CHS}sw" ] && [ -d "$GW_GEN" ] \
  || { echo "GW='$GW' has no cheshire checkout or .generated" >&2; exit 1; }
CHS_SW="${CHS}sw"

BUILD_DIR="${BUILD_DIR:-$HERE/build}"
mkdir -p "$BUILD_DIR"
HOST_LD="${HOST_LD:-$BUILD_DIR/hybrid.ld}"
OUT="$BUILD_DIR/quidditch_gemm_min.elf"

ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)
INC=(-I"$HERE" -I"$TARGET_DIR/transport" -I"$GW_GEN" -I"$CHS_SW/include" -I"$CHS_SW/deps/printf")
CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${INC[@]}")

echo "== compiling minimal host (no IREE), gcc $($GCC -dumpversion) =="
$GCC "${CFLAGS[@]}" -c "$HERE/cheshire_bridge.c"                        -o "$BUILD_DIR/cheshire_bridge.o"
$GCC "${CFLAGS[@]}" -c "$HERE/shared_region_cheshire.c"                -o "$BUILD_DIR/shared_region.o"
$GCC "${CFLAGS[@]}" -c "$TARGET_DIR/transport/cluster_command_stream.c" -o "$BUILD_DIR/cluster_command_stream.o"
$GCC "${CFLAGS[@]}" -c "$HERE/quidditch_gemm_minimal_main.c"           -o "$BUILD_DIR/quidditch_gemm_minimal_main.o"

# Generate the hybrid link script from the SoC addrmap (single source of truth with the firmware).
if [ "$HOST_LD" = "$BUILD_DIR/hybrid.ld" ]; then
  $GCC -E -P -x c "-I$GW_GEN" "$HERE/hybrid.ld.in" -o "$HOST_LD"
fi

echo "== linking $OUT (crt0 + ${HOST_LD##*/} + newlib) =="
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections \
  -T"$HOST_LD" -Wl,-L"$CHS_SW/link" -Wl,-L"$HERE" \
  "$CHS_SW/lib/crt0.o" \
  "$BUILD_DIR/cheshire_bridge.o" "$BUILD_DIR/shared_region.o" \
  "$BUILD_DIR/cluster_command_stream.o" "$BUILD_DIR/quidditch_gemm_minimal_main.o" \
  -Wl,--start-group -lc -lgcc "$CHS_SW/lib/libcheshire.a" -Wl,--end-group \
  -o "$OUT"

echo "== result =="
${PFX}size "$OUT"
${PFX}readelf -hS "$OUT" | grep -E "Entry|\.text|\.bss|\.misc|Name"
echo "ELF: $OUT"

# L2-SPM layout guard: host + QCS/firmware share one aperture carved by hand; assert the
# host L2 footprint stays clear of the device zone + return-code page (the l2spm-overlap
# class). Aperture derived from the same addrmap as hybrid.ld. Set GW_FW_ELF=<qcs_replay.elf>
# to also cross-check the firmware image. chimera-sdk check_section_overlaps.py analog.
if [ "$HOST_LD" = "$BUILD_DIR/hybrid.ld" ]; then
  echo "== L2-SPM layout guard =="
  _l2() { printf '#include "gw_raw_addrmap.h"\n%s\n' "$1" | $GCC -E -P -x c "-I$GW_GEN" - | tail -1; }
  L2BASE=$(( $(_l2 'GW_L2_SPM_BASE_ADDR(0)') )); L2SIZE=$(( $(_l2 'GW_L2_SPM_TOTAL_SIZE') ))
  # Host window = the l2data region hybrid.ld actually used (itself addrmap-derived); reserve
  # the rest of the aperture so any host section outside l2data trips the guard -- the layout
  # offsets are read from the linker script, not re-typed here.
  _ld=$(tr '\n' ' ' < "$HOST_LD")
  L2O=$(( $(sed -n 's/.*l2data[^:]*:[[:space:]]*ORIGIN[[:space:]]*=[[:space:]]*\([^,]*\),.*/\1/p' <<<"$_ld") ))
  L2END=$(( L2O + $(sed -n 's/.*l2data[^:]*:.*LENGTH[[:space:]]*=[[:space:]]*\([^}]*\)}.*/\1/p' <<<"$_ld") ))
  python3 "$HERE/check_l2_overlap.py" "$OUT" ${GW_FW_ELF:+"$GW_FW_ELF"} \
    --readelf "${PFX}readelf" --aperture "$L2BASE:$L2SIZE" \
    --reserve "$L2BASE:$((L2O-L2BASE)):qcs-device-zone" \
    --reserve "$L2END:$((L2BASE+L2SIZE-L2END)):host-reserved-top" \
    || { echo "!! L2-SPM layout guard FAILED (see overlap above)"; exit 1; }
fi
