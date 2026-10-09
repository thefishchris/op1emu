# Narrow upstream correctness ports

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
