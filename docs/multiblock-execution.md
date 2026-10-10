# Multi-block execution

`BlackFinCpu::Run()` executes one guest block, then runs a full service
pass:
- every device's `ProcessWithInterrupt()`;
- the GP timer tick;
- `ProcessEvents()`;
- the SIC re-pend at RTI.

In the v241 SPORT-active workload, the pass costs about 340 ns of host time
per block, against 13–55 ns for the block itself. Multi-block execution
would run several cached blocks back to back and skip passes that provably
change nothing. The design, measurements and staged plan (M0–M4) are kept
with the local evidence (`performance-profile/multiblock-design/REPORT.md`).

## Status

| Milestone | State |
| --- | --- |
| M0: passive eligibility queries and state comparison | Implemented. Diagnostic only. |
| M1: quiet-block batching | Not started. |

M0 does not change when devices are serviced or how guest time advances.
With diagnostics off, the only additions to `Run()` are two flag tests.

## Device eligibility: `Device::ServiceIsNoOp()`

`ServiceIsNoOp()` returns true only when calling `ProcessWithInterrupt()`
now is proven to change nothing guest-visible. That covers registers,
interrupt levels, the SIC/CEC, DMA progress, guest memory, host callbacks and
queued events.

The default is **false**: a device is ineligible until a proof is written for
it. Queries are passive. The ones that need a lock (USB) take the same lock
as the service pass.

| Device | Answer | Proof |
| --- | --- | --- |
| Memory regions, Boot ROM, MMU, EVT/CEC shims, EBIU, OTP, JTAG, PPI, GPIO ports, SIC, PLL, guest core timer, SPORT0/1, GP timer block | always true | `ProcessWithInterrupt()` is the inherited no-op. The guest core timer, PLL and SPORT are driven from the guest clock outside the pass; the GP timers from a separate tick. |
| DMA | true when every active channel is a peripheral channel with row bytes remaining whose bus has no transfer possible (e.g. SPORT not yet due) | MDMA and finished rows progress per pass (poll-count). A channel that is not enabled and running is skipped by the pass. |
| NFC | true when the NAND has no pending reset or page-read completion, `notBusy` already matches the NAND busy state, no read request is waiting to become ready, and the driven interrupt level equals the computed one | Each of these is a state change the pass would make. `NandFlash::HasPendingCompletion()` is conservative (true) unless the NAND model overrides it; MT29F4G08 does. |
| RTC | true when no write is pending, the host-time `STAT` still equals `lastStat`, and the driven level equals the computed one | `Tick()` then rewrites the same values. The RTC is on host time (TEMPORARY). |
| TWI | true when the TWI or its master is disabled | Master transfers progress per pass. |
| USB | true when disabled, or when the existing inactive fast path holds (`fastPathAllowed && lineLevelsValid && !workPending`, under `USB::mutex`) | Same condition as the fast path in `docs/device-polling-fast-paths.md`. A host-thread request can set `workPending` at any time. M1 must bound that latency with a budget. |
| GP timer ticks | true when this pass's host-time tick sample is not due (`cycles % 10000 != 0`, taken before the block) or no timer is running (`GPTimer::TicksAreNoOp()`) | The pass ticks only on that sample, and a tick only advances running timers. The GP timers are on host time (TEMPORARY), so which passes tick depends on host speed, as it already does without diagnostics. |
| Legacy (host-time) core timer | false (default) | Not proven; the functional profile uses the guest core timer instead. |

## Barriers: `BlackFinCpu::EvaluateBatchBarriers()`

With diagnostics on, after each block (after the guest-clock and core-timer
sync, before the service pass), `Run()` records which conditions forbid
skipping the pass. Any bit set means "not eligible".

| Barrier | Condition |
| --- | --- |
| `Mmio` | The block made any access at `0xFFC00000` or above (counted in `BcoreMemory`). |
| `InterruptChange` | IPEND or ILAT changed during the block. |
| `InterruptPending` | `ILAT & IMASK` is non-zero. |
| `Idle` | The block entered IDLE. |
| `GuestDeadline` | The guest core timer expired during the block. |
| `PllCountdown` | A PLL lock or awake-bypass countdown is pending. |
| `DeviceWork` | A device's `ServiceIsNoOp()` is false, or a GP timer tick is due with a timer running. Counted per device. |
| `QueuedEvents` | The event queue is non-empty or SIC latch requests are pending. |
| `Tracing` | Per-block tracing is enabled (`OP1EMU_TRACE_CPU=1`). |

SPORT deadlines are covered through the DMA predicate: a SPORT that is due
makes `TransferPossible()` true.

## State comparison

`CaptureBlockState()` returns named exact values, and `DiffBlockState()`
lists the differences. There is no hashing, except for DMA-written guest
memory. The capture contains:

- **CPU:** all data, pointer, accumulator and DAG registers; LC/LT/LB;
  RETS/RETI/RETX/RETN/RETE; PC, USP, KSP, SEQSTAT, SYSCFG; every ASTAT
  flag; idle and halted state; `packet_entries`.
- **CEC:** IMASK/IPEND/ILAT and EVT0–15.
- **SIC:** IMASK, ISR, IWR and IAR registers; computed IVG lines; pending
  latch requests.
- **DMA:** all 16 channels' registers. With verification on, the number of
  bytes DMA wrote to guest memory and an FNV hash of them. The service pass
  writes guest memory only through DMA.
- **SPORT0/1:** control registers and TX/RX word counters.
- **Guest clock:** CLKIN/CCLK/SCLK ticks and alignments.
- **Core timer:** registers and the expiry count.
- **PLL:** registers and the pending countdown.
- **GP timers:** all eight timers' registers, plus enable and status.
- **NFC and RTC:** registers. RTC `STAT` is excluded because it is host time.
- **USB:** registers and a host-completion counter. The callbacks are
  otherwise invisible to register state.
- **Event queue:** its length.

`SetMMIOLog()` records the MMIO access sequence (PC, address, value, width,
direction) for A/B comparisons.

## Diagnostics

`OP1EMU_BATCH_DIAG` controls the diagnostics:

| Value | Behaviour |
| --- | --- |
| `1` | Count blocks, eligible blocks, barrier bits and per-device work. A `batch-diag` line is logged every 2²⁴ blocks. |
| `verify` | Also capture the state before every service pass judged skippable, and compare it with the state after. The first mismatch is logged and all are counted. |

Any other value is rejected. Both modes are passive: they never skip a pass.

## Validation

**Synthetic tests:** `tests/multiblock_eligibility_tests.cpp`. Every block
judged eligible is verified against the real pass. The test fails on any
mismatch and on a barrier that a scenario must produce but does not.

The scenarios are:
- a quiet loop and a short hardware loop;
- an MMIO store loop (no block eligible);
- core-timer expiries;
- SPORT DMA on a descriptor list plus RX autobuffer;
- NAND/NFC page reads with a controllable busy/pending NAND;
- a TWI master transfer;
- a running GP timer on host-time ticks;
- USB host data;
- SIC interrupt, ISR and RTI;
- IDLE plus a PLL bypass countdown;
- simultaneous core-timer and SPORT deadlines.

A final scenario runs the same workload with diagnostics off and on, and
requires identical per-block state and MMIO sequences.

**Mutation checks:** forcing the DMA, NFC or USB answer to true, or ignoring
a due GP timer tick, produces mismatches.

**Firmware (v241, functional profile, `OP1EMU_BATCH_DIAG=1`):** this
covers boot, UI idle, play and a held note (318.8M blocks; evidence kept
locally).
- **Eligible overall:** 82.4% of blocks; 77–90% per 2²⁴-block interval. This
  is measured, and below the 90–93% design estimate.
- **Barriers in SPORT-active intervals** (they overlap):
  - an interrupt pending but not yet taken (`ILAT & IMASK`, blocked by
    priority): 8–14%;
  - queued host events (panel TE, input): about 6.7%;
  - MMIO: 2–4%;
  - due GP timer ticks: about 3.8%;
  - DMA work: 0.4–0.65%;
  - NFC, TWI, RTC and USB: under 0.1% each.

The queued-event and GP timer barriers are on host time: their share
depends on host speed, and diagnostics slow the host down.
`InterruptPending` is deliberately conservative. Whether a pending but
blocked interrupt can be batched is an M1 question.

**Firmware verification (`OP1EMU_BATCH_DIAG=verify`):** one run covered
boot and about 45 minutes of host time in the SPORT-active UI (335.5M
blocks). It verified 37.8M eligible service passes with one mismatch.

The mismatch was RTC `ISTAT` `0x800C → 0x801C`, the minute event, logged
at host time 15:00:00.000. The RTC is on host time, and the minute boundary
fell between the eligibility query and the pass. This is LIKELY a host-time
race, not a wrong proof: M1 must treat host-time sources as bounded latency.

Verification is about 20× slower, so host-time barriers dominate in that
run: queued events are 69–86% of blocks there, against about 7% without
verification.

**Overhead with diagnostics off:** 364.1 vs 363.7 ns per block (full
`Run()`, 2-NOP hardware loop, six alternating runs each). That is within
noise. A first version that kept the diagnostic code inline in `Run()` cost
about 3%.

## Known limits

- **Host-thread and host-time sources** (USB host, panel TE, input events,
  RTC seconds): these can change state between the eligibility query and the
  pass. The firmware verify run hit this once, on an RTC minute boundary. A firmware
  `verify` mismatch from such a race is a latency question for M1's budget,
  not a wrong proof.
- **Captured state is not exhaustive:** guest SDRAM/L1 writes by the CPU are
  not compared, because the pass does not run the CPU. Host-side device
  internals are compared only through registers and counters.
- **Core-timer coverage:** the legacy core timer and the non-functional
  profiles stay ineligible.
