# Local NAND boot investigation

## Inputs and provenance

Source: <https://github.com/Tolsi/op1dumps>.
The source README describes the flash dump as boot v2.27 with formatted
firmware v2.41. It describes `te-boot-otp-ops-2.28-10.ldr` as an
OTP-repair-patched bootloader, not an ordinary unmodified firmware loader.

No dump contents or generated images belong in source control.

## Findings

- **CONFIRMED:** the extracted image is 553,648,128 bytes. The source's
  `write_data_without_ecc.py` and `check_fw.py` use 4,096 blocks of 64 pages,
  with 2,048 data bytes and 64 spare/OOB bytes interleaved per page.
- **CONFIRMED:** `MT29F4G08::LoadPage` expects a different host-file layout:
  all 536,870,912 data bytes first, then all 16,777,216 OOB bytes.
  Merely renaming the extracted dump to `.img` does not adapt its layout.
- **CONFIRMED:** a bounded header walk over the first 2 MiB of the original
  image finds only three valid BF52x records under contiguous interpretation,
  but 111 records through the first FINAL after removing interleaved OOB.
  Signature, header XOR, FILL payload handling, and length bounds were checked.
  This is structural evidence, not execution evidence.
- **CONFIRMED:** a converted diagnostic image was created at
  `build/nand-split-diagnostic.img`. Re-interleaving its data and OOB reproduced
  every byte of the original image, verified block by block and with SHA-256.
  The extracted original and existing `build/nandflash.img` were not replaced.
- **CONFIRMED:** in NAND-only execution, the ROM issues reset `0xFF`, then
  unsupported `0x50` at the CPU PC snapshot `0xEF007524`, then four zero address
  bytes. It subsequently resets again, reads the Micron ID, and performs a
  large-page `0x00` / five-address-byte / `0x30` read. The warning is not the
  immediate point where execution stops.
- **CONFIRMED:** the unconverted image reaches a self-loop at `0xEF00074E`
  with `R0=2`, `R1=0xFF907F00`, and `RETS=0xEF0079EC` after two 256-byte
  page-data transfers. Its page-zero OOB is read from the erased tail region.
- **CONFIRMED:** the converted image transfers four 256-byte sectors before
  reaching the same self-loop and register state. The nearby ROM instructions
  set `CC` with `R0 == R0`, then branch to themselves. The caller invokes
  `0xEF007B28`, tests its return against zero, and calls `0xEF000716` on the
  nonzero path.
- **LIKELY:** this is a boot-error path, rather than an MMIO polling loop.
  The precise meaning of return/error value 2 remains **UNKNOWN**.
- **CONFIRMED:** independent parity calculation matches the stored ECC of all
  eight sectors in original page zero. The expected register pairs for its
  first four sectors are `5D6/229`, `778/778`, `7CC/7CC`, `222/222`.
  Runtime NFC trace agrees for the first three, but reports `220/220` for the
  fourth. Page-zero data and OOB in the converted file remain unchanged after
  the run.
- **CONFIRMED:** runtime DMA sector hashes match the source sectors. Isolating
  `ComputeEccPair` reproduces the incorrect parity at both `-O0` and `-O3`.
  The defect was `else p2 ^= bit` instead of `else p2p ^= bit`: the even P2
  complement was accumulated into the odd accumulator. This is a host NFC
  implementation bug, not evidence of a bad dump or a JIT defect.
- **CONFIRMED:** correcting the accumulator makes all eight page-zero runtime
  ECC pairs match the stored ECC. With the layout-converted image, execution
  escapes the ROM error loop and reaches loaded code: sample PCs include
  `0xFFA005C2`, `0xFFA001D2`, `0xFFA06DB0`, `0xFFA04CF4`, and `0x0000BAB2`,
  with changing registers and additional NAND/DMA transfers. This establishes
  progress beyond ROM boot, not a fully working OP-1 UI or complete firmware
  boot. The `0x50` warning still occurs without preventing this progress.

CPU PC snapshots during NAND callbacks may refer to the current translated
basic block rather than the exact instruction issuing the access. Subsequent
OTP/content provisioning produced the normal v241 tape UI; see
`docs/local-working-setup.md`. Complete CPU/device correctness is not established.

## Reset and page-read completion

The later PLL/IDLE investigation exposed two NAND busy-to-ready waits that the
previous host-time event handling could not complete while the CPU was idle.
The model now handles only the observed reset and page-read operations at an
explicit NAND operation boundary:

- **CONFIRMED:** the Micron PAGE READ sequence is command `0x00`, five address
  cycles, then command `0x30`. The device drives R/B# low during the array
  transfer and high when data is available.
- **CONFIRMED:** BF52x `NFC_STAT.NBUSY` reflects the synchronized `ND_RB`
  input. `NFC_IRQSTAT.NBUSYIRQ` latches on a rising `ND_RB` edge and is W1C.
- **CONFIRMED:** SIC source 48 can make the observed IDLE wake eligible even
  when the CEC state does not permit delivery of that interrupt.
- **CONFIRMED:** reset command `0xFF` now enters busy. One modeled operation
  boundary completes the pending reset, transitions busy to ready, and latches
  one `NBUSYIRQ`. It does not relatch without another operation.
- **CONFIRMED:** a valid `0x00` plus five-address-byte plus `0x30` sequence
  captures the page and column, enters busy, and withholds page data. One
  modeled operation boundary loads the page, exposes data at the captured
  column, transitions busy to ready, and latches one `NBUSYIRQ`.
- **UNKNOWN:** the exact reset (`tRST`) and array-read (`tR`) durations in the
  target system. The supplied Micron datasheet specifies a maximum `tR` of
  25 us with internal ECC disabled; whether internal ECC is active on the
  physical OP-1 device is also **UNKNOWN**. The deterministic operation
  boundary is an emulator synchronization point, not a claimed hardware
  duration.

The production-model regression uses sparse page/OOB storage and verifies data
is unavailable before completion, the expected OOB bytes appear at column
2048 afterward, SIC source 48 is asserted with unchanged CEC eligibility,
`NBUSYIRQ` is W1C, reset completion remains independent, and no completion is
repeated without a new NAND operation. Program and erase timing behavior was
not changed by this work.

In the first controlled v241 trial, the ROM issued `NFC_CMD=0x00` at
`0xEF007D00`, addresses `00 08 00 00 00` (page 0, column 2048), and
`NFC_CMD=0x30` at `0xEF007D1A`. IDLE `0xEF007D1E` woke at packet 16374 with
SIC1 bit 16 asserted and unchanged CEC state. A subsequent page-0/column-0 read
also woke at IDLE `0xEF007C4A`. That run stopped at IDLE `0xEF007C62`, after
`NFC_PGCTL.PG_RD_START`, with DMA channel 2 running.

## NFC DMA page transfer

The ROM configures the DMA work unit before the page-0/column-0 NAND read:

| DMA2 register | Observed value | Meaning |
| --- | ---: | --- |
| `START_ADDR` | `0xFF907F00` | Destination in guest Data B SRAM |
| `CONFIG` | `0x0087` | enabled, peripheral-to-memory, 16-bit, 1-D, continuous transition, completion interrupt, stop flow |
| `X_COUNT` | `0x0080` | 128 elements |
| `X_MODIFY` | `0x0002` | contiguous 16-bit destinations |
| `Y_COUNT`, `Y_MODIFY` | `0` | inactive because `DMA2D=0` |
| `PERIPHERAL_MAP` | `0x2000` | PMAP 2, NFC on DMA2 |

Immediately before the unsupported IDLE, the model reported
`CURR_ADDR=0xFF907F00`, `CURR_X_COUNT=0x80`, and `IRQ_STATUS=0x08`: running,
not done, and no element transferred. The exact ROM sequence is:

- `0xEF007C12`: disable DMA2 with `CONFIG=0`.
- `0xEF007C14`: W1C `DMA_DONE` through `IRQ_STATUS=1`.
- `0xEF007C1A`: enable DMA2 with `CONFIG=0x87`.
- `0xEF007C2C` through `0xEF007C46`: issue the page-0/column-0 NAND read and
  confirm command.
- `0xEF007C4A`: wait for and acknowledge NFC `NBUSYIRQ`.
- `0xEF007C5E`: write `NFC_PGCTL=1` (`PG_RD_START`).
- `0xEF007C62`: IDLE.
- `0xEF007C64` through `0xEF007C70`: poll DMA2 `DMA_DONE`, loop to IDLE while
  clear, then W1C the latched completion.

- **CONFIRMED:** BF52x page-read procedure configures an NFC receive DMA,
  waits for the NAND data-ready edge, sets `PG_RD_START`, and handles DMA
  completion before reading spare bytes.
- **CONFIRMED:** `CONFIG=0x87` requests 128 16-bit NFC-to-memory elements, or
  exactly 256 bytes at contiguous addresses `0xFF907F00` through
  `0xFF907FFF`. This is not a memory-to-NFC transfer.
- **CONFIRMED:** BF52x decrements `CURR_X_COUNT` per transferred element and
  asserts `DMA_DONE` after the last memory write. DMA2 completion is SIC source
  30, default IVG11; SIC wake enable is independent of SIC/CEC interrupt masks.
- **CONFIRMED:** the emulator now refuses NFC DMA data before `PG_RD_START`.
  At an architectural IDLE device boundary, only the observed DMA2 receive
  shape consumes the already-ready 256 bytes, advances `CURR_ADDR` to
  `0xFF908000`, makes `CURR_X_COUNT=0`, sets `DMA_DONE` once, and asserts SIC
  source 30. W1C clears both completion status and the source.
- **CONFIRMED:** no transfer rate is needed for this boundary. NAND data is
  already available, `PG_RD_START` supplies the peripheral request, and the
  register configuration specifies a finite work unit. The operation boundary
  is deterministic and does not claim a hardware duration or bandwidth.
- **UNKNOWN:** cycle-accurate DAB arbitration, NFC request cadence, DMA FIFO
  behavior, and transfer latency. They are not modeled by this narrow path.

The production regression verifies the exact configuration, source bytes,
destination range, width/count/modifier semantics, pre-handshake blocking,
current registers after completion, `DMA_DONE`, SIC source 30, W1C behavior,
CEC-independent IDLE wake, and no duplicate transfer.

In `build/dma2-nfc-completion-v241.log`, the first DMA wait at `0xEF007C62`
woke at packet 17092 with SIC0 bit 30 asserted and CEC still `0x1F/0x12/0`.
The extended `build/dma2-nfc-followup-v241.log` reached `PLL_DIV=5` at
`0xEF000C76`, `PLL_CTL=0x2000` at `0xEF000C78`, and IDLE `0xEF000C7A` at
packet 29255174. The modeled 512-CLKIN lock boundary then woke it with active
CCLK 400 MHz and SCLK 80 MHz under the provisional 25 MHz CLKIN experiment.
No next unsupported boundary was established because the requested PLL/IDLE
terminal condition had been reached. Logs, screenshots, and private NAND/OTP
inputs must not be committed.

## Diagnostics

Tracing is opt-in:

```sh
OP1EMU_TRACE_CPU=1 OP1EMU_TRACE_NAND=1 \
  ./build/op1emu ./build/nand-split-diagnostic.img
```

`OP1EMU_TRACE_CPU=1` logs the first 64 basic-block entries and powers of two
through 2^32, with registers and ROM-only disassembly at later samples.
`OP1EMU_TRACE_NAND=1` logs at most 256 flash events and 64 NFC DMA summaries.
The latter include requested/actual transfer sizes, ECC reset/count, and the
four ECC registers and a hash of each DMA buffer. Default execution has no
diagnostic trace.

Local captures are `build/boot-nand-trace.log` (unconverted) and
`build/boot-split-trace.log` (converted, before ECC fix), and
`build/boot-ecc-fixed-trace.log` (converted, after fix). Runs were bounded with `timeout` on
the existing desktop display. An Xvfb attempt segfaulted before startup;
its cause was not investigated.

## Verification and next step

The user subsequently clarified that the visible screen is te-boot/recovery,
not the normal OP-1 OS. **CONFIRMED:** bootloader/display startup works;
normal firmware boot remains unverified. Inspection of local `otp.bin` found
only pages `0x04`, `0x07`, and `0xE0` nonzero. The OP-1 boot-related pages
listed by Tolsi (`0x10`-`0x12`, `0xD0`-`0xD3`, `0xD8`, `0xDF`) are all zero.
**LIKELY:** missing OP-1-specific OTP provisioning contributes to recovery-only
startup. The guest's decision path must be traced before treating this as the
confirmed cause. No OTP data was changed during this inspection.

The GUI executable rebuilt successfully and `git diff --check` passed.
`nand_ecc_tests` is now a firmware-free top-level CTest test. It covers zero
and erased sectors, complementary bit parity, the P2 regression, an empty
transfer, and chunked ECC accumulation. The pure parity helper is in
`src/cpu/nand_ecc.cpp`, used by both the NFC and the test executable.
The separate `op-1_emu`
repository passed all five parser/CLI/local-firmware tests; those do not
validate this GUI's CPU, NAND, DMA, or ECC behavior.

Follow-up completed: a separate imported OTP image enabled the normal firmware
path; the missing stock v241 content filesystem was then provisioned with
guest-verified filesystem ECC layout. Initialization completed into the normal
tape UI and the user confirmed successful boot. Reproduction and remaining
limitations are in `docs/local-working-setup.md`. No guessed `0x50` handler or
forced escape from the ROM error loop was added.

Sources:

- <https://github.com/Tolsi/op1dumps/blob/master/README.md>
- <https://github.com/Tolsi/op1dumps/blob/master/write_data_without_ecc.py>
- <https://github.com/Tolsi/op1dumps/blob/master/check_fw.py>
- <https://github.com/Tolsi/op1dumps/blob/master/ADSP_ecc.py>
