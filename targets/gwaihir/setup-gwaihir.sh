#!/usr/bin/env bash
# Prepare the pinned gwaihir SoC tree for a build: apply the co-sim patch, resolve
# the HW deps via bender, and wire the QCS-replayer firmware in. Idempotent -- safe
# to re-run; it resets the gwaihir submodule to its pinned commit first.
#
# Usage: targets/gwaihir/setup-gwaihir.sh
#   Requires: the gwaihir submodule initialized (git submodule update --init) and
#   `bender` on PATH. After this, build with: make -C targets/gwaihir/soc sw
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)   # targets/gwaihir
soc="$here/soc"                                       # the pinned gwaihir submodule

[ -e "$soc/.git" ] || {
  echo "error: gwaihir submodule not initialized -- run:" >&2
  echo "  git submodule update --init $soc" >&2
  exit 1
}
command -v bender >/dev/null || {
  echo "error: 'bender' not found on PATH (needed to resolve the gwaihir HW deps)." >&2
  exit 1
}

echo ">> reset gwaihir submodule to its pinned commit (clean slate)"
git -C "$soc" checkout -q .
git -C "$soc" clean -fdq -e /.bender

echo ">> apply the co-sim patch"
git -C "$soc" apply "$here/patches/gwaihir-cosim.patch"

echo ">> bender checkout (cheshire / cva6 / snitch_cluster per the tree's Bender.lock)"
( cd "$soc" && bender checkout )

echo ">> wire the QCS-replayer firmware into the tree"
QUIDDITCH_GWAIHIR_GEN="$soc" "$here/firmware/gwaihir/link_into_gwaihir_tree.sh" "$soc"

echo ">> gwaihir tree ready: $soc"
echo "   build with: make -C $soc sw   # -> sw/snitch/apps/qcs_replay/build/qcs_replay.elf"
