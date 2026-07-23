#!/usr/bin/env bash
# Copyright 2026 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Wire the QCS-replayer firmware into a gwaihir snitch-app dir so the RTL build
# compiles the repo's VERSIONED source of truth -- not a hand-maintained copy.
#
# The canonical sources live here (targets/gwaihir/firmware/gwaihir/) and in
# targets/gwaihir/transport/; the shared executable-ABI header comes from the
# quidditch submodule. This script (re-)creates
#   <gwaihir>/sw/snitch/apps/qcs_replay/{src/*, app.mk}
# as symlinks back to them, so an edit in the repo is what the sim builds. Pure
# generated artifacts (lib/ = the SPLIT iree+xdsl kernel .o/.a, build/) are left
# to their own producers and are NOT touched.
#
# Usage:
#   ./link_into_gwaihir_tree.sh [GWAIHIR_TREE]
#     GWAIHIR_TREE  gwaihir root (default: $QUIDDITCH_GWAIHIR_GEN, else <repo>/.gwaihir)
#   LINK_MODE=copy  ./link_into_gwaihir_tree.sh   # copy instead of symlink (if a
#                                                 # build step drops symlinks)
#   KERNEL_LIB_DIR=<dir> ...                       # also link the SPLIT kernel lib
#                                                 # (libgemm_square_kernel.a + .h)
# Idempotent: re-run any time to re-establish the links.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
nimbus_root="${NIMBUS_ROOT:-$(cd "$here/../../../.." && pwd)}"
quidditch_root="${QUIDDITCH_ROOT:-$nimbus_root/quidditch}"
fw_dir="$here"                                            # firmware sources (this dir)
tp_dir="$nimbus_root/targets/gwaihir/transport"          # QCS ABI + reader (canonical)
rt_dir="$quidditch_root/runtime/runtime/src/Quidditch"   # shared executable-ABI header (submodule)

gw="${1:-${QUIDDITCH_GWAIHIR_GEN:-$nimbus_root/.gwaihir}}"
mode="${LINK_MODE:-symlink}"

[ -d "$gw/sw/snitch/apps" ] || {
  echo "ERROR: '$gw/sw/snitch/apps' not found." >&2
  echo "  Pass the gwaihir tree as arg 1, or set QUIDDITCH_GWAIHIR_GEN, or fix the .gwaihir symlink." >&2
  exit 1
}

app_dir="$gw/sw/snitch/apps/qcs_replay"
src_dir="$app_dir/src"
mkdir -p "$src_dir" "$app_dir/lib"

# Canonical sources, by home dir. app.mk (the build fragment) is versioned too.
fw_srcs="main.c qcs_replay.c qcs_replay.h qcs_kernel_abi.h qcs_world_comm.c quidditch_snrt_exports.c"
tp_srcs="cluster_command_stream.c cluster_command_stream.h"
rt_srcs="quidditch_executable_abi.h"

wire() { # <abs-source> <dest>
  [ -f "$1" ] || { echo "ERROR: missing canonical source: $1" >&2; exit 1; }
  rm -f "$2"
  if [ "$mode" = copy ]; then cp "$1" "$2"; else ln -s "$1" "$2"; fi
}

for f in $fw_srcs; do wire "$fw_dir/$f" "$src_dir/$f"; done
for f in $tp_srcs; do wire "$tp_dir/$f" "$src_dir/$f"; done
for f in $rt_srcs; do wire "$rt_dir/$f" "$src_dir/$f"; done
wire "$fw_dir/app.mk" "$app_dir/app.mk"

# Optional: the SPLIT iree+xdsl kernel static-lib (a generated artifact).
if [ -n "${KERNEL_LIB_DIR:-}" ]; then
  for f in libgemm_square_kernel.a gemm_square_kernel.h gemm_square_kernel.o; do
    [ -f "$KERNEL_LIB_DIR/$f" ] && wire "$KERNEL_LIB_DIR/$f" "$app_dir/lib/$f"
  done
  echo "linked SPLIT kernel lib from $KERNEL_LIB_DIR"
fi

echo "Wired qcs_replay firmware ($mode) into $app_dir"
echo "  src/  -> $fw_dir + $tp_dir  (versioned source of truth)"
echo "  app.mk-> $fw_dir/app.mk"
echo "  lib/  -> populate with the SPLIT iree+xdsl kernel (KERNEL_LIB_DIR=... or the codegen build); gitignored/generated"
