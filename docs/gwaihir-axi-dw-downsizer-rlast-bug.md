# axi_dw_downsizer: duplicate R-last re-accept underflows the internal axi_demux AR id-counter under overlapping reads

**Repo/rev:** `colluca/axi` @ `06410c3` (as vendored into gwaihir).
**Where hit:** the wide→narrow read path in a Cheshire/FlooNoC join —
`…i_axi_wide_dw_converter.gen_dw_downsize.i_axi_dw_downsizer.i_axi_demux.i_demux_simple.`
`i_axi_mcast_demux_simple.gen_demux.gen_ar_id_counter`.

## Summary
Under **overlapping / back-to-back reads**, `axi_dw_downsizer`'s read FSM re-accepts a burst's already-forwarded **terminal** `r.last` beat, so the internal `axi_demux` AR id-counter is decremented **twice for one AR** → underflow `$fatal`. Serialized single reads never trip it.

## Symptom
```
axi_demux_id_counters.sv:137  gen_ar_id_counter…gen_counters[1].cnt_underflow
  (assert property: pop_en[i] |=> !overflow)  -> $fatal
```
Deterministic (same sim time every run) once double-buffered / many-small overlapping reads are in flight.

## Root cause
The accept gate in `axi_dw_downsizer.sv` (R FSM, `R_PASSTHROUGH/R_INCR_DOWNSIZE/R_SPLIT_INCR_DOWNSIZE`, ~line 568) asserts the master-side ready `mst_r_ready_tran[t]` whenever the output slot is **free OR being consumed** — it does **not** exclude the last-beat drain. The slot only leaves the active state one cycle later, on the wide handshake (~line 638, gated on `burst_len=='1`). In the gap between the terminal beat being forwarded and the wide beat being accepted, the held final beat is accepted — and **counted** — again. `mst_req.r_ready = |mst_r_ready_tran`, and the demux pops on `mst_resp.r_valid & mst_req.r_ready & mst_resp.r.last`, so one AR yields two pops.

## Evidence (waveform, pure AXI)
FSDB of the `i_axi_demux` subtree. On `gen_ar_id_counter.i_ar_id_counter`, ID 1:
- `push_i` (AR) high **1 cycle**.
- `pop_i` (R-last) high **2 cycles, unbroken** — one held handshake, not two separate pulses.
- `mst_r_valids` **never transitions** across the window (held; not a toggling second beat).
- `gen_counters[1].i_in_flight_cnt.counter_q[5:0]` wraps **`0 → 0x3f`** (6-bit underflow) at the pop.

The held (non-toggling) `r_valid` + continuous 2-cycle `pop_i` is the *re-accept of one beat* signature, not two distinct beats.

## Minimal repro (IP-level, self-contained)
Extend `test/tb_axi_dw_downsizer` (or a directed stim):
1. Issue **≥2 overlapping AR reads sharing the same low ID bits** (`AxiLookBits`) through the wide→narrow downsizer.
2. **Backpressure the wide-slave R-ready** so a burst's terminal beat is held ≥2 cycles while the next read is queued onto the same demux slot.

Expect `gen_ar_id_counter.cnt_underflow`. (Serialize the reads / drop the backpressure → passes.)

## Candidate fix
Qualify the "output being consumed" branch of the accept gate with `!r_req_q.r.last`, so once the terminal beat is forwarded the slot stops accepting (and popping) until it retires:
```systemverilog
-              if (!slv_r_valid_tran[t] || (slv_r_valid_tran[t] && slv_r_ready_tran[t])) begin
+              // Don't re-accept while the forwarded terminal beat (r.last) is draining: the finished
+              // burst's held final beat would re-pop the demux AR id-counter (underflow). r.last is 0
+              // at a burst start, so a legit max burst (burst_len=='1 at start) is unaffected.
+              if (!slv_r_valid_tran[t] ||
+                  (slv_r_valid_tran[t] && slv_r_ready_tran[t] && !r_req_q.r.last)) begin
                 mst_r_ready_tran[t] = 1'b1;
```
Note: the tempting `burst_len != '1` guard is **wrong** — `burst_len` legitimately equals `'1` at the *start* of a max downsized read, so it would deadlock. Using the forwarded-`r.last` flag avoids that.

## Uncertainty worth a second opinion
An independent review argued the downsizer read path is byte-identical to upstream `axi_demux_simple`/`axi_dw_downsizer` and that the duplicate `r.last` might be injected **upstream** (FlooNoC wide-R reorder buffer / id-width conversion) with the demux merely the detector. The waveform (held, re-accepted — not a toggling replay) favors the downsizer, but you own both sides. **Discriminator at the downsizer's `mst_resp`:** two *distinct toggling* `r_valid` beats ⇒ upstream replay; a *held* `r_valid` while `r_ready` re-asserts ⇒ this re-accept (the patch above).

## IP-level reproduction ran — the isolated downsizer is EXONERATED (patch NOT applied)
The discriminator above was executed at IP level on the stock (unpatched) `tb_axi_dw_downsizer`
(branch `dw-downsizer-rlast-repro`), driving the described trigger with **legal** stimulus:
same-low-ID overlapping reads (IdWidth 1 and 2 → forced demux-slot collisions), sustained wide-R
backpressure (`RESP_*_WAIT_CYCLES` 2–8), back-to-back AR issue, read-heavy (2000 reads), across
downsize ratios 2:1…8:1 (exercises the split-downsize FSM) and 10 configs × 3 seeds. The tb builds
the exact buggy hierarchy (`axi_demux`→`axi_demux_simple`→**`axi_mcast_demux_simple`**→`axi_demux_id_counters`).

**Result: `cnt_underflow` never fired — 0 errors, 0 `$fatal` in every run.** So the isolated
downsizer does not exhibit the bug under any legal stimulus reproducing the writeup's conditions,
which argues the candidate accept-gate patch above is fixing the **wrong location**. The evidence now
favors the **upstream (FlooNoC wide-R reorder / id-width conversion)** hypothesis — the downsizer's
demux is the *detector*, not the cause. **Do not land the accept-gate patch on this basis.** Next step
is to reproduce in the FlooNoC wide→narrow join (or the full gwaihir sim), where the upstream path is
present. (Caveat: exoneration holds for every legal stimulus tried, not a proof the FlooNoC can't
present a burst/timing the rand_master can't emit.)
