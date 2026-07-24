# gwaihir pin

gwaihir is pinned as a git submodule — `targets/gwaihir/soc` — so the pin is
machine-enforced by `.gitmodules` + the recorded gitlink, not prose.

| What | Pin |
|---|---|
| gwaihir | `targets/gwaihir/soc` @ `4eac10a36a0ff6868b4b5c2ae3dc51de7b90d8ba` (`pulp-platform/gwaihir`) |
| co-sim mods | `patches/gwaihir-cosim.patch`, applied by `setup-gwaihir.sh` (verified: applies clean on the pin) |
| HW deps (bender) | `soc/Bender.lock` (34 packages) — gwaihir's own lock; `bender checkout` resolves it |

Top-level HW deps the lock resolves:

| dep | revision |
|---|---|
| cheshire | `112b036efe3067461a92ee069b89dbd61192460e` |
| cva6 | `737c5837830d88dbe9a9493633ed8a01e6e681bd` |
| snitch_cluster | `06426b0632814cd0acdba5e0eef3dd342bf42c71` |

The co-sim patch does **not** modify bender, so `bender checkout` on the pinned
`soc/` reproduces exactly these deps.

To bump gwaihir: check out the new SHA in `soc/`, re-generate `gwaihir-cosim.patch`
against it, commit the submodule gitlink, and update this table.
