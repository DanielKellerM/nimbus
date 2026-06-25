# Host-side build patches

## `iree-verify-off.patch`
Guards the flatcc `iree_vm_BytecodeModuleDef_verify_as_root` call in
`iree/runtime/src/iree/vm/bytecode/verifier.c` behind
`#if IREE_VM_BYTECODE_VERIFICATION_ENABLE`. Upstream IREE gates the per-op
bytecode verifier (module.c) behind that flag but NOT the flatcc root verify —
this closes that gap (worth an upstream PR).

**Why:** the flatcc table-walk verify is O(table) and, on the bare-metal rv64
host VM under spike, never finished in 595s (it dominated context-create). With
this patch + `-DIREE_VM_BYTECODE_VERIFICATION_ENABLE=0`, the rv64 host VM runs the
full path (context-create → invoke → QCS record) in ~11s under spike. The deploy
module is self-produced (trusted), so skipping verification is safe; keep it ON in
host/CI builds.

**Apply (deploy/RTL build only):**
```
git -C iree apply ../runtime/host/patches/iree-verify-off.patch   # do not commit the submodule change
# then build the rv64 IREE libs with -DIREE_VM_BYTECODE_VERIFICATION_ENABLE=0
```
