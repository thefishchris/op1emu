# Checkpoint: NFC PIO and finite MDMA boundaries completed

**Current 2D MDMA checkpoint:** the observed repeated-four-halfword / 512-byte-pitch
MDMA0 copy now completes as a finite device operation. Its pre-IDLE DONE poll
succeeds, so `018D4736` is avoided; no ineligible wake was invented. A fresh
frozen-input run displays the **normal tape UI** (reels, track 1, `0:00:00`).
The CPU stopped at the first subsequent unsupported IDLE, `019F18F4`, waiting
for a byte flag at `FF907DF6` to clear. No continuation or SPORT work followed.
Exact geometry, evidence, implementation scope, and validation are below.

**Current fix checkpoint:** NAND Random Data Read now retains valid loaded-cache
availability after `05/two-column/E0`. Nine focused tests pass. Frozen-input v241
trials transfer correct OOB ECC `E7 03 18 04`, preserve FAT entry 113 as `0072`,
advance `113 -> 114`, and return from the chain walker to `0190A306`. The first
new stop is IDLE **`018D4736`**, polling MDMA0 destination DONE at `FFC00F28`
with two-dimensional CONFIG `0015/0097`. Tape UI has not yet appeared; no next
hardware fix is included. Full evidence and exact-device timing qualifications
are in [NAND provenance and traversal](v241-nand-provenance-and-traversal.md).

**Pre-fix follow-up (2026-10-08):** immutable-input trials disproved the finite
loaded-code traversal interpretation below. The actual reachable chain is
`3 -> ... -> 113 -> 0x8072 -> 0 -> 0`; two fresh clones reproduce its first
self-cycle after 113 lookups. R.00241 remains displayed, with no new unsupported
IDLE. See [NAND provenance and traversal](v241-nand-provenance-and-traversal.md)
for full hashes, reconstruction checks, exact function boundaries, and evidence.

**Pre-fix construction diagnosis:** the linked follow-up classifies the original current-image
cycle as **D — emulator data-path corruption**. `/yaffs2/user/tape_c.raw` contains
correct FAT16 entry 113 (`72 00`) and matching OOB ECC. Random Data Read `E0`
incorrectly reports data unavailable and blocks its OOB DMA; guest ECC correction
at `FFA07CE2` then flips the high byte to `80`. No fix was implemented during diagnosis. Two fresh
pristine clones stop earlier at a guest database assertion/IDLE before allocating
the FAT table; a pristine-cycle comparison is not yet established.

The earlier post-PLL `NFC_IRQSTAT.RD_RDY` boundary is now complete with
evidence-backed programmed-I/O request/consume/ack behavior. That exposed and
completed a finite MDMA0 fill boundary. A fresh bounded v241 run reaches the
firmware splash screen without another unsupported IDLE. SPORT, CoreTimer,
guest `CYCLES`, general DMA pacing, and the general scheduler remain
disconnected.

## Git

- Checkout: `/home/chris/Projects/op1-emu-gui-test/op1emu`
- Branch/HEAD: `wip/pll-idle-timing` at `18db27f` (`feat: model NAND and NFC DMA idle completions`)
- Bcore branch/HEAD: `wip/op1-timing` at `37de8e8`
- Parent tree heavily dirty and unstaged. Includes earlier boot/audio/GPIO/storage work plus this PLL/IDLE work. Do not clean, reset, or sweep unrelated files into a commit.
- Nothing staged. Do not commit images, NAND/OTP copies, or logs.

## Awake-bypass experiment

Opt-in, all three required:

- `OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000` (provisional/LIKELY; not a board fact)
- `OP1EMU_SHADOW_TIMING=1`
- `OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1`

Valid only while the CPU is awake, a temporary-bypass deadline remains, and effective CCLK equals that CLKIN. Each awake `Core::run` consumes only the new `ShadowTiming::EstimatedCycles()` delta, clipped to the remaining ticks. Excess is block-level overshoot. Idle PLL deadlines do not read shadow cycles. Unknown stalls are not extra ticks. After expiry, conversion stops and reset-active CCLK stays unknown.

## Proof the 512-tick interval completed

Trial `build/pll-awake-bypass-v241.*`. First VR write/IDLE unchanged: `VR_CTL=0x70B0` at `0xEF000C3C`, IDLE `0xEF000C3E`, immediate masked-CEC wake, resume `0xEF000C40`.

Expiry log: block `0xEF005C24`, shadow delta 2, consumed 1, overshoot **1**, remaining 0, total-consumed **512**, expiries **1**, `post-cclk-known=0`. Intra-block expiry instruction is unknown. Functional execution continued. This earlier trial did not observe `PLL_DIV=5` or `PLL_CTL=0x2000`; the later DMA-completion trial described below did.

## Completed NFC boundaries

Reset and page-read completion are now explicit NAND operation boundaries rather
than queued host-time delays:

- **CONFIRMED:** Micron PAGE READ is `0x00`, five address cycles, then `0x30`.
  R/B# is low during array transfer and high when data is available.
- **CONFIRMED:** BF52x `NFC_STAT.NBUSY` reflects synchronized `ND_RB`, while
  `NFC_IRQSTAT.NBUSYIRQ` latches its rising edge and is W1C.
- **CONFIRMED:** SIC source 48 can wake IDLE independently of CEC delivery.
- **CONFIRMED:** one operation boundary completes one pending reset or valid
  page read, produces one busy-to-ready edge, and latches one `NBUSYIRQ`.
  Page data remains unavailable until read completion.
- **UNKNOWN:** exact `tRST` and `tR`. The Micron datasheet gives maximum
  `tR=25 us` with internal ECC disabled; whether device internal ECC is active
  is **UNKNOWN**. The modeled boundary is not a hardware timing claim.

Production-model tests cover reset and a sparse page/OOB read, W1C clearing,
the SIC wake with CEC masked, no duplicate completion, and independence of the
reset and read paths. Existing program and erase timing was left unchanged.

## Controlled v241 result

Trial `build/nfc-page-read-wake-v241.log` retained all three PLL/shadow opt-ins.
The ROM issued `NFC_CMD=0x00` at `0xEF007D00`, address bytes
`00 08 00 00 00` (page 0, column 2048/OOB start), and `NFC_CMD=0x30` at
`0xEF007D1A`. IDLE `0xEF007D1E` woke at packet **16374**, with SIC1 bit 16
asserted and CEC `IMASK/IPEND/ILAT=0x1F/0x12/0` unchanged. A subsequent
page-0/column-0 read woke at IDLE `0xEF007C4A`.

`PLL_DIV=5` and `PLL_CTL=0x2000` were not observed. Reset-active PLL ambiguity
therefore remains unresolved and was not bypassed with a new assumption.

## Reconstructed DMA2 operation

At IDLE `0xEF007C62`, DMA2 had `START_ADDR=0xFF907F00`, `CONFIG=0x87`,
`X_COUNT=0x80`, `X_MODIFY=2`, inactive Y parameters, PMAP 2/NFC,
`CURR_ADDR=0xFF907F00`, `CURR_X_COUNT=0x80`, and `IRQ_STATUS=0x08`.
`CONFIG=0x87` is a 1-D, 16-bit, peripheral-to-memory receive in stop mode with
completion interrupt enabled. The requested transfer is therefore 128 16-bit
elements, or 256 bytes of page-0/column-0 NAND data, to
`0xFF907F00..0xFF907FFF`.

The exact instruction/MMIO sequence is:

- `0xEF007C12`: DMA2 `CONFIG=0`; `0xEF007C14`: W1C `DMA_DONE`.
- `0xEF007C1A`: DMA2 `CONFIG=0x87`.
- `0xEF007C2C..0xEF007C46`: page-0/column-0 NAND read sequence.
- `0xEF007C4A`: IDLE until NFC `NBUSYIRQ`, then acknowledge it.
- `0xEF007C5E`: `NFC_PGCTL=1` (`PG_RD_START`).
- `0xEF007C62`: IDLE; after wake, poll DMA2 `DMA_DONE`, loop while clear, and
  W1C completion at `0xEF007C70`.

**CONFIRMED:** the BF52x NFC procedure says `PG_RD_START` initiates page-read
DMA after NAND data is available. `CURR_X_COUNT` decrements per element,
`DMA_DONE` is asserted after the last memory write, DMA2 maps to SIC source 30,
and SIC wake eligibility is independent of SIC/CEC interrupt masking.

**CONFIRMED:** no transfer latency is required to model this finite boundary.
The implementation gates NFC DMA reads on `PG_RD_START`, services only the
observed DMA2 receive shape at IDLE, writes the 256 ready bytes, leaves
`CURR_ADDR=0xFF908000` and `CURR_X_COUNT=0`, asserts `DMA_DONE` and SIC source
30 once, and supports W1C acknowledgement. It does not add DMA pacing or
auto-complete other DMA channels. Cycle-accurate DMA arbitration, FIFO/request
cadence, and latency remain **UNKNOWN**.

## Controlled v241 result

In `build/dma2-nfc-completion-v241.log`, IDLE `0xEF007C62` woke at packet
**17092** with SIC0 bit 30 asserted and CEC `IMASK/IPEND/ILAT=0x1F/0x12/0`
unchanged. Execution continued through repeated NAND/NFC DMA operations and
into loaded firmware without an unsupported-IDLE stop.

The extended `build/dma2-nfc-followup-v241.log` reached the requested terminal
condition:

- `PLL_DIV=5` at `0xEF000C76`, packet **29255172**.
- `PLL_CTL=0x2000` at `0xEF000C78`, packet **29255173**.
- IDLE `0xEF000C7A`, packet **29255174**, IDLE entry **100265**.
- The existing deterministic PLL lock boundary advanced 512 CLKIN ticks and
  woke at `0xEF000C7C` with active control `0x2000`, provisional CCLK 400 MHz,
  and provisional SCLK 80 MHz.

No next unsupported boundary was established: the run had reached the required
PLL/IDLE terminal condition. The runner continued beyond it only because it
does not automatically terminate on that write sequence; no later behavior was
investigated.

## Bounded post-PLL observation

Fresh divisor-10 trials `build/post-pll-boundary-v241.log` (55 host seconds)
and `build/post-pll-nfc-rdrdy-v241.log` (45 host seconds) used the four required
PLL/shadow opt-ins. A temporary read-only diagnostic captured device state and
the recent MMIO ring at the candidate boundary; it was removed immediately
afterward and is not a source change to preserve.

The first trial reproduced the prior terminal sequence exactly:

- `PLL_DIV=5` at `0xEF000C76`, packet **29255172**.
- `PLL_CTL=0x2000` at `0xEF000C78`, packet **29255173**.
- IDLE at `0xEF000C7A`, packet **29255174**, followed by the existing 512-CLKIN
  lock boundary and wake to `0xEF000C7C`.
- At wake, SIC ISR was `0x00000001/0x00010000` and CEC
  `IMASK/IPEND/ILAT=0xC05F/0x8000/0`.

**CONFIRMED:** the first instruction after wake is `0xEF000C7C`
(`cc = R2 == R3`). Firmware returns from the ROM clock routine, continues in
loaded code, configures additional interrupt/peripheral state, and performs a
new NAND programmed-I/O read. No later IDLE was observed before the stop.

The MMIO sequence immediately preceding the eventual wait was:

- loaded code at `0x0197EA60..0x0197EA80` updated SIC assignment registers;
  it then wrote port mux state, disabled DMA3, configured NFC, cleared all NFC
  IRQ status bits, masked all NFC IRQs, issued NAND reset, observed and cleared
  `NBUSYIRQ`, and entered the loaded NAND read routine;
- `0xFFA07FA8..0xFFA07FE0` issued command `0x00`, address bytes
  `04 08 00 21 00`, and command `0x30`;
- `0xFFA07FEA/0xFFA07FFE` polled `NFC_IRQSTAT`, which progressed from `0` to
  `0x9` (`NBUSYIRQ|RD_RDY`), and `0xFFA08006` acknowledged bit 0;
- `0xFFA0801E` wrote zero to `NFC_DATA_RD`, after which the byte-read loop at
  `0xFFA08032..0xFFA0804C` waited on `RD_RDY`, read `NFC_READ`, stored one byte,
  and acknowledged readiness between bytes.

The first entry to this loop still had `NFC_IRQSTAT=0x8` and advanced. The
stable boundary is a later invocation/iteration:

```text
0xFFA08032  lsetup(0xFFA08036, 0xFFA0803C) lc0 = P4
0xFFA08036  R3 = W[P5] (Z)       ; P5 = 0xFFC03708, NFC_IRQSTAT
0xFFA08038  cc = bittst(R3, 3)   ; RD_RDY
0xFFA0803A  if cc jump 0xFFA08040
0xFFA0803C  nop
0xFFA0803E  jump.s 0xFFA08032
```

**CONFIRMED:** at packet **40000004**, the stable poll state was NFC
`CTL/STAT/IRQSTAT/IRQMASK=0/0x11/0/0x1F`, `COUNT=0x23C`, and `PGCTL=0`.
The complete 32-entry recent-MMIO ring consisted of reads at `0xFFA08036` from
`0xFFC03708`, all returning zero. The same PC and register context appeared at
basic-block samples 8,388,608, 16,777,216, and 33,554,432 and at the 40- and
50-host-second samples. This is a stable firmware polling loop, not a conclusion
drawn merely from elapsed host time.

**CONFIRMED:** the first new unsupported condition is therefore NFC
`IRQSTAT.RD_RDY` during a programmed-I/O NAND read, not NAND reset completion,
the modeled page-read busy boundary, `PG_RD_START`, DMA2 completion, or another
PLL wait. The current model has no evidence-backed programmed-I/O
ready/refill cadence beyond its existing flash-data availability behavior.
Exactly what BF524 NFC event must reassert `RD_RDY` here is **UNKNOWN**.

State at the stable boundary:

- SIC `MASK=0x00100000/0x00000100`, `ISR=0/0`,
  `IWR=0x00010000/0x00010000`; CEC `IMASK/IPEND/ILAT=0xD65F/0x8000/0`.
- DMA1 was disabled with PMAP 4/SPORT0 TX; DMA2 was disabled with
  `CURR_ADDR=0xFF908000`, PMAP 3/SPORT0 RX, and no completion status. No DMA
  activity participates in this wait.
- CoreTimer was powered, enabled, and auto-reloading: `TCNTL=7`,
  `TPERIOD=0x61A80`, `TSCALE=0`, `TCOUNT=0x1F860` at the stable capture.
  Firmware has therefore configured the timer, but it is not the polled
  condition. Its existing host-time-backed update remains disconnected and was
  not used to advance this boundary.
- SPORT0 and SPORT1 had `TCR1/RCR1=0`, reset-like word-length registers
  `0x1F`, and status zero. No SPORT enable, SPORT DMA transfer, or SPORT audio
  event was observed. DMA PMAP values show that firmware prepared SPORT0 DMA
  routing, but SPORT itself had not started.
- No normal tape UI was observed before the stop.

The second fresh process reached the same addresses and state, but its PLL
writes were 893 packet entries later than the first trial. Packet-entry timing
across processes is therefore not treated as deterministic while host-backed
devices and the scheduler remain disconnected. Host durations above are only
observational metadata, not guest timing.

## Programmed-I/O NFC completion

The BF52x hardware reference defines `NFC_DATA_RD` as a byte-read request,
`NFC_IRQSTAT.RD_RDY` as W1C completion state, and the `NFC_READ` access as the
operation that consumes the requested byte. Firmware pipelines the next
`NFC_DATA_RD` request before consuming and acknowledging the current byte.

**CONFIRMED:** the implementation now retains that one pending request,
reasserts `RD_RDY` only after the current byte is consumed and acknowledged,
increments the read-only 10-bit `NFC_COUNT` for each successful `NFC_READ`, and
allows the external NFC request to complete when NAND is not busy. The NAND
model continues to return erased `0xFF` after the selected page/OOB buffer is
exhausted; exact physical behavior at that endpoint is **UNKNOWN**. PIO ECC
accumulation is also **UNKNOWN** and remains unimplemented.

The controlled trials `build/post-nfc-pio-handshake-v241.log` and
`build/post-nfc-pio-handshake-no-count-v241.log` both passed the former
`0xFFA08036` wait and stopped at IDLE `0xEF00086A`, packet **18592**. The
count change was therefore not causal. Correct W1C behavior had removed a
stale SIC source 48 assertion and exposed a finite MDMA0 completion wait.

## Finite MDMA completion

The BF52x reference assigns MDMA stream 0 and 1 interrupts to SIC sources 42
and 43. It also defines zero `X_COUNT` as 65,536 elements. MDMA runs without a
peripheral request cadence until its finite transfer completes.

**CONFIRMED:** IDLE service now completes only matched MDMA source/destination
pairs in the observed narrow shape: one-dimensional, unsynchronized, stop
mode, no descriptors, matching element widths and counts, with a destination
completion interrupt. It supports a source that filled the internal FIFO and
completed before destination enable, arbitrary source/destination modifies,
and zero `X_COUNT` as 65,536 elements. Destination completion raises the
corresponding SIC source and W1C deasserts it. This is not general DMA pacing.

The intermediate trials exposed three distinct model gaps:

- `post-nfc-pio-mdma-v241`: source had already completed into the FIFO;
- `post-nfc-pio-mdma2-v241`: source used constant address modify zero;
- `post-nfc-pio-mdma3-v241`: both channels used zero `X_COUNT`.

After those corrections, `build/post-nfc-pio-mdma4-v241.log` ran for 60 host
seconds without `idle-unsupported`, reached `PLL_DIV=5` at packet **29104038**
and `PLL_CTL=0x2000` at packet **29104039**, and passed the old NFC poll
transiently.

## Current bounded splash observation

Fresh current-image trials extended the same result to 180 and 380 host
seconds. `build/post-nfc-pio-mdma-ui-v241.log` had no unsupported IDLE, displayed
the firmware **R.00241** splash by 120 seconds, and retained it through the
360-second capture. Audio generation remained zero. At 370 seconds the guest
had executed **3,098,402,121** packet entries, so this is not a halted CPU.

From approximately 110 seconds, execution is dominated by ordinary loaded code
at `0x01909F00..0x0190A31C`. A temporary read-only register/stack diagnostic,
removed after capture, established the call chain
`0x0190AF00 -> 0x0190A306 -> 0x0190A226`. The initial table prefix and observed
bound `0xFFFF` were interpreted as a finite chain; **that interpretation was
disproved by the immutable-input follow-up** linked above. It is still not an
MMIO poll or an unsupported hardware boundary. A 65,535-element MDMA
counterfactual produced the same table and traversal, so the documented
65,536th element is not causal.

The old `boot-content241-ui-check` screenshots reached the tape UI, but they
are not a valid A/B comparison: current `build/nand-working241.img` was modified
at 2026-10-07 21:21, after that run, and its per-run NAND clone is no longer
available. The current trials copied the same current base and reproduced the
same traversal. The follow-up establishes a reachable zero self-cycle with the
current input hash, rather than a finite path to the tape UI.

## Smallest next task

Immutable provenance and reproducibility are established in the linked follow-up.
The origin of table index 113's `0x8072` value is now diagnosed in the linked
construction follow-up, and the narrow Random Data Read fix is verified in the
previous checkpoint. The exact observed two-dimensional MDMA0 operation is now
documented and supported below, and tape UI has appeared. Next, if requested,
identify the producer/wake condition of the byte flag at `FF907DF6` at the
new `019F18F4` boundary before proposing another change.
Do not classify the
software cycle as a hardware wait, optimize guest code, fabricate
a table terminator, connect SPORT, alter CoreTimer or guest `CYCLES`, or redesign
the scheduler without a new explicit unsupported boundary.

## Reproduce

Use a new unused prefix. GUI trial needs X11/Xwayland. Nine selected tests
passed before the trial: `pll_idle_tests`, `execution_accounting_tests`,
`nand_ecc_tests`, `usb_reset_tests`, `mcp23017_tests`, `pcm_sample_tests`,
`sport_audio_tests`, `audio_ring_tests`, and `otp_import_tests`.

```bash
cmake --build build --target op1emu pll_idle_tests execution_accounting_tests sport_audio_tests usb_reset_tests mcp23017_tests pcm_sample_tests audio_ring_tests nand_ecc_tests -j2
ctest --test-dir build -R '^(pll_idle_tests|execution_accounting_tests|sport_audio_tests|audio_ring_tests|pcm_sample_tests|mcp23017_tests|nand_ecc_tests|otp_import_tests|usb_reset_tests)$' --output-on-failure
OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000 OP1EMU_SHADOW_TIMING=1 OP1EMU_TRACE_CLOCK=1 OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1 \
  python3 /tmp/opencode/boot_trials.py NEW-UNUSED-PREFIX 10 420
```

## Preserve

- `/tmp/opencode/boot_trials.py` (whitelist includes the three PLL/shadow opt-ins; depends on untracked `tools/check_gui_controls.py`)
- Trial evidence: `build/nfc-reset-wake-v241.log`,
  `build/nfc-page-read-wake-v241.log`, `build/dma2-reconstruct-v241.log`,
  `build/dma2-nfc-completion-v241.log`, `build/dma2-nfc-followup-v241.*`,
  `build/post-pll-boundary-v241.*`, `build/post-pll-nfc-rdrdy-v241.*`,
  `build/post-nfc-pio-handshake-v241.log`,
  `build/post-nfc-pio-handshake-no-count-v241.log`,
  `build/post-nfc-pio-mdma4-v241.*`, `build/post-nfc-pio-mdma-long-v241.*`,
  `build/post-nfc-pio-mdma-ui-v241.*`,
  `build/pll-awake-bypass-v241.*`, and prior
  `build/pll-idle-foundation-v241.*` (logs, screenshots, private NAND/OTP).
  Do not commit.
- Docs: `docs/bf524-pll-idle-foundation.md`, `docs/op1-clock-readiness.md`, this file.
- Local PDFs/text under `/tmp/opencode/`; hashes in the readiness doc.
- Unrelated dirty work and `src/cpu/timing_probe.h`.

## Finite 2D MDMA0 reconstruction and completion

Recovery baseline: parent `4d40b7d`, Bcore `37de8e8`; remote HEAD was verified
equal to the parent before changes. The existing unrelated dirty host/device
work remains outside this checkpoint. Read-only reconstruction trial:
`/tmp/opencode/mdma2d-reconstruct-v241.*`, with code disassembly captured from
live loaded memory and complete MDMA register readbacks.

### Exact captured registers

| Field | MDMA0 source (channel 13, base `FFC00F40`) | MDMA0 destination (channel 12, base `FFC00F00`) |
| --- | --- | --- |
| START_ADDR | `FF800994` | `03000000` |
| CONFIG | `0015` | `0097` |
| X_COUNT | `0004` (4) | `00C6` (198) |
| X_MODIFY | `0002` (+2) | `0002` (+2) |
| Y_COUNT | `09AB` (2475) | `0032` (50) |
| Y_MODIFY | `FFFA` (-6) | `0076` (+118) |
| CURR_ADDR at pre-fix IDLE | `FF800994` | `03000000` |
| CURR_X_COUNT at pre-fix IDLE | `0004` | `00C6` |
| CURR_Y_COUNT at pre-fix IDLE | `07AB` (1963) | `0032` |
| NEXT_DESC_PTR / CURR_DESC_PTR | `0 / 0` | `0 / 0` |
| IRQ_STATUS at pre-fix IDLE | `0009` (RUN plus an already-latched DONE) | `0008` (RUN, DONE clear) |
| PERIPHERAL_MAP model readback | `D000` (MDMA0 source role) | `C000` (MDMA0 destination role) |

Both channels have WDSIZE=1 (16-bit), DMA2D=1, SYNC=0, FLOW=0 (stop),
NDSIZE=0, DI_SEL=0. Source WNR=0, DI_EN=0; destination WNR=1, DI_EN=1.
The source has already supplied 512 four-element rows (4096 bytes) into the
model's abstract FIFO, leaving 1963 rows; the destination has not consumed them.
The 4096-byte model FIFO is not a claim about hardware FIFO depth/timing.

**CONFIRMED geometry:** source row width is four halfwords/eight bytes and
source row-start delta is `(4-1)*2-6 = 0`, so it repeatedly reads
`FF800994..FF80099B`. There are 2475 source rows. Destination row width is
198 halfwords/396 bytes, with 50 rows and row-start delta
`(198-1)*2+118 = 512` bytes. Destination addresses are
`03000000 + row*512 + column*2` for row 0..49, column 0..197.
The copied address span is `03000000..0300638B`, with 116-byte gaps between
rows; **19,800 bytes / 9,900 elements** are transferred, not the full span.
The source/destination row shapes differ, but total counts match:
`4*2475 = 198*50 = 9900`.

### Firmware instructions

- `018D461A/018D462E`: destination X_COUNT/X_MODIFY.
- `018D465A/018D4672`: destination Y_COUNT/Y_MODIFY.
- `018D4686`: disabled destination CONFIG=`0096`.
- `018D469E`: source START_ADDR=`FF800994`.
- `018D46B6/018D46C0`: source X_COUNT/X_MODIFY.
- `018D46D8/018D46EA`: source Y_COUNT/Y_MODIFY.
- `018D46F8`: disabled source CONFIG=`0014`.
- `018D471E`: destination START_ADDR, initially `03000000`.
- `018D4724`: source CONFIG=`0015`; `018D472A`: destination CONFIG=`0097`.
- `018D472C..018D4730`: pre-IDLE poll of destination IRQ_STATUS at
  `FFC00F28`, branch to `018D4740` when bit 0 (DONE) is set.
- `018D4736`: IDLE if the poll failed; `018D4738..018D473C`: post-wake DONE
  poll, loop back while clear.
- `018D4740`: W1C acknowledgement, writing R7=1 to destination IRQ_STATUS.
- `018D474C..018D4750`: compute the next destination chunk and advance R4.

### Authoritative hardware semantics and decision

Analog Devices **ADSP-BF52x Hardware Reference, Rev. 1.0, March 2010**,
local `bf52x-hrm-1.0.pdf/.txt`, chapters 5/6, is the reference actually inspected.
The earlier local file named `adsp-bf52x_hwr_rev1.2.pdf` is an HTML download-error
artifact and was not used as evidence.

**CONFIRMED** from pp.6-7..6-8, 6-11..6-13, 6-73..6-81, and table 5-1:

- MDMA requires both channels enabled, with equal element width and total
  transfer count. Row shapes may differ. Source CONFIG is written before
  destination CONFIG; the source fills a shared FIFO and destination drains it.
- Ordinary MDMA transfers continuously once enabled; an externally triggered
  HMDMA mode is a separate feature, not the operation reconstructed here.
- X_COUNT is elements per row; Y_COUNT is rows. Modifies are signed byte deltas.
- X_MODIFY advances within a row. At a nonfinal row's last element, **Y_MODIFY
  replaces X_MODIFY**, applied relative to that last element's address.
- At the final work-unit element, X_MODIFY is applied, not Y_MODIFY.
- Final visible registers are **CURR_X_COUNT=0, CURR_Y_COUNT=1**, with
  CURR_ADDR=last-element address+X_MODIFY. Source final address is `FF80099C`;
  destination final address is `0300638C`.
- Stop mode executes one work unit; NDSIZE must be zero. With DI_SEL=0 and
  DI_EN=1, destination interruption occurs after the complete array, not each row.
- DMA_DONE follows the last memory access and is W1C. MDMA0 maps to SIC source
  **42**, ISR1/IWR1 bit 10; MDMA1 maps to 43 (not added to this 2D extension).
- X_COUNT=0 means 65,536 elements. Neither observed X nor Y count is zero.

**UNKNOWN/out of this scope:** a zero-Y-count work unit, pin/arbitration details,
cycle-accurate bandwidth/latency, and externally handshaked operation. The new
2D predicate rejects zero X/Y counts; the established 1D zero-X=65,536 path
remains unchanged and passes its regression. No zero-Y assumption is introduced.

**Decision A:** the observed memory accesses, count termination, final addresses,
and DONE event require no additional external condition. They can be completed
at a deterministic functional device-operation boundary. This is not evidence
of zero silicon latency or a newly implemented timing model.

Crucially, captured SIC_IWR1=`00010000` has **MDMA0 bit 10 clear**. Simply
finishing at IDLE cannot legitimately wake the CPU via source 42. The supported
finite operation therefore completes on MDMA0 CONFIG enable once both channels
are ready, before the driver's pre-IDLE poll. **ASSUMED abstraction:** the
atomic configuration-operation boundary represents functional completion without
modeling elapsed hardware time. All bytes move through the existing FIFO/data
path; no DONE bit is fabricated without the actual memory transfers.

### Implementation and regression scope

`src/cpu/dma.cpp/.h` add only the aligned repeated-four-contiguous-halfword,
512-byte-destination-pitch MDMA0 shape: two-dimensional, 16-bit, nonzero counts,
equal total counts, stop mode, no descriptors, no SYNC, no row interrupts,
source DI_EN=0 and destination DI_EN=1. Only MDMA0 source/destination CONFIG
writes invoke the new device-operation service; the existing IDLE service can
also recognize this shape. Other shapes remain outside the completion predicate.

The existing X/Y address advance already performed `-X_MODIFY+Y_MODIFY` at
nonfinal row ends. Finite 16-bit MDMA stop state now preserves the documented
final Y count of 1, and completion IRQ selection uses work-unit completion.
Peripheral and descriptor paths retain their previous behavior. The existing
1D MDMA implementation, SPORT, timers, guest CYCLES, scheduler, and abstract
FIFO capacity were not changed.

`tests/mdma_2d_cases.h`, called by `execution_accounting_tests.cpp`, covers the
exact 4*2475 to 198*50 geometry, alternating FIFO phase across destination rows,
source-only FIFO-prefilled operation, source/destination address progression,
untouched gaps/guards, final addresses/counts, preserved programmed counts,
one DONE/SIC assertion, W1C, no repeated copy, and rejecting mismatched totals
and zero-Y shapes. The existing 1D zero-count test still passes. Tests use the
production memory/DMA/SIC interfaces, synchronously, without host waits or
firmware PC/address special cases.

All nine focused project tests passed. An exact selectively staged snapshot
was independently built under `/tmp/opencode/mdma2d-stage`; its emulator library,
`execution_accounting_tests`, and `pll_idle_tests` passed. The other seven local
focused tests belong to the preserved dirty setup, not clean snapshot targets.

### Controlled result and stop

Fresh trial `/tmp/opencode/mdma2d-fixed-v241.*` used unchanged frozen NAND
`c94057cb8000aa9516242c426eeb5c69df03300fd9c4e0f19893c6ef374d5a42` and OTP
`06c993496c3aba43ca345c62a2caa27b69d68e160cad8c29196152991a238c5a`.
Read-only probe executable hash:
`288ceaa9d44d8950e71d33a02fa1c2b5e9b7a2911c3c9790f6c9eaea389919cb`.

**CONFIRMED:** after the enable block, PC is `018D4740`, destination DONE=1,
CURR_ADDR=`0300638C`, X/Y=`0/1`; source CURR_ADDR=`FF80099C`, X/Y=`0/1`.
The pre-IDLE DONE poll succeeds; W1C at `018D4740` leaves destination status=0.
No entry/wake at `018D4736` is required. SIC42 assertion/deassertion and disabled
wake behavior are covered by the deterministic production-interface regression;
no SIC_IWR override was added. Subsequent same-shape work units also complete.

**CONFIRMED tape UI:** `mdma2d-fixed-v241-0334.png` displays normal reels,
track 1, and `0:00:00`, rather than R.00241 splash. Before capture, the CPU was
stopped immediately at the first subsequent unsupported IDLE:

- IDLE `019F18F4`, resume `019F18F6`, packets **1,402,209,588**.
- RETS=`FFA0BF74`, RETI=`01A340FE`, current IVG15.
- Preceding code reads byte `[P5]`, P5=`FF907DF6`, sees value 1, and executes
  IDLE; it would proceed when that byte becomes zero.
- SIC mask=`10104000/00100300`, ISR=`0/3`, IWR=`00014000/00010000`;
  CEC IMASK/IPEND/ILAT=`7FDF/8000/0`.
- The producer/eligible wake condition for this flag is **UNKNOWN**. It was
  not investigated or bypassed after tape UI appeared.

Host metadata: CPU stop at **329.025528 seconds**; process lifetime including
screenshot/shutdown **334.747 seconds**. Packet count is observational, not
deterministic timing. No execution continued through the new IDLE boundary.
The private trial NAND ended at
`ad80c61c602421d46c13e772c8e6cb18ef630c9d92bb977a104056233569475f`;
OTP and preserved base inputs remained unchanged. No private image, executable,
log, or screenshot is staged. No push is authorized/performed by this step.
