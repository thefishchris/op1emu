# Original OP-1 CLKIN, PLL activation, fixed costs and IDLE

## Subsequent implementation checkpoint

[BF524 PLL/IDLE foundation](bf524-pll-idle-foundation.md) supersedes the earlier
"not implemented" status below. The narrow model and architectural IDLE are
synthetic-tested. One v241 observation now stops after the first same-value
VR_CTL wake because its awake bypass interval lacks a running input-clock source.
The later changed-multiplier PLL sequence was not reached. A subsequent opt-in,
`OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1`, consumes new shadow-cycle deltas only
while temporary CCLK equals the provisional CLKIN. Its v241 trial passes the
first VR bypass and stops at NFC IDLE `0xEF00754C`. 25 MHz remains provisional;
shadow timing is not generally authoritative, and SPORT is unchanged.
The sections below retain the earlier investigation evidence.

## Update: user-supplied board close-up

The subsequently supplied `/home/chris/Projects/op-1_emu/board-pic.jpg` provides
better routing evidence than the earlier iFixit images. Its source URL is not
yet known; preserve that provenance distinction. Image size is 960 x 720 and
SHA-256 is `dd4291f273b7241e4d1f3fbc3eae6ecddf720bfb3278074d12efcdf89a9fb50c`.
The image itself was not edited or copied into this checkout.

Visible identities/markings:

- Original OP-1 DSP board `TE002EA001GP3`, different from the older iFixit board.
- `ADSP-BF524 KBCZ-4`, silicon marking `0.2`.
- Upper-left clock component clearly marked `24.00`.
- Lower-left component marked `25.00` (consistent with the older board's
  `25.000` marking).
- A separate small metal-can component also exists; its role is not inferred.

The BF524 A1 corner dot is visible at the package's upper-right in this photo.
Comparing that orientation to data-sheet Figure 76 (289-ball **top view**) puts
column 23 along the photo's lower package edge. CLKIN R23 and XTAL P23 are
approximately in the lower-edge middle/left region, whereas USB_XI AB23 and
USB_XO AA23 are near that edge's left end.

The lower 25 MHz component's two visible signal paths, through a passive network,
approach the package in the expected CLKIN/XTAL region. The upper 24 MHz network
approaches near the expected USB oscillator region. This is a meaningful
independent pin-map/topology cross-check, rather than choosing 25 MHz because
16 times 25 happens to equal the advertised core speed.

**Updated confidence: LIKELY, now substantially stronger:** 25 MHz is the main
CLKIN source, with a direct passive crystal/oscillator connection and no visible
external frequency-conversion IC in that network. The actual BGA balls and
buried fanout remain obscured; a photograph is not an electrical continuity
measurement. Absence of any intervening divider/multiplier is therefore still
provisional, not CONFIRMED.

This makes **25 MHz CLKIN / 400 MHz CCLK / 80 MHz SCLK a defensible, explicitly
provisional frequency assumption** for a controlled experiment on matching
original hardware, contingent on the observed MSEL=16, DF=0, CSEL=0, SSEL=5
normal-PLL request becoming active. It is not yet a measured active CCLK or a
permanent board fact for every revision. No frequency was selected in code.

The complete SPORT experiment remains blocked by the active-PLL and IDLE/wakeup
policy described below. Those gaps were not solved by the photograph. A source
link and higher-resolution original would strengthen provenance/routing; a
schematic or continuity/clock measurement would promote the CLKIN inference
from LIKELY to CONFIRMED.

The following sections retain the earlier investigation record, before this
new routing evidence was available.

## Decision

**Do not connect shadow cycles to SPORT yet.** A legible original-board photograph
now establishes the presence of a 25 MHz component, but not its electrical net
to BF524 CLKIN. An absolute CCLK is still not established to the requested
standard. No oscillator frequency has been selected in code.

Two shadow components were refined: ordinary `Ireg += 2` issue, and the generic
unconditional-redirection component for RTS. The full RTS cost remains flagged
unsupported; LINK/UNLINK/integer multiply still have no BF524-verified totals.
IDLE was investigated with a bounded site observer, not implemented as a wait.

SPORT, CoreTimer, CYCLES, DMA and event scheduling are unchanged. No PLL register
device, lock delay, power-mode implementation or guest time conversion was added.

## Board evidence

Source: iFixit **Teenage Engineering OP-1 DSP Board Replacement**, guide 65424,
step 6, second photograph; original image ID 883185, GUID `Ao1X5QASOWNuqFV6`.

- Guide: <https://www.ifixit.com/Guide/Teenage+Engineering+OP-1+DSP+Board+Replacement/65424>
- Guide metadata: <https://www.ifixit.com/api/2.0/guides/65424>
- Actual full-resolution photograph:
  <https://guide-images.cdn.ifixit.com/igi/Ao1X5QASOWNuqFV6.full>
- SHA-256: `f22456d5957ecc061e27349efce051023bed4fd8ddf271c9dd230791ff75c54a`.
- Local file: `/tmp/opencode/op1-dsp-front2.jpg`, 3172 x 2379 pixels.

This is an original OP-1 board, with `ADSP-BF524 KBCZ-4C2` visible. Board marking
is `OP-1 DSP TE002EA001D`. It is not an OP-1 Field board.

**CONFIRMED photograph observations:** adjacent frequency-marked components
include `25.000`, `24.000` and `TXC 16.0`. The 25 MHz marking is recoverable from
the crop `(x=1575,y=1500,width=95,height=130)`, rotated 90 degrees. Cropping,
rotation and contrast adjustment were used; no generated/upscaled text was
treated as new evidence beyond the original photographed strokes.

**LIKELY candidate:** the 25 MHz component could supply the main clock.
**UNKNOWN:** which marked component supplies CLKIN, whether it is a direct
crystal/oscillator connection, and whether any intervening divider/multiplier
exists. There are multiple clock domains and multiple nearby components; location
or agreement with a nominal 400MHz specification is not enough to identify a net.
The BGA fanout and internal PCB routing cannot be established from these photos.

The applicable BF52x data sheet distinguishes main CLKIN/XTAL from the dedicated
USB_XI/USB_XO source (pp. 13, 16–17). It permits either an external clock feeding
CLKIN or a crystal across CLKIN/XTAL. On the **289-ball KBCZ package** in the
photo, CLKIN is R23 and XTAL is P23 (Table 67, p. 79). Do not use the A10/A11
assignments from the separate 208-ball package table to infer this board's net.

No original OP-1 schematic/service document identifying the CLKIN net was located
in the sources consulted. Public firmware research at
<https://raw.githubusercontent.com/op1hacks/docs/master/README.md> did not supply
that net. Other original-board photographs from the same guide show the other
side, but do not resolve the BGA connection or pre-CLKIN circuitry.

**Conditional arithmetic only:** if the 25 MHz component is verified as a direct
CLKIN source, the observed normal-PLL request gives CCLK=400MHz and SCLK=80MHz.
This is not a confirmed clock assignment, nor an inference from the product's
400MHz specification. The physical clock-source branch stops here pending a
schematic, continuity/routing evidence, or measurement at CLKIN/CLKBUF/CLKOUT.

## Timing documents: actual URLs and hashes

Vendor-authored PDFs were downloaded and inspected locally. Failed WebFetch
requests were followed by `curl -L`, including HTTP/1.1 retries, then archival
retrieval. A redirect to an HTML page was not accepted as a PDF.

| Document used | Exact downloaded URL | SHA-256 |
| --- | --- | --- |
| Blackfin Programming Reference Rev. 2.0, March 2012 | `https://archive.org/download/manuallib-id-2701005/2701005.pdf` | `b2455a927cabb6c0bec76ca07157f7c92d8152a7748c58a4e28f2f8d934e8ba3` |
| BF52x Hardware Reference Rev. 1.0, March 2010 | `https://archive.org/download/manuallib-id-2700101/2700101.pdf` | `f83eae4837f544eca4396869f4a5957d9a77fb1c93947dbb6534008446cef37d` |
| BF522/523/524/525/526/527 data sheet Rev. C, March 2012 | `https://archive.org/download/manuallib-id-2590772/2590772.pdf` | `63b08e2a4e5fb2f0240e367347532c6c4d6a7e9064531527850e276e3d03ad6a` |
| EE-197, September 24, 2003 (comparison evidence only, explicitly BF531/532/533) | `https://archive.org/download/manuallib-id-2701053/2701053.pdf` | `76dc9a8a34aa1d63a823e2604f370d667e5388af401813883fadd984859e7af7` |

Primary vendor URLs tried for the newly downloaded PDFs:

- <https://www.analog.com/media/en/technical-documentation/data-sheets/ADSP-BF522_BF523_BF524_BF525_BF526_BF527.pdf>
- <https://www.analog.com/media/en/technical-documentation/application-notes/EE-197.pdf>
- EE-197 legacy URL also tried:
  <https://www.analog.com/static/imported-files/application_notes/EE-197.pdf>

Direct downloads reset HTTP/2 streams or timed out in HTTP/1.1. The apparent
BDTIC mirror actually embeds the same vendor URL in an HTML object. The archival
PDFs above are the actual documents read. They remain local under `/tmp/opencode/`.

An archived Analog Devices employee discussion was also inspected:

- Original:
  <https://ez.analog.com/dsp/blackfin-processors/f/q-a/57547/bf527-multi-cycle-instructions-and-latencies-any-document>
- Actual archive:
  <https://web.archive.org/web/20190717230942id_/https://ez.analog.com/dsp/blackfin-processors/f/q-a/57547/bf527-multi-cycle-instructions-and-latencies-any-document>

The discussion clarifies that BF52x and BF531/532/533 are separate processor
families; it does **not** establish EE-197 timing applicability to BF524. It
recommends EE-332 for profiling. The direct EE-332 URL
`https://www.analog.com/media/en/technical-documentation/application-notes/EE-332.pdf`
timed out under curl as well, and the archive searches performed did not locate
a usable copy. EE-332 was not used as timing evidence. A BF52x-applicable timing
table or explicit vendor confirmation is still needed for the uncovered totals.

## PLL: minimum state and activation semantics

**CONFIRMED vendor rules**, BF52x HR pp. 18-3–18-6, 18-14–18-16, 18-26–18-31:

- PLL_DIV changes apply dynamically; they do not require PLL relock by themselves.
- PLL_CTL writes record the requested configuration; they do not immediately
  change active PLL behavior. The documented SysControl programming sequence
  and its IDLE commit/wakeup step matter.
- On a multiplier change or PLL power reapplication, the internal lock counter
  restarts and counts CLKIN cycles until PLL_LOCKCNT expires. PLL_LOCKED and
  PLL wakeup then become asserted. The documented register reset count is
  `0x0200`, not a guessed host-time delay.
- The HR's prose and register summary state CLKIN units; the old Rev. 1.0
  figure caption on p. 18-28 inconsistently says SCLK. An implementation must
  follow a verified register specification/revision, not silently ignore this
  discrepancy. No delay was implemented here.
- If PLL_CTL/VR_CTL is rewritten with the same value, wakeup can be immediate
  while execution remains bypassed at CLKIN for PLL_LOCKCNT duration. Thus
  “returned from IDLE” and “running at multiplied CCLK” are not universally
  interchangeable states.
- BF524 lacks the BF523/525/527 internal voltage regulator. Do not transplant
  an internal-regulator voltage-ramp model into BF524. Relevant VR_CTL mode,
  clock-buffer and wake controls still require explicit treatment.
- Wakeup eligibility is distinct from CEC interrupt eligibility. Masked CEC
  interrupts do not imply that all wake sources are disabled.

**Minimum proposed representation, not implemented:**

1. Readable PLL_CTL, PLL_DIV, VR_CTL and PLL_LOCKCNT register values, with their
   BF524 masks/reset semantics.
2. Separate active PLL_CTL/mode from requested PLL_CTL/mode; active divider
   state changes on the documented PLL_DIV write.
3. Pending transition cause (changed multiplier/power, same-value programming,
   or relevant VR_CTL request), lock/bypass status and remaining CLKIN ticks.
4. An IDLE programming-sequence boundary, PLL_STAT fields derived from that
   state, and DPMC wake signaling honoring the selected wake sources.
5. For unobserved sleep/hibernate/voltage operations, an observable unsupported
   path rather than guessed settling times or unconditional successful lock.

A register-latching model plus a transition **bounded by programmed input-clock
ticks** can represent the observed normal-mode sequence without a broad power
framework. It cannot yet supply nanoseconds because CLKIN is unidentified.
Analog/external-voltage stabilization must not be replaced with a made-up delay.

### Actual observed programming sequence

In the new v241 run:

- PLL_DIV=5 at `0xEF000C76`, packet 29,281,781.
- PLL_CTL=0x2000 at `0xEF000C78`, packet 29,281,782.
- **IDLE at `0xEF000C7A` immediately next**, packet 29,281,783.
- The surrounding ROM code compares the temporary divider with the requested
  divider, potentially restores PLL_DIV, and returns at `0xEF000C84`.

This is strong evidence of the intended SysControl activation protocol on real
hardware, not proof of activation in this emulator. All clock registers are
still unmapped and Bcore's IDLE remains a no-op. No PLL_STAT/lock result from this
checkout can establish a physical active clock.

## Five requested fixed-cost gaps

| Instruction | Result for BF524 shadow accounting |
| --- | --- |
| `dagMODik_ADD2` / `Ireg += 2` | Ordinary one-issue component supported by PR 4-7–4-9 and the add-immediate form at 15-17–15-18. Removed unsupported fixed-class coverage for this opcode only; DAG dependencies stay unknown. Numeric baseline remains 1. |
| RTS | PR 7-12 describes an unconditional RETS-to-PC transfer; the generic PR 4-21 redirect component is modeled as baseline 1 plus loss 4. This is a **derived first-order component**, not a newly verified BF524-specific complete RTS total. Full RTS remainder and dependency coverage remain unknown. |
| LINK | PR explicitly establishes multi-cycle behavior and stack operations, but not a BF524 total here. Keep unknown. |
| UNLINK | Same: multi-cycle/stack semantics established; BF524 total unverified. Keep unknown. |
| Integer multiply `Dreg *= Dreg` | Semantics established by PR 15-52–15-53; BF524 fixed total unverified in the material retrieved. Keep unknown. |

EE-197 reports totals RTS=5, LINK=3, UNLINK=2, multiply=3 for **BF531/532/533**,
with L1 execution assumptions. These numbers were not imported as BF524 facts.
The RTS component's numerical agreement with that table is corroboration, not
the applicability proof. No costs were fitted to boot duration.

## Actual IDLE usage

New observer: at most 16 IDLE PCs, counts per site, first/last packet entry,
bounded first/power-of-two CEC/return-register samples, and one code-context dump
per mapped site. It changes no instruction behavior. The ten observed site counts
sum exactly to the IDLE opcode count; no overflow was needed.

Private boot-only trial: `build/idle-clock-followup-v241.*`, 420s, GPIO 17,
SPORT divisor 10, identical fresh NAND/OTP copies, CPU/audio diagnostics and
clock-write observation. The final screenshot shows the normal tape UI. At 420s:

- 2,316,835,109 packet attempts.
- 3,030,426,399 partial shadow cycles with the refined components.
- 97.1137% modeled issue components; 2.8863% unsupported fixed-cost packets.
- 4,303,080 IDLE entries. Counts describe Bcore's current busy execution, not
  the number/duration of hardware sleep intervals.

Observed roles:

| PC | Count at 420s | Evidence/interpretation |
| --- | ---: | --- |
| `0xEF000C3E` | 1 | VR_CTL write immediately followed by IDLE; programming-sequence wait |
| `0xEF000C7A` | 1 | PLL_DIV/PLL_CTL writes immediately followed by IDLE |
| `0xEF00086A` | 19,914 | ROM register/status loop around stores at peripheral offset 0x20 and a completion-bit test; peripheral wait, not ordinary compute |
| `0xEF00754C`, `0xEF0076C4` | 4 each | LIKELY NFC status waits, based on offset 0x3708 and transfer/control offsets; the MMIO base register was not independently sampled |
| `0xEF007C4A`, `0xEF007C62`, `0xEF007D1E`, `0xEF007D50` | 4,224 / 8,438 / 4,360 / 69,760 | Other ROM peripheral wait sites |
| `0x019F18F4` | 4,196,374 | Runtime byte-flag wait loop followed by DSP work |

The runtime site first appears at packet 1,559,335,725 (first logged in the 340s
sample), well after SPORT activation at approximately 138.44s. Its code reads
`B[P5]`, skips IDLE when the flag is zero, otherwise executes IDLE and branches
back to check again. The following block contains nested hardware loops and
multiply-accumulate processing. **LIKELY:** a DSP worker/buffer handshake.
**UNKNOWN:** the exact flag identity and which producer clears it; it was not
inferred solely from its location near audio code.

ROM samples include CEC IMASK/IPEND `0x1F/0x12`, i.e. masked reset context, yet
the code deliberately uses IDLE waits. Runtime samples have `0x7FDF/0x8000`,
with no latched event at the sampled entry. This demonstrates why a future idle
implementation must handle wakeup independently from merely checking CEC ILAT.

### Proposed IDLE policy

- Count the IDLE instruction's issue attempt once. Stop ordinary instruction
  issue while idle; do not charge its current millions of retry loops as real
  hardware busy-work.
- For PLL programming, advance the elapsed-time domain through the documented
  input-tick transition and wake condition. Do not use host sleeps or a guessed
  constant number of CPU cycles.
- For peripheral waits/runtime flag waits, allow the relevant independent
  peripheral/elapsed-time domain to progress to the next modeled wake event.
  Re-enter normal instruction execution and recheck the flag/status afterward.
- Wake source eligibility must respect SIC wake enables and need not imply an
  eligible CEC interrupt. If the next wake deadline is unknown, report that gap
  rather than freezing time forever or inventing a wake period.

This is a proposal only. It needs a narrow wake/deadline mechanism before the
SPORT causal trial; neither IDLE-as-NOP nor stopped time with no wake mechanism
is an evidence-backed hardware policy.

## Suitability and exact next prerequisite

**Ideal memory/no-stall issue mode is reasonable for a bounded causal test**, if
clearly labeled: it can test whether host/JIT time accumulation drives audio
demand without claiming silicon-accurate cache or bus timing. Missing dependency
and memory costs must remain visible and boot success cannot validate them.

**The current model is not ready:** CLKIN's net is unidentified; PLL active state
is not represented; and observed IDLE waits require elapsed-time/wake progress
that packet counts alone cannot supply. LINK/UNLINK/multiply full costs remain
unverified, and RTS still has explicit incomplete-cost coverage.

The smallest next step is to obtain/verify the original-board CLKIN connection
or measure CLKIN/CLKBUF/CLKOUT, using the 25 MHz photo component as a concrete
candidate rather than as a selected frequency. A schematic or continuity proof
to KBCZ CLKIN/XTAL would directly close this gap. Then verify the small normal-PLL
activation state and IDLE wake policy while still disconnected from device
clocks. A BF52x timing table or explicit vendor statement could separately close
the remaining fixed-cost totals. No SPORT experiment should proceed by assuming
25 MHz simply because it yields 400 MHz.

Verification: the added DAG/RTS component and IDLE-site tests pass, all eight
selected regressions pass, the shadow implementation passes strong-warning
syntax checking, and parent/submodule diff checks pass. No commit was made for
this follow-up; the accepted packet-entry checkpoint remains `55335b4`.
