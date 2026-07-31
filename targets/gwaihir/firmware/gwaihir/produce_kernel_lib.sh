#!/usr/bin/env bash
# Produce the SPLIT iree+xdsl gemm kernel static library that the qcs_replay
# firmware links, FROM the vendored .mlir source -- reproducibly. Every tool path
# is derived from the Quidditch build's CMakeCache (the source of truth cmake
# already resolved), never hand-set, so a fresh clone can't pick the broken venv/
# xdsl-opt or a pulp-as-less toolchain. Output is the KERNEL_LIB_DIR that
# link_into_gwaihir_tree.sh wires into the gwaihir app's lib/.
#
#   QUIDDITCH_BUILD_DIR   Quidditch runtime build with CMakeCache.txt (required)
#   QUIDDITCH_CFG_HEADER  override the codegen cfg header (default: from the cache)
#   QUIDDITCH_COMPILE     quidditch-compile binary (else found under the build)
#   arg1 (in.mlir)        kernel source (default: the 64x64 4x4 gemm sample)
#   arg2 / KERNEL_LIB_DIR output dir (default: nimbus/.gwaihir-kernel-lib)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
nimbus_root="${NIMBUS_ROOT:-$(cd "$here/../../../.." && pwd)}"
quidditch_root="${QUIDDITCH_ROOT:-$nimbus_root/quidditch}"

build="${QUIDDITCH_BUILD_DIR:?set QUIDDITCH_BUILD_DIR to the Quidditch build dir (with CMakeCache.txt)}"
cache="$build/CMakeCache.txt"
[ -f "$cache" ] || { echo "ERROR: no CMakeCache.txt in $build" >&2; exit 1; }
cget() { sed -n "s/^$1[^=]*=//p" "$cache" | head -1; }

xdsl="${QUIDDITCH_XDSL_OPT:-$(cget XDSL_OPT_PATH)}"
tcr="${QUIDDITCH_TOOLCHAIN_ROOT:-$(cget QUIDDITCH_TOOLCHAIN_ROOT)}"
# The codegen cfg header (QUIDDITCH_L1_BASE etc.), NOT the firmware snitch_cluster_cfg.h.
cfg="${QUIDDITCH_CFG_HEADER:-$(cget QUIDDITCH_CLUSTER_CFG_HEADER)}"
[ -n "$xdsl" ] && [ -x "$xdsl" ] || { echo "ERROR: xdsl-opt unresolved ($xdsl) -- is this a Quidditch build cache?" >&2; exit 1; }
[ -d "$tcr/bin" ] || { echo "ERROR: toolchain root unresolved ($tcr)" >&2; exit 1; }
[ -f "$cfg" ] || { echo "ERROR: cluster cfg header unresolved ($cfg)" >&2; exit 1; }
grep -q 'QUIDDITCH_L1_BASE' "$cfg" || { echo "ERROR: $cfg lacks QUIDDITCH_L1_BASE -- wrong header; codegen would use the occamy default" >&2; exit 1; }

qc="${QUIDDITCH_COMPILE:-$(command -v quidditch-compile 2>/dev/null || true)}"
[ -n "$qc" ] && [ -x "$qc" ] || qc=$(find "$build" "$build/.." -maxdepth 4 -name quidditch-compile -type f 2>/dev/null | head -1)
[ -x "${qc:-}" ] || { echo "ERROR: quidditch-compile not found (set QUIDDITCH_COMPILE)" >&2; exit 1; }

mlir="${1:-$quidditch_root/runtime/samples/gemm_square/gemm_square_4x4.mlir}"
[ -f "$mlir" ] || { echo "ERROR: kernel source not found: $mlir" >&2; exit 1; }
outdir="${2:-${KERNEL_LIB_DIR:-$nimbus_root/.gwaihir-kernel-lib}}"
mkdir -p "$outdir"
obj="$outdir/gemm_square_kernel.o"
lib="$outdir/libgemm_square_kernel.a"
ar="${LLVM_AR:-$tcr/bin/llvm-ar}"; [ -x "$ar" ] || ar=$(command -v llvm-ar)

echo "producing $lib"
echo "  from   : $mlir"
echo "  tools  : quidditch-compile=$qc"
echo "           xdsl-opt=$xdsl  toolchain=$tcr"
"$qc" "$mlir" --iree-input-type=auto --iree-input-demote-f64-to-f32=0 \
  --iree-hal-target-backends=quidditch \
  --iree-quidditch-xdsl-opt-path="$xdsl" \
  --iree-quidditch-toolchain-root="$tcr" \
  --iree-quidditch-cluster-cfg-header="$cfg" \
  --iree-quidditch-static-library-output-path="$obj"
"$ar" rcs "$lib" "$obj"

# Gate: the query symbol + the $xdsl_kernel microkernels (absent = a scalar xdsl-opt fallback).
"$tcr/bin/llvm-nm" "$lib" 2>/dev/null | grep -q 'quidditch_gemm64_dispatch_0_library_query' \
  || { echo "ERROR: produced .a lacks quidditch_gemm64_dispatch_0_library_query" >&2; exit 1; }
kern=$("$tcr/bin/llvm-nm" "$lib" 2>/dev/null | grep -c 'xdsl_kernel' || true)
[ "${kern:-0}" -gt 0 ] || { echo "ERROR: no \$xdsl_kernel microkernels -- xdsl-opt fell back to scalar (wrong .venv?)" >&2; exit 1; }
echo "  OK: \$xdsl_kernel x$kern (SSR/FREP streaming present)"
echo "KERNEL_LIB_DIR=$outdir"
echo "  wire it in: KERNEL_LIB_DIR=$outdir ./link_into_gwaihir_tree.sh"
