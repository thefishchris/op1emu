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
| M1: quiet-block batching | Implemented. EXPERIMENTAL and opt-in (`OP1EMU_EXPERIMENTAL_MULTIBLOCK=1`); off by default. |

M0 does not change when devices are serviced or how guest time advances.
With diagnostics off, the only additions to `Run()` are two flag tests.
M1 is described in the "M1" sections below.

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

## M1: batched execution

### Switch

- **`OP1EMU_EXPERIMENTAL_MULTIBLOCK=1`:** accepted only with
  `OP1EMU_PROFILE=functional` and the guest clock. Strict and unset profiles
  reject it, as does a guest clock without the functional profile. It is
  listed among the active timing assumptions.
- **`OP1EMU_EXPERIMENTAL_MULTIBLOCK=crosscheck`:** also re-derives every
  incremental proof in full. A disagreement is counted, the first one is
  logged, and the pass is run. This is for validation.
- **Exclusions:** M1 cannot be combined with `OP1EMU_BATCH_DIAG`. Per-block
  tracing (`OP1EMU_TRACE_CPU=1`) ends every batch.
- **bcore:** unchanged.

### Architecture

`Run()` loops over `RunBlock()`, which is the former `Run()` body,
unchanged except for one decision point:
- `RunBlock()` executes one cached block with its full per-block prologue
  and bookkeeping: CYCLES, the entry-point invalidation, PLL and ratio
  checks, packet and shadow accounting, the guest clock and the core-timer
  `Sync()`, then the IDLE check and `cec_check_pending()`.
- **Decision point:** after `cec_check_pending()` and before the device loop,
  `BatchContinueBarrier()` decides whether the rest of the pass may be
  skipped. The rest is the device loop, GP timer tick, `ProcessEvents()` and
  the SIC re-pend at RTI.
- **Skipped:** `RunBlock()` returns and `Run()` runs the next block.
  Otherwise the pass runs and `Run()` returns. Per guest block, the order of
  execution, guest-clock advance, core-timer sync, interrupt check, device
  processing, event processing and SIC re-pend is the order without
  batching; skipped passes are simply absent.
- **Batching off:** `Run()` executes exactly one block and its pass.

**MMIO is synchronous.** Every guest MMR access reaches the device's
`Read32`/`Write32` at the exact instruction (bcore → `EmulatorMemory` →
`Emulator`). A DMA start, SIC level change or NAND command therefore takes
effect inside the block. Effects that wait for the pass (DMA progress, NFC
completions, TWI progress) are why a block with any MMIO always gets its
pass before the next block runs.

### What ends a batch

The checks run in this order. Any one of them runs the pass and returns
from `Run()`:

| Check | End reason |
| --- | --- |
| 64 blocks executed in this `Run()` (`kMaxBatchBlocks`) | budget |
| Per-block tracing | tracing |
| Any MMR access (`0xFFC00000` and up) during the block | MMIO |
| IPEND or ILAT changed (dispatch, RTI, raise, latch) | interrupt change |
| `ILAT & IMASK` non-zero | interrupt pending |
| Core-timer expiry during the block | guest deadline |
| PLL lock or awake-bypass countdown outstanding | PLL |
| GP timer tick due on this block's host-time sample with a timer running | device work |
| Queued events or SIC latch requests (`eventsPending_`, set on every push) | queued events |
| Next PC registered by the caller (`AddBatchStopPC`) | stop PC |
| Device proof failed (below) | device work |

IDLE never batches: `RunBlock()` services IDLE and returns before the
decision point.

**Device proof:**
- **First skip in a `Run()`:** `ServiceIsNoOp()` for every device, all at
  one host instant.
- **Later skips:** only what can change without ending the batch:
  - DMA (SPORT readiness follows guest time);
  - USB host-thread work (`HostWorkPending()`, set by every host request);
  - the RTC's host-time `STAT` at the decision instant.
- **Other device state** changes only through MMR writes (which end the
  batch), the pass itself (not run), or events (which end the batch).
  `crosscheck` verifies this claim.

### Host-time boundary policy

Host-time sources are RTC, CYCLES, GP timer ticks, panel TE, USB host and
input events. They are TEMPORARY host-time behaviour, nondeterministic
with or without batching.

**The M0 RTC mismatch.** M0 sampled host time twice: once for the
eligibility query, and again when the pass ran. A minute boundary fell
between the two samples. M1 samples host time once per skipped pass, at the
decision point, and uses that instant both as the instant of the skipped
pass and as the next block's host time (CYCLES, the GP timer sample, RTC).
A skipped pass is therefore exactly a pass run at that instant. No guest
code runs between the two, and a block without MMIO reads no host time.
The RTC change is seen at the next decision point, at most one block later
in host time, where it ends the batch.

| Source | Policy |
| --- | --- |
| RTC | Exact proof at the decision instant (`StatUnchangedAt`). A new second or minute ends the batch on the next block boundary. |
| CYCLES | Updated every block, as before, from the block's host time. |
| GP timer ticks | The tick condition is this block's host-time sample, as before. A due tick with a running timer ends the batch. |
| Panel TE, input, frontend events | Delivered as queued events. A push from any thread sets `eventsPending_`, and the next decision point sees it. Delayed events (TE's 1000-pass edge) keep every pass until they run. |
| USB host (usbipd thread) | Every host request sets `workPending`. The next decision point sees it. |
| Work between `Run()` calls (frontend polling, sensors, runtime bound) | Waits for at most one batch (≤ 64 blocks, a few µs to tens of µs of host time). This is the only added latency, and it applies to host-time inputs only. |

**Classification.** A late host event lands at the next block boundary,
exactly as if it had arrived after that pass. This is LIKELY equivalent to
some unbatched host timeline, not proven equal to any particular run. Host
time is not deterministic in either mode.

**Speed.** Host-time sources fire at the same rate per host second with
batching on. Because the guest runs faster, they fire less often per guest
second: the panel TE stays at 62.4 per host second. That is the existing
host-time behaviour, not a new semantic.

### Deterministic equivalence

`tests/multiblock_execution_tests.cpp` uses an injected host clock: host
time is a function of guest CLKIN time. Each scenario runs three times from
the same start:
- unbatched;
- batched with `crosscheck`;
- batched plain.

**What must match** after every guest block: the complete captured state
(as in M0, plus GP timers), a per-scenario memory hash where relevant, the
MMIO sequence, and the final guest memory. No cross-check may disagree.

Inputs are injected at the same block in every run, through the paths real
host inputs use: queued events, USB host requests and SIC pins.

**Scenarios, all identical:**
- quiet loop;
- hardware loop with exit and re-arm;
- memory stores in a loop;
- MMIO every block;
- MMIO between loops;
- core-timer expiry with an ISR;
- nested interrupts (`[--SP] = RETI`);
- SPORT DMA words and rows;
- core timer plus SPORT deadlines;
- NAND/NFC completions;
- TWI master transfer;
- GP timer host-time ticks;
- RTC second and minute rollover;
- USB host requests (SETUP, OUT, IN);
- queued events (TE-like delay);
- IDLE with a core-timer wake;
- PLL same-value bypass countdown.

**Further checks:** a stop-PC scenario (every arrival returns to the
caller), and a profile gate (strict and unset refuse, off by default,
exclusive with M0 diagnostics).

**Mutation checks** remove one check at a time (evidence kept locally):

| Removed check | Plain batching diverges | Caught by cross-check | Caught by scenario expectation |
| --- | --- | --- | --- |
| RTC host-time check | yes | yes | — |
| DMA re-check | yes | yes | — |
| Events flag | yes | yes | — |
| GP timer tick | yes | yes | — |
| First full proof | yes | yes | — |
| USB host check | no | yes | — |
| MMIO barrier | no | — | yes |
| IPEND/ILAT barrier | no | — | yes |
| Expiry barrier | no | — | yes |
| PLL barrier | no | — | yes |

**USB:** the host handlers apply their effects synchronously, so no
scenario shows a visible effect of a delayed USB pass. The check stays.

**`InterruptPending`:** removing it changed nothing, because
`cec_check_pending()` has already run and the skipped part of the pass does
not read a blocked pending interrupt. It stays, as M1 requires, and ends
5.6% of passes in the firmware.

### Firmware (v241 functional, same script and binary, OFF / ON / crosscheck)

Each run covers boot, UI, play, stop and a held note. Rates come from a
scratch-only instrumented copy (evidence kept locally).

**All runs:**
- **Script:** completed in every run, with no unsupported stop or assertion.
- **Screenshots:** the tape UI, play, stop and held note match. The tape
  counter is further along with batching on, because more guest time
  passes per host second.
- **Cross-check:** 359.7M incremental proofs re-derived in full, 0
  disagreements.

| Per phase | OFF | ON |
| --- | --- | --- |
| Blocks per host second | 1.20–1.25M | 2.15–2.22M (1.76–1.84×) |
| Guest seconds per host second | 0.048–0.073 | 0.110–0.164 (2.25–2.56×) |
| Host dispatches per block | 1.0 | 0.115–0.150 (average batch 6.7–8.7) |
| Device passes skipped | 0 | 85.1–88.5% |
| SPORT TX/RX words per guest second | 88,200 / 88,200 | 88,200 / 88,200 |
| IRQ28 / IVG11 (SPORT TX DMA) per guest second | 689 | 689 |
| IRQ20 / IVG10 per guest second | 579–658 | 579–658 |
| Core timer / IVG6 per guest second | 1,000 | 1,000 |
| Panel TE (IRQ41) per host second | 61.9–62.4 | 61.8–62.4 |

**Guest seconds per host second** improves more than blocks per host
second. That is mostly because host-time work, such as TE handling and
redraws (IVG13), happens less often per guest second at higher speed, so the
firmware runs fewer blocks per guest second.

**NAND activity** (IRQ16/48, IVG9) is identical against guest time for the
first 2 guest seconds of boot in all three runs. After that it tracks
closely; the phase windows differ only because the script's host-time
waits fall at different guest times.

**Batch ends over the whole ON run** (687.9M blocks):

| End reason | Share of blocks |
| --- | --- |
| Interrupt pending | 5.6% |
| MMIO | 2.6% |
| Device work | 2.0% |
| Queued events | 1.9% |
| Budget | 0.6% |
| Interrupt change | 0.05% |

**Overhead with batching off:** a full `Run()` on a 2-NOP hardware loop
takes 422.2 vs 418.7 ns per block (median of 9 alternating runs), within
noise. `RunBlock()` is inlined into `Run()`, and the host clock defaults to
a direct `system_clock::now()`.

**Batched, isolated benchmark:** 105 / 120 / 160 ns per block for a 1-, 2-
and 10-instruction loop, against about 420 ns unbatched.

## Known limits

- **Host-thread and host-time sources** (USB host, panel TE, input events,
  RTC seconds): in M0 these can change state between the eligibility query
  and the pass (one RTC minute boundary in the firmware verify run). M1
  samples host time once per skipped pass; see "Host-time boundary policy".
  Equivalence with batching on is exact only for deterministic inputs. For
  host-time inputs it is LIKELY equivalent to some unbatched host timeline,
  not proven.
- **Caller work between `Run()` calls** (frontend polling, sensors) waits for
  up to 64 blocks with batching on.
- **Stop PCs:** callers that inspect `PC()` between `Run()` calls must
  register those PCs (`AddBatchStopPC`). The headless and GUI DXE loaders do.
- **Captured state is not exhaustive:** guest SDRAM/L1 writes by the CPU are
  not compared, because the pass does not run the CPU. Host-side device
  internals are compared only through registers and counters.
- **Core-timer coverage:** the legacy core timer and the non-functional
  profiles stay ineligible.
