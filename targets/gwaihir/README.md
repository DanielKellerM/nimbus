# Quidditch host runtime (Cheshire/Carfield host-device split)

In the current standalone runtime the IREE VM/HAL runs on the cluster DM core
(`runtime/runtime/src/Quidditch`, an inline/sync HAL). Measured per-inference
cost is ~95% VM/HAL orchestration on that one core while the 8 compute cores
idle — the IREE VM does not belong on the cluster at scale.

This tree is the host-device split: the **IREE VM/HAL runs on a CVA6 host**
(Cheshire/Carfield, Linux), and the **Snitch cluster is a pure accelerator**
(executables + iDMA + a thin command-buffer replay). The cluster "device half"
(`dispatch.c` fan-out + kernels + snRuntime + the worker loop) is reused
verbatim; the host side is a new IREE HAL device that submits jobs over shared
memory instead of executing inline.

## Layout

- `transport/` — the host↔cluster command-stream ABI and a Phase-0 roundtrip
  proof. `cluster_command_stream.{h,c}` is dependency-free so it compiles for
  both the rv64 host and the rv32 firmware. `make -C transport check`.

## Phased bring-up

0. **Prove the serialize/replay contract** on the dev box, no RTL — host
   serializes a job, "cluster" parses + replays it, fields round-trip.
   *(done: `transport/test_command_stream.c`)*
1. Two-process mmap transport (shared "DRAM" + polled doorbell/completion);
   wire the writer to an IREE deferred command buffer and the reader to the
   existing `dispatch.c` fan-out.
2. Real Carfield mailbox/CLINT doorbell + PLIC completion IRQ; CVA6 under Linux.
3. Async pipelining (host prepares job N+1 while the cluster runs job N).

The crux risk is host-VA → device-PA translation for binding pointers and the
completion-IRQ → IREE-semaphore wiring under Cheshire's self-invalidation
coherence; the ABI keeps binding addresses as device-physical to make that
boundary explicit.
