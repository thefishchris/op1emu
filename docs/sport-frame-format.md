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

Decision: **the codec master clock, and therefore the SPORT0 frame rate, are
UNKNOWN.** No SPORT guest-clock pacing is implemented, and no 48 kHz value is
assumed. The emulator's fixed `sampleRateHz_ = 48000` remains an unsourced
legacy constant.

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
