# Boot ROM image

The BF52x processor in the OP-1 has a 32 KiB on-chip boot ROM at
`0xEF000000–0xEF007FFF`. The emulator **does not contain one**: you supply an
image at run time, just as you supply the NAND and OTP images
(`README.md`, Inputs).

## What the ROM does for v241

**At reset:** execution starts at `0xEF000000`. The ROM reads the OTP,
programs the voltage regulator and PLL, and boots the OP-1 boot loader from
NAND.

**After boot:** the boot loader and the v241 firmware also call ROM entry
points. A probe of a full v241 session observed calls to `0xEF000018`,
`0xEF00001A` and `0xEF000038`, and to two further ROM routines from the boot
stage. All ROM execution is over about half a second of guest time into the
boot.

**Without a ROM image:** v241 cannot boot, and the firmware's ROM calls have
nothing to run.

## Supplying an image

The image is a raw dump of the ROM address range: exactly **32,768 bytes**,
the byte at file offset `n` being the byte at `0xEF000000 + n`.

**Obtaining one:**
- You must obtain an image you are entitled to use, for example by reading
  the ROM of your own BF52x-based hardware.
- The ROM is Analog Devices firmware. This project does not distribute it,
  and the GNU simulator's ROM headers are empty placeholders.

**Selecting it:**

| Front end | Setting |
| --- | --- |
| GUI | `OP1EMU_BOOTROM_FILE=PATH` |
| Headless runner | `--boot-rom PATH` (takes precedence over `OP1EMU_BOOTROM_FILE`) |
| Launcher | `run-op1.sh --boot-rom PATH` (or `OP1EMU_BOOTROM_FILE`) |

**Handling:** the file is only read, so it may be read-only. `run-op1.sh`
passes it by path and does not copy it.

**Validation:**
- The image must be an existing regular file of exactly 32,768 bytes, and
  fully readable.
- Anything else (missing, empty, a directory, the wrong size, unreadable)
  stops start-up with an error. The error names the setting, the path, the
  problem and the required size.

### Revisions

| Revision | SHA-256 | Behaviour |
| --- | --- | --- |
| Validated | `9fe4f939d4bf098fbfbc774e67241bd3ddfd6a410005cf40927e99cc710f871d` | Every v241 result in this repository was observed with it. It is reported at start-up as the validated revision. |
| Any other 32 KiB image | — | Accepted, with an "unvalidated revision" warning. Firmware behaviour with it is UNKNOWN. |

The validated image is a BF52x silicon-revision 0.2 ROM image. The GNU
simulator models BF522, BF524 and BF526 as sharing one ROM family, and v241
boots with it. Its fit with the OP-1's BF524 is LIKELY, not CONFIRMED.

The timing evidence depends on this exact revision:
- the strict-profile stop at `EF000C3E`;
- the PLL/IDLE investigation;
- the NAND ECC error path at `EF000748`.

To compare an image with the validated revision:

```sh
sha256sum PATH/TO/bootrom.bin
```

## Running without an image

| Situation | Behaviour |
| --- | --- |
| **Booting from NAND** (no LDR file) | Refused at start-up by the GUI and the headless runner. The GUI checks before opening a window. |
| **LDR mode and test programs** | Allowed. The ROM region is then an explicit stub, not zero-filled memory. |

**The stub:**
- **Execution:** any block that would start in the ROM range is stopped
  before it runs, with `boot ROM not loaded: execution at …`. This covers
  jumps, calls, returns and interrupt vectors, in both ordinary and
  multi-block execution.
- **Data access:** any read or write of the ROM range stops the CPU at the
  end of the current block, with `boot ROM not loaded: read from …`. The
  access itself returns 0.

## History

**Before:** earlier revisions compiled a ROM array from
`ext/bfin_sim/bf526-0.2.h` into every binary.
- **Provenance:** GDB's own copy of that header is an empty placeholder. The
  populated copy carried no copyright, licence or provenance notice.
- **Reason for removal:** its redistribution status is unknown.

**Now:** both the array and the `ext/bfin_sim` submodule are gone.

Tests use synthetic images only (`bootrom_tests`, `bootrom_cli_tests`).
