# Running a model on gwaihir — end-to-end

**Status.** A 2-layer PyTorch MLP (StableHLO front-door) runs headless on the full
16-cluster gwaihir SoC RTL under VCS co-sim (`PRELMODE=5`, no CVA6 host): `done=144/144
fail=0`, and all 256 output elements agree with the torch golden to f64 last-bit
(`max_abs_err 1.7e-16`, 0/256 mismatches). The pipeline below is what produced that run.
The same steps take any StableHLO/torch model *through the compiler*; only the MLP and a
64×64 gemm have actually executed on the SoC (others compile but are unproven on HW).

This is the deployment runbook — the individual scripts are self-documenting (`--help` /
header), this ties them into one path. All commands are from the nimbus root unless noted.

## What you need (Gwaihir-team prerequisites)

- **A built Quidditch** (the compiler; the `quidditch` submodule). Follow
  `quidditch/README.md` — Docker + the `ghcr.io/opencompl/quidditch/toolchain` image,
  `uv`, CMake ≥3.21, clang/lld, Python 3.11; ~1 h to build. This yields the build dir with
  `CMakeCache.txt`, `quidditch-compile`, `.venv/bin/xdsl-opt`, and the rv32 `toolchain/`.
  Everything downstream *derives its tool paths from that `CMakeCache.txt`* — you never set
  them by hand.
- **rv32/rv64 firmware toolchains**: `SN_LLVM_BINROOT` (riscv32-snitch-LLVM) and
  `CHS_SW_GCC_BINROOT` (riscv64-gcc). On IIS these are in `targets/gwaihir/soc/iis-env.sh`;
  off-site, obtain them and point the two vars at your copies.
- **`bender`**, and the SoC gen tools (`floogen`, `peakrdl`) — from the soc python venv.
- **A commercial simulator**: VCS or Questa (the SoC co-sim has no open-source path). The
  standalone Verilator *cluster* sim exists for the compiler/autotuner but is not this run.

## One-time setup

```
git clone git@github.com:DanielKellerM/nimbus.git && cd nimbus
git submodule update --init --recursive         # quidditch + targets/gwaihir/soc
./targets/gwaihir/setup-gwaihir.sh              # applies the co-sim patch, bender checkout
```

## Deploy a model (7 steps)

The default (no args) builds the stock 64×64 gemm; the example threads a torch MLP through.
`$QB` = your Quidditch build dir (with `CMakeCache.txt`).

**1. Export the model → StableHLO + reference** (torch_xla needs modern glibc, so run in the
container — see `quidditch/runtime/samples/gemm_square/README-pytorch-frontdoor.md`):

```
MLP_OUT=$PWD/model python quidditch/runtime/samples/gemm_square/export_mlp.py
#   -> model/{torch_mlp.stablehlo.mlir, mlp_ref_bits.h, mlp_in{0..4}_*.npy, mlp_golden.npy}
```

**2. Lower to Flow IR** (the generator derives the QCS job from this):

```
$QB/../iree-build/tools/iree-compile --iree-input-type=auto \
  --iree-input-demote-f64-to-f32=0 --compile-to=flow \
  model/torch_mlp.stablehlo.mlir -o model/model.flow.mlir
```

**3. Build the kernel library** (derives every tool path from the CMakeCache; gates against
the silent scalar fallback):

```
QUIDDITCH_BUILD_DIR=$QB KERNEL_NAME=mlp \
  ./targets/gwaihir/firmware/gwaihir/produce_kernel_lib.sh \
  model/torch_mlp.stablehlo.mlir
#   -> .gwaihir-kernel-lib/libmlp_kernel.a  + prints KERNEL_LIB and QUERY=<symbol>
```

**4. Link the .a into the firmware tree:**

```
KERNEL_LIB_DIR=$PWD/.gwaihir-kernel-lib ./targets/gwaihir/firmware/gwaihir/link_into_gwaihir_tree.sh
```

**5. Build the offload image** (`--input-dir` needs `input{N}.npy` matching the flow's
`hal.tensor.import "inputN"` — alias the exported files):

```
mkdir -p model/inputs && n=0; for f in model/mlp_in*_*.npy; do ln -sf ../$(basename $f) model/inputs/input$n.npy; n=$((n+1)); done
./targets/gwaihir/firmware/gwaihir/gen_qcs_offload_image.py \
  --flow model/model.flow.mlir --input-dir model/inputs --out model/job.hex
#   prints the dispatch/binding layout and the output offset to read back
```

**6. Build the qcs_replay firmware** (NOT `make sw` — that omits qcs_replay and won't link
the kernel; build the target explicitly with the wiring vars from step 3):

```
export SN_LLVM_BINROOT=... CHS_SW_GCC_BINROOT=... CXX=g++-9.2.0   # your toolchains
make -C targets/gwaihir/soc qcs_replay \
  QCS_KERNEL_LIB=libmlp_kernel.a QCS_KERNEL_LIBRARY_QUERY=<QUERY from step 3>
```

**7. Run headless on the SoC + read back the output:**

```
make -C targets/gwaihir/soc vcs-run-batch PRELMODE=5 \
  SN_BINARY=$PWD/targets/gwaihir/soc/sw/snitch/apps/qcs_replay/build/qcs_replay.elf \
  OFFLOAD_IMAGE=$PWD/model/job.hex
#   expect: [OFFLOAD] COMPLETE done=144/144 fail=0 ; dumps target/sim/l2mem.bin
# verify (offset from step 5, e.g. 0x26000; f64):
python - <<'PY'
import numpy as np
y = np.frombuffer(open('targets/gwaihir/soc/target/sim/l2mem.bin','rb').read(), '<f8', 256, 0x26000)
g = np.load('model/mlp_golden.npy').ravel()
print('max_abs_err', np.abs(y-g).max())   # ~1.7e-16
PY
```

## Notes / known constraints

- **Single-buffered on the SoC.** Double-buffering (`dual_buffer=true`) is proven on the
  standalone cluster but blocked on the SoC by a Cheshire-AXI downsizer R-last bug — see
  `docs/gwaihir-axi-dw-downsizer-rlast-bug.md` (owner: Luca). The single-buffered path runs
  clean.
- **`vsim-run-batch`** is the Questa equivalent of `vcs-run-batch` (see
  `patches/README.md` for the six `PRELMODE` arms 0–5).
- The kernel `.a` and offload image are build artifacts (`.gwaihir-kernel-lib/` is
  gitignored); regenerate them with steps 3 + 5 for any model.
