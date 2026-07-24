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
quidditch/            git submodule — the compiler, device runtime, and interface-v0 ABI headers
targets/gwaihir/      the gwaihir deployment:
  soc/                git submodule — pulp-platform/gwaihir @ 4eac10a (the SoC tree)
  patches/            co-sim mods applied on top of the pinned soc/ (+ the IREE verify-off patch)
  {cheshire,hal,       cheshire host, cluster HAL, transport (QCS + shared region),
   transport,firmware} firmware (QCS replayer) — wired into soc/ by setup-gwaihir.sh
  setup-gwaihir.sh    one-command prep: reset soc/ → apply patch → bender checkout → wire firmware
sim/                  co-sim helpers (the gwaihir cluster-sim builder)
docs/                 design + bring-up docs
```

Both external deps are **git submodules** (`.gitmodules`), so `git submodule
update --init` pins and fetches them — no manual clone. gwaihir is *not* declared via a
nimbus-root `Bender.yml`: gwaihir is itself a top-level bender project whose own build
resolves its HW deps (cheshire/cva6/snitch_cluster) from *its* `Bender.lock`; the submodule
pins **which** gwaihir, and `setup-gwaihir.sh` runs gwaihir's own `bender checkout`.

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

gwaihir is a pinned submodule (`targets/gwaihir/soc` @ `4eac10a`); the co-sim mods
ride on top as `patches/gwaihir-cosim.patch`. `setup-gwaihir.sh` applies the patch,
runs bender, and wires the firmware in one idempotent command. The pinned dep tree is
recorded in `targets/gwaihir/GWAIHIR_PIN.md`; patch details in `patches/README.md`.

```
git clone git@github.com:DanielKellerM/nimbus.git && cd nimbus
git submodule update --init --recursive          # fetches quidditch + gwaihir (soc/) at their pins

# rv64 host build only: skip the slow flatcc verify in the IREE submodule.
git -C quidditch/iree apply "$PWD/targets/gwaihir/patches/iree-verify-off.patch"

targets/gwaihir/setup-gwaihir.sh                 # patch soc/ + bender checkout + wire firmware
make -C targets/gwaihir/soc sw                   # -> sw/snitch/apps/qcs_replay/build/qcs_replay.elf
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
