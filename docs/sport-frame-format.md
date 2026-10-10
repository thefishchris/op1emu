# SPORT frame format: TCR2 / RCR2 decoding

## Evidence

**CONFIRMED** from the rendered register diagrams of the ADSP-BF52x Hardware
Reference: Figure 24-26 `SPORT_TCR2` (p. 24-50) and Figure 24-28 `SPORT_RCR2`
(p. 24-56). The text extraction loses the bit columns, so the diagrams were
rendered from the PDF and read directly.

| Bits | `SPORT_TCR2` | `SPORT_RCR2` | Meaning |
| --- | --- | --- | --- |
| 4:0 | `SLEN` | `SLEN` | word length = value + 1 (00000 and 00001 illegal) |
| 8 | `TXSE` | `RXSE` | secondary side enable |
| 9 | `TSFSE` | `RSFSE` | stereo frame sync: the frame sync becomes the L/R clock |
| 10 | `TRFST` | `RRFST` | 1 = right channel first (stereo mode) |

Both registers reset to `0x0000`. Stereo serial operation (HRM 24-11): setting
`TSFSE`/`RSFSE` changes the frame sync to an LRCLK-style left/right clock
(I²S, left-justified), so each frame carries one left and one right word.

## Previous defect

`src/cpu/sport.cpp` decoded `TSFSE`/`RSFSE` from bit 8 and `TXSE`/`RXSE` from
bit 9 (swapped). The v241 firmware writes `TCR2`/`RCR2` = `0x021F`: 32-bit
words, stereo L/R frame sync, primary side only, left first. The emulator
read it as "secondary enabled, mono" and passed interleaved left/right words
to the host as consecutive mono samples.

## Fix and the preserved cadence

The fields now follow the diagrams, and `DecodeSportConfig2()` and the
`TransmitConfig2()`/`ReceiveConfig2()` accessors expose the decoded settings
(`tests/sport_config_tests.cpp`).

SPORT pacing still counts **words**, exactly as before. The DMA word and
interrupt cadence is therefore unchanged by the decode fix, and words are
grouped into whole L/R frames only for the audio callbacks. With the existing
host-time ÷10 pacing, a stereo stream now delivers 2,400 frames per host
second (still 4,800 words), so host underruns remain. Pacing by frames at the
nominal rate (which would double the word rate for stereo) is left to the
SPORT clock work.

## Observations (v241, functional profile, divisor 10, snapshot NAND)

- **Guest words, unchanged by the fix:** pairs of 32-bit words. The first
  word (left) is a smooth signed signal (lag-1 r ≈ 0.97). The second word
  (right) was exactly zero throughout the captured note window.
- **Host-bound PCM, loudest one-second window:**
  - before: 50% zero samples, first-difference/signal energy ratio 2.0
    (strong near-Nyquist content), zero-crossing rate 0.41;
  - after: left channel 0% zeros, energy ratio 0.055, zero-crossing rate 0.17;
    right channel silent.
- **Unchanged by the fix:** SPORT0 TX 4,800 words and 37.5 DMA completions
  per host second. Host underruns remain (about 95% of output frames).

## External audio clock: MCLK and sample-rate investigation (2026-10-09)

Decision at the time: **the codec master clock, and therefore the SPORT0
frame rate, are UNKNOWN.** No 48 kHz value is assumed. The emulator's fixed
`sampleRateHz_ = 48000` remains an unsourced legacy constant, used only by the
legacy host-time pacing. Firmware static analysis later found a LIKELY
44.1 kHz engine rate, which the functional profile now assumes (see
"Functional-profile guest-clock pacing" below). The physical clock is still
unmeasured.

### SPORT0 is externally clocked (CONFIRMED)

Firmware writes, captured at the register interface (v241, functional
profile), with meanings from the rendered HRM Figure 24-25 (`SPORT_TCR1`,
p. 24-49):

| Register | Writes | Meaning |
| --- | --- | --- |
| `TCR1`, `RCR1` | `0x4400`, then `0x0001` | ITCLK=0 (external serial clock), ITFS=0 (external frame sync), TFSR=1, TCKFE=1; then enable |
| `TCR2`, `RCR2` | `0x021F` | see above |
| `TCLKDIV`, `TFSDIV`, `RCLKDIV`, `RFSDIV` | none | not used |

The SPORT's bit and frame clocks come from outside the BF524. SCLK and the
SPORT dividers do not set the audio rate.

**Separate defect (not fixed here):** `SPORT_TCR1`/`RCR1` model only TSPEN,
TDTYPE and TLSBIT. Writes to ITCLK, ITFS, TFSR, DITFS, LTFS, LATFS and TCKFE,
and to the divider offsets, are dropped silently and read back as 0
(`RegisterDevice` ignores unknown offsets). The HRM also says TCR1 writes other
than TSPEN are ignored while the SPORT is enabled.

### Codec (CS42L52 CONFIRMED; register semantics LIKELY)

- **Package marking (CONFIRMED):** `CIRRUS 42L52CNZ B1KC1727 CN`, read from
  the user-supplied original DSP-board photo `board-pic.jpg` (board
  `TE002EA001GP3`). On the board revision photographed by iFixit the codec is
  not visible.
- **Bring-up:** I²C address 0x4A, performed once, about 0.5 s of host time
  before SPORT0 is configured and enabled, and never changed afterwards
  (600-second capture, including play and a held note). The sequence:
  1. chip-ID read of register `01` (the emulator dummy returns `0x00`; a real
     part returns about `0xE0`; separate stub);
  2. `05=20, 06=A5, 02=00, 08=00, 09=00, 12=00, 13=00, 16=00, 17=00, 0D=80,
     04=A5, 0F=00, 0E=03`.
- **Decoded with the Linux `cs42l52` driver definitions** (LIKELY; the Cirrus
  DS680 datasheet was not consulted directly):
  - `06 = 0xA5`: codec is **master**, I²S for ADC and DAC, 24-bit word length.
  - `05 = 0x20`: auto-detect off, **single-speed** mode, no 32 kHz group, no
    27 MHz MCLK, ratio code `R_128`, no MCLK/2. In the driver's table this
    combination is 12.288 MHz → 48 kHz or 11.2896 MHz → 44.1 kHz, i.e.
    **frame rate = MCLK / 256**.
  - `02 = 00` (all powered), `04 = A5` (headphones on, speakers on-high),
    `0D = 80` (headphone gain 0.7099, unmuted), `0E = 03` (soft ramp and
    zero-cross; de-emphasis off), `0F = 00` (unmuted). The whole sequence is
    consistent with a CS42L52.

### MCLK source (UNKNOWN)

- **Visible clock parts on original DSP boards:**
  - `25.00`/`25.000` — LIKELY the BF524 CLKIN; see `op1-clock-readiness.md`.
  - `24.00`/`24.000` — near the USB oscillator pins.
  - an unidentified small part (`7S15S`/`75155`, not legible).
  - `TXC 16.0`, on the iFixit board.

  None is an audio MCLK frequency. No oscillator is visible next to the codec,
  and none is visible on the photographed underside of the older board.
- **The single-speed configuration needs MCLK of about 10–13 MHz** for its
  4–50 kHz range. A direct 24 or 25 MHz feed would give 93.75 or 97.66 kHz,
  which is out of range. Divided candidates (12 MHz → 46.875 kHz,
  12.5 MHz → 48.83 kHz, 12.288 MHz → 48 kHz, 11.2896 MHz → 44.1 kHz) cannot
  be distinguished from the available evidence.
- **The BF524 is not observed generating MCLK:** the GP timers were never
  enabled, and the firmware strings contain no PWM, MCLK or clock-buffer
  references. This is negative, LIKELY evidence.

### I²C device 0x58 (LIKELY speaker amplifier, not a clock)

- **Writes**, configured twice early in boot (at about 2.5 s and 31.5 s):
  `01=03, 02=3F, 03=01, 04=00, 05=10, 06=74, 07=03`.
- **Firmware debug descriptors** from `classd_controller.cpp` include `agc fix
  gain`, `outp lim level`, `max gain` and `comp ratio`. These match a TI
  TPA2028D1-class smart-gain class-D amplifier, whose fixed I²C address is
  0x58 and whose registers 1–7 are function control, attack, release, hold,
  fixed gain, AGC/limiter, and compression/max gain.

### Firmware constants (supporting only)

- In `OP1_vdk.ldr`, the 32-bit little-endian byte pattern of 48000 appears 25
  times (any alignment) and that of 44100 four times. The single 12288000 match sits in a sequence of
  `0x00BB7000/0x00BB8000/0x00BB9000` values and is coincidental. Usage was not
  traced, so this proves nothing.
- The strings `off / 32 khz / 44.1 khz / 48 khz` belong to the codec
  debug-page field descriptors (`AudioCodecController`), most plausibly the
  de-emphasis options. They are not a runtime sample-rate setting.
- Public specifications list a "24-bit/96 kHz ADC/DAC". That is converter
  capability; the configured single-speed mode excludes 96 kHz operation.

### What would resolve it

Any one of:
- a schematic or continuity trace of the CS42L52 MCLK pin;
- an oscilloscope or frequency-counter measurement of MCLK or LRCK on a real
  original OP-1;
- metadata from audio recorded or exported by an original OP-1 (the AIFF
  `COMM` sample rate), if it reflects the codec rate rather than a resampled
  export;
- tracing the firmware's use of the 48000/44100 constants in the audio path.

## Functional-profile guest-clock pacing (2026-10-09)

### Labels

- **CONFIRMED:** SPORT0 is externally clocked (`TCR1`/`RCR1` = `0x4400`,
  dividers never written); the codec is a CS42L52; the frame format is
  `0x021F` (32-bit words, stereo L/R frame sync, primary side, left first).
- **LIKELY:** the native audio engine rate is 44.1 kHz, from firmware static
  analysis (tape AIFC `COMM` writer, `BodeAudioEngine` 1/44100, clip database,
  sampler prelisten, rate arithmetic; no 48000 use). Under the observed codec
  setup (single speed, `R_128`, frame = MCLK/256) that means
  MCLK = 11.2896 MHz.
- **ASSUMED:** the functional emulation uses a 44,100 Hz SPORT frame rate.
  The startup log lists it: `ASSUMED: external SPORT0 audio clock corresponds
  to 44.1 kHz native frame rate (LIKELY from firmware audio-engine evidence;
  codec MCLK inferred as 11.2896 MHz)`.
- **UNKNOWN:** the physical MCLK/LRCK, which has not been measured on hardware.

### Scope

- The assumption lives in `src/cpu/timing_profile.cpp`
  (`TimingConfig::sportFrameHz`). Only the functional profile with the guest
  clock and a CLKIN frequency sets it.
- `strict`, unset, and functional with `OP1EMU_EXPERIMENTAL_GUEST_CLOCK=0`
  keep the legacy host-time pacing unchanged, including its ÷10 slowdown.
- In guest mode SPORT reads no host time, so the ÷10 slowdown and the local
  `OP1EMU_AUDIO_PACING_DIVISOR` control have no effect.

### Scheduling

Exact integer arithmetic on the canonical guest CLKIN timeline. There is no
floating point, no accumulated rounding, and no host time.

- **Start:** each direction's start is the guest CLKIN tick of its first DMA
  access after `TSPEN`/`RSPEN`. This is the legacy anchor point, kept. Underrun
  before DMA starts is not modelled.
- **Frames due** after `e` CLKIN ticks:
  `floor(e × 44100 / 25,000,000)`, evaluated as
  `(e / CLKIN) × fs + (e mod CLKIN) × fs / CLKIN` (`SportFramesDue`). It is
  exact and has no intermediate overflow.
- **Words due** = frames due × words per frame. The words per frame come from
  the decoded `TCR2`/`RCR2` (`TSFSE`/`RSFSE`), with no special case for
  `0x021F`:
  - stereo: 2 words per frame, so **88,200 words per guest second**;
  - mono: 1 word per frame, 44,100 words per guest second.
- **Delivery:** DMA requests are satisfied with whole frames up to the words
  due, as before. The difference is that time is now guest time.
- **Deadline:** the first tick with `N` frames due is
  `ceil(N × CLKIN / fs)` (`SportClkinForFrames`).
- **IDLE:** functional IDLE (`AdvanceFunctionalIdle`) adds a third candidate
  next to the PLL lock and the core timer. It is the earliest tick at which a
  running SPORT DMA channel can finish its current row, the only point where
  completion, a data interrupt, an autobuffer reload or a descriptor fetch
  occurs. SPORT is externally clocked and DMA is not a core clock domain, so
  both keep running in IDLE (SCLK peripherals, guest-clock architecture §3).
- **Each step:**
  1. advance to the earliest candidate;
  2. move every SPORT DMA word due at that timestamp, on all four SPORT DMA
     peripheral types;
  3. sync the core timer;
  4. only then let the wake decision run.

  SPORT words due at the current time are also moved on IDLE entry, before the
  wake check. Equal deadlines share one step.
- **Wakes:** reaching a SPORT deadline never wakes the core by itself. A DMA
  interrupt raised at the deadline follows the existing DMA → SIC path and
  wakes only through `SIC_IWR`, exactly like any other SIC source.
- **Clock domains:** guest CLKIN advances while awake only when the PLL ratios
  are known. Before that, SPORT time does not advance; this is a lower bound,
  as for the rest of the guest clock.

### Preserved behaviour (observed, not changed)

- **DMA:** the DMA and interrupt routing is unchanged.
- **Bound DMA interrupts:** the emulator binds DMA interrupts only for
  channels 0–3 and 12–15. Under the default peripheral map, SPORT0 TX (DMA4,
  SIC IRQ 17) completions raise no interrupt. v241 remaps one SPORT0
  direction onto DMA1: its SIC IRQ 28 rises matched the SPORT completion
  count exactly in both runs below, so the firmware's audio interrupt is
  delivered.
- **SIC forwarding delay:** in the committed code the SIC → CEC forward is queued with the
  default 1 ns `QueueEvent` delay, so an IWR wake latches its IVG at the next
  awake `ProcessEvents`.

### Tests

`tests/sport_guest_clock_tests.cpp` covers:
- exact arithmetic and first-tick deadlines;
- 44,100 frames and 88,200 stereo words per guest second;
- mono at 32 and 16 bits;
- no drift over ten guest seconds with irregular steps;
- no host time (frozen guest time stays frozen while host time passes);
- disable/re-enable;
- in functional IDLE: deadline ordering, exact RX completion cadence (3,445
  rows of 128 frames in ten guest seconds), no wake without an IWR source, an
  IWR-enabled DMA3 wake exactly at its row deadline, and interleaving with the
  core timer;
- the host-time divisor variable having no effect;
- strict, unset and functional without the guest clock never pacing SPORT on
  the guest clock.

`tests/sport_config_tests.cpp` still pins the stereo decode.

### Firmware results (v241, functional profile, snapshot NAND, two runs)

Setup: an isolated diagnostic build of this change, with per-host-second
counters and the guest-word capture; none of that instrumentation is
committed. Each run used the same headless script as the earlier audio
diagnosis: boot, two UI screenshots, play/stop, three held notes. Both runs
had `OP1EMU_AUDIO_PACING_DIVISOR=10` set; the log reports it as ignored.

| | run A | run B |
| --- | --- | --- |
| SPORT start (host / guest CLKIN) | 154.1 s / 32,703,696 | 146.9 s / 32,677,543 |
| guest/host ratio, boot to SPORT start | 0.0085 | 0.0089 |
| guest/host ratio, SPORT running | 0.0265 | 0.0264 |
| guest seconds with SPORT running | 8.25 | 8.32 |
| TX and RX words per guest s | 88,200.2 | 88,200.1 |
| SPORT DMA completions per guest s (each direction) | 689.07 | 689.05 |
| SIC IRQ 28 rises = completions | 5,683 = 5,683 | 5,733 = 5,733 |
| core timer expiries per guest s | 1,000.0 | 1,000.0 |

- **Cadence:** 689.06 completions per guest second is 88,200 / 128 words
  (64-frame rows). Before this change, the same firmware showed 37.5
  completions and 4,800 words per *host* second under ÷10 host pacing. The
  0.1-word excess is sampling skew between counters read at host-second
  boundaries. Words due are exact by construction and by test.
- **IDLE:** SPORT row deadlines appear as functional-IDLE steps at the
  power-manager IDLE `019F18F4` (`source=sport`, with words moved at each
  step). There was no unsupported boundary, no assertion, and both runs exited
  normally.
- **UI:**
  - The tape UI appears.
  - `play` runs the transport; the counter advances.
  - `stop` holds it.
  - The held notes show the synth indicator.
  - Panel TE is still on host time, so UI frames per *guest* second (about
    240) are not meaningful.
- **Starvation:** the guest runs at about 1/38 of real time, so SPORT delivers
  about 1,400 frames per host second against a 48,000 Hz host device. That is
  about 98% underrun, now caused entirely by execution throughput rather than
  pacing (the host sink also still resamples nothing: it plays 44.1 kHz frames
  at 48 kHz). Live audio is not real time.
- **Determinism:**
  - The SPORT arithmetic is deterministic.
  - Firmware-level runs are not: boot already diverges because RTC, panel TE
    and GP timers use host time. The two word captures are identical for the
    first 200 frames (the first nonzero sample is at frame 72 in both), then
    differ.
- **Offline audio:** the captured guest words are written as 44.1 kHz,
  32-bit stereo WAV files (evidence kept locally, not committed). Loudest
  second, at 7.25 guest s:
  - left channel: a smooth signal (RMS about 0.006 FS; first-difference/energy
    0.059 and 0.066; zero-crossing rate 0.14 and 0.17);
  - right channel: zero, as before.
