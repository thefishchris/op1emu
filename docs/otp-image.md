# OTP image selection

The BF524 one-time-programmable (OTP) memory is modelled by `src/cpu/otp.cpp`
and backed by a host file. v241 needs OP-1-specific OTP provisioning to leave
the recovery path (`docs/nand-boot-investigation.md`), so the backing file is
a per-device input like the NAND image. OTP contents are device data: keep
them local and never commit them.

## File format

A raw image of exactly 8,192 bytes: 512 pages of 16 bytes, page `p` at offset
`p × 16`. Each page is loaded as four 32-bit words in host byte order, so on
the supported little-endian hosts the file is little-endian. The image has no
header or checksum. The emulator therefore validates only its type and size,
not its contents.

| Item | Status |
| --- | --- |
| 512 × 128-bit pages, 8 KiB in total | Emulator layout. LIKELY matches the BF52x OTP organisation. |
| Pages `0x0E0–0x0FF` and `0x1E0–0x1FF` hold a 2-byte ECC entry for each page of their half | Emulator encoding, `OTP::CalculateEcc`. Its match to real silicon is ASSUMED. |
| Loading verifies ECC | No. Whether real dumps carry the same ECC encoding is UNKNOWN. |

## Selecting an image: `OP1EMU_OTP_FILE`

**Unset** keeps the historical default, which is unchanged: the emulator opens
`otp.bin` in its working directory.
- If `otp.bin` cannot be opened, an erased one is created. It gets a
  synthesized chip ID, which is derived from a host pointer and therefore not
  reproducible, and the `ADSP-BF524` part string.
- The default path does not validate the file. A short `otp.bin` is loaded
  partially, and a file that cannot be opened leaves an all-zero OTP after an
  error log. Use `OP1EMU_OTP_FILE` when the input matters.

**Set** selects an existing image explicitly. The file must:
- be a regular file (symbolic links to one are followed);
- be exactly 8,192 bytes;
- be readable.

**It is never created, truncated or resized.** Anything else stops CPU
construction with an error that names the variable, the path, the problem and
the required size. For example:

```
OP1EMU_OTP_FILE: OTP image 'x.otp' is 8191 bytes, so it is truncated or not
an OTP image; expected an existing raw OTP image of exactly 8192 bytes
(512 pages x 16 bytes, see docs/otp-image.md); unset OP1EMU_OTP_FILE to use
./otp.bin
```

The GUI logs this message and exits with status 1, as does
`op1emu-headless` (prefixed `Headless:`). An empty value is rejected rather
than treated as unset.

**Writable images** receive guest OTP programming page by page, as the
default `otp.bin` does.

**Read-only images** (for example `chmod 0444`) are loaded. Guest OTP writes
then stay in emulator memory for the session and are discarded on exit. The
first such write is logged. This lets a frozen fixture be used without risk of
modification.

`run-op1.sh` copies the OTP given with `--otp` into a fresh run directory and
points `OP1EMU_OTP_FILE` at the copy, so the source image is never modified.

**Tests:** `otp_storage_tests` cover, with synthetic images only:
- a valid image loads unchanged through the guest-visible registers;
- a guest write reaches only its page;
- a read-only image keeps writes in memory and is never modified;
- these are rejected without being created or changed: a missing path, an
  empty path, a directory, and images of 0, 16, 8191, 8193 and 16384 bytes;
- the default still creates an erased 8,192-byte image;
- the CPU honours, rejects and ignores the variable correctly;
- `OTP::CalculateEcc` matches the importer's ECC vectors.

## Making an image from a textual dump

`tools/import_otp_dump.py` converts a sparse textual OTP listing into an
image. The listing is in the format of `TE-OP-1-BF524-OTP-dump.txt` in the
public Tolsi/op1dumps repository. The source, the base and the output are
all device data; keep them local.

```sh
python3 tools/import_otp_dump.py DUMP.txt NEW.otp --base BASE.otp \
    [--override-page PAGE:LOW64:HIGH64 ...]
```

**Input lines:**
- `0xPAGE LOW64 HIGH64`: exactly 16 hex digits per word. The first word
  becomes bytes 0–7 of the page and the second bytes 8–15, each
  little-endian. This word order is LIKELY: it follows the source's
  description, and v241 accepted the result.
- `0xPAGE 00`: an all-zero page.
- A leading `!` is accepted and ignored. Its meaning in the source is
  UNKNOWN.
- `...` lines and blank lines are skipped.

**Rejected:** any other line, duplicate pages, pages ≥ `0x200`, entries on
the ECC storage pages, and text over 64 KiB. The error gives the line
number. Nothing is silently corrected.

**Overrides:** `--override-page` replaces a malformed source entry
explicitly. It may only name a page that the source lists.

**Base and output:**
- `--base` must be an existing 8,192-byte image. Every page the dump does not
  list is copied from it unchanged. Each imported page gets its ECC entry in
  the emulator's encoding (see the table above); the importer mirrors
  `OTP::CalculateEcc`, and both tests check the same vectors.
- The output must not exist yet. A failed import leaves no output, and the
  source and base are never modified.

The result is only as good as the dump: full real-chip OTP behaviour is not
implied.

**Tests:** `otp_import_tests` cover, with synthetic data only:
- parsing, malformed and truncated lines, overrides and ECC placement;
- the command line: a valid import, a refused existing output, missing
  inputs, truncated and oversized bases, cut-off and non-UTF-8 dumps, and
  malformed overrides.
