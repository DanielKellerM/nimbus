# gwaihir deployment patches

Two patch targets, applied to *external* trees the build wires into (neither is
committed back into its submodule):

## `gwaihir-cosim.patch` → the `soc/` submodule
Applies on the pinned gwaihir submodule (`targets/gwaihir/soc` @ `4eac10a`). Carries
the co-sim bring-up mods on top of upstream: the 2×2/mini NoC cfg
(`cfg/mini_gwaihir_noc.yml`), the SoC/tile/pkg RTL (`hw/cheshire_tile.sv`,
`hw/cluster_tile.sv`, `hw/gwaihir_pkg.sv`, `hw/gwaihir_top.sv`, `hw/mem_tile.sv`), the
phase-2 testbench (`target/sim/{src/tb_gwaihir_top.sv,include/tb_gwaihir_tasks.svh,
src/fixture_gwaihir_top.sv}` + the `vcs`/`vsim` `OFFLOAD_IMAGE` plusarg), the snitch
start shim, `sw/sw.mk`, the addrmap/linker single-sourcing (the `Makefile` `SN_CFG`
recipe deriving the cluster/L2-SPM/DRAM apertures from the NoC yaml, `cfg/snitch_cluster.json`,
and `sw/snitch/runtime/impl/memory.ld` → `memory.ld.in` deriving ORIGIN/LENGTH from
`gw_raw_addrmap.h`), and the cheshire seed/offload test hosts. The testbench
carries all six preload arms: 0 JTAG · 1 serial-link · 2 UART · 3 hybrid host ·
4 DRAM host · 5 headless offload (PRELMODE=5, no host). HW deps
(cheshire/cva6/snitch_cluster) come from `soc/Bender.lock` via `bender checkout`, not
this patch.

**Provenance — regenerate, don't hand-edit.** This patch is a build artifact of a
single canonical branch, not a hand-maintained file: the merged co-sim branch on the
gwaihir fork (`DanielKellerM/gwaihir`, `merge/cosim-both-final`, base `4eac10a`),
which unifies the DRAM-host line and the PRELMODE=5 headless line that had drifted
apart (a patch-vs-fork-branch split). To update, commit to that branch and regen:
```
git -C <gwaihir-fork> diff 4eac10a..merge/cosim-both-final \
  > <nimbus>/targets/gwaihir/patches/gwaihir-cosim.patch
```

Normally you don't apply this by hand — `../setup-gwaihir.sh` does it. Manually (from
the nimbus root; the patch path must be absolute since `git -C` changes directory):
```
git submodule update --init targets/gwaihir/soc
git -C targets/gwaihir/soc apply "$PWD/targets/gwaihir/patches/gwaihir-cosim.patch"
( cd targets/gwaihir/soc && bender checkout )
```
Verified: applies clean to the pinned `soc/` @ `4eac10a` and reconstructs the co-sim
tree byte-identically.

## `iree-verify-off.patch` → the IREE tree (Quidditch submodule)
Guards the flatcc `iree_vm_BytecodeModuleDef_verify_as_root` call in
`iree/runtime/src/iree/vm/bytecode/verifier.c` behind
`#if IREE_VM_BYTECODE_VERIFICATION_ENABLE`. Upstream gates the per-op verifier
behind that flag but not the flatcc root verify — this closes the gap (worth an
upstream PR).

**Why:** the flatcc table-walk verify is O(table) and, on the bare-metal rv64 host
VM under spike, never finished in 595s (it dominated context-create). With this
patch + `-DIREE_VM_BYTECODE_VERIFICATION_ENABLE=0`, the rv64 host VM runs the full
path (context-create → invoke → QCS record) in ~11s. The deploy module is
self-produced (trusted), so skipping verification is safe; keep it ON in host/CI.

Apply to the IREE tree in the pinned Quidditch submodule (do not commit the
submodule change):
```
git -C quidditch/iree apply <nimbus>/targets/gwaihir/patches/iree-verify-off.patch
# then build the rv64 IREE libs with -DIREE_VM_BYTECODE_VERIFICATION_ENABLE=0
```
