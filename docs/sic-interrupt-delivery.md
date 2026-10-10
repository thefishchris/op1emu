# SIC -> CEC interrupt delivery (level-sensitive model)

## Hardware behaviour

The labels below and their supporting excerpts are kept locally in
`op1emu-local-evidence/sic-forwarding-investigation/hrm/`.

- **CONFIRMED (HRM 5-5/5-6, 5-13; PGR 4-34, 4-53/54):** SIC peripheral
  inputs are level-sensitive. `SIC_ISR` shows the request level. It is
  unaffected by `SIC_IMASK`, read-only, and clears only when the peripheral
  deasserts.
- **CONFIRMED (PGR 4-53):** the CEC sets `ILAT[n]` on a rising edge of the
  IVGn input, and clears it when IVGn is accepted (IPEND set).
- **CONFIRMED (PGR 4-41 note; HRM 5-5):** a re-trigger of an IVG that is
  being serviced does not set ILAT. RTI "enables further interrupt generation
  on that interrupt input".
- **CONFIRMED (HRM 5-13; PGR 4-54):** if the peripheral still requests when
  its ISR returns, the ILAT bit is set again immediately and the ISR runs
  again after the RTI.
- **CONFIRMED (PGR 4-41):** a request latched while CEC-masked stays latched.
  A software W1C to ILAT cancels it.
- **LIKELY (PGR 4-34 block diagram):** `SIC_IMASK` and `SIC_IARx` gate and
  route the level before the CEC. An unmask or re-route of an asserted request
  is therefore a rising edge at the target IVG.

## Model

- **Source levels:** `SIC::isr` holds the peripheral levels (`SIC_ISR`).
  `SIC::SetInterruptLevel` does nothing more when the level is unchanged;
  rewriting an unchanged level is not a hardware event.
- **IVG lines:** `SIC::lines`, bit n, is the line into IVGn: the OR of
  asserted, `SIC_IMASK`-enabled sources routed to IVGn by `SIC_IARx`. It is
  recomputed only when a level, `SIC_IMASK` or `SIC_IARx` changes. Each
  rising edge of a line is forwarded once.
- **Latch requests:** the CPU records forwarded edges in a pending IVG mask
  (`QueueInterruptRaise`, guarded by the event-queue mutex because a host
  thread may drive USB lines). It does not use the generic event queue.
- **Latching:** at each `ProcessEvents`, `LatchRequestedInterrupts` latches
  every requested IVG with `cec_raise`, except an IVG that is being serviced
  (IPEND set). That request is dropped, because the hardware does not latch
  it.
- **Re-pend at RTI:** `RepeatAssertedAfterReturn` runs at the end of each
  awake `Run()`, after the device loop, and does three things:
  1. finds every IVG7–15 whose IPEND bit was set before the block and is now
     clear, meaning its service ended;
  2. if its line is still asserted, latches it again;
  3. re-checks pending, so the ISR is entered before the next block.

  Running after the device loop matters. Peripherals that update their
  request only there (RTC, NFC, USB, GPIO) have then already dropped a request
  the ISR acknowledged, so no spurious re-entry results.

Latching saves and clears bcore's `did_jump` flag around `cec_raise`, then
restores it (`LatchIvg`). `cec_raise` returns without latching while
`did_jump` is set, and any dispatch earlier in the same `Run()` sets it — for
example the core timer's IVG6.

The old per-block re-forwarding hid that loss. Once only edges are
forwarded, an edge lost this way is never delivered: the v241 boot stalled
with the NAND DMA request asserted on IVG9 and ILAT clear.
`tests/sic_level_interrupt_tests.cpp` covers the case.

SIC wakeup from IDLE is unchanged (`SIC_ISR & SIC_IWR`).

## Assumptions and limitations

- **Delivery latency:** an edge seen after a block is latched at the end of
  that `Run()` and dispatched after the next block. This is the same
  one-block granularity as before.
- **Priority at re-pend (UNKNOWN impact):** bcore's RTI already dispatches
  any other pending interrupt inside the block. If that happens in the same
  RTI where a still-asserted, higher-priority IVG re-pends, the higher IVG
  nests over the other handler instead of being entered first. Doing better
  needs a bcore RTI hook.
- **Self-deasserting pulses (UNKNOWN):** a source that asserts and deasserts
  by itself during service of its own IVG is not delivered after RTI. The
  documented peripherals hold their request until software clears it.
- **User mode (existing behaviour):** latching uses `cec_raise`, which raises
  an illegal-use exception in user mode. The v241 firmware was measured never
  to take this path.
