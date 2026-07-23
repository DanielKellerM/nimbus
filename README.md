# Nimbus

**Audience:** anyone deploying a Quidditch-compiled ML kernel to a real Snitch SoC,
or bringing up a new SoC target.

**Status:** v0 — the gwaihir deployment carved out of Quidditch with history
preserved; the two-root build rewire is done and `qcs_replay.elf` rebuilt from this
layout is byte-identical to the pre-split baseline. Pinned to Quidditch
`a87f9e2` (submodule `.gitmodules`; `git submodule status` for the live SHA). See
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
else. The contract is three surfaces, all owned by Quidditch and present in the pinned
submodule: `quidditch/runtime/runtime/src/Quidditch/quidditch_executable_abi.h`,
`.../quidditch_snrt_abi.h`, and the generated `snitch_cluster_cfg.h`. The firmware's
vendored ABI copy `#include`s the first and `static_assert`s its layout, so a stale
pin fails the build rather than drifting silently.

## Building

Bring up the submodule first:

```
git submodule update --init --recursive
```

Target builds live under `targets/<soc>/`; see `docs/nimbus-design.md` for the
per-target build flow. A new SoC is a new `targets/<soc>/` + placement config +
linker script + `firmware/<soc>/` — not a rewrite.
