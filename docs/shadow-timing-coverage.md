# Shadow BF524 timing coverage and OP-1 clock observation

Follow-up: `docs/op1-clock-readiness.md` records physical board-photo evidence,
PLL activation semantics, refined ADD2/RTS components and actual IDLE sites.
The statistics below describe the earlier model/trial; CLKIN remains unverified
and no device clock conversion has been made.

## Checkpoint and scope

Packet-entry checkpoint: parent commit
`55335b4f225757f229aced2d688658bc72819593`, referencing Bcore commit
`e1f434f46fe391f6400d0e00ec0395a631acb89d`.
Only the accounting/accessor, its tests, CMake test registration and
`docs/bf524-execution-accounting.md` were staged. Earlier dirty boot/audio/GPIO,
storage, diagnostics, build-linking and other work was preserved.

The follow-up shadow model is opt-in through `OP1EMU_SHADOW_TIMING=1` and is
**observational only**. It has no connection to SPORT, CoreTimer, CYCLES,
DMA availability, queued-event delays or boot decisions. CPU-thread access is
through `BlackFinCpu::ShadowTimingStats()`. The observer must be installed before
Bcore initialization and remain alive while its JIT code exists. Disabled
translation emits no shadow packet callbacks or unknown-opcode increments.

Model/test/clock-observation changes for this follow-up remain uncommitted.

## Implemented first-order convention

Sources and exact manual editions are in `docs/bf524-execution-accounting.md`.
These rules use the Analog Devices Programming Reference, not GNU sim code.

1. Count packet **attempts** independently through the existing counter.
2. Count one issue opportunity per executed instruction/packet, including a
   single opportunity for a whole parallel packet. Ordinary no-stall issue
   throughput is the baseline (PR 4-7–4-9, 20-1–20-2).
3. Push/Pop Multiple replace that one with **N**, the number of registers
   transferred (PR 4-9). This is N total issue/decode cycles, not 1+N.
4. Add conditional branch pipeline loss according to runtime CC, branch polarity
   and the encoded prediction bit: 0 for predicted/actual not-taken; 4 for
   predicted/actual taken; 8 for either misprediction. Add 4 for unconditional
   JUMP/CALL redirection (PR 4-21). The convention is **issue + documented
   latency**, not issue + latency + another total-cost table. Faulting branches
   are marked unknown and their ordinary branch loss is withdrawn.
5. For parallel issue, use the maximum slot issue component and union of
   uncovered reasons. Do not sum the slots' baseline issues.
6. Observe actual loop-register writes: add a ten-cycle replay for LC writes and
   LT/LB writes when the corresponding LC is nonzero (PR 4-30–4-31). Do not
   charge this to LSETUP's internal register initialization or hardware counter
   decrements.
7. Track each LSETUP's first actual loopback. Add four cycles once when its loop
   top is not the instruction immediately following LSETUP; adjacent loops have
   no such penalty (PR 4-26, 7-16). This tracks the selected backedge rather than
   counting a translated block as a loop iteration.

`EstimatedCycles()` is the sum of recorded issue opportunities and documented
penalties. **It is a partial, idealized estimate, not measured silicon cycles or
a verified bound.** For unsupported fixed-cost classes only their one issue
opportunity is counted; the unknown remainder is flagged, never supplied with a
tuned constant. Memory waits, MMIO latency, cache misses, dependency stalls,
DMA contention, synchronization completion, idle elapsed time and event entry
overhead have **no fabricated cycle additions**.

The implemented opcode whitelist is conservative. Integer `ALU2op_MUL`, returns,
LINK/UNLINK, synchronization, cache operations, IDLE, event instructions and
unlisted opcodes retain explicit unsupported coverage. In particular, the
lowercase `dagMOD*` opcodes encountered in audio packets are not whitelisted.
The branch hint affects only the observer, not branch semantics.

### Coverage meanings

- `known-issue`: packets whose fixed **issue component** is modeled. It does not
  establish known total elapsed cost, because waits/hazards can be absent from
  the model.
- `unknown-fixed`: packets with at least one unsupported fixed-cost component.
  Together with known-issue, this partitions the observed packets.
- `unknown-union`: packets with any uncovered fixed-cost or stall-opportunity
  reason. Reasons overlap and must not be summed to obtain a packet fraction.
- Memory/dependency categories count **opportunities**, not observed stalls or
  stall durations. No real cache, pipeline readiness or memory arbitration
  model exists here to determine which opportunities become stalls.
- Instruction supply is unmodelled globally, even for packets without a data
  access. Its per-packet opportunity count is derived directly from packet
  attempts (2,131,744,489 at the final snapshot), not counted cache misses or
  bus fetch transactions. The final logger exposes this derived quantity too;
  the recorded trial predates that additional summary line. `unknown-union`
  is the union of the selected per-packet reasons above, not a guarantee that
  the remaining 6.68% has fully known total timing. Including the global
  instruction-supply assumption, every packet has potentially unmodelled time.
- `events`: actual CEC dispatches, counted separately from packet attempts;
  event-entry overhead remains unknown and adds no fixed cycles.
- Unknown-opcode histograms count executed unsupported **slot occurrences**.
  Parallel packets can contribute more than one occurrence. Histograms are
  registered at translation time, but incremented only by runtime JIT entries.

## Deterministic verification

The firmware-free `execution_accounting_tests` now also verifies:

- Enabled/disabled shadow observation gives the same PC/register/CYCLES results.
- All combinations of conditional branch polarity, CC and prediction, including
  unexecuted translated suffixes; an unconditional jump has 1+4 modeled cycles.
- Push/pop costs for 1, 4, 8 and combined 14 registers; memory waits remain unknown.
- Parallel loads count one issue, with one per-packet memory opportunity.
- SSYNC contributes only its baseline and an explicit unknown remainder.
- Adjacent/nonadjacent LSETUP loops, first-penalty once, and no spurious replay
  charge for LSETUP initialization.
- LC/active LT restore replay penalties and existing architectural loop results.
- Fault, event-dispatch and return coverage without invented event-entry cycles.
- Unknown-opcode histogram counts are runtime counts, not translation counts.

All eight selected regression suites passed after rebuilding. The final
accounting suite passed after the low-overhead observer refinement; existing
SPORT supplied-clock regressions remain part of the selected checks. Builds
still report existing LLVM/disassembler/dependency warnings; no new shadow/test
warnings were introduced in the final build.

## Bounded real-firmware observations

Both observations used private fresh copies of the same initialized v241 NAND
and provisioned OTP, GPIO model 17, unchanged SPORT divisor 10, CPU/audio
diagnostics, and clock-write observation. No input actions or audio captures
were enabled. Original storage images were never passed to the emulator.

- `build/shadow-coverage-v241.*`: 360s initial observation. Its 300s screenshot
  still showed splash; foreground execution continued afterward. It also tried
  observing read results at unmapped clock registers. Those reads are not valid
  hardware evidence. This attempt is retained as local investigation evidence.
- `build/shadow-coverage-final-v241.*`: 420s final observation. The observer walks
  only set unknown-reason bits, adds unsupported-opcode histograms, and records
  clock **writes only**, restoring the adapter's original read paths. No timing
  cost rule was changed to affect startup duration.

The final run showed splash at 300s and **normal tape UI at 360s and 420s**.
Thus observed wall-clock startup lies in (300,360] seconds for this instrumented
run. SPORT first activated at approximately 139.94s. Host delivery remained
approximately 4,800 frames/s with the retained ~90% steady underrun silence.
The observer adds host overhead: this is not a timing-correctness or nominal
48kHz guest-time experiment. Guest elapsed seconds cannot yet be stated.

### Last complete final-run snapshot: 410 seconds

The process was stopped at 420s; the last complete periodic record is 410s.
These are cumulative totals **through that snapshot**, not an extrapolation to
process termination:

| Measurement | Value |
| --- | ---: |
| Executed packet attempts | 2,131,744,489 |
| Issue/decode components | 2,219,353,780 |
| Documented penalty components | 524,173,464 |
| Partial estimated cycles | 2,743,527,244 |
| Known issue-component packets | 2,033,216,130 (95.3780%) |
| Unsupported fixed-cost packets | 98,528,359 (4.6220%) |
| Packets carrying any unknown reason | 1,989,330,582 (93.3194%) |
| First-loop penalties | 59,598 |
| Loop replay penalties | 1,025,174 |
| CEC event entries with unknown overhead | 268,913 |

Between the 150s and 410s samples (after SPORT activation), the run executed
1,561,468,758 packets and accumulated 1,948,860,783 additional partial estimated
cycles. Unsupported fixed-cost packet fraction in that interval was 4.9190%.

### Encountered unmodelled reasons

| Reason | Count at 410s |
| --- | ---: |
| Dependency opportunities | 1,464,312,415 |
| Memory-wait opportunities | 779,954,559 |
| Other unsupported fixed costs | 56,672,388 |
| LINK/UNLINK fixed remainder | 17,710,310 |
| Return fixed remainder | 12,925,764 |
| Cache operations | 6,285,955 |
| IDLE elapsed time | 2,722,071 |
| Synchronization outstanding work | 2,162,461 |
| Event instruction cost | 49,410 |
| Event entry overhead (not packet-union occurrences) | 268,913 |

No faulting-attempt category was observed in the final firmware sample.
Frequent unsupported opcodes (runtime slot occurrences):

| Opcode | Count |
| --- | ---: |
| `dagMODik_ADD2` | 38,062,546 |
| `ProgCtrl_RTS` | 12,656,852 |
| `Linkage_LINK` | 8,866,225 |
| `Linkage_UNLINK` | 8,844,085 |
| `ALU2op_MUL` | 7,980,821 |
| `CaCTRL_FLUSH_pp` | 4,180,800 |
| `ProgCtrl_STI` | 3,409,837 |
| `ProgCtrl_CLI` | 3,409,836 |
| `ProgCtrl_IDLE` | 2,722,071 |
| `dagMODik_SUB4` | 2,522,577 |
| `ProgCtrl_SSYNC` | 2,152,981 |

Other encountered opcodes include `dagMODim_ADD`, `dagMODik_SUB2`, RTI/RTX,
RAISE/EXCPT, CSYNC, FLUSH/FLUSHINV variants. Complete counts are in the log's
final `shadow-unknown-opcode` group, lines 2190–2210. Snapshot totals and reason
counts are at lines 2164–2189.

## Observed clock programming and limits

`OP1EMU_TRACE_CLOCK=1` records at most 128 CPU-thread writes through
`EmulatorMemory` to PLL_CTL, PLL_DIV, VR_CTL, PLL_STAT and PLL_LOCKCNT addresses.
It returns no replacement values and registers no PLL device. Each record
includes whether the address is actually mapped. The final run observed:

| Address/register | Value | PC | Packet entry |
| --- | --- | --- | ---: |
| `0xFFC00008` VR_CTL | `0x0000` (16-bit write) | `0xEF000C3C` | 1,968 |
| `0xFFC00004` PLL_DIV | `0x0005` (16-bit write) | `0xEF000C76` | 29,280,888 |
| `0xFFC00000` PLL_CTL | `0x2000` (16-bit write) | `0xEF000C78` | 29,280,889 |

The PLL writes occurred approximately 28.35s after CPU startup. No subsequent
clock writes were observed through the final run, and the trace did not reach
its limit. Both trials saw PLL_DIV=5 and PLL_CTL=0x2000; VR_CTL differed, which
is consistent with the untrustworthy pre-existing unmapped read path.

**CONFIRMED programming intent**, using BF52x HR chapter 18:

- PLL_CTL `0x2000`: MSEL=16, DF=0; BYPASS, PLL_OFF, STOPCK and PDWN bits clear.
- PLL_DIV `0x0005`: CSEL=0 (VCO/1), SSEL=5 (VCO/5).
- Under normal PLL operation this requests **CCLK = 16 × CLKIN** and
  **SCLK = (16/5) × CLKIN**.

**Not confirmed as active hardware state:** all these addresses report
`mapped=0`. This checkout has no PLL/DPM register device. Writes are discarded
by the existing memory bus. Unmapped 32-bit reads return zero; unmapped 8/16-bit
reads can return uninitialized values in the existing bus implementation. The
initial read trace therefore cannot establish reset values, lock, bypass or
voltage/power state. This pre-existing missing MMIO behavior was not repaired
or replaced for the timing experiment. The final observer leaves all reads
unchanged and records only writes.

**CLKIN remains UNKNOWN.** The original OP-1's iFixit device page lists a nominal
400MHz core, but this does not identify the oscillator or prove active PLL
transitions. The TE original guide and DSP-board replacement guide consulted
did not establish CLKIN or its physical connection. No convenient oscillator
frequency was selected and no absolute CCLK transition is claimed.

Board/vendor references consulted:

- <https://www.ifixit.com/Device/Teenage_Engineering_OP-1>
- <https://www.ifixit.com/Guide/Teenage+Engineering+OP-1+DSP+Board+Replacement/65424>
- <https://teenage.engineering/guides/op-1/original>

## Readiness and smallest next step

The model is **useful for characterization**: supported issue components cover
95.38% of packet attempts, and missing fixed-cost work is concentrated in a
small number of instruction families. This identifies tractable next research
targets rather than requiring every possible BF524 pipeline effect at once.

It is **not yet a defensible elapsed-time source for a nominal-rate SPORT
experiment**. A 4.62% unsupported packet fraction cannot bound missing cycles:
an infrequent wait can dominate time. Memory/dependency opportunities are dense,
and IDLE/event/synchronization behavior is unmeasured. No absolute CCLK is known.
The result does not prove that an ideal-memory model would be useless for a
provisional causal test, but it does not validate its sufficiency either.

Smallest next work, still before device conversion:

1. Establish CLKIN from a board schematic, readable oscillator identification
   plus routing evidence, or a measurement; verify how the firmware's PLL
   programming sequence establishes active CCLK. The requested ratio is now
   known, but the clock MMIO model is missing.
2. Resolve the frequent unsupported DAG-modify, RTS, LINK/UNLINK and integer
   multiply issue costs from BF524-applicable vendor evidence and add only
   those fixed-cost rules/tests. Determine whether an explicit ideal-memory
   assumption and a separately defined IDLE policy are adequate for one causal
   experiment; do not silently treat these holes as zero elapsed time.

No SPORT/CoreTimer/CYCLES/scheduler conversion was made.
