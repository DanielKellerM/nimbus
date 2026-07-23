# Nimbus

**Audience:** anyone deploying a Quidditch-compiled ML kernel to a real Snitch SoC,
or bringing up a new SoC target.

**Status:** v0 — the gwaihir deployment carved out of Quidditch with history
preserved; the two-root build rewire is done and `qcs_replay.elf` rebuilt from this
layout is byte-identical to the pre-split baseline. Quidditch is pinned via the
`quidditch` submodule (`git submodule status` for the live SHA). See
`docs/nimbus-design.md` for the design of record and migration plan.

Nimbus is the SoC **deployment/integration** layer for the [Quidditch](quidditch/)
Snitch ML compiler. Quidditch compiles a program for a Snitch cluster and declares the
runtime ABI; Nimbus takes a specific SoC — host, transport, and firmware — and runs it
on real hardware. Nimbus depends on Quidditch (as a git submodule); Quidditch never
depends on Nimbus.

## Layout

```
quidditch/          git submodule — the compiler, device runtime, and interface-v0 ABI headers
targets/gwaihir/    the gwaihir/cheshire/CVA6 deployment: cheshire host, cluster HAL,
                    transport (QCS + shared region), firmware (QCS replayer), patches
sim/                co-sim helpers (the gwaihir cluster-sim builder)
docs/               design + bring-up docs
```

## The seam

Nimbus consumes Quidditch's compiled kernel output plus its declared ABI, and nothing
else. The contract is three surfaces, all owned by Quidditch: two headers checked in
under the pinned submodule —
`quidditch/runtime/runtime/src/Quidditch/quidditch_executable_abi.h` and
`.../quidditch_snrt_abi.h` — and `snitch_cluster_cfg.h`, which the Quidditch codegen
generates at build time (not checked in). The firmware's vendored ABI copy `#include`s
the executable-ABI header and `static_assert`s its layout, so a stale pin fails the
build rather than drifting silently.

## Building (gwaihir)

Nimbus does not vendor the gwaihir SoC tree; it pins it by reference and wires its
firmware into an external checkout. The pin — gwaihir SHA, the co-sim patch, and the
exact bender HW-dep lock — is recorded in `targets/gwaihir/GWAIHIR_PIN.md`
(`gwaihir-4eac10a.Bender.lock` is the vendored `Bender.lock`). Patch details in
`targets/gwaihir/patches/README.md`.

```
git clone --recurse-submodules git@github.com:DanielKellerM/nimbus.git
cd nimbus

# 1. Prepare the gwaihir tree: upstream base + the co-sim patch + HW deps.
git clone https://github.com/pulp-platform/gwaihir.git <gwaihir-tree>
git -C <gwaihir-tree> checkout 4eac10a
git -C <gwaihir-tree> apply "$PWD/targets/gwaihir/patches/gwaihir-cosim.patch"
cmp <gwaihir-tree>/Bender.lock targets/gwaihir/gwaihir-4eac10a.Bender.lock  # verify HW-dep pin
( cd <gwaihir-tree> && bender checkout )

# 2. (rv64 host build only) skip the slow flatcc verify in the IREE submodule.
git -C quidditch/iree apply "$PWD/targets/gwaihir/patches/iree-verify-off.patch"

# 3. Wire the QCS-replayer firmware into the gwaihir tree and build the app.
QUIDDITCH_GWAIHIR_GEN=<gwaihir-tree> \
  targets/gwaihir/firmware/gwaihir/link_into_gwaihir_tree.sh
make -C <gwaihir-tree> sw            # builds sw/snitch/apps/qcs_replay/build/qcs_replay.elf
```

The firmware sources (Nimbus) and the executable-ABI header (the `quidditch`
submodule) are wired through two roots — `NIMBUS_ROOT` and
`QUIDDITCH_ROOT=$NIMBUS_ROOT/quidditch` — both env-overridable in the link script.
At split time `qcs_replay.elf` built this way was verified byte-identical to the
pre-split Quidditch build, confirming the move changed no bytes. Recorded `sha256sum`
(MANUAL snapshot for the default gwaihir cfg; owner: repo maintainer — recompute if
the kernel or cfg changes):
`40d65e8235e1b0a19ec41b01fae4167297bf8475bc209b85634c5c682fac72b4`. The full
CVA6-host → cluster co-sim run is in `docs/nimbus-design.md`.

A new SoC is a new `targets/<soc>/` + placement config + linker script +
`firmware/<soc>/` — not a rewrite.
