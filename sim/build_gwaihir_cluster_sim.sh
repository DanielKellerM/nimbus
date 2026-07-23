#!/bin/bash
# Build a standalone Verilator sim of the GWAIHIR snitch_cluster (rev 06426b0) for
# the autotuner harness -- isolated cluster cycles, no SoC/NoC. See the plan in
# ~/.claude/plans. Reuses the gwaihir tree's bender cache; needs verilator>=5.046.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
GW=$(readlink -f "$REPO/.gwaihir")                                  # -> /scratch gwaihir tree
GWSC=$GW/.bender/git/checkouts/snitch_cluster-c03065787076a51c     # gwaihir's snitch_cluster rev
CFG=${CFG:-$GW/cfg/snitch_cluster.json}                            # gwaihir cluster cfg (nr_clusters:4)
LLVM=${LLVM:-/usr/scratch2/vulcano/colluca/tools/riscv32-snitch-llvm-almalinux8-15.0.0-snitch-0.5.0/bin}
VLT=${VLT:-$HOME/opt/verilator-5.046/bin/verilator}
OCC_SVH=$REPO/snitch_cluster/hw/generated/snitch_cluster_addrmap.svh  # source of peripheral reg offsets
export PATH=$REPO/.venv/bin:$LLVM:$PATH                            # clustergen needs json5/mako/hjson

cd "$GWSC"
# bender deps from the gwaihir tree's cached db (no network)
mkdir -p .bender/git
[ -e .bender/git/db ] || ln -s "$GW/.bender/git/db" .bender/git/db
bender checkout

# RTL gen (clustergen + bootrom) for the gwaihir cfg, snitch LLVM wired
make rtl CFG_OVERRIDE="$CFG" \
  SN_RISCV_CC="$LLVM/clang" SN_RISCV_LD="$LLVM/ld.lld" \
  SN_RISCV_OBJDUMP="$LLVM/llvm-objdump" SN_RISCV_OBJCOPY="$LLVM/llvm-objcopy"

# peakrdl 1.5.0 raw-header does not recurse into the peripheral regblock; inject the
# (identical) peripheral reg offsets from the committed occamy svh, ifndef-guarded.
SVH=$GWSC/hw/generated/snitch_cluster_addrmap.svh
have=$(grep -oE '`define [A-Z0-9_]+' "$SVH" | awk '{print $2}' | sort -u)
grep -E '`define SNITCH_CLUSTER_PERIPHERAL_REG_' "$OCC_SVH" \
  | awk -v h="$have" 'BEGIN{split(h,a," ");for(i in a)H[a[i]]=1} !H[$2]{print "`ifndef "$2; print $0; print "`endif"}' \
  > /tmp/gw_periph_guarded.svh
ln=$(grep -nE 'endif' "$SVH" | tail -1 | cut -d: -f1)
{ head -n $((ln-1)) "$SVH"; cat /tmp/gw_periph_guarded.svh; tail -n +"$ln" "$SVH"; } > /tmp/gw_svh.new
cp /tmp/gw_svh.new "$SVH"

# Verilate (your verilator-5.046; the snitch flow's -DASSERTS_OFF + Wno-* are in verilator.mk)
make verilator CFG_OVERRIDE="$CFG" \
  SN_RISCV_CC="$LLVM/clang" SN_RISCV_LD="$LLVM/ld.lld" \
  SN_RISCV_OBJDUMP="$LLVM/llvm-objdump" SN_RISCV_OBJCOPY="$LLVM/llvm-objcopy" \
  SN_VLT="$VLT" SN_VERILATOR_SEPP= SN_VLT_JOBS="${SN_VLT_JOBS:-32}"

echo "gwaihir cluster sim: $GWSC/target/sim/build/bin/snitch_cluster.vlt"
