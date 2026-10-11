# Temporary hacks, assumptions and known limitations

This is the register of everything in the committed emulator that is not
plain, evidence-backed hardware behaviour. Each entry is classified as in
`AGENTS.md`:
- **TEMPORARY HACK:** a deliberate workaround with a removal condition.
- **ASSUMED:** a working assumption, not CONFIRMED.
- **Known limitation:** behaviour that is missing or incomplete.

Update this file whenever one of these changes.

## Temporary hacks

### H1. Legacy SPORT pacing ÷10

| Field | Details |
| --- | --- |
| Where | `SPORT::DMARead` / `SPORT::DMAWrite`, `src/cpu/sport.cpp` (`FIXME: too slow, / 10`) |
| What | Without a guest clock, SPORT0/1 DMA makes samples available at one tenth of the nominal host-time rate |
| When active | Only when SPORT is paced on host time: profile `strict`, profile unset, or `functional` with `OP1EMU_EXPERIMENTAL_GUEST_CLOCK=0`. The default `functional` profile paces SPORT on the guest clock at an ASSUMED 44.1 kHz frame rate (A6), so this hack is inactive there. |
| Why | Inherited from the original code. With full-rate host-time delivery the guest falls behind its audio DMA. The ÷10 keeps the legacy profiles usable. |
| What it bypasses | Correct coupling between guest execution speed and audio sample timing. In the legacy profiles host audio receives about 10 % of real-time samples, so the output is mostly underrun silence. |
| Evidence | `docs/sport-frame-format.md`, `docs/emulator-profiles.md` |
| To remove | Derive SPORT timing from modelled clocks (codec/SCLK) in every profile, or retire the host-time profiles |

### H2. Legacy core timer ÷10

| Field | Details |
| --- | --- |
| Where | `CoreTimer`, `src/cpu/coretimer.cpp` (`FIXME: currently the emulator runs slow, so let ticks /= 10`) |
| What | The host-time core timer counts one tenth of the elapsed CYCLES-derived ticks |
| When active | `strict`, unset, or `functional` without the guest clock. The `functional` profile uses the guest-clock core timer (`src/cpu/guest_coretimer.cpp`), which has no slowdown. |
| Why | As H1: the host-time timer otherwise fires far faster than the slow guest can service it |
| To remove | As H1 |

### H3. Host-time CYCLES and GP timer cadence

| Field | Details |
| --- | --- |
| Where | `BlackFinCpu::RunBlock`, `src/cpu/cpu.cpp` |
| What | **CYCLES** is set from host microseconds × 400 (an ASSUMED 400 MHz core clock). The general-purpose timers advance by one tick each time that host-derived count is a multiple of 10,000 (`FIXME: use correct clock`). |
| When active | All profiles |
| Consequences | Guest reads of CYCLES and the GP timers depend on host speed, so runs on different hosts are not reproducible. The deterministic regression harness substitutes a guest-derived host clock for this reason (`BlackFinCpu::SetHostClock`). |
| To remove | Clock CYCLES and the GP timers from the guest clock and the modelled PLL/SCLK |

### H4. Fixed-value I²C stand-ins

| Field | Details |
| --- | --- |
| Where | `BlackFinCpu::BlackFinCpu`, `src/cpu/cpu.cpp`, using `DummyI2CPeripheral` (`src/cpu/twi.h`) |
| What | Seven unidentified or unmodelled I²C devices answer every read with a fixed byte. Comments guess: an ADC at `0x4a`, an LTC2941 battery gauge at `0x64` (`0x3C`), an Si4713 FM radio at `0x11` (`0x80`). One of them selects a firmware display-orientation flag. |
| Why | v241 probes these devices during start-up and needs a plausible answer to continue |
| What it bypasses | Real device registers: battery state, radio, ADC and others |
| To remove | Identify each part (board inspection, firmware access patterns) and model the registers v241 uses |

### H5. Expander 0 pin 6 held high

| Field | Details |
| --- | --- |
| Where | `src/cpu/cpu.cpp` (`TODO: this bit controls DAT_01f007e0`) |
| What | The input pin is forced high at power-on; its meaning is UNKNOWN |
| To remove | Identify the signal on the board |

## Assumptions (ASSUMED or LIKELY, not CONFIRMED)

The `functional` profile lists and logs its timing assumptions at start-up
(`src/cpu/timing_profile.cpp`, `docs/emulator-profiles.md`):

| # | Assumption | Status |
| --- | --- | --- |
| A1 | CLKIN = 25 MHz | PROVISIONAL/LIKELY, inferred from a board photo (`docs/op1-clock-readiness.md`) |
| A2 | PLL awake bypass: the same-value VR countdown advances from shadow-timing estimates while CCLK = CLKIN | EXPERIMENTAL (`docs/pll-idle-awake-bypass-checkpoint.md`) |
| A3 | Shadow timing: an observational issue-cost model, used as the awake-bypass cycle source | Model, not cycle accurate (`docs/shadow-timing-coverage.md`) |
| A4 | Guest clock cost model `min-cost` | Not cycle accurate (`docs/guest-clock-architecture.md`) |
| A5 | The core timer keeps counting during plain IDLE | Not CONFIRMED BF524 behaviour |
| A6 | The external SPORT0 audio clock gives a 44.1 kHz frame rate | LIKELY from firmware evidence (`docs/sport-frame-format.md`) |

Other assumptions, which apply in every profile:
- **SPORT RX:** with no audio input attached, receive DMA delivers silence
  (zero samples). A real ADC would give near-zero noise.
- **GPIO expanders:** all eight are MCP23017s. The interrupt wiring between
  them and to PORTG is ASSUMED (`FIXME` in `cpu.cpp`; see
  `docs/mcp23017-expander.md`).
- **Default `otp.bin`:** created when missing, with a synthesized chip ID and
  part string (`docs/otp-image.md`). Booting v241 needs a real OTP image
  instead.

## CONFIRMED behaviour relied on

These rest on documentation or direct firmware observation, not assumption:
- **NAND:** the image layout (all page data, then all OOB) and the NFC ECC
  parity fix (`docs/nand-boot-investigation.md`, `nand_ecc_tests`).
- **v241 filesystem OOB layout:** tags at OOB+4 and parity at OOB+32,
  observed in a page the guest wrote (`docs/yaffs-provisioning.md`).
- **SIC:** level-sensitive interrupt delivery
  (`docs/sic-interrupt-delivery.md`).
- **MCP23017:** IOCON bit 0, interrupt acknowledgement and open-drain
  behaviour (`docs/mcp23017-expander.md`).

## Known limitations

| Area | Limitation |
| --- | --- |
| Speed | Execution is well below real time: about 0.1–0.16 guest seconds per host second on the development machine, even with opt-in multi-block execution (`docs/multiblock-execution.md`). Boot to the tape UI takes minutes. |
| Host audio | Because the guest runs below real time, the host audio device receives far fewer samples than it plays, so output is mostly underrun silence in every profile (more so in the legacy ÷10 profiles). Audio quality is not a supported feature. |
| USB | Partial. `USB_OTG_DEV_CTL` is not modelled (reads 0). There is no bus-reset indication, no SESSION/VBUS state, no emulation of the status stages a USB/IP control transfer needs, and no USB DMA. A TX packet completes as soon as the guest marks it ready, even with no host IN request. USB/IP enumeration and the firmware updater are not supported. |
| Real-time clock | Reads host wall time |
| Determinism | Host-time-derived CYCLES, GP timers and RTC (H3), and host-paced frontend polling (display TE, inputs), make ordinary runs vary between hosts and runs |
| Sensors | The frontends feed randomised accelerometer values unless the headless script fixes them (`accel X Y Z`) |
| GPIO expanders | The `BANK = 1` register map and OLAT pin driving are wrong. Device `0x27` is probably an MCP23008 (`docs/mcp23017-expander.md`). |
| NAND | The MT29F4G08 model does not implement command `0x50` (logged once during v241 boot) |
| CPU | `HaltExecution`, `SaveContext` and `RestoreContext` are empty stubs. This is not a cycle-accurate BF524 model. |
| Scope | Only the original OP-1 with firmware v241 (and v246 for inspection) has been exercised; the OP-1 Field is not targeted |
