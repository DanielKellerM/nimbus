# gwaihir device-half: rv32 QCS replayer firmware

Cluster-side (rv32 Snitch) firmware for the Phase-2 host-device split on gwaihir.
The Cheshire/CVA6 host writes a `qcs_job_descriptor_t` + command stream into L2
SPM and rings the cluster doorbell (`cl_clint_set`); this firmware's DM core
reads the QCS stream and replays it onto the compute cores:

- `COPY`/`FILL`/`UPDATE` → iDMA / memcpy (DM core)
- `DISPATCH` → build an `iree_hal_executable_dispatch_state_v0_t` + workgroup
  state and invoke the kernel over the grid via the snRuntime fan-out (DM core
  assigns, compute cores run, `snrt_cluster_hw_barrier`).

This is the rv32 / real-snRuntime port of the Phase-1 dev-box replayer
([`../cluster_replay.c`](../cluster_replay.c)) — same logic, error codes, and
untrusted-stream bounds discipline; only the I/O primitives + dispatch fan-out
are device-specific.

## Files
- `qcs_replay.{h,c}` — the replayer (this dir is the canonical source).
- `qcs_kernel_abi.h` — minimal vendored copy of IREE's executable-library v0
  structs, field-for-field identical to `iree/hal/local/executable_library.h`
  (verified) so a real iree+xdsl kernel.o links + is called with the exact
  layout. Kept dependency-free so the rv32 build doesn't pull in `iree/base/*`.
- `main.c` — entry: L2-SPM region setup, kernel registration (STUB today), the
  replay call, and the Phase-0 completion handshake into the descriptor.
- `app.mk` — gwaihir snitch-app build fragment.

`cluster_command_stream.{h,c}` (the QCS ABI + reader) are NOT duplicated here —
the canonical copy is [`../../transport/`](../../transport/).

## Build (in the gwaihir tree)
Populate the gwaihir snitch-app dir from THIS repo (the source of truth) with
[`link_into_gwaihir_tree.sh`](link_into_gwaihir_tree.sh): it symlinks the sources
above + `../../transport/cluster_command_stream.{h,c}` + `app.mk` into
`<gwaihir>/sw/snitch/apps/qcs_replay/{src,}`, so the RTL build compiles the
versioned repo source, not a hand-maintained copy. It is idempotent (re-run any
time); pass `LINK_MODE=copy` if a build step drops the symlinks. Then:
```
./link_into_gwaihir_tree.sh    # gwaihir tree = <repo>/.gwaihir or $QUIDDITCH_GWAIHIR_GEN
export SN_LLVM_BINROOT=.../riscv32-snitch-llvm-.../bin
export CHS_SW_GCC_BINROOT=.../riscv64-gcc-12.2.0/bin
source .venv/bin/activate
make qcs_replay        # or `make sn-apps`  (NOT SN_BUILD_APPS=ON — name collision)
```
→ `build/qcs_replay.elf` (rv32, loads at L2-SPM base `0x7000_0000`). Verified:
compiles + links `-Werror`, zero undefined refs.

## Status / next
The real iree+xdsl `gemm64` kernel links via the SPLIT `lib/` (registered in
`gw_register_kernels` through the library-query path); the stub survives only
behind `QCS_USE_STUB_KERNEL`. The host half runs on RTL (CVA6 boots from
LLC-cached DRAM). Current blocker is downstream: the cluster cores jump to `PC=0`
— a wake / entry-vector issue (the host must seed+wake every cluster, not just
cluster 0). Next: resolve that, then stage L2 bindings into TCDM per tiling.
See [`../../../../docs/host-device-split-phase2.md`](../../../../docs/host-device-split-phase2.md).
