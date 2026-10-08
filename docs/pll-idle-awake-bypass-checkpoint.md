# Checkpoint: NFC DMA passed; PLL_DIV/CTL IDLE observed

Investigation stopped at the requested `PLL_DIV=5`, `PLL_CTL=0x2000`, IDLE
terminal condition. SPORT, CoreTimer, guest `CYCLES`, general DMA pacing, and
the general scheduler remain disconnected.

## Git

- Checkout: `/home/chris/Projects/op1-emu-gui-test/op1emu`
- Branch/HEAD: `wip/pll-idle-timing` at `75ebcdb` (`wip: model BF524 PLL and architectural idle`)
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

## Smallest next task

Start a fresh bounded observation immediately after the completed
`0xEF000C7A` PLL lock IDLE and stop at the first explicit unsupported hardware
or timing boundary. Do not infer behavior from the post-terminal portion of the
existing run, invent DMA pacing, connect SPORT, alter CoreTimer or guest
`CYCLES`, or redesign the scheduler without evidence.

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
  `build/pll-awake-bypass-v241.*`, and prior
  `build/pll-idle-foundation-v241.*` (logs, screenshots, private NAND/OTP).
  Do not commit.
- Docs: `docs/bf524-pll-idle-foundation.md`, `docs/op1-clock-readiness.md`, this file.
- Local PDFs/text under `/tmp/opencode/`; hashes in the readiness doc.
- Unrelated dirty work and `src/cpu/timing_probe.h`.
