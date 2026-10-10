# Execution profiles: strict and functional

The emulator has two explicit execution profiles. Timing assumptions are
selected, validated and listed in one place, `src/cpu/timing_profile.cpp`,
and reported at startup:

```text
Emulator profile=functional assumptions=6
  timing assumption: CLKIN=25000000 Hz: PROVISIONAL/LIKELY (board-photo inference), not CONFIRMED
  timing assumption: PLL awake bypass: ...: EXPERIMENTAL
  timing assumption: shadow timing: observational issue model, used here as the awake-bypass cycle source
  timing assumption: guest clock cost model min-cost: ASSUMED, not cycle accurate
  timing assumption: ASSUMED: CoreTimer continues during plain IDLE for functional profile (...); not CONFIRMED BF524 behaviour
  timing assumption: ASSUMED: external SPORT0 audio clock corresponds to 44.1 kHz native frame rate (LIKELY from firmware audio-engine evidence; codec MCLK inferred as 11.2896 MHz); ...
```

| Profile | Purpose | May stop at unresolved hardware behaviour? |
| --- | --- | --- |
| `strict` | correctness, research, debugging | yes, by design |
| `functional` | usable progress with explicit, documented assumptions | yes, if no documented assumption covers it |
| unset | pre-profile behaviour, for compatibility | as before |

Neither profile fakes hardware state.

- **IDLE:** architectural IDLE stays real in both. There is no no-op IDLE and
  no packet spinning through IDLE.
- **PLL:** registers come from the PLL model in both; nothing is a silent
  zero.
- **Host time:** neither profile adds host-time guest semantics.
  - **SPORT:** the pre-existing ÷10 slowdown is a TEMPORARY HACK. It applies
    only without the functional guest clock (strict, unset, or functional with
    `OP1EMU_EXPERIMENTAL_GUEST_CLOCK=0`). Functional paces SPORT on the guest
    clock at the ASSUMED 44.1 kHz frame rate, with no slowdown and no host
    time.
  - **Core timer:** the legacy ÷10 applies only without the guest clock
    (strict or unset). Functional uses the guest-clock core timer, which has
    no slowdown.

## Selecting a profile

- `OP1EMU_PROFILE=strict|functional` applies to the GUI and headless.
- `--profile strict|functional` applies to headless only (`op1emu-headless`
  or `op1emu --headless`).
- Any other value is rejected at startup.
- With neither set, the profile is **unset** and behaviour is unchanged: each
  individual variable below is read exactly as before, and nothing is enabled
  implicitly.

## Precedence

1. `--profile` (headless) overrides `OP1EMU_PROFILE`.
2. Under `functional`, an individual variable that is **explicitly set**
   overrides that one profile default. For example, `OP1EMU_SHADOW_TIMING=0`
   turns shadow timing off, which also disables the awake bypass because it is
   a prerequisite. The startup log names the source of each setting
   (`default`, `profile`, `environment`).
3. Under `strict`, assumption-bearing variables are **rejected**, not
   overridden: `OP1EMU_EXPERIMENTAL_CLKIN_HZ` (any value) and
   `OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1` stop startup with an error.
   Observational and already opt-in strict variables stay allowed.
4. Value validation is the same in every profile (for example, CLKIN accepts
   only `25000000`).

## Timing control inventory

| Variable | Default | Purpose | `strict` | `functional` | Externally configurable |
| --- | --- | --- | --- | --- | --- |
| `OP1EMU_EXPERIMENTAL_CLKIN_HZ` | unset (no CLKIN Hz) | provisional board CLKIN (LIKELY 25 MHz); prerequisite of the awake bypass | **rejected** | set to 25000000 | yes; overrides the profile |
| `OP1EMU_SHADOW_TIMING` | 0 | observational shadow issue model; cycle source of the awake bypass | allowed (observational) | set to 1 | yes; overrides the profile |
| `OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS` | 0 | advance the same-value-VR bypass countdown (CCLK=CLKIN) from awake shadow deltas | `=1` **rejected**, `=0` allowed | set to 1 | yes; overrides the profile |
| `OP1EMU_EXPERIMENTAL_GUEST_CLOCK` | 0 | guest clock and HRM core timer (docs/guest-clock-architecture.md) | allowed, opt-in; a running core timer **holds** plain IDLE; SPORT stays on host time | set to 1; enables the plain-IDLE core-timer and 44.1 kHz SPORT frame-rate assumptions | yes; `=0` under functional also drops both assumptions |
| `OP1EMU_GUEST_CLOCK_COST` | `min-cost` | guest clock cost model | allowed | not set | yes |
| `OP1EMU_TRACE_CLOCK` | 0 | MMIO clock-observation trace | allowed | not set | yes |

Some controls exist only in uncommitted local work at the time of writing:
`OP1EMU_AUDIO_PACING_DIVISOR` (defaults to the same ÷10), `OP1EMU_OTP_FILE`,
`OP1EMU_GPIO_MODEL` (defaults to 17), and the `OP1EMU_TRACE_*` and
`OP1EMU_TIMING_PROBE` diagnostics. The profiles do not set them, and none is
needed for the v241 boot at its default. The pacing divisor affects only the
legacy host-time SPORT pacing; under the functional guest clock it is ignored
(and logged as ignored).

## What each profile assumes

- **strict** enables no timing assumption. On the current v241 fixture it stops
  explicitly after 1,969 packets at `EF000C3E` with `awake PLL bypass
  countdown needs a running CLKIN time source`: the firmware's same-value VR
  write starts a 512-tick bypass countdown that no evidence-backed clock can
  advance while awake. This is identical to the unset default.
- **functional** enables the assumptions the known-good local boot already
  used:
  - **CLKIN = 25 MHz:** PROVISIONAL/LIKELY, from a board photo, not
    CONFIRMED.
  - **Shadow timing:** observational issue estimate, used here only as the
    cycle source for the awake bypass.
  - **PLL awake bypass:** EXPERIMENTAL ideal/no-stall advance of the bypass
    countdown while CCLK = CLKIN, with block-level expiry.

  plus, since 2026-10-09, one decided assumption:
  - **Guest clock** (min-cost, ASSUMED) with the HRM core timer.
  - **CoreTimer during plain IDLE = ASSUMED RUNNING, for usability.** The
    CCLK-core domain advances during plain architectural IDLE, and the core
    timer's IVG6 may wake the core. This is not a CONFIRMED BF524 behaviour:
    the documentary search found no answer, and the counterfactual shows the
    choice is guest-observable (guest-clock architecture §10–§11).
  - **SPORT frame rate = 44.1 kHz, ASSUMED** (since 2026-10-09).
    - CONFIRMED: SPORT0 is externally clocked.
    - LIKELY: 44.1 kHz is the firmware's engine rate, implying an 11.2896 MHz
      codec MCLK.
    - UNKNOWN: the physical clock, which has not been measured.
    - In effect: SPORT words are paced on guest CLKIN (44,100 frames and
      88,200 stereo words per guest second) instead of host time. Details are
      in `docs/sport-frame-format.md`.

  IDLE then advances deterministically to the earliest modelled deadline (PLL
  lock, core timer expiry, or the end of a SPORT DMA row), with no packet
  spinning and no host time. A SPORT deadline wakes the core only through a
  DMA interrupt enabled in `SIC_IWR`. The
  v241 fixture boots to the tape UI, crosses the boot-ROM PLL IDLE and the
  power-manager IDLE `0x019F18F4`, and stays interactive.
- **strict** never applies these assumptions: core timer during plain IDLE =
  UNKNOWN, and a running core timer is a `held-domain-active` stop; SPORT
  stays on the legacy host-time pacing.

## Reproducible commands

`run-op1.sh` needs an explicit profile and inputs. It never modifies them: NAND
opens with `--nand-snapshot`, and the OTP is copied into a fresh private run
directory, which is printed. Private images stay outside Git.

```sh
# A. strict research run (headless, stop at the first unsupported boundary)
./run-op1.sh --headless --profile strict --nand PATH/TO/nand.img --otp PATH/TO/otp.bin \
    -- --stop-on-unsupported-idle --max-runtime 600000

# B. functional boot to the tape UI (GUI)
./run-op1.sh --profile functional --nand PATH/TO/nand.img --otp PATH/TO/otp.bin

# B'. the same, headless, with a screenshot at the first unsupported boundary
./run-op1.sh --headless --profile functional --nand PATH/TO/nand.img --otp PATH/TO/otp.bin \
    -- --stop-on-unsupported-idle --max-runtime 1200000 --screenshot-on-stop NEW.ppm
```

`OP1EMU_NAND` and `OP1EMU_OTP_SOURCE` may replace `--nand` and `--otp`.

**Build dependency:** booting a genuine v241 dump needs the NFC ECC
P2-complement fix (`docs/nand-boot-investigation.md`), which is committed in
`src/cpu/nand_ecc.cpp` and regressed by `nand_ecc_tests`. Without it, the boot
ROM takes its ECC error path (IDLE at `EF000748`, `RETS=EF0079EC`) under any
profile. Committed history now boots the fixture to the tape UI with
`--profile functional`.
