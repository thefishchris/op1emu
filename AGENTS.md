# OP-1 Emulator Engineering Charter

This charter applies to everyone who changes this repository, human or
automated. It describes how the project works, not just what it contains.

## Project scope

This project emulates the original Teenage Engineering OP-1 (Analog Devices
Blackfin BF524). It does not target the OP-1 Field. It is an independent
project, not affiliated with or endorsed by Teenage Engineering.

**Current milestone:** firmware **v241** boots to the normal tape screen.
- Tape play, stop and record entry work from the GUI and from headless
  scripts.
- A held synth key produces engine output.
- See the feature status table in `README.md`.

**Firmware versions:** other firmware versions, including v246, may be used
as evidence for research, but v241 is the supported target. Do not claim
support for any other version without observed firmware progress.

**Not finished:**
- Emulation is well below real time.
- Live audio is mostly underrun silence.
- USB is incomplete.
- Several devices are stand-ins.
- The emulator is not cycle accurate.

Keep `README.md` "Limitations" and `docs/hacks.md` truthful about this.

## User-supplied inputs

The emulator needs data that this project does not contain and must never
contain:
- the BF52x boot ROM image (`docs/boot-rom.md`);
- an OP-1 NAND image;
- an OTP image (`docs/otp-image.md`);
- OP-1 firmware.

Rules:
- Never commit firmware, ROM, NAND, OTP or other device dumps, or any file
  derived from them: extraction output, converted images, screenshots of
  dumps, byte arrays.
- Never put ROM, OTP or NAND bytes, or device-unique values, in source,
  tests, logs, documentation or commit messages.
- Tests must use synthetic data. Tests that need real inputs must skip
  cleanly when the inputs are absent; public CI never has them.
- Tools that read user inputs must treat them as untrusted (see "Binary
  parsing safety") and must not modify the originals: open NAND images as
  snapshots and work on copies of OTP images.
- Do not write local paths, machine names or personal details into tracked
  files.

## Execution profiles, assumptions and temporary hacks

The emulator has explicit execution profiles (`docs/emulator-profiles.md`):

| Profile | Purpose | Behaviour at unresolved hardware |
| --- | --- | --- |
| `strict` | correctness, research, debugging | stops by design; assumption-bearing settings are rejected |
| `functional` | usable progress | applies only documented assumptions, and stops where none applies |

Rules:
- **No faked hardware:** neither profile fakes hardware state. Unknown
  behaviour goes through an explicit, observable stub.
- **Recording assumptions:** every assumption a profile applies is listed in
  `src/cpu/timing_profile.cpp`, printed at startup and recorded in
  `docs/hacks.md` with its classification.
- **Temporary hacks:** acceptable only during investigation, and must be
  obvious. Each needs an entry in `docs/hacks.md` recording:
  - what it does;
  - where it lives;
  - what it bypasses;
  - the evidence;
  - what must be learned to remove it.
- **Long-term target:** zero unexplained hacks.
- **When behaviour changes:** update `docs/hacks.md` in the same change
  whenever a hack, assumption or limitation is added, changed or removed.

## Evidence over guesses

This is an emulator and reverse-engineering project. Never invent hardware
behaviour to make firmware advance.

**Sources:**
- Analog Devices documentation, including the BF52x and Blackfin
  references;
- OP-1 reverse engineering and board evidence;
- firmware observations;
- controlled experiments;
- public research.

**Classify uncertain findings explicitly:**
- `CONFIRMED`
- `LIKELY`
- `ASSUMED`
- `UNKNOWN`
- `TEMPORARY HACK`

Assumptions must not silently become implementation facts. Record the
evidence, the interpretation, the implementation consequence and the
remaining uncertainty in `docs/`.

**Claims:**
- Never claim CPU execution without evidence: PC values, executed
  addresses, register changes, memory accesses.
- Never claim firmware progress without observing it.
- Never claim hardware accuracy, real-time behaviour or audio quality that
  has not been measured.

## Boot-driven hardware investigation

When firmware reaches unsupported hardware behaviour:
1. Stop at the first explicit unsupported boundary.
2. Record the PC, the surrounding instructions, MMIO state, interrupt state
   and relevant device state.
3. Identify the exact hardware condition the firmware is waiting for.
4. Research only enough hardware behaviour to model that condition.
5. Implement the smallest evidence-backed behaviour.
6. Add deterministic regression tests.
7. Re-run only until the next unsupported boundary.
8. Document the result before continuing.

**Not allowed:** making firmware advance with arbitrary timing constants,
host sleeps, periodic wakes or fabricated device behaviour.

**Scope:** do not broaden a narrow device investigation into a general
redesign unless evidence shows it is necessary.

## Correctness first

Prioritise, in order:
1. correctness;
2. observability;
3. testability;
4. maintainability;
5. performance.

Do not optimise prematurely, hide failures or return fake values from
unknown hardware. Performance work must not change guest-visible behaviour
unless that change is the documented intent. Verify it with a deterministic
replay.

## Keep changes small

For each task:
1. Inspect the current state.
2. Implement the smallest useful change.
3. Build it.
4. Test it.
5. Run it.
6. Inspect the results.
7. Document the discoveries.
8. Commit it logically.

A feature is not finished merely because it compiles.

## Tests

- Maintain unit tests, integration tests and optional input-dependent tests.
- Cover happy paths, malformed input and boundary cases.
- Add regression tests for bug fixes when practical.
- `ctest` must pass on a clean checkout.
- Tests that depend on private inputs must skip cleanly when the inputs are
  absent.
- For changes that can affect guest behaviour, compare a deterministic v241
  replay (guest-derived host time, fixed input script) against the previous
  baseline, and report any difference.

## Binary parsing safety

Treat all firmware and device images as untrusted input.
- **Validate:** offsets, lengths, integer arithmetic, decompressed sizes,
  archive paths and block counts.
- **Protect against:**
  - integer overflow;
  - path traversal;
  - malformed LZMA, tar archives and LDR blocks;
  - out-of-bounds access.
- **Structs:** do not cast binary buffers to native structs unless packing,
  alignment and endianness are handled explicitly.

## C and C++ discipline

**Prefer C++** unless evidence makes C more appropriate for a component.

**Use:**
- fixed-width integers;
- RAII and explicit ownership;
- smart pointers where appropriate;
- minimal global state;
- explicit endian conversion.

**Avoid:**
- undefined behaviour;
- unaligned casts;
- signed overflow;
- unchecked pointer arithmetic;
- unnecessary shared ownership;
- excessive template cleverness.

## Compiler quality

- Keep project code warning-clean under strong warnings (approximately
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion`). New code must
  not add warnings.
- Debug builds should work with AddressSanitizer and
  UndefinedBehaviorSanitizer (for example via `CMAKE_CXX_FLAGS`). The build
  does not yet provide a dedicated option.
- Keep the code friendly to clang-format and clang-tidy, and match the
  surrounding style. The repository has no `.clang-format` yet; if one is
  added, follow it.

## Architecture

**Keep these separate:**
- Blackfin CPU execution;
- the memory bus;
- MMIO;
- the OP-1 platform;
- devices;
- the host front ends (GUI and headless);
- audio and MIDI backends;
- debugging.

**Host APIs:** must not leak into guest CPU or device code.

**Input:** logical host input flows through the OP-1 platform and MMIO
layers, never directly into the CPU.
- MIDI support must stay generic, with logical OP-1 controls kept separate
  from physical controllers.
- Keyboard and mouse fallback must remain, so MIDI hardware is never
  required.

**The Blackfin CPU core:**
- It is Bcore (`ext/bcore` submodule). The decision, its alternatives,
  limitations and revisit conditions are in
  `docs/blackfin-emulation-options.md`.
- Do not copy in, replace or substantially rework a Blackfin CPU
  implementation without updating that record and having it reviewed.

## Timing

- **No host sleeps:** do not drive guest behaviour with host `sleep()`
  calls or host wall-clock intervals.
- **Guest time:** comes from the emulated guest clock
  (`docs/guest-clock-architecture.md`). Remaining host-time dependencies
  are listed in `docs/hacks.md` and should shrink, not grow.
- **Threading:** prefer deterministic, single-threaded guest execution. Do
  not add threading without a demonstrated need.

## Logging

- Use levels now, and add categories as logging grows. Candidates: `cpu`,
  `firmware`, `loader`, `memory`, `mmio`, `display`, `audio`, `midi`,
  `input`, `dma`, `interrupt`, `flash`, `usb`.
- Do not emit instruction traces by default.
- Collapse repeated MMIO polling instead of flooding logs.
- Diagnostic traces must be opt-in and bounded.

## Dependency and licensing discipline

**Before adding a dependency,** evaluate:
- necessity and licence;
- maintenance and package availability;
- transitive size and portability.

Do not import large frameworks without a clear benefit.

**Pin** fetched dependencies to an exact tag or commit.

**Licences:**
- Preserve third-party licences and attribution.
- Verify licence compatibility before incorporating emulator cores or
  reverse-engineering code.
- Record a dependency's licence in `docs/licensing.md`.

**This project's position:**
- This repository's own files are MIT.
- The built programs include Bcore, which this project distributes under
  GPL-3.0-or-later, and cqueue (LGPL-3.0).
- See `docs/licensing.md`. Its open questions stay open until they are
  actually resolved.

**Wording:**
- Do not describe GNU-derived or uncertain code as independent or
  clean-room.
- Do not claim legal clearance that has not been given.

**Artwork:** follows `docs/artwork-attribution.md`. Do not add third-party
artwork, logos or firmware screenshots without recording their source and
licence.

## Git practices

- **Commits:** keep the primary branch buildable. Make small, logical
  commits with messages that state intent (for example,
  `fix: preserve NAND random-read data availability`). Never use messages
  like `updates`, `stuff` or `fixes`.
- **Before committing:** commit known-good state before risky or
  architectural changes. Use branches or worktrees for substantial
  independent work.
- **Never commit:**
  - firmware, dumps or private inputs;
  - generated binaries, caches, extraction output or build directories;
  - local scratch files.
- **Review:** inspect `git status`, `git diff` and `git diff --cached`
  before committing.
- **Published history:**
  - never force-push or rewrite published history;
  - correct mistakes with new commits (for example, `git revert`);
  - push with explicit refspecs, never `--all`, `--mirror` or `--tags`;
  - check the target ref first, and use `--dry-run`.
- **Publication and rewriting:** pushing, publishing, merging into `main`,
  tagging and rewriting history each need the maintainer's explicit
  approval.

## Documentation

- **Where knowledge lives:** keep important reverse-engineering knowledge
  under `docs/`. Keep the repository understandable without chat history or
  local notes.
- **Decision records:** record major decisions in short decision records.
  `docs/blackfin-emulation-options.md` is the model.
- **Feature status:** keep a truthful feature status table in `README.md`.
- **Local procedures:** documents that describe one-off local procedures
  must say so, and must not depend on files outside the repository.
