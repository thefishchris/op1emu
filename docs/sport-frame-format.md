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
