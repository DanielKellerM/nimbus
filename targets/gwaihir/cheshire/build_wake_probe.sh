#!/usr/bin/env bash
# Build the minimal CVA6 wake probe (cva6_wake_probe.c): crt0 + the probe only --
# no IREE, no bridge, no shared_region, no QCS. text -> Cheshire SPM, tiny data/bss
# + stack -> L2-SPM via the SAME hybrid.ld the minimal host uses (generated from the
# SoC addrmap). Output: cva6_wake_probe.spm.elf, ready for PRELMODE=3 CHS_BINARY=.
# Set WAKE_POSITIVE_CONTROL=1 to append the shipped 32-bit store as an in-run A/B.
set -euo pipefail

usage() {
  cat >&2 <<EOF
usage: GW=<gwaihir build tree> [RV=<bin>] [BUILD_DIR=<dir>] [WAKE_POSITIVE_CONTROL=1] ${0##*/}
  GW         gwaihir build tree with .bender (cheshire checkout) + .generated (addrmap)  [required]
  RV         riscv64 gcc bin dir   [default: \$CHS_SW_GCC_BINROOT, else gcc-12.2.0]
  BUILD_DIR  .o/.elf output dir     [default: <script dir>/build]
EOF
  exit 2
}

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RV="${RV:-${CHS_SW_GCC_BINROOT:-/usr/pack/riscv-1.0-kgf/riscv64-gcc-12.2.0/bin}}"
PFX="$RV/riscv64-unknown-elf-"
GCC="${PFX}gcc"
[ -x "$GCC" ] || { echo "no riscv gcc at '$GCC' -- set RV" >&2; exit 1; }

: "${GW:?set GW to the gwaihir build tree (has .bender + .generated)}" || usage
CHS=$(ls -d "$GW"/.bender/git/checkouts/cheshire-*/ 2>/dev/null | head -1)
GW_GEN="$GW/.generated"
[ -n "$CHS" ] && [ -d "${CHS}sw" ] && [ -d "$GW_GEN" ] \
  || { echo "GW='$GW' has no cheshire checkout or .generated" >&2; exit 1; }
CHS_SW="${CHS}sw"

BUILD_DIR="${BUILD_DIR:-$HERE/build}"
mkdir -p "$BUILD_DIR"
HOST_LD="$BUILD_DIR/hybrid.ld"
OUT="$BUILD_DIR/cva6_wake_probe.spm.elf"

ARCH=(-march=rv64gc_zifencei -mabi=lp64d -mstrict-align -mcmodel=medany -mexplicit-relocs)
COMMON=(-O2 -g -ffunction-sections -fdata-sections -fno-builtin)
INC=(-I"$HERE" -I"$GW_GEN" -I"$CHS_SW/include" -I"$CHS_SW/deps/printf")
DEFS=()
[ "${WAKE_POSITIVE_CONTROL:-0}" = 1 ] && DEFS+=(-DWAKE_POSITIVE_CONTROL)
CFLAGS=("${ARCH[@]}" "${COMMON[@]}" "${INC[@]}" "${DEFS[@]}")

echo "== compiling wake probe, gcc $($GCC -dumpversion) =="
$GCC "${CFLAGS[@]}" -c "$HERE/cva6_wake_probe.c" -o "$BUILD_DIR/cva6_wake_probe.o"

# Same hybrid link script as the minimal host: .text -> Cheshire SPM (the only
# ExecuteRegion for host code), data/bss/stack -> L2-SPM, both from the addrmap.
$GCC -E -P -x c "-I$GW_GEN" "$HERE/hybrid.ld.in" -o "$HOST_LD"

echo "== linking $OUT (crt0 + hybrid.ld) =="
$GCC "${ARCH[@]}" -nostartfiles -Wl,--gc-sections \
  -T"$HOST_LD" -Wl,-L"$CHS_SW/link" \
  "$CHS_SW/lib/crt0.o" "$BUILD_DIR/cva6_wake_probe.o" \
  -Wl,--start-group -lc -lgcc "$CHS_SW/lib/libcheshire.a" -Wl,--end-group \
  -o "$OUT"

echo "== result =="
${PFX}size "$OUT"
# Prove the failing store is a SINGLE 64-bit sd to cl_clint_set (0x...211A0):
echo "== main disasm (expect one 'sd' to the cl_clint_set reg) =="
${PFX}objdump -d "$OUT" | sed -n '/<main>:/,/ret\|wfi/p'
echo "ELF: $OUT"
