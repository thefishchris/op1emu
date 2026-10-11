# Guest clock and event-deadline architecture

Status: **DESIGN, not implemented.** Written 2026-10-09 against parent
`1b21548` + working tree and bcore `ae6b207`.

Revision 2 (2026-10-09), after design review. The overall architecture is
approved as the direction. Changes in this revision:

- D2 is restated as an ASSUMED minimum-cost model that drives a deterministic
  lower-bound timeline, not cycle accuracy.
- Core-timer progress during IDLE is downgraded to **UNKNOWN**.
- Deadlines now carry their clock domain. Whether a deadline can be used
  depends on the current power and clock state (new D4).
- The first core timer work covers awake behavior only (D7).
- An A/B experiment is designed to decide core-timer behavior during IDLE (§6).

Revision 3 (2026-10-09), after the documentary investigation (§6.1). No
hardware or JTAG is available, so only documents were searched. The
conclusion is unchanged: **core timer during IDLE = UNKNOWN**, and no IDLE
advancement through the core timer is permitted. What changed:

- Evidence rows E16–E21 were added.
- The CCLK-core IDLE state now depends on `PLL_CTL` STOPCK/PDWN (§3):
  sleep and deep-sleep requests stop CCLK (CONFIRMED by derivation), while
  plain IDLE stays UNKNOWN.
- `CYCLES` during IDLE now has explicitly conflicting sources (D6).

Revision 4 (2026-10-09), after review of the first firmware trial. The IDLE
deadline rule (D4) is refined in section 10 to separate event eligibility
from state evolution, and a counterfactual check of the boot-ROM PLL IDLE is
recorded there. Not yet implemented.

Revision 5 (2026-10-09): a profile-scoped decision, implemented (section 11).
**Strict:** core timer during plain IDLE = UNKNOWN; a running core timer holds
IDLE (unchanged). **Functional:** core timer during plain IDLE = ASSUMED
RUNNING, for usability. The global evidence classification (D6, section 3)
is not changed: this is a documented functional-profile assumption, not a
BF524 hardware claim.

Related documents:

- `docs/upstream-guest-clock-audit.md`: what upstream PR #13 (`TimeSource`)
  does and why it is not adopted as-is.
- `docs/pll-idle-awake-bypass-checkpoint.md`: PLL/IDLE foundation and the
  `0x019F18F4` boundary capture.
- `docs/bf524-execution-accounting.md`, `docs/shadow-timing-coverage.md`:
  packet-entry accounting and the opt-in shadow issue model.
- Host-derived time paths: `docs/hacks.md` (H3). The original inspection notes
  (`guest-progress-clock-inspection.md`) are local and were not committed.
- `docs/hacks.md`: existing timing hacks (audio and core timer slowdowns).

## 1. Problem

Guest-visible time currently comes from the host:

- `CYCLES` and the core timer use `system_clock` microseconds × 400;
- SPORT uses `steady_clock` ÷ a divisor;
- the RTC uses `system_clock`;
- panel TE comes from the frontend's host poll;
- GP timers tick when `cyclesElapsed % 10000 == 0`;
- `QueueEvent` delays are counted in `Run()` calls, despite being typed as
  nanoseconds.

Separately, architectural IDLE is real. While idle, `Run()` returns before any
time-driven device update, and `ServiceIdle()` knows only NAND/NFC/MDMA
completions, the PLL lock deadline, and SIC wakes. At `0x019F18F4` the
firmware idles with the core timer armed (`TCNTL=7`, `TPERIOD=400000`,
`TSCALE=0`, IVG6 unmasked) and no asserted SIC wake source. The emulator stops
with `idle-unsupported`.

Goal: one deterministic, evidence-labelled guest time, derived from the
modelled clock tree and the executed work, never from the host. It must
advance through architectural IDLE only to deadlines whose source clocks are
known to run while the core is idle.

Non-goals for the first implementation:

- nominal SPORT/audio timing, RTC, panel TE, GP timers;
- DMA bandwidth and cycle-accurate costs;
- real-time pacing for interactive use;
- a general scheduler rewrite;
- **any change that lets the `0x019F18F4` boundary advance**: that waits on §6.

## 2. Evidence base

| # | Fact | Label | Source |
| --- | --- | --- | --- |
| E1 | Core timer runs at CCLK; `TCOUNT` decrements every `TSCALE+1` CCLK; at 0 sets sticky `TINT`, raises IVTMR; `TAUTORLD` reloads from `TPERIOD`, else stops | CONFIRMED | BF52x HRM 11-1..11-7 |
| E2 | IVTMR goes directly to the CEC, not through the SIC; edge-sensitive; cleared automatically when serviced; `TINT` is not W1C | CONFIRMED | HRM 11-3, 5-4 |
| E3 | `TPERIOD` writes copy to `TCOUNT`; `TCOUNT` may be written directly before enable; `TPERIOD`/`TCOUNT` writes are ignored while running; TMREN with TMPWR=0 is undefined | CONFIRMED | HRM 11-3, 11-5, 11-6 |
| E4 | CCLK = VCO / 2^CSEL; SCLK = VCO / max(SSEL, 2^CSEL); VCO = CLKIN × MSEL (÷2 if DF); in bypass CCLK = SCLK = CLKIN | CONFIRMED | HRM 18-3..18-5, Table 18-4; implemented in `src/cpu/pll.cpp` |
| E5 | `PLL_DIV` can change at any time; `PLL_CTL` changes take effect through the IDLE programming sequence, and PLL lock completes while the core is idle | CONFIRMED | HRM 18-5, PLL programming sequence; existing PLL model |
| E6 | Full-on and active modes have CCLK enabled; in these modes the core "can either execute instructions or be in the IDLE core state"; sleep mode disables CCLK and keeps SCLK | CONFIRMED | HRM Table 18-4, 18-8, 1-26 |
| E7 | In IDLE, "when all the pending processes have completed, the core disables its clocks" | CONFIRMED wording; whether this stops the CCLK-driven core timer is **UNKNOWN** | Blackfin PRM 3-9 |
| E8 | A SIC source enabled in `SIC_IWR` wakes an idled core, even if masked in `SIC_IMASK` | CONFIRMED | HRM 5-6 |
| E9 | With interrupts globally disabled, IDLE ends only through a WAKEUP (SIC_IWR) | CONFIRMED | PRM 3-9 |
| E10 | bcore ends architectural IDLE whenever it dispatches an eligible core event | CONFIRMED (code) | `ext/bcore/src/cec.cpp:215` |
| E11 | Packet entries count executed instruction packets, including loop replays and early exits; they are not CCLK | CONFIRMED (project decision) | `docs/bf524-execution-accounting.md` |
| E12 | CLKIN = 25 MHz | LIKELY / PROVISIONAL | board-photo inference; `OP1EMU_EXPERIMENTAL_CLKIN_HZ` |
| E13 | At the boundary, `PLL_CTL=0x2000` (MSEL 16, DF 0, no bypass), `PLL_DIV=5` (CSEL 0, SSEL 5): CCLK = 16 × CLKIN, SCLK = 3.2 × CLKIN | CONFIRMED (observed registers) | checkpoint doc |
| E14 | Reset `PLL_CTL` documentation is internally inconsistent; the active reset ratio is not inferred | CONFIRMED (project decision) | `src/cpu/pll.h` |
| E15 | BF524 IDLE supply current has a nonzero CCLK-domain activity scaling factor (`IDD-IDLE` ASF 0.44, specified at a stated fCCLK) | CONFIRMED wording; does **not** identify which CCLK-domain units run, so it does not resolve E7 | BF524 datasheet, power tables |

| E16 | The documented CCLK gates are in the PLL output clock generator: `PLL_CTL` STOPCK ("Stop Clock: CCLK off", sleep mode) and PDWN ("all internal clocks off", deep sleep). No other CCLK gate is described in chapter 18 | CONFIRMED | HRM Figure 18-1, Figure 18-6, 18-9 |
| E17 | The cycle counter "counts CCLK cycles while the program is executing", with all cycles counted "while the processor is in User or Supervisor mode"; it stops in Emulator mode | CONFIRMED wording; IDLE is not mentioned | PRM 21-28 |
| E18 | Idle is one of two "non-processing states", distinct from the User/Supervisor/Emulator modes; Idle "stops all processor activity" | CONFIRMED | PRM 3-2, 3-9, Figure 3-1 |
| E19 | The ADSP-BF522/BF524/BF526 anomaly list (rev H, 2016-01-25) has no entry about IDLE, the core timer, `CYCLES`, or core clock gating | CONFIRMED (absence) | ADI anomaly list, retrieved from a third-party mirror (analog.com download failed) |
| E20 | Linux/Blackfin (through v4.16) defaults `TICKSOURCE_CORETMR=y` for all BF5xx, including BF524. Its idle path is `sti; idle` with interrupts enabled, and the core timer clockevent has no `CLOCK_EVT_FEAT_C3STOP` ("stops in idle") flag | INDIRECT | `arch/blackfin/Kconfig`, `kernel/process.c`, `include/asm/irq.h`, `kernel/time-ts.c` |
| E21 | Linux/Blackfin defaults `CYCLES_CLOCKSOURCE=y` (non-SMP): `CYCLES` is the preferred clocksource (rating 400, `CLOCK_SOURCE_IS_CONTINUOUS`) while the same idle path executes `idle` | INDIRECT | `arch/blackfin/Kconfig`, `kernel/time-ts.c` |

Only BF52x and BF5xx-generic documentation is used. Newer Blackfin+
documentation is deliberately not used to infer BF524 IDLE behavior. INDIRECT
rows record what BF5xx software relied on. They cannot raise a label above
LIKELY, and they do not override a conflicting ADI statement.

## 3. Clock domains and their state while idle

Every timed behavior belongs to exactly one domain. Each domain has an activity
state per power state:

| Domain | Examples | Awake (full-on/active) | Architectural IDLE | Label for IDLE column |
| --- | --- | --- | --- | --- |
| CLKIN | PLL lock countdown | runs | runs | CONFIRMED (E5: lock completes during the IDLE sequence) |
| SCLK | GP timers, SPORT, DMA, NFC, SIC wake sources | runs | runs | CONFIRMED that SCLK peripherals raise wakes to an idled core (E8); sleep mode also keeps SCLK (E6) |
| CCLK-core, plain IDLE (`PLL_CTL` STOPCK=0, PDWN=0) | core timer, `CYCLES`/`CYCLES2` | runs | **UNKNOWN** | E6/E15/E16 (+ indirect E20/E21) vs E7/E18; resolved only by §6 |
| CCLK-core, sleep request (STOPCK=1) or deep sleep (PDWN=1) | same | — | **stopped** | CONFIRMED by derivation: these modes disable CCLK (E6, E16), and the core timer and cycle counter are CCLK-clocked (E1, E17) |
| RTC crystal | RTC | runs | runs | CONFIRMED (RTC can wake even from deep sleep, HRM 1-26) |
| Host-time (temporary) | anything not yet converted | host | host | TEMPORARY HACK; never eligible as an IDLE deadline |

A domain whose IDLE state is UNKNOWN is *held*. That is not a claim that it
stops. It means the emulator will not let time pass across it while idle, in
either direction (D4). A domain that is **stopped** is a CONFIRMED state:
time may pass across it while its counters stay frozen. Its deadlines are
never eligible.

The domain state is a function of the power/clock registers at IDLE entry, not
a fixed property. Today `BF524PLL` already reports STOPCK/PDWN requests as an
observable unsupported stop. Sleep modes are therefore not modelled yet, and
the "stopped" row only takes effect once they are. The `0x019F18F4` boundary
has `PLL_CTL=0x2000` (STOPCK=0, PDWN=0), so it is plain IDLE, and the
CCLK-core domain there is UNKNOWN.

## 4. Decisions

Labels on decisions describe the strength of the underlying behavior claim,
not the confidence in the software structure.

### D1. Master time is CLKIN ticks, and clock domains are exact ratios of it

The guest clock holds one monotonic `uint64_t` count of CLKIN ticks. CCLK and
SCLK are derived domains whose ratio to CLKIN comes from the active PLL state
(E4). Each domain keeps its own integer cycle count and an exact fractional
remainder, so conversions never accumulate rounding error. Ratios are rationals
`num/den`, with numerator `MSEL` (64 for 0) and denominator `(DF?2:1) × divider`.

- Why CLKIN: the PLL lock countdown is already in CLKIN ticks (`BF524PLL`).
  CLKIN keeps running across PLL changes and IDLE, whereas CCLK changes rate
  (E5).
- **Absolute CLKIN frequency is not needed for CCLK/SCLK-domain devices.**
  Converting between them uses the register-determined ratio alone (E13). Hz
  is needed only to relate guest time to seconds: RTC, panel TE period, SPORT
  sample rate, host audio, and human-readable logs. Those stay gated on an
  explicit provisional CLKIN (E12), exactly as today.
- While the ratio is unknown (before the first PLL programming, E14), the
  CCLK domain still counts executed work, but anything requiring a CLKIN/SCLK
  conversion reports an observable unsupported stop. No reset ratio is
  guessed.
- Domain counters advance only while their domain is active (§3). A held
  domain's counter does not move.
- The current "awake bypass" experiment, which advances CLKIN from shadow
  deltas only while CCLK = CLKIN, becomes a special case of D1+D2. It is
  retired once D1+D2 is validated, not before.

### D2. Awake execution advances a deterministic lower-bound timeline

After each executed block, CCLK advances by the block's cost, then CLKIN and
SCLK advance by the exact converted amounts. The cost model is a named, logged
choice:

- **`min-cost` (default): 1 CCLK-equivalent per executed packet entry.**
  **ASSUMED.** This is a *minimum-cost model*, not a measured or documented
  per-packet cycle count. The project does not yet have a BF52x/Blackfin
  reference statement proving that every executed packet consumes at least
  one CCLK in the sense this model needs. Until such a citation is recorded,
  "packet count ≥ CCLK count" must not be treated as CONFIRMED.
- **`shadow-estimate` (opt-in):** the existing `ShadowTiming` estimate (ideal
  issue + documented penalties, with unknowns reported). It is still
  observational until its coverage is reviewed.

What this timeline is: a deterministic guest time that depends only on
executed work, the PLL ratios, and the chosen cost model. It reproduces across
hosts and runs.

What it is not: cycle-accurate. It omits stalls, cache and memory waits,
branch penalties and event-entry costs, so under `min-cost` guest time is
expected to run *slow* relative to the work executed. The firmware can do more
work per guest second than hardware would. Guest-time results must name the
cost model, and must not be cited as hardware timing.

### D3. Deadlines carry their domain and wake semantics

A timed device reports pending timed events as descriptors, not bare times:

```cpp
enum class ClockDomain { Clkin, Sclk, CclkCore, RtcCrystal };

struct Deadline {
    uint64_t clkinTick;   // master time of the event, rounded up (D9)
    ClockDomain domain;   // which clock must be running for it to occur
    const char* source;   // e.g. "coretimer.expiry", "pll.lock"; for logs
    bool raisesWake;      // the event asserts a wake-capable request (D5)
};

class TimedDevice {
public:
    virtual ~TimedDevice() = default;
    // Bring state up to now, counting only domain cycles that were active.
    virtual void SyncTo(const ClockState& clocks) = 0;
    // Pending timed events under the current registers; empty if none.
    virtual std::vector<Deadline> PendingDeadlines(const ClockState& clocks) const = 0;
};
```

`ClockState` is the guest clock's view: master time, domain counts and ratios,
and each domain's activity (running, held, or stopped). The exact API may
change during implementation. What must stay is this: a time alone is not
enough. Every deadline names its domain, and the consumer checks eligibility.

### D4. Deadline eligibility depends on power and clock state

A deadline is **eligible to advance time while idle** only if all of these hold:

1. its domain's IDLE state (§3) is CONFIRMED running;
2. its device is enabled and powered under the current registers;
3. the domain's ratio to CLKIN is known (D1).

Consequences:

- The PLL lock (CLKIN) is eligible. SCLK peripheral deadlines become eligible
  as each peripheral is converted. RTC deadlines become eligible when the RTC
  is converted and absolute Hz is available.
- **Core timer deadlines are *not* eligible while idle.** Its domain is
  UNKNOWN (§3, D6). While the core is awake, they are used normally.
- Host-time domains are never eligible.

IDLE handling in `ServiceIdle()`:

1. Service the existing finite completions (NAND/NFC/MDMA), as today.
2. Collect deadlines from the PLL and every timed device. Split them into
   eligible and ineligible.
3. If no eligible deadline exists: stop with `idle-unsupported`, as today. The
   report now lists the ineligible deadlines with their domain and reason, for
   example `coretimer.expiry domain=cclk-core idle-state=UNKNOWN`.
4. Otherwise, if any device in a **held** domain is running, advancing across
   it would require choosing hypothesis A or B (§6). Stop with
   `idle-unsupported reason=held-domain-active`. Nothing is frozen or advanced
   silently.
5. Otherwise, advance the master clock to the earliest eligible deadline,
   never past it, sync devices, and evaluate wake (D5). If not woken, repeat
   from 2. Logs are collapsed per idle episode.

Step 4 is deliberately strict. At `0x019F18F4` the core timer is running, so
any idle advance there is blocked until §6 resolves its domain. This prevents
the boundary from being "solved" by either hypothesis by default.

### D5. Wake evaluation covers SIC wakes and core events

The core leaves IDLE when either:

- a SIC source is asserted in `ISR & IWR` (E8, CONFIRMED; existing
  `SIC::WakePending`), or the PLL wake fires (existing); or
- a core event is latched and would be dispatched by the CEC under the current
  IMASK/IPEND/global-disable state. **LIKELY**: the PRM says IDLE lasts until
  an interrupt "that requires servicing", E9 is consistent with a masked core
  event not waking, and E10 already ends IDLE on dispatch.

Whether the core *timer* can raise such an event while idle is the D6/§6
question, not part of D5. D5 only defines how a latched event is evaluated.

Implementation consequence: while idle, deliver queued core-event raises and
run `cec_check_pending` before evaluating wake, instead of only after a SIC
wake. The existing invariant stays: a wake never issues a packet in the same
`Run()` call.

### D6. Core timer during IDLE: UNKNOWN

- For "continues": E6 (CCLK enabled in full-on/active modes; IDLE is a core
  state within them), E15 (nonzero CCLK-domain IDLE activity), and the timer's
  separate TMPWR power control.
- For "stops": E7. The Blackfin Programming Reference states that, once
  pending processes complete, the core disables its clocks in IDLE, and the
  core timer is CCLK-driven.
- No BF52x source examined states which applies to the core timer
  (§6.1 results).

Sub-questions, answered separately (revision 3):

| Question | Answer | Basis |
| --- | --- | --- |
| Does CCLK itself stop in plain IDLE? | **UNKNOWN**; LIKELY that the PLL's CCLK output keeps running and any stop is downstream core gating | E6, E16 (the only documented CCLK gates are STOPCK/PDWN) vs E7 (unspecified "its clocks") |
| Is the core timer's clock maintained independently of core gating? | **UNKNOWN** | The HRM states only "runs at CCLK" and gives TMPWR as its own low-power control; nothing addresses IDLE |
| Do `CYCLES`/`CYCLES2` advance through IDLE? | **UNKNOWN, conflicting** | E17/E18 imply counting only while executing in a mode, and Idle is not a mode; INDIRECT E21 (Linux used `CYCLES` as its continuous clocksource across `idle`) implies it keeps counting |
| Can the core timer expire while idle? | **UNKNOWN** | Depends on the row above; INDIRECT E20 leans yes |
| Can IVG6 wake the core from IDLE? | **UNKNOWN** for the timer as a source; D5's dispatch rule is LIKELY *if* the event is latched | INDIRECT E20: a periodic core-timer tick with `sti; idle` would hang an otherwise idle system if it could not |
| Do power/control registers change this? | **Yes, partly CONFIRMED**: STOPCK/PDWN stop CCLK (§3). The core timer's own TMPWR=0 stops it (E3). Nothing documented changes plain-IDLE behavior | E3, E16 |

Decision: **UNKNOWN**. The core timer is modelled as a deadline-capable device
(D3), but in plain IDLE its domain is held (§3), so its deadlines are
ineligible there (D4). `CYCLES`/`CYCLES2` share the CCLK-core domain and are
likewise held, so they do not advance across plain IDLE. The INDIRECT Linux
evidence points toward hypothesis A. It is recorded, but it does not change the
label: it conflicts with E7/E18, and by the §6.1 rule it could at most reach
LIKELY, which is not enough to permit IDLE advancement. No firmware run is used
to pick a hypothesis. Only an authoritative ADI statement or the §6.2 hardware
experiment decides it.

### D7. Core timer awake behavior per the HRM (first core timer work)

This covers only the awake behavior required by E1–E3. Guest-clock mode only:

- **Clock source:** the CCLK-core domain count (D2), not host time; no `÷10`.
- **State:** `tcount`, `tperiod`, `tscale`, the CCLK anchor at which the
  current prescale interval began, and `TINT`.
- **Enable:** on `TMREN` 0→1 with TMPWR=1, anchor at the current CCLK. TMREN
  with TMPWR=0 is undefined (E3). Log it as unsupported and do not count.
- **`SyncTo`:** decrement `tcount` by `floor(elapsed / (TSCALE+1))` and keep
  the prescale remainder. Handle multiple expiries arithmetically under
  auto-reload. On each expiry set `TINT` and raise IVTMR once per edge. Stop
  after a one-shot expiry.
- **Deadline:** anchor + `tcount × (TSCALE+1)` CCLK, converted up to a CLKIN
  tick (D9), with `domain = CclkCore` and `raisesWake = true`.
- **Register access:** reads sync first, so `TCOUNT` reads return the
  guest-time value at block-start granularity. Writes follow E3: `TPERIOD`
  copies to `TCOUNT`, and both are ignored while running. `TINT` is not W1C.
  The auto-clear on service matches E2 and is kept.
- **Awake delivery:** expiry is detected after each block, so an interrupt is
  delivered late by at most one block. The internal count stays exact. This
  is a documented approximation; splitting blocks at deadlines is a later
  refinement.
- The host-time path keeps the existing documented `÷10` hack unchanged until
  it is retired.

### D8. Mode selection, mixed domains, ownership

- Guest-clock mode is **opt-in** at first (proposed `OP1EMU_GUEST_CLOCK=1`
  plus a headless flag). The default stays as it is today, so the established
  tests and GUI baseline do not move.
- In guest-clock mode, devices not yet converted keep host time. Each such
  source gets a `docs/hacks.md` entry (TEMPORARY HACK) when the mode lands:
  SPORT pacing, GP timers, RTC, panel TE, and `QueueEvent` delays. Host-time
  domains are never IDLE-eligible (D4). Results in mixed mode must say so.
- `BlackFinCpu` owns the guest clock, declares it before the devices, and
  passes it by `const&`. Only the CPU loop advances it. Devices never call
  `std::chrono` (audit §Structurally reusable 1–2).
- Host input stays queued from the frontend thread and is applied at the next
  block boundary. Headless scripts remain the deterministic path.
- The host-time path is not a permanent peer mode (audit §Incompatible 7). It
  is removed once every guest-visible source is converted.

### D9. Overflow and arithmetic rules

- `uint64_t` CLKIN ticks: at 25 MHz, wrap takes about 23,000 years. Additions
  are checked in debug builds.
- Ratio conversion uses `num ≤ 64`, `den ≤ 30` and remainder carry. For
  multiplications that could exceed 64 bits, divide out whole quotients
  first, or use `unsigned __int128` behind a compiler static assert.
- Deadlines round **up** to the first CLKIN tick at or after the event, so an
  event is never observed early.

## 5. Implementation order

Each step is a separate small commit with tests, and is reviewed before the
next. Default-mode behavior does not change. The 11-test working-tree baseline
must keep passing after each step.

1. **Guest clock alone.** Ratio math, remainder carry, round-up conversion,
   domain activity states (running/held/stopped), overflow guards. Unit tests:
   - bypass 1:1;
   - the E13 ratios (16:1 CCLK, 16:5 SCLK);
   - CSEL/SSEL clamping, DF, unknown ratio;
   - long-run no drift;
   - a held domain does not advance while the master advances.
2. **CPU wiring behind the opt-in flag.** Awake CCLK advance by the
   packet-entry delta (D2 `min-cost`, logged as ASSUMED). Feed PLL ratio
   changes. In guest-clock mode only, `CYCLES` comes from the CCLK-core
   domain. Tests: a synthetic block sequence gives exact `CYCLES` and CLKIN
   counts.
3. **Core timer awake behavior (D7).** Unit tests from the HRM:
   - TSCALE 0, 1 and 255;
   - auto-reload and one-shot;
   - a direct `TCOUNT` write before enable shortens the first period;
   - `TPERIOD`/`TCOUNT` writes ignored while running;
   - `TINT` sticky and cleared by writing 0;
   - multiple expiries in one sync;
   - the deadline is exact in CCLK and rounded up in CLKIN at the E13 ratio;
   - an awake synthetic program receives IVG6 at the expected point.
4. **Deadline eligibility and IDLE plumbing (D3–D5).** Synthetic regressions:
   - IDLE with only a PLL lock deadline wakes exactly at lock;
   - IDLE with a running core timer and no other source stops with
     `held-domain-active`, listing the core timer deadline;
   - masked core events do not wake.

   A firmware run to `0x019F18F4` is used only to confirm the improved stop
   report: the core timer is listed as ineligible, and nothing advances.
5. **§6 experiment.** Only its outcome may change the CCLK-core IDLE state in
   §3. The change is a one-line, documented, evidence-cited update with
   regression tests for the chosen behavior.
6. **Later, separately evidenced:** RTC (crystal domain, needs absolute Hz),
   panel TE, GP timers on SCLK, `QueueEvent` delays in guest time, and SPORT
   last. Retire the awake-bypass experiment and the host-time path when their
   replacements are validated.

## 6. Experiment: does the BF524 core timer progress during IDLE?

Goal: distinguish **A** (the core timer keeps counting while the core is in
architectural IDLE) from **B** (it stops once IDLE's clock gating takes
effect). Neither may be supplied by a synthetic event.

### 6.1 Documentary search first (cheap, may resolve it)

Search BF52x-specific sources only:

- the ADSP-BF52x silicon anomaly list;
- ADI engineer-to-engineer notes on BF52x/BF5xx power management and power
  estimation (the datasheet cites EE-297 for the power vectors);
- BF52x HRM chapter 18 and the Blackfin PRM sections on IDLE and the core
  timer, re-read in full.

Explicitly excluded as BF524 evidence: Blackfin+ (BF70x) documentation.

Public BF5xx software (for example, how Linux on Blackfin paired a core timer
tick with `IDLE`) may be recorded as **indirect** evidence of what
practitioners relied on. It cannot raise the label above LIKELY by itself.

**Results (2026-10-09, revision 3).** Sources searched, in priority order:

1. **BF52x HRM (rev 1.0, local text).**
   - Chapter 11 (core timer): runs at CCLK, has TMPWR, and the BF52x "unique
     behavior" section reads "None". Nothing about IDLE. Its example
     (Listing 11-1) waits in a busy `jump` loop, not `IDLE`.
   - Chapter 18: CCLK is gated only by STOPCK/PDWN (E16); full-on and active
     modes keep CCLK enabled and allow the IDLE core state (E6); the PLL lock
     counter runs from the IDLE instruction (E5).
   - Chapter 5: SIC_IWR semantics (E8).
   - No statement on the core timer or CCLK in plain IDLE.
2. **Blackfin PRM (rev 2.0, local text).**
   - 3-9: Idle state, "the core disables its clocks" (E7).
   - 3-2: Idle is a non-processing state (E18).
   - 16-3: the `IDLE` instruction description has no clock statement.
   - 21-28: cycle counter (E17).
   - No core-timer-specific IDLE statement.
3. **Anomaly lists.**
   - BF522/BF524/BF526 rev H: nothing relevant (E19). The OP-1's DSPID
     `0x02` corresponds to silicon revision 0.2 in this list.
   - The BF523/BF525/BF527 list (rev K) could not be retrieved; the analog.com
     download timed out repeatedly. It is **not examined**.
4. **ADI application notes.**
   - EE-297 (cited by the BF524 datasheet power tables), EE-293 and EE-298
     define the IDLE power vector only as "the core executing IDLE".
   - The BF524 datasheet's `IDD-IDLE` activity factor (E15) does not identify
     which units run.
   - No EE-note found addresses the core timer or `CYCLES` in IDLE.
5. **ADI example code/BSP/VisualDSP sources.** No accessible source found
   that answers the question. Not examined further.
6. **Linux/Blackfin (INDIRECT).** E20, E21. This software ran on BF52x boards
   (the BF524 is a selectable CPU), and it treats the core timer as a working
   tick and wake source, and `CYCLES` as a continuous clocksource, across
   `idle`. A 2006 LKML thread from the Blackfin port describes "the timer
   interrupt waking up the processor every one jiffy", without naming which
   timer.
7. **Other BF52x bare-metal code (INDIRECT).** None found that is specific
   enough to use.

Outcome: ADI sources remain ambiguous (E7/E18 against E6/E16), and the only
direct pointer toward hypothesis A is INDIRECT. **UNKNOWN is retained.**

Correction (2026-10-09): an earlier revision said `ext/bfin_sim` was not
checked out. That was wrong; a shell glob error was misread. The then-pinned
`ext/bfin_sim` (`08e9153`, a subset of the GNU Blackfin simulator, since removed
from this repository) has been
examined: it contains no core-timer device, and its `IDLE` handler is a no-op
with the "timewarp to next event" code commented out. It provides no evidence
either way.

### 6.2 Hardware experiment (authoritative)

Run on a real BF524 (an original OP-1, or another BF524 board), using a
minimal bare-metal test loaded to L1. The key design choice is that the wake
comes from an **independent reference whose IDLE behavior is CONFIRMED**, so
the core timer's own count can be read after the fact. The test never relies
on the core timer waking the core.

1. Program a known PLL state and record `PLL_CTL`/`PLL_DIV`.
2. Reference: a GP timer on SCLK (CONFIRMED to raise wakes while idle, E8),
   with its SIC source enabled in `SIC_IWR` and unmasked, and period `T_ref`.
   Alternative: an RTC stopwatch (crystal domain). Using both cross-checks
   the reference.
3. Core timer: TMPWR=1, TMREN=1, TAUTORLD=0, a known `TSCALE`, and `TCOUNT`
   large enough not to reach 0 within `T_ref`. **Mask IVG6 in IMASK**, so the
   core timer can neither wake the core nor be serviced during the test.
4. Read `TCOUNT₀`, `CYCLES₀` and the reference count immediately before
   `IDLE`. Interrupts are globally enabled; only the reference interrupt is
   unmasked.
5. On the reference wake, first read `TCOUNT₁`, `CYCLES₁` and the reference
   count.
6. Repeat with several `T_ref` values and both PLL states (bypass and
   full-on).

Interpretation, with `Δref` converted to CCLK through the recorded ratio:

- **A:** `TCOUNT₀ − TCOUNT₁ ≈ Δref_cclk / (TSCALE+1)`.
- **B:** `TCOUNT₀ − TCOUNT₁` is roughly constant (only the awake
  instructions around `IDLE`) and independent of `T_ref`.
- **Partial:** a constant offset plus proportional growth, or saturation.
  Record it as is. It may reveal the "after pending processes complete"
  latency in E7.

The same data answers `CYCLES` during IDLE independently (`CYCLES₁ − CYCLES₀`).

A second, optional run unmasks IVG6 with a core timer period shorter than
`T_ref`, keeping the reference as a fallback wake. Under A the core should wake
at the core timer expiry, which tests D5 for the core timer. Under B the
reference wakes it and `TINT` is clear.

Open logistics, which **must be resolved before running**: how to load
bare-metal code onto the target safely (JTAG, or a boot-mode/LDR path that
does not touch the OP-1's NAND or firmware), and how to read results back
(JTAG memory read, or a result held in L1/SDRAM across a controlled reset).
Device-safety review is required before touching a real OP-1.

### 6.3 What does not count as evidence

- Running v241 under hypothesis A or B and observing whether `0x019F18F4`
  progresses. An emulator hypothesis switch may be used to generate
  *predictions* for comparison with hardware, but its outcome cannot set the
  §3 label.
- Static analysis of v241's idle and tick code. It can show what the firmware
  authors appear to expect, and may be recorded as indirect evidence, but not
  as BF524 behavior.

## 7. Observability

- New log category `clock`: mode, cost model (with its ASSUMED label), ratio
  changes, domain activity changes, and a per-idle-episode summary (start/end
  CLKIN, eligible deadlines taken and their sources, wake reason).
- `idle-unsupported` adds the full deadline table: source, domain, time,
  eligibility, and the reason for any ineligibility.
- Headless runs report CLKIN/CCLK totals and the cost model, so runs are
  comparable.

## 8. Open questions and risks

- **CCLK-core domain during IDLE (D6)** is UNKNOWN and gates the
  `0x019F18F4` boundary. Only §6 can change that.
- **D2 `min-cost`** is ASSUMED and makes guest time slower than hardware.
  Firmware that calibrates against the core timer, or uses busy-wait
  timeouts, may behave differently from hardware. Compare against
  `shadow-estimate`.
- **Block-granular interrupt delivery** while awake could matter for tight
  timer/ISR interplay. Measure it before deciding on block splitting.
- **Mixed domains (D8):** with the core timer on guest time and SPORT on host
  time, the timer/audio ratio differs from today's.
  Local, uncommitted notes (`audio-startup-divergence.md`) showed that ratio alone did not explain the
  startup regression, but mixed-mode runs must not be read as audio evidence.
- **Throughput:** upstream reports about 5–10 M guest cycles/s. Real-time
  pacing is a later frontend concern: a host may only throttle, never drive,
  guest time.
- **Unknown reset ratio (E14):** the guest clock must handle a CCLK-only
  period before the first PLL programming without inventing a frequency.

## 9. Implementation status (2026-10-09)

Implemented behind `OP1EMU_EXPERIMENTAL_GUEST_CLOCK=1` (default off; default
mode is unchanged and keeps the legacy `CoreTimer`):

- **Steps 1–2 (D1, D2).** `src/cpu/guest_clock.{h,cpp}`: CLKIN master time
  with exact sub-tick phase, CCLK/SCLK domain counts, and `ClockRatios` from
  `BF524PLL::Ratios()` (register-only, no Hz). Ratios are sampled at block
  start. Awake cost is the packet-entry delta per `Run()` (`min-cost`,
  ASSUMED), or `OP1EMU_GUEST_CLOCK_COST=shadow-estimate`, which requires
  `OP1EMU_SHADOW_TIMING=1`. The existing PLL lock countdown during IDLE (CLKIN
  domain, E5) also advances the guest CLKIN and SCLK, with CCLK held. `CYCLES`,
  SPORT, RTC, panel TE and GP timers are untouched and stay on host time.
- **Step 3 (D7).** `src/cpu/guest_coretimer.{h,cpp}`: HRM chapter 11 awake
  semantics on the guest CCLK. The header lists every ASSUMED choice; the
  most consequential is the auto-reload period of TPERIOD × (TSCALE+1). IVTMR
  is latched straight into the CEC after the block that reaches expiry.
- **Part of step 4 (D4 rule 4).** In plain IDLE with the guest core timer
  counting, `ServiceIdle()` stops with
  `held-domain-active device=coretimer domain=cclk-core idle-state=plain-idle`.
  It does this before any deadline is taken, including the PLL lock, and logs
  the guest clock and core timer state. No IDLE deadline advancement through
  the CCLK-core domain exists.

### First firmware trial (v241, guest-clock mode)

Inputs: snapshot NAND `c94057cb…` (unchanged) and a private copy of OTP
`06c99349…`, with the environment of the earlier boundary trials
(provisional CLKIN, shadow timing, PLL awake-bypass experiment, GPIO 17, audio
divisor 10) plus the guest clock. Two runs stopped identically.

- **Stop:** `PC=EF000C7A` (resume `EF000C7C`), boot ROM `bfrom_SysControl`
  PLL programming. It writes `PLL_DIV=5` and `PLL_CTL=0x2000`, then executes
  `IDLE` with `SIC_IWR0=1` (DPMC only); CEC IMASK/IPEND/ILAT = `C05F/8000/0`.
  This is the boot's first PLL relock.
- **Reason:** `held-domain-active device=coretimer domain=cclk-core
  idle-state=plain-idle`. The core timer was already running: `TCNTL=7`,
  `TPERIOD=0x61A80`, `TSCALE=0`, `TCOUNT=0x60E86`, next expiry at CCLK
  29,487,134, CLKIN unknown.
- **Guest clock:** CLKIN 358, CCLK 29,090,200, SCLK 358, CCLK without ratio
  29,089,842, no ratio alignments. Packets 29,090,200; IDLE entries 89,468,
  all earlier ones woken by existing NAND/NFC/DMA completions.
- **Ratios:** the active `PLL_CTL` was never programmed before this point, so
  the CCLK:CLKIN ratio was unknown for almost the whole boot. The only known
  interval was 358 CCLK at 1:1 during the early same-value-VR bypass. The
  pending relock targets CCLK 16:1 and SCLK 16:5.

Consequence: in guest-clock mode the firmware does **not** reach the later
`0x019F18F4` boundary. The first IDLE with a running core timer is the PLL
relock, whose only deadline is CONFIRMED (512 CLKIN). Under either hypothesis
the timer would not expire during that interval (396,934 decrements remain
versus at most 512 CCLK at 1:1). However, hypothesis A leaves `TCOUNT` lower by
up to 512 and B leaves it unchanged, so taking the deadline still requires
choosing. The rule stops instead of choosing. Default mode passes this IDLE
because the legacy core timer is not consulted during IDLE.

Open decision for review, not implemented: whether a held domain may cross
an eligible deadline when no held-domain event can occur in the interval
under either hypothesis, and what state the held device carries afterwards.

## 10. Refined IDLE rule and the PLL-IDLE counterfactual (revision 4)

### 10.1 Rule (decided in review; not yet implemented)

For each candidate eligible deadline, an active device on a domain whose IDLE
behavior is UNKNOWN is assessed on two separate questions:

1. **Event eligibility:** can the device produce an event (interrupt edge or
   status change) before the candidate deadline under *any* plausible
   hypothesis for its domain? This is decided by a bound, for example the
   remaining count against the maximum domain cycles in the interval.
2. **State evolution:** does its internal state change over the interval?
   For an UNKNOWN domain, the answer is unknown by definition.

The device blocks advancement only if (1) is possible, or if its ambiguous
state from (2) becomes guest-observable before it is deterministically
overwritten or reinitialized. It does not automatically block an earlier
CONFIRMED deadline. When ordering is safe but state evolution is unknown, the
condition is called **`unresolved-domain-state`**, distinct from
`held-domain-active`. Crossing is permitted only if the ambiguity is shown
(or, in future, tracked) to be destroyed before any observation. No general
symbolic-state engine is planned. Neither hypothesis for the CCLK-core domain
is chosen.

### 10.2 Counterfactual at the boot-ROM PLL IDLE (diagnostic only)

Temporary, uncommitted instrumentation let guest-clock runs cross the PLL
lock at `EF000C7A`. It applied only when the core timer's remaining count
exceeded a conservative bound of 512 CLKIN × 64 = 32,768 CCLK, and it traced
every guest core-timer access and expiry afterwards. Each run used the same
inputs as section 9; only the hypothesis differed:

| Hypothesis | Core-timer shift across the lock | Post-lock `TCOUNT` |
| --- | --- | --- |
| A (counts; modelled 1:1 relock, 512 CCLK) | −512 | `0x60C86` |
| B (frozen) | 0 | `0x60E86` |
| A16 (sensitivity: 16:1 during lock, 8,192 CCLK) | −8,192 | `0x5EE86` |

Results (runs A1, A2, B1, B2, A16):

- **Event ordering:** safe. The remaining count, 396,934, far exceeds every
  bound, and no expiry occurred before the PLL wake in any run.
- **Guest accesses after the wake:** **none.** From the wake to the next
  boundary (`0x019F18F4`, about 3,200 expiries later), the firmware never
  read or wrote `TCNTL`, `TCOUNT`, `TPERIOD` or `TSCALE`. The ambiguity is
  never overwritten, reinitialized, re-anchored, or read directly.
- **First divergence:** the first expiry. A is 512 CCLK earlier than B
  (B: CCLK 29,487,134; A: 29,486,622, detected at 29,486,623 at the block
  end). Both land at PC `019F190E`. At the second expiry the interrupted PC
  already differs (A `FFA0803C`, B `FFA08036`, CCLK 29,886,622 versus
  29,887,134). IVG6 therefore preempts different instructions, a
  guest-visible difference through RETI and execution interleaving.
- **Reconvergence:** none. With auto-reload and no rewrite, the 512-CCLK
  phase offset persists through every recorded expiry (offsets of −511 to
  −518 reflect block-granular detection).
- **Attribution:** repeats of the same hypothesis agree exactly (PC and CCLK)
  for the first 280 expiries, up to about CCLK 141.5M. Divergence from the
  host-time devices (SPORT, RTC, panel TE, GP timers) starts only after that.
  The A/B divergence at expiry index 1 (about CCLK 29.9M) is well inside the
  reproducible region, so it is caused by the hypothesis.
- **Next boundary:** every run reaches `0x019F18F4` and stops there with
  `held-domain-active`. No eligible deadline exists at that point, so the
  refined rule gives the same stop.

**Decision outcome:** A and B produce different guest behavior before any
reconvergence. This PLL-IDLE crossing is **not** observationally safe, and the
core timer during plain IDLE remains a real blocker. `unresolved-domain-state`
crossing is not authorized here, and neither hypothesis is chosen.
Documentary evidence still does not decide the hardware behavior (section 6).

## 11. Functional-profile IDLE deadlines (revision 5)

### 11.1 Assumption and scope

The functional profile (`docs/emulator-profiles.md`) enables the guest clock
and **ASSUMES that the CCLK-core domain keeps running during plain
architectural IDLE**, so the core timer counts and its direct CEC IVG6 request
may wake the core. The section 10.2 counterfactual shows this choice is
guest-observable. It is therefore an assumption, labelled `ASSUMED` in the
startup log, and never applied by `strict` or an unset profile. Sleep and deep
sleep (`STOPCK`/`PDWN`) are unaffected: they remain explicit unsupported stops
in the PLL model.

### 11.2 Arbitration (`BlackFinCpu::AdvanceFunctionalIdle`)

Each `ServiceIdle()` call, after the existing finite completions and only
when no wake is already pending, takes **one** deadline step:

1. Refresh the guest clock's ratios from the PLL. If they are unknown while a
   candidate exists, stop with `functional idle deadline needs known clock
   ratios`.
2. Candidates, in exact sub-tick units of the canonical CLKIN timeline:
   - **PLL lock:** CLKIN domain (CONFIRMED running in IDLE), wakes via DPMC
     (SIC source 0, subject to `SIC_IWR`).
   - **Core timer expiry:** CCLK-core domain (ASSUMED running), wakes via
     direct IVG6.

   A stopped or disabled timer is not a candidate.
3. Advance by the earliest candidate only: `GuestClock::AdvanceIdleUnits`,
   with the CCLK phase tracked exactly, plus the PLL by the CLKIN edges
   crossed. Then sync the core timer, which latches `TINT` and raises IVG6.
   Equal deadlines are serviced in the same step before any wake decision.
4. Run `cec_check_pending`. An eligible IVG6 dispatches through the existing
   CEC model (bcore ends IDLE, `RETI` = post-IDLE PC). A PLL wake is still
   routed through the existing SIC path. A deadline that wakes nothing (for
   example a masked IVG6) leaves the core idle for the next step.

No guest packets execute while idle, no host time is consulted, and there is
no PC special case.

### 11.3 Firmware results (v241 fixture, snapshot NAND, two runs)

- **Boot-ROM PLL IDLE `EF000C7A`:** the PLL lock is earlier. The step is 512
  units at 1:1; the core timer advances by the same interval, `TCOUNT`
  `0x60E86` → `0x60C86`. PLL state commits to ratios 16/1/5, the DPMC wake
  resumes at `EF000C7C`, and packets stay 29,090,200. This is identical in both
  runs (and equal to counterfactual hypothesis A).
- **Power-manager IDLE `019F18F4`:** the policy byte stays 1 (no clear, no USB
  event). The first step (run f1) entered IDLE at packet 1,294,447,509 and
  advanced 239,113 CCLK to the exact expiry (`TCOUNT` `0x3A609` → reload
  `0x61A80`, `TINT` latched). IVG6 dispatched to `EVT6=FFA09A88` with
  `RETI=019F18F6`, and packets were unchanged through the step. More than
  65,000 further timer-driven steps followed in each run, with no new
  unsupported boundary.
- **Interactive UI:** the tape UI appears at about 4.5 host minutes. `play`
  runs the transport (counter to 0:00:11) and `stop` holds it (0:00:16, still
  held 30 s later). There was no hang or assertion over about 11 minutes.
- **Repeatability:** all 178,937 logged idle events agree through the boot-ROM
  PLL wake. The runs then diverge during awake execution, before the first
  `019F18F4` entry (packets 1,294,447,509 versus 1,290,260,068), because SPORT,
  RTC, panel TE and GP timers still use host time. The idle mechanism itself
  is host-time free. Full repeatability needs those devices on the guest clock.

## 12. SPORT on the guest clock (revision 6, functional profile)

### 12.1 Assumption and scope

SPORT0 is CONFIRMED externally clocked, so neither SCLK nor the SPORT dividers
set its rate. The functional profile **ASSUMES a 44,100 Hz frame rate**. The
evidence is LIKELY (firmware engine rate; MCLK inferred as 11.2896 MHz); the
physical clock is UNKNOWN (unmeasured). The labels, the arithmetic and the
evidence are in `docs/sport-frame-format.md`. `strict` and unset profiles never
apply the assumption: SPORT keeps host-time pacing there, and it is never
IDLE-eligible (D4).

### 12.2 Timeline and IDLE arbitration

- **Words due:** a pure function of guest CLKIN since each direction's first
  DMA access: `floor(Δclkin × fs / CLKIN)` frames × words per frame. The
  stereo `0x021F` configuration gives 88,200 words per guest second.
- **Deadline candidate:** `AdvanceFunctionalIdle` gains a third candidate. It
  is the earliest CLKIN tick at which a running SPORT DMA channel can finish
  its current row (`DMA::PeripheralRowBytesRemaining`, then
  `SPORT::{Transmit,Receive}DueClkin`). Only a strictly future tick is a
  candidate, so an unsatisfiable row never stalls the timeline.
- **Domain:** SPORT is externally clocked and DMA is an SCLK-domain block, so
  it keeps running in plain IDLE (§3). No new IDLE-domain assumption is
  needed beyond the frame rate itself.
- **Order within a step:**
  1. advance to the earliest candidate;
  2. `ServiceSportDma()` moves every SPORT word due at that timestamp, across
     all SPORT DMA channels (repeated transfers until nothing moves);
  3. sync the core timer;
  4. wake evaluation (D5).

  `ServiceIdle` also services SPORT DMA at the current time before its wake
  check, so a DMA interrupt due at IDLE entry is seen.
- **Wakes:** event occurrence is independent of CPU wake. A DMA completion
  latches `DMA_DONE` and the SIC `ISR` bit. It ends IDLE only if the source is
  enabled in `SIC_IWR`; reaching a SPORT deadline does not wake the core by
  itself.

### 12.3 Remaining host-time sources

After this change, these remain on host time in the functional profile:
- RTC;
- panel TE;
- GP timers;
- `CYCLES`;
- `QueueEvent` delays.

Firmware-level repeatability is therefore still not expected.
