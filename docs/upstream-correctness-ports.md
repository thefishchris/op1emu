# Narrow upstream correctness ports

> **Historical record.** Paths under `/tmp/opencode/` name local scratch files from the
> original investigation (scripts, PDFs, logs, image copies). They are not part of the
> repository and are not needed to build or run the emulator. Current procedures:
> `README.md`, `docs/emulator-profiles.md` and `docs/headless-regression.md`.

These ports are hand-applied to parent `24fdc71` / Bcore `37de8e8`; they are
not a merge of upstream and do not adopt a guest clock or timing architecture.
The power-manager IDLE at `019F18F4` remains Decision B.

## NAND snapshot storage — e4d2c560250cb75fa3549faf626271fcc616e257

Adopted: `MT29F4G08`'s optional read-only backing file, per-page in-memory dirty
overlay, overlay-first reads, programming data/OOB with the existing NAND AND
rule, and full 64-page data/OOB erase in the overlay. GUI selectors are
`--nand-snapshot` and `--nand-rw`; the existing GUI default stays persistent.
Evidence runs must select snapshot mode or use a private writable clone.

Differences: a missing/unreadable snapshot source throws instead of logging and
continuing with fabricated erased storage. It is never created/truncated. Input
files are opened before starting the GUI's USB thread, so startup failures can
return normally. The existing early file-open guard in persistent programming
is retained. No upstream claim about every historical YAFFS power-failure cause
is promoted to an emulator fact; the isolation property is independently tested.

Omitted: synchronous NFC transfers at CONFIG, NFC IVG resolver/SIC refactor,
WR_DONE interrupt suppression while its ISR is active, and both removals of
`UpdateInterrupts()`. These change completion/interrupt behavior and were not
independently established for our state machine. In particular, removing the
call in `SetNotBusy()` breaks the recomputation relied on by our deterministic
reset/page-read IDLE completion path. No `nand.cpp`, SIC, DMA, or CPU hardware
behavior is changed by the storage port.

Regression: `NandSnapshotStorage` tests read-after-program, data/OOB AND behavior,
repeat programming, overlay erase boundaries, snapshot reopening/discard,
missing-source rejection/no creation, and default persistent-write readback.
All nine focused tests pass; an exact staged-source library and baseline
execution-accounting/PLL tests also pass independently.

**CONFIRMED v241:** fresh `upstream-snapshot-v241` trial selects `--nand-snapshot`,
returns Random Data Read ECC `E7 03 18 04`, retains FAT113=`0072`, advances
113->114, returns from the walker, and displays normal tape UI. It stops at the
known power-manager IDLE `019F18F4`, which is not a regression. Snapshot source
hash before and after is
`c94057cb8000aa9516242c426eeb5c69df03300fd9c4e0f19893c6ef374d5a42`;
OTP hash remains `06c993496c3aba43ca345c62a2caa27b69d68e160cad8c29196152991a238c5a`.
Original/frozen inputs are also unchanged. Host process runtime 282.705 seconds
is observation metadata only. Logs/screenshots/manifests remain private under
`/tmp/opencode/upstream-snapshot-v241.*`; no image contents are committed.

No TimeSource, packet-based guest timing, timer fixes, CYCLES conversion,
profiling, optimizer pipeline, or wholesale upstream submodule update is adopted.

## Bcore poison shifts — 9e02552c58cb88a6cededa1fa71cb8de001665bc

Only the ASHIFT32-by-32 zero result and DEPOSIT X modulo-32 shift-count mask
were adopted in `src/lift_visitor.cpp`, on top of `37de8e8`. Local Bcore commit
`ae6b207` contains those two expressions plus independent regressions/docs.
The parent gitlink records this local audited commit, not upstream's pointer.

Deliberately omitted: Core/JIT init/codegen API changes, optimizing every module,
TargetMachine/TTI/vectorizer configuration, DataLayout/triple wiring, memcpy/
memset/memmove symbols, JIT invalidation changes, tools' new optimization options,
and upstream test-runner modifications. Packet entries, architectural IDLE and
shadow timing remain unchanged.

Validation: DEPOSIT lengths 0..31, boundary ASHIFT operands, result flags, cached
execution, and idle accounting pass at existing Core levels 0/1/2. Four generated
self-checking ELF cases pass through the existing `emu` executable. The external
assembled matrix could not be built because the Blackfin cross toolchain is
absent; no result is claimed for it. Parent execution_accounting_tests and all
nine focused tests, relinked against this Bcore, pass.

## Headless / fail-fast — 2d82cd4 and d6d1d36

Exact audited commits:
`2d82cd43f40974b81fc955c40021fbc5a608aeab` and
`d6d1d36e6b9c68a0692e5d6c8d46c264b3c71204`.

Adopted frontend contracts: shared JSON key wiring; bounded nonblocking line
input with partial-line/EOF support; press/release/tap/wait/quit; pinned sensors;
full-frame coverage tracking; preserving partial frames on same-geometry
reinitialization; clockwise RGB565 PPM capture; and frame-count waits with host
observation timeouts. Core::run false now latches `BlackFinCpu::Failed()` and
skips device processing. Failure is not retried; GUI execution threads stop and
return nonzero. Real architectural IDLE, including an unsupported wake condition,
does not set Failed. A protected execution-result seam permits testing the real
Run path without treating guest exceptions as artificial JIT failures.

Adaptations: `op1emu-headless` is independently buildable without linking the
GUI/audio/cqueue backend; GUI `--headless` forwards to the same runner. Headless
execution is initially single-threaded, uses the existing platform/display/key
interfaces, and opens no window, audio device, or USB/IP server. The guest USB
controller is still present, with no external host connection fabricated.
Unlike upstream, headless NAND defaults to snapshot; `--nand-rw` is explicit.
Screenshots require new output paths instead of overwriting existing files.
Framebuffer allocation has an explicit host resource bound. Optional runtime
bounds, stop-at-unsupported-IDLE, and screenshot-on-stop make existing boundary
observations scriptable; an unsupported IDLE is reported separately from JIT
failure. GPIO frame callbacks retain the existing host frontend cadence; there
is no new sleep or guest-time conversion. Monotonic frontend deadlines measure
host input/observation bounds only, not device/core time.

Omitted: upstream's detached forever-running USB thread, broad main rewrite,
host-thread/sleep scaffolding, and all timing/profiling changes. GUI shutdown
instead uses a host libuv async stop and joins its existing loop thread, avoiding
both std::terminate and a detached thread outliving guest devices. Sensor script
behavior is opt-in; the existing GUI random sensor feed remains unchanged.
No TimeSource, GP/CoreTimer clock change, guest CYCLES rewrite, scheduler redesign,
one-cycle-per-packet clock, upstream pointer, or O1 IR pipeline is introduced.

Tests: headless unit cases cover parsing, malformed/partial input, EOF, key
mapping and queued platform input, sensor values, complete-frame coverage,
same-geometry reinitialization, RGB565 rotation, exclusive screenshots, and
frame-wait success/timeout. CLI cases cover help, clean quit, malformed commands,
missing snapshot sources, and frame timeout. Failure regression checks false
backend result, unchanged PC/packet count, skipped device callbacks, no retry,
and real IDLE remaining nonfailed. All nine established focused tests and both
new headless targets pass. Exact staged headless executable/library and four
staged test targets pass in an isolated worktree. These tests do not require
proprietary firmware; the headless executable avoids the pre-existing unstaged
cqueue include fix needed by the GUI target on this compiler.

**CONFIRMED headless v241:** pinned-sensor script waited for the first real
frame and captured the startup display. The stop-on-IDLE runner captured normal
tape UI as a 400x200 PPM, returned **0**, and stopped at known `019F18F4`
(resume `019F18F6`), packets 1,353,315,222, frames 184. Host CPU runtime
311.312 seconds is metadata only. No JIT failure was reported and no continuation
through the known power-manager boundary occurred. Frozen NAND and copied OTP
hashes remained the same as above. Private evidence is under
`/tmp/opencode/headless-port-boot.*`, `headless-startup.ppm`, and
`headless-tape.ppm/.png`.

See `docs/headless-regression.md` for invocation and exit-code contracts.
