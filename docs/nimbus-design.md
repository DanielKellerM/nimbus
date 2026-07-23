# Nimbus — the SoC deployment layer (design)

**Audience:** anyone splitting the repo or bringing up a new Snitch SoC on Nimbus.

**Status:** DESIGN, verified viable — not yet executed. The boundary and seam below
were checked against the tree by a multi-agent audit (2026-07): dependency direction
is clean and non-circular, the split is a *move* not a rewrite, and the QCS wire
format is separable from placement. Verdict was **needs-adjustment**: execute as
sketched with the three corrections in [§6](#6-corrections-folded-into-the-split)
folded into the split commit. This doc is the design of record; it is gitignored
scratch (repo convention) until the split lands.

## 1. Summary

Split the current single repo along the boundary interface-v0 already cut:

- **Quidditch** stays the compiler + Snitch backend — SoC-agnostic. "Compile an ML
  program for a Snitch cluster, declare the runtime ABI, provide the on-Snitch device
  runtime." Upstreamable to `opencompl/Quidditch`.
- **Nimbus** becomes the deployment/integration layer — "take a specific SoC
  (gwaihir/cheshire/CVA6), wire the host, transport, and firmware, and run it on real
  hardware." Pulls Quidditch in as a git submodule.

Nimbus depends on Quidditch; Quidditch never depends on Nimbus. The compiler and
device runtime do not know the deployment exists — verified: nothing under `codegen/`
or `runtime/runtime/` includes, links, or path-reaches into `runtime/host/`, and
`runtime/CMakeLists.txt` never descends into host.

## 2. Why split

One repo today serves three audiences: the compiler consumer, someone bringing up a
new Snitch SoC, and the gwaihir bring-up itself. The gwaihir-specific glue (linker
scripts, the cheshire bridge, the L2-SPM placement, the VCS/co-sim scripts, the deploy
patches) rots the compiler's cleanliness and blocks upstreaming Quidditch. The split
lets Quidditch be upstream-clean and lets Nimbus own the SoC-specific reality.

## 3. The boundary

| Component | Path today | Home | Rationale |
|---|---|---|---|
| Compiler (IREE target + xDSL) | `codegen/` | Quidditch | SoC-agnostic backend |
| On-Snitch device runtime | `runtime/runtime/src/Quidditch/` | Quidditch | Runtime half of the compiler contract; generic Snitch (decision D1) |
| Declared ABI headers | `runtime/runtime/src/Quidditch/quidditch_*_abi.h` | Quidditch | The interface-v0 seam |
| Samples | `runtime/samples/` | Quidditch | Exercise the backend |
| Autotuner (default path) | `tools/autotune/` (less the gwaihir helper) | Quidditch | Backend perf harness; SoC selected via env |
| Submodules | `iree`, `snitch_cluster`, `xdsl` | Quidditch | Compiler deps |
| Host VM runner + cheshire glue | `runtime/host/cheshire/` | Nimbus | CVA6-specific; linker scripts; SPM placement |
| Host-side cluster HAL | `runtime/host/hal/cluster/` | Nimbus | Drives a cluster over a specific fabric |
| Transport (QCS + shared region) | `runtime/host/transport/` | Nimbus | Host↔device over the SoC fabric (decision D2) |
| Firmware (QCS replayer) | `runtime/host/firmware/` | Nimbus | gwaihir snitch-app + `link_into_gwaihir_tree.sh` |
| Deploy patches | `runtime/host/patches/` | Nimbus | gwaihir/iree deploy patches |
| gwaihir sim helper | `tools/autotune/build_gwaihir_cluster_sim.sh` + `.gwaihir` | Nimbus | SoC-specific (correction C1) |

Scale of the Nimbus subtree: ~47 files / ~7.3k lines under `runtime/host/`.

## 4. The seam

The contract between the two repos is the interface-v0 declared surface. The audit
confirmed the device runtime includes zero host headers and the firmware consumes the
ABI headers *by inclusion*, not by copy (`qcs_kernel_abi.h` → `quidditch_executable_abi.h`).
Three cross-repo surfaces must be named in the interface manifest
(`docs/snitch-runtime-interface.md`) so they cannot drift silently after separation:

1. **`quidditch_executable_abi.h`** — the compiler-emitted executable/dispatch ABI,
   version-gated in the firmware. Already declared.
2. **`quidditch_snrt_abi.h`** — the C-callable snRuntime ABI. Already declared.
3. **The generated `snitch_cluster_cfg.h`** — cluster geometry and L1 addresses that
   both `QuidditchTarget.cpp` and the Nimbus host parse. Single-sourced by clustergen
   from one cfg JSON in the `snitch_cluster` submodule (which stays on Quidditch), so
   no code change is needed — but it is a real third seam and must be documented as
   an intentional generated-config surface (correction C2).

Beyond the headers, the QCS command-stream + shared-region *format* lives wholly on
the Nimbus side. The one real SoC address is confined to a single line
(`runtime/host/firmware/gwaihir/main.c:159`, `GW_L2_SPM_BASE_ADDR(0)` from the gwaihir
generated addrmap) — never the wire format. This is why the format is separable
(decision D2).

## 5. Repo topology

```
nimbus/                     outer repo (the deployment product)
├── quidditch/              git submodule → the compiler + backend + device runtime
├── targets/gwaihir/        host/, firmware/, transport/, cheshire glue, linker scripts, patches
├── sim/                    co-sim wiring (run_on_sim, VCS recipes, the gwaihir sim helper)
└── docs/                   deployment/bring-up docs (the Nimbus doc set)
```

Nimbus pins a Quidditch revision, builds it, and links the compiled kernel objects
into a target's firmware.

## 6. Corrections folded into the split

These come from the verification audit; do them *in* the split commit, not as
follow-ups.

- **C1 — relocate the gwaihir sim helper.**
  `tools/autotune/build_gwaihir_cluster_sim.sh` and the tracked `.gwaihir` symlink
  hardcode the gwaihir tree, a pinned `snitch_cluster` rev, and the gwaihir cfg — they
  cannot stay on the SoC-agnostic side. Move them to Nimbus. The default autotune path
  stays agnostic (it targets the `snitch_cluster` submodule sim, selected via the
  `QUIDDITCH_VLT` env var); only this opt-in helper relocates. Drive gwaihir autotune
  from Nimbus by setting that env var.

- **C2 — declare the two extra cross-repo surfaces.**
  Add the generated `snitch_cluster_cfg.h` seam and the vendored IREE base-struct copy
  in `runtime/host/firmware/gwaihir/qcs_kernel_abi.h` to the interface manifest as
  first-class, checked contracts. The vendored copy is a field-for-field mirror of the
  Quidditch fork's `executable_library.h` (the export table is a fork extension, not
  upstream), currently guarded by `_Static_assert`s that name the source of truth. After
  the repos separate a stale submodule pin can drift it silently, so add a CI/dev check
  that the pin and the vendored layout agree.

- **C3 — two-root build rewire.**
  The host Makefiles (`app`, `hal/cluster`, `firmware`) and `build_cheshire_dram.sh`
  root their include paths off a single repo-root `SRC` that reaches into both host
  (stays) and `iree/` + `runtime/runtime/src` (move into the submodule). A plain `SRC`
  override is insufficient because host sources also hang off `SRC`. Introduce
  `NIMBUS_ROOT` for host/transport sources and `QUIDDITCH_ROOT=$(NIMBUS_ROOT)/quidditch`
  for the `-I iree` and `-I runtime/runtime/src` include roots. Pure path plumbing, no
  seam change.

## 7. Decisions

- **D1 — device-runtime home: Quidditch.** The loader/dispatch/command_buffer/executable
  are the compiler's runtime counterpart, SoC-agnostic; moving them to Nimbus would break
  the one-directional seam.
- **D2 — transport ownership: all-Nimbus for v0.** The Quidditch device runtime never
  touches the QCS format, so promoting it to a Quidditch seam header now would widen the
  interface with no Quidditch-side consumer. Revisit "declare the wire format in Quidditch"
  only when a second SoC's firmware needs the same structs; at that point first extract
  the placement constants from `transport/cluster_command_stream.h` into a Nimbus
  placement header, then promote only the pure structs.
- **D3 — Nimbus scope: gwaihir-first, not a speculative generic framework.** Keep the
  per-SoC-subdir discipline (`targets/<soc>/`, `firmware/<soc>/`) so a second SoC is a new
  placement config + linker script + firmware dir, not a rewrite — but do not build the
  generic abstraction until a second SoC actually exists.

## 8. Migration plan

1. **Freeze the seam.** Confirm the interface-v0 headers + the two C2 surfaces are the
   only Quidditch↔host coupling (audited: yes). Snapshot the regression oracle: the
   single-cluster byte-exact sim and the `qcs_replay.elf` firmware build.
2. **Carve `targets/gwaihir/`.** Extract `runtime/host/**` (plus the C1 helper + `.gwaihir`)
   with history into a new `nimbus` repo via `git filter-repo`.
3. **Add Quidditch as a submodule** of Nimbus, pinned at the current SHA.
4. **Rewire builds** (C3): Nimbus points at `quidditch/` for the compiler, device runtime,
   and headers; delete the moved tree from Quidditch.
5. **Prove parity.** Reproduce the gwaihir SW build and the single-cluster byte-exact sim
   from the two-repo layout. Verify the linked ELF, not just build success.

## 9. Parity / done criteria

- Quidditch builds and passes its existing gates standalone (no `runtime/host`).
- `codegen/`, `runtime/runtime/`, `runtime/samples/`, `tools/autotune/` contain zero
  SoC tokens (`gwaihir`, `cheshire`, `cva6`/`CVA6`, `GW_L2`, `SPM_BASE`, the SoC-map
  addresses) — enforced by a lint/grep gate (see risks).
- Nimbus builds `qcs_replay.elf` against the pinned Quidditch submodule and reproduces
  the single-cluster byte-exact sim result.
- The interface manifest names all three seam surfaces with a checked ABI stamp.

## 10. Risks & mitigations

- **Vendored-ABI drift.** `qcs_kernel_abi.h` mirrors the Quidditch fork's
  `executable_library.h`; a lagging submodule pin can drift it silently (already an active
  maintenance burden). → CI check that the pin and the vendored layout agree (C2).
- **Submodule-pin skew.** A stale Quidditch pin means the compiled `kernel.o` and the
  host/firmware interface-v0 headers can diverge — the silent-ABI-mismatch class this repo
  has hit before. → Gate on a version/ABI stamp at load.
- **cfg-geometry divergence.** Host `CFG_CLUSTER_NR_CORES`/`SNRT_CLUSTER_NUM` vs what the
  compiler baked. Single-sourced today; a host that regenerates the cfg independently would
  break dispatch with no compile-time error. → Policy check in the manifest (C2).
- **Build-path scratch-copy traps.** The C3 rewire touches build flows that have historically
  compiled a scratch copy rather than the repo file. → Verify the linked ELF, not just that
  the build succeeded.
- **gwaihir-token re-creep.** Contributors will want an RTL sim from the autotuner and be
  tempted to re-add SoC pointers on the Quidditch side. → Lint/grep gate over
  `codegen/`, `runtime/runtime/`, `runtime/samples/`, `tools/`.
