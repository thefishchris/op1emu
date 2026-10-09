# Upstream deterministic guest-clock audit

Audit date: 2026-10-09. Scope: compare upstream op1emu's deterministic
guest-time work with this fork's PLL/IDLE timing foundation, **for plumbing
only**. This is not a design document; the design is
`docs/guest-clock-architecture.md`. Nothing here was merged, cherry-picked, or
adopted.

## Provenance

- Upstream repository: `https://github.com/op1emu/op1emu`.
- The work was developed on a `deterministic-guest-clock` branch and opened as
  **PR #13**, "feat: add a deterministic guest-time mode (--deterministic)",
  author xndcn.
- **Correction to `HANDOFF.md`:** the handoff describes this as an unmerged
  branch. It is not. PR #13 was **merged into upstream `main` on
  2026-10-09 14:53Z**, and the `deterministic-guest-clock` branch no longer
  exists on the remote (`git ls-remote` lists no such ref).
- PR head commits (the hashes recorded in the handoff) and their replacements
  on upstream `main`:

  | PR #13 commit | Upstream `main` commit | Subject |
  | --- | --- | --- |
  | `c914560` | `f872735` | feat: add a deterministic guest-time mode (--deterministic) |
  | `432fa9e` | `f5080a3` | fix: start wall time after CPU setup; bound --rtc-epoch to the RTC range |
  | `656c9e8` | `3634ecc` | fix: send the random accelerometer feed once under --deterministic |

- Upstream `main` at audit time: `3634ecc`. It also contains unrelated work
  this fork has not adopted, including `44a9aca` (bcore O1 IR pipeline),
  `9ac2135` (steady_clock CPU cycle base), and `2b56c09` (GP timer tick
  accumulation). These were not audited beyond their subjects.
- The PR moves the bcore submodule to upstream bcore `173ca1a`
  (op1emu/bcore#4), which adds `CpuState::packets`.
- PR #13 is the first of five stacked PRs (#14–#17: profiling hooks and
  `op1prof` tooling). Those are out of scope.

Local, temporary audit refs (fetched without adding remotes; to be deleted
after the design review):

- parent: `refs/upstream-audit/pr13` (= `656c9e8`), `refs/upstream-audit/main`
  (= `3634ecc`)
- `ext/bcore`: `refs/upstream-audit/main`

The common ancestor of this fork and PR #13 is `3b926b1`
(`chore: update bcore submodule`).

## What upstream `TimeSource` does

`src/cpu/time_source.h` (`c914560`, amended by `432fa9e`):

- A `TimeSource` with two models, chosen once at CPU construction by
  `--deterministic`:
  - **Wall** (default): `Cycles()` = host `steady_clock` microseconds since
    start × 400; `Nanoseconds()` = host nanoseconds since start.
  - **Deterministic**: `Cycles()` = packets retired; `Nanoseconds()` =
    packets × 2.5 (one CCLK per packet at a fixed 400 MHz, `kHz`).
- `Retire(packets)` adds a block's packet count; it is called after every
  `core_->run()`. Packets are counted in both modes.
- `Slowdown()` returns `kWallSlowdown = 10` in wall mode and 1 in
  deterministic mode. CoreTimer and SPORT divide by it.
- `SystemNow()` supplies RTC calendar time: the host `system_clock` in wall
  mode, or a fixed epoch (default 2024-01-01, `--rtc-epoch`) plus guest
  nanoseconds in deterministic mode. `432fa9e` bounds the epoch to the RTC's
  15-bit day field (`kMaxEpochSeconds`, ends 2059-09-18).
- `StartWallClock()` (`432fa9e`) resets the wall origin at the end of the CPU
  constructor so device/JIT setup is not counted as guest time.
- `PanelOscillator::Service(nowNs)`: in deterministic mode only, the panel TE
  line (PORTG3) is driven as a 60 Hz guest-time oscillator with a 1 µs low
  pulse, using exact integer deadlines `(frame * 1e9 + 59) / 60`. In wall mode
  the frontend's host poll still drives TE.

Plumbing changes around it:

- `BlackFinCpu` owns `TimeSource time_` and `PanelOscillator panel_`, declared
  before the devices that hold references to them.
- `CoreTimer`, `RTC`, and `SPORT` constructors take `const TimeSource&`; their
  direct `std::chrono` calls are replaced by `TimeSource` calls.
- `Run()` reads `time_.Cycles()` at block start, writes `CYCLES`, calls
  `coreTimer->UpdateCycles()`, runs the block, then calls
  `time_.Retire(cpuState_->packets)`, and in deterministic mode calls
  `ServicePanel()` before `ProcessEvents()`.
- `656c9e8`: in headless deterministic runs, leading accelerometer/volume
  script commands are applied before the CPU thread starts, and the random
  accelerometer feed sends one sample per poll.
- Tests: `tests/time_source_test.cpp`.

Upstream's own reported limits (PR body): an "issue model with no pipeline,
cache or PLL timing"; deterministic mode runs roughly 5–10 M guest cycles/s,
so interactive use stays on wall time; no GUI smoke test was run.

## Structurally reusable (as patterns, not code)

1. **One time authority, injected.** A single CPU-owned time object passed by
   `const&` to devices, declared before them, with no device calling host
   clocks directly. This fork's uncommitted SPORT `NanosecondClock` injection
   already moves in the same direction.
2. **Read-then-retire per block.** Guest time is sampled before a block and
   advanced by that block's accounted work afterwards. This keeps "what time is
   it" separate from "time advances".
3. **Exact integer deadlines.** `PanelOscillator` computes its next edge from a
   frame index with integer rounding instead of accumulating a floating or
   truncated period. This is a usable template for a deadline object.
4. **Wall-origin placement** (`StartWallClock`) and an **explicit, bounded RTC
   epoch** for reproducible calendar time.
5. **Pre-start input application** for reproducible headless runs.

## Incompatible with this fork's model

1. **Fixed 400 MHz, no PLL.** `kHz` is a constant. This fork models BF524
   PLL registers, lock countdown in CLKIN ticks, and frequency changes that take
   effect through architectural IDLE (`src/cpu/pll.{h,cpp}`). Upstream has no
   CLKIN, no CCLK/SCLK split, and no separate RTC crystal domain; every device
   derives from the same 400 MHz-per-packet value.
2. **Packets treated as CCLK.** One packet is one cycle in deterministic mode.
   This fork's established decision is that packet entries are execution
   accounting, not CCLK (`docs/bf524-execution-accounting.md`).
3. **No architectural IDLE.** Upstream time advances only by executed packets.
   This fork stops packet execution in IDLE (`ext/bcore/src/core.cpp:42`), so
   upstream's model would freeze guest time at any IDLE and no timed wake could
   ever occur. **LIKELY (not verified):** upstream boots past IDLE sites because
   upstream bcore does not halt there, so the firmware's `idle; jump` loop keeps
   retiring packets and therefore advances guest time.
4. **Pull-only, no deadline query.** Devices learn the time only when `Run()`
   pokes them. Nothing can report the earliest pending device event, which is
   what advancing through IDLE requires. Even `PanelOscillator`'s deadline is
   private.
5. **Wall-mode slowdown retained.** `kWallSlowdown = 10` is renamed, not
   removed. This fork classifies the equivalent dividers as TEMPORARY HACK
   (`docs/hacks.md`).
6. **Different packet counter semantics.** Upstream bcore `173ca1a` stores a
   per-block packet *index* in `CpuState::packets` (`emit_packet_index`, reset
   to 0 in `Core::run`). This fork's bcore exposes a cumulative
   `CpuState::packet_entries` (`ext/bcore/src/lift_visitor.cpp:307`). Porting
   `time_.Retire(cpuState_->packets)` unchanged would be wrong here.
7. **Two coexisting time models.** Upstream keeps wall and deterministic modes
   permanently. This charter forbids host-time guest semantics as an end state;
   any wall-time path here is a documented temporary hack, not a peer mode.

## CoreTimer observations affecting the design

From this fork's working tree (`src/cpu/coretimer.cpp`, `src/cpu/cpu.cpp`) and
the BF52x Hardware Reference, chapter 11:

- **CONFIRMED (code):** `Run()` returns before `coreTimer->UpdateCycles()`
  while `cpuState_->idle` is set (`src/cpu/cpu.cpp:405`), and `ServiceIdle()`
  has no CoreTimer deadline. It reports `idle-unsupported` on its first check.
  The core timer therefore **cannot** expire during IDLE in the current
  emulator, whatever the hardware does. The `0x019F18F4` "UNKNOWN next wake"
  is at least partly an emulator limitation.
- **CONFIRMED (code):** `ServiceIdle()` evaluates only SIC wake
  (`SIC::WakePending`, `ISR & IWR`) and the PLL wake. A core event such as
  IVTMR (IVG6) does not pass through the SIC; bcore clears `idle` when it
  dispatches any eligible core event (`ext/bcore/src/cec.cpp:215`), but
  `ServiceIdle()` does not run `ProcessEvents()` before a SIC wake, so a queued
  core-timer raise would not be delivered during IDLE.
- **CONFIRMED (code):** the cycle base is host `system_clock` (not even
  monotonic) × 400 (`src/cpu/cpu.cpp:361,465`). Upstream moved to
  `steady_clock` in `9ac2135`.
- **CONFIRMED (code):** `ticks /= 10` remains (`docs/hacks.md`, "Core timer
  slowdown").
- **CONFIRMED (code), at odds with HRM:**
  - `startCycles == 0` is a "not yet started" sentinel; cycle 0 is
    indistinguishable from "unset".
  - `TCOUNT` is recomputed as `TPERIOD - elapsed/scale` rather than
    decremented from its current value, so a direct `TCOUNT` write before
    enabling (HRM 11-5: allowed, "to let the initial period be shorter or
    longer") is ignored for the first period.
  - The HRM says `TPERIOD` and `TCOUNT` writes are ignored while the timer is
    running; the model accepts them.
  - Expiry is detected only at block boundaries.
  - `UpdateCycles` returns early when `tcount == 0`.
- **CONFIRMED (HRM 11-1..11-3):** the core timer runs at CCLK; `TCOUNT`
  decrements every `TSCALE + 1` CCLK; reaching 0 sets sticky `TINT` and raises
  IVTMR; `TAUTORLD` reloads from `TPERIOD`; the request goes directly to the
  CEC (not the SIC), is edge-sensitive and is cleared automatically when
  serviced. `TINT` is not W1C (write 0 to clear; optional). TMREN with
  TMPWR=0 is undefined.
- **Captured state at `0x019F18F4`** (`docs/pll-idle-awake-bypass-checkpoint.md`):
  `TCNTL=7`, `TPERIOD=0x61A80` (400000), `TSCALE=0`, `TCOUNT=0x55780`, no
  `TINT`; IMASK `0x7FDF` enables IVG6; `EVT6=FFA09A88`. The `TCOUNT` value
  was produced by the host-time ÷10 model and is not a trustworthy guest
  count.
- **UNKNOWN:** whether the core timer keeps counting while the core is in
  IDLE. See the architecture document for the evidence on both sides.

## Local dirty-work inventory (must remain untouched)

Recorded at audit time with parent `HEAD` `1b21548` on `wip/pll-idle-timing`
(3 ahead of origin, not pushed) and bcore `ae6b207` on `wip/op1-timing`
(1 ahead of `fork`, not pushed). Per instruction: no staging, committing,
splitting, cleaning, stashing, or pushing. The working tree, including the 11
registered parent tests, is the baseline.

Modified, tracked:

```text
.gitignore                      gui/usbipd.cpp            src/cpu/sport.cpp
CMakeLists.txt                  src/cpu/coretimer.cpp     src/cpu/sport.h
README.md                       src/cpu/cpu.cpp           src/cpu/usb.cpp
docs/bf524-execution-accounting.md  src/cpu/dma.cpp       src/cpu/usb.h
docs/pll-idle-awake-bypass-checkpoint.md  src/cpu/nand.cpp  src/peripheral/MT29F4G08.cpp
docs/v241-nand-provenance-and-traversal.md  src/cpu/nand.h  src/peripheral/MT29F4G08.h
ext/cqueue/cqueue.cpp           src/cpu/otp.cpp           src/peripheral/mcp230xx.cpp
gui/audio_output_miniaudio.cpp  src/cpu/otp.h             src/peripheral/mcp230xx.h
gui/audio_output_miniaudio.h    gui/main.cpp              tests/execution_accounting_tests.cpp
gui/glfw_display.cpp            gui/ui.json
```

Untracked:

```text
AGENTS.md  CLAUDE.md  HANDOFF.md  run-op1.sh
micron_technology_micts06228-1-1759217.pdf
docs/audio-startup-divergence.md  docs/controls-audio.md
docs/guest-progress-clock-inspection.md  docs/hacks.md  docs/local-working-setup.md
gui/audio_ring.h  gui/pcm_sample.h
src/cpu/nand_ecc.cpp  src/cpu/nand_ecc.h  src/cpu/timing_probe.h
tests/audio_ring_tests.cpp  tests/mcp23017_tests.cpp  tests/nand_ecc_tests.cpp
tests/otp_import_tests.py  tests/pcm_sample_tests.cpp  tests/sport_audio_tests.cpp
tests/usb_reset_tests.cpp
tools/check_gui_controls.py  tools/import_otp_dump.py  tools/provision_yaffs.cpp
```

This document and `docs/guest-clock-architecture.md` were committed together
as the design checkpoint; they are not part of the dirty inventory above.

Explicitly preserved: all tests; the `ext/cqueue/cqueue.cpp` `<cstdint>` fix;
`src/cpu/timing_probe.h` and its hooks; `docs/hacks.md`; private artifacts under
`build/` (NAND/OTP images and clones, logs, screenshots) and `/tmp/opencode/`.

Classification for a *later* commit task (not acted on):

| Class | Contents |
| --- | --- |
| Self-contained candidates | cqueue fix; `.gitignore`; NAND ECC (`nand_ecc.*`, its test, `provision_yaffs`); OTP import (`import_otp_dump.py`, its test, `otp.*`, `OP1EMU_OTP_FILE`); USB session/reset (`usb.*`, `usbipd.cpp`, its test; feeds the `SESSION=0` evidence); MCP23017/18 (`mcp230xx.*`, its test, expander-model selection); host audio (`audio_ring.h`, `pcm_sample.h`, `audio_output_miniaudio.*`, two tests); GUI controls (`ui.json`, `glfw_display.cpp`, `OP1EMU_HOLD_COM`, `check_gui_controls.py`, `docs/controls-audio.md`); docs and project instructions |
| Hold for the guest-clock design | SPORT clock injection and `OP1EMU_AUDIO_PACING_DIVISOR` (`sport.*`, `sport_audio_tests.cpp`, `audioClock` in `cpu.cpp`) |
| Diagnostics | `timing_probe.h` and hooks in `dma.cpp`, `coretimer.cpp`, `cpu.cpp`, `sport.cpp`; `OP1EMU_TRACE_*` hunks in NAND, OTP, USB, MCP, display code |
| Mixed, need hunk-level split | `CMakeLists.txt`, `src/cpu/cpu.cpp`, `gui/main.cpp`, `src/cpu/sport.cpp`, `src/peripheral/MT29F4G08.cpp`, `tests/execution_accounting_tests.cpp` |
| Local only, never commit | `HANDOFF.md`, `run-op1.sh` (private image paths), Micron datasheet PDF (third-party copyright), `build/`, `/tmp/opencode/` |

Noted gap: `docs/hacks.md` lists only the audio and core timer slowdowns. The
GP-timer `cyclesElapsed % 10000` tick and the opt-in PLL awake bypass are not
listed there.
