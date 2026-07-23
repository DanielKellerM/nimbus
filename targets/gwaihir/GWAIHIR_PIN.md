# gwaihir pin

Nimbus does not vendor the gwaihir SoC tree; it pins it by reference. This file is
the machine-readable record of that pin.

| What | Pin |
|---|---|
| gwaihir | `pulp-platform/gwaihir` @ `4eac10a36a0ff6868b4b5c2ae3dc51de7b90d8ba` (`origin/main`) |
| co-sim mods | `patches/gwaihir-cosim.patch` (applies clean on `4eac10a`) |
| HW deps (bender) | `gwaihir-4eac10a.Bender.lock` — the exact `Bender.lock` from gwaihir@`4eac10a`, 34 packages |

Top-level HW deps the lock resolves:

| dep | revision |
|---|---|
| cheshire | `112b036efe3067461a92ee069b89dbd61192460e` |
| cva6 | `737c5837830d88dbe9a9493633ed8a01e6e681bd` |
| snitch_cluster | `06426b0632814cd0acdba5e0eef3dd342bf42c71` |

The co-sim patch does **not** modify bender, so `bender checkout` in a gwaihir tree
at `4eac10a` reproduces exactly these deps. Verify the tree you build against matches
this pin:

```
git -C <gwaihir-tree> rev-parse HEAD            # == 4eac10a...
cmp <gwaihir-tree>/Bender.lock targets/gwaihir/gwaihir-4eac10a.Bender.lock
```

To bump gwaihir: re-checkout the new SHA, re-generate `gwaihir-cosim.patch`, refresh
`gwaihir-4eac10a.Bender.lock` (rename to the new SHA), and update this table.
