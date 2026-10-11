# Blackfin CPU emulation options

**Status:** engineering decision **approved by the project maintainer** on
2026-10-11 for the first functional release (see [Review record](#review-record)).
The legal and provenance questions under
[Outstanding uncertainties](#outstanding-uncertainties) remain **unresolved**.
This approval is an engineering decision. It is not legal advice, a
determination by legal counsel, or permission from any third-party rights
holder.

`AGENTS.md` requires this record to be updated and reviewed before a
Blackfin CPU implementation is copied in, replaced or substantially
reworked. op1emu inherited the Bcore
CPU core from the upstream project before this record existed. This
document records that choice after the fact, together with the evidence
about Bcore's provenance and the conditions attached to keeping it.

Findings are classified as in `AGENTS.md`: CONFIRMED, LIKELY, ASSUMED,
UNKNOWN.

## Requirements

- **ISA:** execute the BF524's Blackfin instruction set. That covers:
  - 16- and 32-bit instructions and 64-bit parallel-issue packets;
  - DSP, multiply-accumulate and video operations;
  - hardware loops;
  - Core Event Controller (CEC) and Event Vector Table (EVT) behaviour:
    interrupts, exceptions, supervisor and user modes.
- **Firmware:** run OP-1 firmware v241 to the tape UI fast enough to be
  usable.
- **Guest clock:** expose instruction-packet accounting, block boundaries
  and IDLE, which the guest-clock and multi-block designs need
  ([guest clock architecture](guest-clock-architecture.md),
  [multi-block execution](multiblock-execution.md)).
- **Licensing:** a licence that allows publishing this project, and clear
  provenance (`AGENTS.md`, "Dependency discipline").

## Bcore as used here

**Implementation (CONFIRMED, from Bcore's source and README):**
- Bcore decodes Blackfin instructions with a template pattern matcher
  (`decoder.h` and the `instruction16/32.def.h` tables).
- It lifts each basic block to LLVM IR (`lift_visitor.cpp`, about 7,900
  lines) and runs it with LLVM 15's LLJIT. Translated blocks are cached and
  invalidated on writes.
- It models the CEC (`cec.cpp`, about 500 lines) and the EVT (`evt.cpp`).
- It includes a disassembler (`disasm_visitor.cpp`) whose output matches
  `bfin-elf-objdump`.
- op1emu supplies the memory bus, MMIO, system interrupt controller and
  devices. Bcore provides the core only.

**Changes this project made in its fork (CONFIRMED, from Git history):**
- runtime packet-entry accounting
  ([execution accounting](bf524-execution-accounting.md));
- the opt-in observational shadow timing model, whose rules come from the
  ADI Programming Reference, not GNU code
  ([shadow timing coverage](shadow-timing-coverage.md));
- architectural IDLE handling;
- two shift-lowering corrections ported from upstream Bcore
  ([upstream ports](upstream-correctness-ports.md));
- licensing files only (COPYING, NOTICE, provenance record).

**Measured results:**
- **Bcore's own suite** (fork commit `51284db`, built with LLVM 15.0.7 and
  the ADI 2014R1-RC2 `bfin-elf` toolchain, against the GNU simulator's
  assembler tests at the pinned fixture commit):
  - disassembler comparison: 828/828;
  - emulator, with Bcore's CI ignore list: 792/792. Without the list, the 36
    listed tests are exactly the ones that fail or time out.
- **In op1emu (CONFIRMED):** v241 boots to the tape UI. Transport and a
  held note work in the `functional` profile.
- **Speed (CONFIRMED on the development machine):** about 0.1–0.16 guest
  seconds per host second. In the measured v241 SPORT-active workload, most
  host time goes to op1emu's per-block device servicing (about 340 ns per
  block), not to Bcore's translated code (13–55 ns per block)
  ([multi-block execution](multiblock-execution.md)).

**Known implementation limitations (CONFIRMED, from Bcore's CI ignore
list):** these are not implemented or not passing:
- single-step (SSSTEP) exceptions;
- non-operating-environment mode;
- DAG protection violations;
- CPLB and memory protection, and LMU exceptions (alignment, illegal
  address, protection);
- the debug trace buffer;
- the core timer and some interrupt-control MMR tests;
- MMR access exceptions;
- full opcode-coverage tests (some instructions are unimplemented);
- one illegal parallel-combination test;
- one undefined-instruction test.

The core is not cycle accurate; guest time comes from op1emu's clock model.
Whether the OP-1 firmware relies on any of the missing features beyond what
v241 boot, transport and a held note exercise is UNKNOWN.

## Provenance (summary)

Bcore's own README, `CLAUDE.md`, `EXPERIENCES.md` and source comments record
the following:
- it was generated with an AI coding assistant;
- the assistant was directed to consult the GNU Blackfin simulator
  (`bfin-sim.c`, `dv-bfin_cec.c`, `dv-bfin_evt.c`) and the binutils
  disassembler (`bfin-dis.c`);
- all four are GPL-3.0-or-later, copyright Free Software Foundation, with
  the simulator files contributed by Analog Devices.

A comparison against those sources found:
- **CONFIRMED:** the CEC's exception-raise and return paths follow
  `dv-bfin_cec.c` statement by statement, with the same locals, order and
  branches;
- **CONFIRMED:** the instruction lifter follows `bfin-sim.c` semantically.
  It cites function names and line numbers and shares 15 helper names, but
  it emits LLVM IR and shares no statements with the GNU C;
- **CONFIRMED:** the disassembler shares 80 of 196 distinctive output strings
  with `bfin-dis.c`;
- **CONFIRMED:** only 3 trivial lines are literally identical across the
  whole tree.

The fork therefore distributes Bcore under GPL-3.0-or-later. Upstream's MIT
notice is preserved, and every file is classified in Bcore's
`docs/provenance.md`. What this means for op1emu is in
[licensing](licensing.md).

## Options

Only the options below were considered. Their depth of evaluation differs,
as stated.

| Option | What was evaluated | Strengths | Tradeoffs and risks |
| --- | --- | --- | --- |
| **A. Bcore** (fork of op1emu/bcore) | In use. Measured against its own test suite and in v241 boots (above). Provenance compared file by file with the GNU sources. | Works now. The LLVM JIT keeps the core's own cost low. The fork already carries the accounting and IDLE hooks the guest-clock work needs. | Documented GNU derivation: the fork and the built programs are distributed under GPL-3.0-or-later. Requires LLVM 15 exactly. Gaps listed above. The upstream code was AI-generated, with its history squashed to one commit. |
| **B. GNU Blackfin simulator** (`sim/bfin` in binutils-gdb), ported into op1emu | Its source was read for the provenance comparison. It was **not** built, run or benchmarked as an op1emu core. | Clear provenance: FSF, contributed by ADI, GPL-3.0-or-later. Mature, with its own device models. | Integration effort is UNKNOWN: it is written for the GNU simulator framework. ASSUMED slower than a JIT, as an interpreter (not measured). Same GPL outcome for the built programs as A, without A's JIT or the fork's hooks. |
| **C. op1emu/bfin_sim** | Previously a submodule, used only for its compiled-in boot ROM array; removed when the boot ROM became a runtime input ([boot ROM](boot-rom.md)). Not evaluated as a CPU core. | — | A 19-file subset of B. No `COPYING` file in that repository. If B is wanted, B's original source is the better base. |
| **D. Clean-room rewrite of the GNU-derived Bcore parts** | Scoped only: CEC/EVT about 540 lines, lifter about 7,900 lines, disassembler about 2,800 lines. No rewrite was attempted. | Could allow MIT for the rewritten parts. Keeps Bcore's JIT and infrastructure. | It needs a documented process: a specification from the ADI manuals, implementers who have not seen the GNU sources, and a provenance log. The lifter is large. Everything must be revalidated (Bcore suite, op1emu tests, deterministic v241 replay). |
| **E. Other Blackfin emulators** | **Not evaluated.** No survey has been done. | — | No candidate is claimed or compared here. |

## Decision

**For the first functional release, op1emu uses the existing Bcore
implementation (option A).** The project maintainer approved this
engineering decision on 2026-10-11, together with the conservative licensing
approach below.
- **Licensing:** the fork distributes Bcore under GPL-3.0-or-later and
  preserves the upstream MIT notice and the FSF/ADI attribution. op1emu's
  built programs are therefore distributed under GPL-3.0-or-later.
- **Why:** it is the only option evaluated that already runs v241 with the
  hooks the guest-clock work needs. B would bring the same licence outcome,
  with UNKNOWN integration cost and no JIT. D is not realistic before a
  first release.
- **Licensing posture:** the conservative approach. Bcore and the linked
  emulator binaries are distributed under GPL-3.0-or-later, and the existing
  MIT notices are kept. The GPL distribution follows the more restrictive
  reading of Bcore's provenance. It does not depend on settling whether
  individual files are derivative works, and it does not settle that
  question.

## Outstanding uncertainties

Every item below is **UNRESOLVED** as of 2026-10-11. The maintainer's
approval of the engineering decision does not resolve any of them.

**Licensing:**
- whether the GNU-derived and uncertain Bcore files are derivative works in
  law;
- how the AI-assisted generation affects authorship;
- whether the NOTICE wording is sound;
- the LGPL-3.0 cqueue requirements in binaries (see
  [licensing](licensing.md));
- the upstream Bcore author has not been asked how the code was produced.

**Hardware model:**
- the CEC model follows the GNU simulator's model rather than independent
  BF524 hardware evidence. Where the two differ from silicon, op1emu
  inherits the simulator's behaviour. UNKNOWN whether this affects the OP-1
  firmware.
- the unimplemented features listed above (CPLB/memory protection,
  single-step, DAG protection, some opcodes) are UNKNOWN in their effect
  beyond the v241 paths exercised so far;
- there is no cycle timing in the core. Guest-time decisions belong to
  op1emu's clock model and its documented assumptions ([hacks](hacks.md)).

## Conditions for revisiting this decision

Revisit when any of these occurs:
1. MIT (or other non-GPL) distribution of the built programs becomes a goal:
   start option D with CEC/EVT, the strongest derivation evidence and the
   smallest part.
2. New provenance evidence, for example from the upstream author or a legal
   review, reclassifies Bcore files in either direction.
3. Firmware behaviour is blocked by a missing core feature that cannot be
   added to Bcore reasonably.
4. CPU core execution, rather than device servicing, becomes the dominant
   cost, or LLVM 15 becomes unavailable on supported hosts.
5. A survey of other Blackfin emulators (option E) is carried out.

Any change of core requires the same evidence as this record: the Bcore
suite or an equivalent, the op1emu tests, and a deterministic v241 replay
compared against the previous core.

## Review record

| Reviewer | Date | Outcome |
| --- | --- | --- |
| Project maintainer | 2026-10-11 | **Approved** (engineering decision): use the existing Bcore implementation for the first functional release, under the conservative GPL-3.0-or-later distribution approach with the MIT notices preserved. Legal and provenance questions stay open. |

**Not reviewed by:**
- legal counsel;
- the upstream Bcore author;
- the Free Software Foundation, Analog Devices or any other rights holder.

None of them has reviewed or approved this record. Record any future review
by them here, with its date and scope.
