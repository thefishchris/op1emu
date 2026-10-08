# BF524 PLL / architectural IDLE foundation

## Result and boundary

**Implemented and synthetic-tested:** a disconnected BF524 PLL input-tick domain,
requested versus committed configuration, architectural CPU IDLE, SIC wake
eligibility independent of interrupt eligibility, and next-known-PLL-deadline
advancement while idle. **Not ready for the SPORT causal experiment.**

The one controlled v241 observation exposed an earlier prerequisite than the
previous no-op-IDLE observation: same-value VR programming wakes immediately,
but leaves an **awake** 512-CLKIN-tick bypass interval. There is no authoritative
running input-clock source in this checkout. Execution stops explicitly before
issuing the next packet. Neither shadow estimates nor host time were substituted.

The later `PLL_DIV=5 -> PLL_CTL=0x2000 -> IDLE` sequence passes synthetic tests.
The first foundation trial did not reach it. A later, separately opted-in
awake-bypass trial passed the first VR interval and stopped at an NFC status
IDLE, still before that PLL sequence. No tape UI or firmware boot is claimed.

## Evidence and uncertainty

Primary sources are the exact vendor-authored PDFs and hashes recorded in
[clock readiness](op1-clock-readiness.md): BF52x HR Rev. 1.0 pp. 18-5, 18-14/15,
18-26–30; Programming Reference Rev. 2.0 p. 16-3. Register diagrams were inspected
visually, including the **BF522/524/526** variants, not just flattened text.

- **CONFIRMED documentation:** PLL_DIV is dynamic; multiplier/PLL-power
  reapplication needs lock progression starting at the programming IDLE;
  counter units are CLKIN ticks (HR prose and Table 18-8). Figure 18-8's SCLK
  caption conflicts with that prose; it is not used as the unit definition.
- **CONFIRMED documentation:** same-value PLL_CTL/VR_CTL programming can wake
  immediately while both clocks stay bypassed for PLL_LOCKCNT. Wake and return
  to multiplied clocks are different events.
- **CONFIRMED documentation:** BF524 has no internal voltage regulator. VLEV
  read/write storage does not implement a voltage ramp or stabilization delay.
- **UNKNOWN reset active frequency:** HR Figure 18-6 states hex reset `0x0B00`,
  but its binary reset drawing shows bits 11 and 10 set (`0x0C00`). Its BYPASS
  field and the reset FULL_ON status also do not justify inferring an active
  multiplier from the stated hex reset. The model exposes stated reset readbacks
  and leaves active PLL_CTL/frequency unknown until supported CTL programming.
  This discrepancy requires a corrected BF524 register specification or reset
  readback evidence; it has not been silently resolved by choosing a frequency.
- **LIKELY / PROVISIONAL:** board-photo inference of 25 MHz CLKIN. Only explicit
  `OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000` selects this experimental assumption.
  Default frequency is unspecified. The PLL class also takes an explicit input
  frequency for synthetic tests. Tick countdowns do not require a frequency.

## Implemented PLL state

MMRs occupy `0xFFC00000` through `0xFFC00013`, with 16-bit values at 4-byte strides.

| Register | Reset readback | Write mask / behavior |
| --- | --- | --- |
| PLL_CTL | `0x0B00` | `0x7FEB`; request only until programming IDLE |
| PLL_DIV | `0x0004` | `0x003F`; immediately effective; SSEL=0 unsupported |
| VR_CTL | `0x70B0` | `0xF7F0`; requested/committed storage, normal-mode clock-buffer/wake controls |
| PLL_STAT | `0x00A2` | Read-only; supported mode/lock derived after activation |
| PLL_LOCKCNT | `0x0200` | Low 16 bits; captured countdown at activation |

MSEL=0 encodes 64; DF divides CLKIN by two; CSEL divides VCO by 1/2/4/8.
Effective SSEL is raised to at least the CCLK divider as documented on HR 18-5.
PLL_DIV does not start a lock countdown. Unsupported/reserved power requests
have a readable reason and no fabricated wake/deadline.

On supported CTL activation the committed target is separate from requested
MMR storage; temporary bypass/lock state determines the effective clocks. On
multiplier change or PLL power reapplication, PLL_LOCKED clears, the input-tick
countdown runs, and expiry asserts PLL wake. Normal BYPASS/PLL_OFF bookkeeping
is represented; STOPCK, PDWN, hibernate and reserved VR FREQ modes are unsupported.
Normal VR clock-buffer changes commit and wake without inventing voltage settling.
The same-value case preserves PLL_LOCKED and wakes immediately, but retains the
bypass deadline; expiry does not produce a second wake.

PLL_STAT uses bit 0 ACTIVE_PLLENABLED, bit 1 FULL_ON, bit 3 ACTIVE_PLLDISABLED,
bit 5 PLL_LOCKED. VSTAT bit 7 preserves reset compatibility; it is **not** a
modeled BF523/525/527 regulator or evidence of a BF524 voltage measurement.
During the first same-VR bypass, status is `0xA1` and both provisional clocks
are 25 MHz even though the eventual reset multiplier is unknown.

Under committed `0x2000`, divider `5`, and expired lock count, the explicitly
provisional frequencies are CCLK=400 MHz and SCLK=80 MHz. A 512-tick interval at
the provisional 25 MHz input is 20,480 ns. This is conditional arithmetic, not
a measured delay or a host-performance result.

## IDLE and wake mechanism

Bcore has `idle`, `idle_pc`, and `idle_entries` separate from `halted`.
IDLE checks supervisor privilege, enters idle once, and terminates its JIT block.
The normal PC/hardware-loop epilog supplies the resume PC. User-mode IDLE faults
without entering idle. `Core::run()` refuses all issue/translation/accounting
while idle. An eligible CEC dispatch also clears idle and vectors normally.

The platform handles IDLE before ordinary device polling can advance execution:

1. Commit a pending supported PLL/VR request.
2. Check already-asserted SIC ISR & IWR wake conditions independently of SIC
   IMASK and CEC mask/priority. If no pending wake exists and the PLL provides a
   deadline, advance **only** its independent CLKIN domain by that exact amount.
3. Assert DPMC SIC source 0 on PLL wake. SIC IWR gates wake, while normal interrupt
   routing remains separate. Wake never requires an eligible CEC interrupt and
   never itself issues another packet. An eligible interrupt may vector after
   wake through the existing CEC routing.
4. Acknowledge the modeled PLL wake after consumption. Masked PLL wake, unknown
   completion/deadline, unsupported power requests, or an outstanding **awake**
   bypass deadline stop the implementation path with a bounded diagnostic.

This is not a global scheduler. It can progress the PLL domain during IDLE and
consume an already-asserted wake source. It supplies no future peripheral
deadline and no periodic wake. Unsupported stops preserve state and latch an
observable `UnsupportedIdle()` flag; the frontend remains live, but further
`Run()` calls issue no packets or device work. No host sleep is used.

Diagnostics include idle/resume PC, packet/IDLE counts, return registers,
SEQSTAT/CEC/SIC state, PLL readbacks, all DP registers, selected non-destructive
OTP/NFC/TWI/DMA registers, the last 32 actual system-MMIO operations, and bounded
surrounding disassembly. The ordinary host-paced CoreTimer/CYCLES/SPORT/DMA and
the old frontend event queue remain provisional existing behavior; the new PLL
domain and shadow observer are not connected to them. Already-asserted SIC
interrupt forwarding now explicitly queues with zero delay so wake dispatch
does not invent one packet of latency.

## Synthetic verification

Firmware-free `pll_idle_tests` and expanded `execution_accounting_tests` cover:
reset/read/write/masks, requested versus active, dynamic/effective dividers,
511+1 tick lock boundary, power reapplication, same-value immediate wake versus
bypass expiry, zero lock count, BF524 VR behavior, unsupported modes, IWR gating
independent of interrupt masks, one IDLE attempt, no issue while idle,
post-IDLE resume, privilege fault, NMI and eligible maskable interrupt wake,
platform PLL-deadline wake in masked reset context, unknown-wake stopping, and
the actual same-reset-VR request / missing-awake-clock stop.

The controlled observation exposed a status-readback edge case: when reset
active PLL_CTL was unknown, the initial code incorrectly kept reset STAT `0xA2`
through temporary bypass. After the observation, the model was corrected to
return `0xA1` during that interval and given deterministic model/platform
regressions. **The retained GUI-trial log predates this readback correction.**
The stopping condition, packet/IDLE counts and wake/deadline behavior are unchanged.
No second GUI trial was run.

Final verification: the GUI and all requested test targets build; all nine
selected CTest entries pass (0.45 s total on this host). The new PLL source also
passes `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion` syntax checking
with inherited unused-virtual-parameter warnings suppressed. Parent and Bcore
`git diff --check` pass. Initial full rebuilding emitted existing LLVM-header
redundant-move warnings; no new PLL/test-source warnings were emitted.

## One controlled v241 observation

Private prefix: `build/pll-idle-foundation-v241.*`, including fresh private NAND
and OTP copies and screenshots at 60-second intervals through 420 seconds.
Runner terminates its own process (`exit=-15`). These artifacts are not source
files and must not be committed. The runner's environment whitelist was extended
to pass the explicit provisional CLKIN opt-in; NAND/OTP/GPIO/divisor selection is
otherwise the same handoff setup.

**CONFIRMED emulator observation:**

- At packet 1,968, `0xEF000C3C` writes VR_CTL=`0x70B0`, equal to the mapped reset
  readback. The earlier unmapped observation wrote zero at the same address;
  that was not evidence of an actual BF524 hibernate request.
- Packet 1,969 executes IDLE at `0xEF000C3E`, resume `0xEF000C40`, entries=1.
- PLL wake is consumed immediately via SIC source 0. CEC remains IMASK/IPEND/ILAT
  `0x1F/0x12/0`; no CEC handler is required or delivered.
- Before the next packet, the explicit guard reports
  `awake PLL bypass countdown needs a running CLKIN time source`.
- There are **512 ticks remaining**, zero input ticks consumed, CPU idle=false
  after wake, and the implementation path is stopped at `0xEF000C40`.
- State: RETS=`0xEF000B8E`, RETI=0, SEQSTAT=0, active IVG=1; SIC masks=0/0,
  ISR=0/0 after wake acknowledgement, IWR=`1/0`; PLL CTL/DIV/VR/LOCKCNT
  `0x0B00/4/0x70B0/0x0200`. P4=`0xFFC00000`, P5=`0xFFB00E58`,
  SP=`0xFFB00D80`, FP=`0xFFB00DE8`, R4=`0x70B0`, R7=`0x51`.
- Preceding operations include VR read at `0xEF000954`, OTP reads/acknowledgments,
  saved IWR0/1 reads at `0xEF000BF0/F6`, writes IWR0=1 and IWR1=0 at
  `0xEF000BFE/0xEF000C04`, then the VR write and IDLE. Following code would
  restore the saved IWR values at `0xEF000C42/48`; it did not execute.
- OTP CONTROL/STATUS/TIMING=`0x4008/0/0x1485`; NFC CTL/STAT/IRQSTAT/IRQMASK
  `0x200/0x11/4/0x1F`; selected TWI and SPORT-DMA state is zero/inactive.
- Shadow partial estimate at the stop: **2,435**, for **1,969** packet attempts.
  It is observational only and cannot consume the missing 512 ticks.
- **First unresolved event/time case:** same-value VR programming at
  `0xEF000C3E`, specifically post-wake running bypass expiry. Its producer is
  the documented DPMC/PLL programming transition, not an inferred audio flag.
  There is no unsupported peripheral IDLE reached in this trial.
- The later PLL-IDLE at `0xEF000C7A` and runtime DSP/flag IDLE at `0x019F18F4`
  were not reached. The runtime flag's producer remains **UNKNOWN**; it is not
  labeled a firmware bug.
- Final 420-second screenshot has a blank guest display. No splash/tape UI.
  Audio diagnostics show zero generated frames.

**Host performance observation only:** IDLE/wake was logged at 571,242 host
microseconds after the CPU's existing start timestamp (about 0.57 s). The process
stayed alive until the runner's 420-second termination. There is no boot-duration
measurement because boot did not complete. These host times validate no guest
clock, instruction latency, or PLL delay.

Diagnostic comparison with the old 420-second no-op-IDLE trial:

| Trial | Packet attempts | IDLE entries | Outcome |
| --- | ---: | ---: | --- |
| Prior observational/no-op IDLE | 2,316,835,109 | 4,303,080 | Tape UI screenshot |
| Architectural IDLE / narrow PLL | 1,969 | 1 | Explicit post-VR-wake time-source stop |

This difference is **not a speedup or timing validation**: the new trial stops
at an unsupported interval, rather than busy-executing past it.

## Reproduction and smallest next step

```bash
cmake -S . -B build
cmake --build build --target op1emu pll_idle_tests execution_accounting_tests sport_audio_tests usb_reset_tests mcp23017_tests pcm_sample_tests audio_ring_tests nand_ecc_tests -j2
ctest --test-dir build -R '^(pll_idle_tests|execution_accounting_tests|sport_audio_tests|audio_ring_tests|pcm_sample_tests|mcp23017_tests|nand_ecc_tests|otp_import_tests|usb_reset_tests)$' --output-on-failure

# X11/Xwayland required; choose a NEW, unused prefix.
OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000 OP1EMU_SHADOW_TIMING=1 OP1EMU_TRACE_CLOCK=1 \
  python3 /tmp/opencode/boot_trials.py NEW-UNUSED-PREFIX 10 420
```

The first foundation trial's next prerequisite was an awake bypass time source.
That narrow experiment is now recorded below. SPORT conversion remains blocked.

## Experimental awake bypass

`OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1` is valid only together with
`OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000` and `OP1EMU_SHADOW_TIMING=1`, and only
while the PLL reports a temporary-bypass deadline whose effective CCLK equals
that provisional CLKIN. It is an **experimental ideal/no-stall path**, not a
general authorization of shadow timing.

Around each awake `Core::run` call, the platform measures
`EstimatedCycles()` before and after. Only that positive delta advances the PLL
input-tick countdown, and only up to the remaining deadline. Old estimates are
not re-consumed. Idle calls do not enter this path; the existing idle PLL
deadline mechanism remains separate and does not read shadow cycles. Unknown
stall coverage is counted in the diagnostic but is not added as extra ticks.
If a block's delta crosses expiry, only the remaining ticks are consumed and
the excess is recorded as block-level overshoot. The exact intra-block expiry
instruction is not claimed.

After expiry, conversion stops. Functional execution may continue while no
timed transition needs a known CCLK/CLKIN ratio. The reset-active multiplier
is still not invented. Another timed PLL transition with an unknown ratio stops
explicitly again.

Deterministic tests cover a 512-cycle/512-tick expiry, no consumption without
new awake cycles, idle/awake non-double-counting, one expiry, unknown
post-bypass CCLK, branch-penalty consumption with overshoot 3 on a 515-cycle
crossing, and refusal unless all three opt-ins are present.

### Controlled observation: `build/pll-awake-bypass-v241.*`

Same unused-prefix runner, divisor 10, GPIO 17, fresh private NAND/OTP, shadow
timing, provisional 25 MHz, and the new opt-in. Runner exit `-15` is its own
420-second termination. Artifacts are not source files.

**CONFIRMED emulator observation:**

- The first same-value VR write and IDLE still occur at packets 1,968/1,969,
  PC `0xEF000C3E`, immediate masked-CEC wake, resume `0xEF000C40`.
- Awake conversion then consumes **exactly 512** newly accounted shadow cycles.
  The expiry block is `0xEF005C24`: shadow delta 2, consumed 1, **overshoot 1**,
  remaining 0, expiries 1. Post-expiry CCLK is unknown. Intra-block expiry
  instruction is unknown.
- Functional execution continues. No second clock conversion occurs.
- The later `PLL_DIV=5` / `PLL_CTL=0x2000` writes are **not observed**. Reset
  active-PLL ambiguity does not stop this path: status returns to `0x00A2`,
  active CTL remains unset, and ordinary execution proceeds until the next IDLE.
- Next explicit stop: IDLE at **`0xEF00754C`**, resume `0xEF00754E`, packet
  **15,813**, IDLE entry **2**. Reason: no known eligible wake completion or
  deterministic deadline. CEC remains `0x1F/0x12/0`; SIC ISR is clear and IWR
  is all-ones, so this is not a masked-interrupt miss.
- P3 is `0xFFC00000`. The instruction before IDLE writes `NFC_CMD=0xFF`; the
  next instruction reads `NFC_IRQSTAT` bit 0 (`NBUSYIRQ`) and branches back to
  IDLE while clear. At the stop, `NFC_IRQSTAT=0`, `NFC_STAT=0x11`,
  `NFC_IRQMASK=0x16`. **LIKELY:** NAND not-busy completion after command `0xFF`.
  The completion deadline is not modeled. A preceding unknown NAND command
  `0x50` at `0xEF007524` is context, not the sampled wake condition.
- Shadow estimate at the stop is **17,946** for 15,813 packets. It remains
  observational and is not connected to NFC, SPORT, CoreTimer, CYCLES, or DMA.
- Host timestamps, about 0.56 s to the first IDLE and 0.80 s to the NFC stop,
  are performance observations only. The 420-second screenshot is blank. No UI
  or boot-duration result is claimed.

This is closer only in that the specific awake-bypass stop is experimentally
passed. It is **not sufficient for the SPORT causal experiment**. The smallest
next step is a modeled NFC `0xFF` not-busy completion/IRQSTAT event with an
evidence-backed deadline, then another bounded observation to see whether the
changed-multiplier PLL sequence is reached. Do not connect SPORT yet.

All unrelated dirty work was preserved. Parent HEAD remains `55335b4`; Bcore HEAD
remains `e1f434f46fe391f6400d0e00ec0395a631acb89d`. Nothing was staged or committed.
