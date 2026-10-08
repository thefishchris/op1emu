# BF524 executed-work accounting and timing evidence

## Result and scope

**CONFIRMED:** Bcore now counts runtime **instruction/packet entries**. A
standalone 16- or 32-bit instruction counts once; a valid 64-bit multi-issue
packet also counts once, not once per slot. The counter includes faulting
attempts and retries. It is not a retired-instruction count or elapsed cycles.

`BlackFinCpu::PacketEntryCount()` exposes the cumulative unsigned 64-bit count
on the CPU thread. It starts at zero with CpuState initialization, survives JIT
invalidation, and wraps modulo 2^64. Translation and disassembly do not count.
Differences of unsigned snapshots give executed entries between CPU runs.

No cycles/nanoseconds are assigned. SPORT, CoreTimer, guest-visible CYCLES,
DMA pacing, PCM conversion, clocks and boot decisions retain their existing
implementation. This task ran synthetic guest programs, not a new firmware
boot experiment; host/JIT instrumentation overhead is not emulated guest time.

## Why not count in bfin_hwloop_step()?

`BBTranslator::translate()` builds one `insn_entry` block for each outer
`decodeInstruction()` invocation. The decoder handles all slots of a parallel
packet within that invocation. At runtime, the entry block is reached once
before slot semantics. Sequential flow visits subsequent entry blocks; taken
branches, hardware loopbacks and exceptions return before visiting a suffix.

The hwloop helper is called in `LiftVisitor::emit_epilog()`. Normal sequential
execution reaches it. Conditional branches get separate, mutually exclusive
taken/fallthrough epilogs; loopback returns occur after the epilog. However,
some paths return **before** it:

- `emit_jump()` returns directly after raising an odd-target instruction
  alignment exception.
- Privileged push/pop paths can return after a supervisor/illegal-instruction
  exception, before finalization.
- Simulator pseudo-debug assertion failures can return directly.

Therefore this helper is not an exact count of all attempted packet execution.
Adding missing epilogs would risk changing architectural loop behavior. Instead,
the former `emit_insn_len()` entry hook is renamed `emit_packet_entry()` and
emits a 64-bit increment into the existing runtime entry block, alongside the
unchanged instruction-length store. This adds no helper call, guest-visible
register write, interrupt decision, or new execution exit.

The count is deliberately an **entry/attempt** count: error exceptions abort
the faulting instruction architecturally, but its attempted execution still
consumes work. Service exceptions count their EXCPT instruction; asynchronous
CEC dispatch adds zero packets, and subsequent handler instructions count
normally. Event-entry overhead must be accounted separately in a future cycle
model. Decoder-accepted simulator pseudo-instructions also count as Bcore
entries; they are not evidence of hardware instructions or their cycle costs.

### Changed code

- `ext/bcore/include/cpu_state.h`: nonarchitectural `uint64_t packet_entries`,
  appended so existing architectural field offsets are unchanged.
- `ext/bcore/src/bb_translator.cpp` and `src/lift_visitor.{h,cpp}` within Bcore:
  runtime entry increment, before execution of any slot.
- `src/cpu/cpu.{h,cpp}`: CPU-thread-only read accessor.
- `tests/execution_accounting_tests.cpp` and root `CMakeLists.txt`: regression
  target using real Bcore JIT execution, without a Blackfin cross-toolchain.

### Runtime evidence

`execution_accounting_tests` verifies count **and** architectural outcomes:

| Case | Expected executed entries |
| --- | --- |
| NOP, NOP, 32-bit immediate, indirect jump in one block | 4 |
| Five subsequent executions of a cached one-jump block | +5 |
| Invalidation followed by retranslation/execution | 0 for invalidation, +1 for execution |
| Code setup and disassembly | 0 |
| Conditional branch taken vs not taken with a translated suffix | 1 vs 4 |
| Two-instruction hardware loop, three iterations, then terminal jump | 2, 2, 3 (7 total) |
| Three-slot packet at a hardware loop bottom | 1 per packet |
| MNOP plus two parallel loads, then jump | 2; both loads and pointer increments checked |
| Taken conditional branch at loop bottom | 1; loop counter decremented once |
| Privileged fault with translated but unexecuted suffix | 1 attempted instruction |
| Odd indirect jump, RTX, corrected retry | 1 entry each; original fault/return PCs checked |
| EXCPT service exception and RTX | 1 each; service return PC checked |
| External IVG11 dispatch | 0; handler RTI adds 1 |
| BlackFinCpu synthetic NOP + jump and cached jump | 2 then +1 through the public accessor |
| Counter overflow | Defined unsigned wrap, unchanged branch outcome |

The tests explicitly check that Bcore execution does not change CYCLES storage
as a consequence of work counting. The CPU integration fixture uses idle
storage-free NAND to satisfy the existing NFC polling interface; it does not
boot firmware or access NAND contents. All eight selected regression suites
passed, including deterministic SPORT supplied-clock tests. Test execution is
bounded by a 30-second CTest timeout.

## Primary timing authorities

These are Analog Devices-authored manuals, read in full-text PDF extraction,
not simulator timing implementations. Vendor-hosted downloads timed out;
the actual editions inspected came from archive mirrors. Page references below
are printed manual page numbers, not PDF viewer page numbers.

1. **Blackfin Processor Programming Reference**, Revision 2.0, March 2012,
   part 82-000556-01; scope explicitly includes ADSP-BF5xx.
   [Inspected PDF](https://archive.org/download/manuallib-id-2701005/2701005.pdf).
   [Vendor's newer Rev. 2.2 URL](https://www.analog.com/media/en/dsp-documentation/processor-manuals/Blackfin_pgr_rev2.2.pdf)
   is a discovery link, not the edition read.
   Inspected PDF SHA-256:
   `b2455a927cabb6c0bec76ca07157f7c92d8152a7748c58a4e28f2f8d934e8ba3`.
2. **ADSP-BF52x Blackfin Processor Hardware Reference**, Revision 1.0,
   March 2010, part 82-000525; explicitly covers BF524.
   [Vendor URL](https://www.analog.com/media/en/dsp-documentation/processor-manuals/BF52xHRM_Rev.1.0.pdf),
   [inspected mirror](https://archive.org/download/manuallib-id-2700101/2700101.pdf).
   Inspected PDF SHA-256:
   `f83eae4837f544eca4396869f4a5957d9a77fb1c93947dbb6534008446cef37d`.

Downloads/extractions are local `/tmp/opencode/blackfin-pgr-2.0.{pdf,txt}` and
`bf52x-hrm-1.0.{pdf,txt}`. They are not added to the repository.

EE-197 was discovered as a potentially useful vendor note, but its published
scope is BF531/532/533. Its contents and applicability to BF524 were not
verified here. Do not silently import its numeric costs as BF524 facts. GNU sim
implementation/timing was neither copied nor used as hardware authority.

## Confirmed hardware semantics

### CYCLES / CYCLES2 and CCLK

Programming Reference pp. **21-29–21-31**:

- The enabled 64-bit counter advances on CCLK cycles, including execution,
  wait states, interrupts and events in User/Supervisor mode. It stops in
  Emulator mode. SYSCFG.CCEN, bit 1, enables/disables it.
- CYCLES is the low 32 bits and CYCLES2 the high 32 bits. Reading CYCLES
  captures the high part for coherent subsequent CYCLES2 reads; another
  CYCLES read refreshes that snapshot.
- These are system registers, not MMRs, and are writable in all modes on BF524.
  Software can reset/change them: the architectural counter must not become
  the scheduler's sole monotonic time source.

BF52x Hardware Reference pp. **18-3–18-6, 18-26–18-27**:

- In normal PLL operation, VCO derives from CLKIN, DF and MSEL, and CCLK
  is VCO divided by the CSEL-selected divisor 1, 2, 4 or 8. MSEL=0 encodes
  multiplier 64; DF selects direct or half-rate PLL input. SCLK uses SSEL.
- Bypass/power/clock-stop modes change this relationship. PLL_CTL changes
  require the documented programming sequence; PLL_DIV divider changes are
  dynamically effective.
- **UNKNOWN for this setup:** actual OP-1 CLKIN and active clock transitions.
  The current application's assumed 400MHz is not sufficient evidence of the
  physical or guest-configured CCLK rate.

### Instruction / packet throughput

Programming Reference pp. **4-7–4-9, 20-1–20-2**:

- The pipeline/alignment logic can supply one 16-, 32- or 64-bit instruction
  per cycle under suitable no-stall conditions. Pipeline depth is not a
  per-instruction cost to sum across straight-line execution.
- A multi-issue packet contains one 32-bit instruction and two 16-bit slots;
  its execution time is that of the slowest slot, not the sum of slot costs.
- Push/Pop Multiple occupy decode for the number of registers transferred.
  Their count of registers provides a documented multi-cycle base cost.
- Sequencer, DAG/register hazards, compute hazards and memory all introduce
  stalls; one packet is therefore **not universally one CCLK cycle**.

### Nontrivial classes and costs

| Class / condition | Vendor evidence | First-order consequence |
| --- | --- | --- |
| Conditional branch, predicted not-taken / actual not-taken | PR p. 4-21: latency 0 CCLK | No branch redirection penalty in this case |
| Conditional branch, predicted not-taken / actual taken | PR p. 4-21: latency 8 CCLK | Outcome and prediction bit must be retained |
| Conditional branch, predicted taken / actual taken | PR p. 4-21: latency 4 CCLK | Different cost from unpredicted taken branch |
| Conditional branch, predicted taken / actual not-taken | PR p. 4-21: latency 8 CCLK | Misprediction matters even without a taken branch |
| Unconditional branches | PR p. 4-21: latency 4 CCLK | JUMP/CALL change-of-flow cannot be charged like ordinary arithmetic |
| Push/Pop Multiple | PR p. 4-9: decode duration equals transferred register count | Operand-dependent multi-cycle cost |
| LINK/UNLINK | PR pp. 4-16–4-17: explicitly multi-cycle | Exact total cost not established from these passages; do not infer it by summing equivalent source lines |
| First hardware-loop backedge, nonzero LSETUP start offset | PR p. 4-26: four-cycle latency | Most loopbacks are not explicit branch instructions; track this exceptional first backedge |
| Loop-register restoration | PR pp. 4-30–4-31: LC restore causes ten-cycle replay; LT/LB restore while LC nonzero does too | ISR loop-context restoration cannot be ignored |
| CSYNC/SSYNC | PR pp. 6-73–6-74: drain/synchronize pending core/system operations | Outstanding-operation dependent; no universal constant established here |
| Memory, cache fill, MMIO, data hazards, DMA contention | PR pp. 4-9, 6-38, 6-71–6-74, 21-25 | State-dependent waits; do not charge host memory/JIT latency as guest waits |
| Interrupt/exception entry and returns | PR pp. 4-53–4-54, 4-64–4-66 | Event overhead must be separate from handler packet count; no universal fixed cost established here |
| IDLE and clock transitions | PR pp. 3-9–3-10, 16-3; HR chapter 18 | No executing packets can still mean elapsed peripheral time; requires an explicit idle/wakeup policy |

The branch figures above are documented **latencies**, not a claim that a
not-taken branch takes zero total cycles. A cycle model must state its issue
baseline and penalty conventions and validate that it does not double-count
pipeline occupancy. Bcore currently discards branch prediction timing hints by
routing BP decoders to the same semantics; a future timing model must preserve
that distinction without changing the branch result.

On interrupt entry, the PR gives a minimum of three CCLK cycles from a
general-purpose interrupt edge to IPEND assertion, with larger delays depending
on core state. This is **not** a universal edge-to-first-handler-instruction
cost. Service exceptions abort following instructions; error exceptions abort
the faulting instruction too. Memory-fill waits can overlap higher-priority ISR
execution. Counting packets cannot reconstruct these waits.

## Minimum useful model and smallest next step

**Yes, a scoped first-order issue/latency model can be evidence-backed**, using
documented no-stall issue throughput, packet-level max slot cost, register-count
multi-cycle operations and observed branch outcomes/predictions. It need not
reproduce every pipeline/cache/DMA stall to test whether host/JIT time is the
source of runaway audio production. It must label omitted memory/dependency
stalls as an **ASSUMED ideal-memory/no-stall mode**, not as BF524 hardware facts.

**No, the packet-entry total alone is not sufficient for the SPORT experiment.**
It lacks operation classes, branch outcomes/prediction, loop replay events,
event overhead and frequency. The material read does not establish every
fixed opcode cost, and full firmware coverage of a proposed model is unmeasured.
An idealized model can support a provisional causal experiment after these
gaps are made explicit; it cannot establish hardware-accurate audio cadence.

The smallest next implementation is a **shadow issue-cost accumulator** plus
unknown-cost coverage, still disconnected from device clocks. Start with the
documented classes and deterministic sequence tests; preserve branch hints and
runtime outcomes, and distinguish packet attempts from retirement and event
entry. Expose unsupported classes/memory assumptions rather than silently
assigning every packet one cycle or tuning constants to make boot advance.
Observe which missing classes the actual audio/boot path exercises before
deciding whether more precise stalls are needed. In parallel, establish CLKIN
and the active PLL settings from board evidence/guest observations before
converting accounted cycles to nanoseconds.

A later SPORT-only experiment would use a separate monotonic elapsed-cycle
accumulator, independent of CCEN and guest CYCLES writes. CYCLES/CoreTimer
conversion remains separate work. Correct nominal audio per guest second
still does not guarantee sufficient host real-time throughput.
