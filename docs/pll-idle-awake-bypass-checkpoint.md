# Checkpoint: awake PLL bypass passed; next stop is NFC IDLE

No further investigation was done after this note. SPORT, CoreTimer, guest `CYCLES`, DMA pacing, and the general scheduler remain disconnected.

## Git

- Checkout: `/home/chris/Projects/op1-emu-gui-test/op1emu`
- HEAD: `55335b4f225757f229aced2d688658bc72819593` (`feat: expose and test Bcore packet-entry accounting`)
- Bcore: `e1f434f46fe391f6400d0e00ec0395a631acb89d`, dirty, not committed
- Parent tree heavily dirty and unstaged. Includes earlier boot/audio/GPIO/storage work plus this PLL/IDLE work. Do not clean, reset, or sweep unrelated files into a commit.
- Nothing staged. Do not commit images, NAND/OTP copies, or logs.

## Awake-bypass experiment

Opt-in, all three required:

- `OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000` (provisional/LIKELY; not a board fact)
- `OP1EMU_SHADOW_TIMING=1`
- `OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1`

Valid only while the CPU is awake, a temporary-bypass deadline remains, and effective CCLK equals that CLKIN. Each awake `Core::run` consumes only the new `ShadowTiming::EstimatedCycles()` delta, clipped to the remaining ticks. Excess is block-level overshoot. Idle PLL deadlines do not read shadow cycles. Unknown stalls are not extra ticks. After expiry, conversion stops and reset-active CCLK stays unknown.

## Proof the 512-tick interval completed

Trial `build/pll-awake-bypass-v241.*`. First VR write/IDLE unchanged: `VR_CTL=0x70B0` at `0xEF000C3C`, IDLE `0xEF000C3E`, immediate masked-CEC wake, resume `0xEF000C40`.

Expiry log: block `0xEF005C24`, shadow delta 2, consumed 1, overshoot **1**, remaining 0, total-consumed **512**, expiries **1**, `post-cclk-known=0`. Intra-block expiry instruction is unknown. Functional execution continued. No `PLL_DIV=5` or `PLL_CTL=0x2000` write was observed. Reset-active PLL ambiguity did not stop this path.

## Exact next stop

IDLE `0xEF00754C`, resume `0xEF00754E`, packet **15813**, IDLE entry **2**. Reason: `no known eligible wake completion or deterministic deadline`. CPU left idle. CEC `IMASK/IPEND/ILAT=0x1F/0x12/0`. SIC masks 0/0, ISR 0/0, IWR all-ones. PLL readback `0x0B00/4/0x70B0`, status `0x00A2`, lock count `0x0200`; active CTL still unset. `P3=0xFFC00000`. Shadow estimate 17946, observational only. Host time to this stop was about 0.80 s; not a guest-timing result. Screenshot blank. No tape UI.

## NFC state at the stop

- Before IDLE: `W[P3+0x3744]=R7` with `R7=0xFF`, so `NFC_CMD=0xFF`.
- After IDLE, not executed: read `NFC_IRQSTAT` (`P3+0x3708`) bit 0; branch back to IDLE if clear.
- Readbacks: `NFC_CTL=0`, `NFC_STAT=0x11`, `NFC_IRQSTAT=0`, `NFC_IRQMASK=0x16`.
- Recent MMIO before the command is repeated reads of `NFC_IRQSTAT` at `0xEF007CB0`, all zero.
- Preceding unknown NAND command `0x50` at `0xEF007524` is context, not the sampled wait.

## Hypothesis

**LIKELY, not confirmed:** this IDLE waits for `NFC_IRQSTAT.NBUSYIRQ` (bit 0) after NAND reset command `0xFF`. The following instruction names that bit. `NFC_STAT.NBUSY` (bit 0 of `0x11`) already reads ready, so this is an edge latch, not the level bit. CEC delivery is not required: IMASK is the reset mask and SIC ISR is clear. `IRQMASK=0x16` does not mask bit 0, but the code polls status rather than taking the interrupt.

## Already implemented versus missing

Implemented: `NFC_CMD` forwards to the flash model. `CMD_RESET=0xFF` calls `SetBusy()`, which queues a host `1 ns` event that only clears the flash `isBusy` flag. `NFC::ProcessWithInterrupt` copies `!IsBusy()` into `notBusy` and latches `NBUSYIRQ` on a rising edge. IRQSTAT bits exist and are W1C. Page program/read completion can set other IRQ bits.

Missing for this wake: the IDLE path returns before `ProcessEvents` and `ProcessWithInterrupt`, so the queued busy-clear and `NBUSYIRQ` latch do not run. There is no documented NAND-reset deadline in CLKIN/SCLK ticks, and the `1 ns` host delay must not be reused. Command `0x50` is an explicit unknown. Do not invent a periodic wake.

## Smallest next task

Model only the NAND `0xFF` busy interval and the `NBUSYIRQ` rising edge with an evidence-backed completion, then wake this IDLE without delivering a CEC interrupt unless eligibility says so. Re-run the same v241 observation only far enough to see whether `PLL_DIV=5 -> PLL_CTL=0x2000 -> IDLE` is reached. Do not connect SPORT.

## Reproduce

Use a new unused prefix. GUI trial needs X11/Xwayland. Nine selected tests passed before the trial, including `pll_idle_tests` and `execution_accounting_tests`.

```bash
cmake --build build --target op1emu pll_idle_tests execution_accounting_tests sport_audio_tests usb_reset_tests mcp23017_tests pcm_sample_tests audio_ring_tests nand_ecc_tests -j2
ctest --test-dir build -R '^(pll_idle_tests|execution_accounting_tests|sport_audio_tests|audio_ring_tests|pcm_sample_tests|mcp23017_tests|nand_ecc_tests|otp_import_tests|usb_reset_tests)$' --output-on-failure
OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000 OP1EMU_SHADOW_TIMING=1 OP1EMU_TRACE_CLOCK=1 OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1 \
  python3 /tmp/opencode/boot_trials.py NEW-UNUSED-PREFIX 10 420
```

## Preserve

- `/tmp/opencode/boot_trials.py` (whitelist includes the three PLL/shadow opt-ins; depends on untracked `tools/check_gui_controls.py`)
- Trial evidence: `build/pll-awake-bypass-v241.*` and prior `build/pll-idle-foundation-v241.*` (logs, screenshots, private NAND/OTP). Do not commit.
- Docs: `docs/bf524-pll-idle-foundation.md`, `docs/op1-clock-readiness.md`, this file.
- Local PDFs/text under `/tmp/opencode/`; hashes in the readiness doc.
- Unrelated dirty work and `src/cpu/timing_probe.h`.
