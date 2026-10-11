# YAFFS provisioning tool

`provision_yaffs` is an offline tool. It places a raw YAFFS2 image produced
by `mkyaffs2image` into an erased region of an emulator NAND image. It writes
the out-of-band (OOB) data that the OP-1 filesystem layer reads. The
emulator does not use it at run time.

```sh
cmake --build build --target provision_yaffs
./build/provision_yaffs BASE.img RAW.yaffs2 NEW.img START-BLOCK TAGS-OFFSET ECC-OFFSET
```

The arguments are three paths and three decimal integers.

**Safety.** The tool never modifies `BASE.img` or `RAW.yaffs2`.
- **No overwrite:** it refuses an existing `NEW.img`, including a symbolic
  link, whether or not the link resolves. There is no overwrite option.
- **Validate first:** all inputs are checked before the output is created.
- **No partial output:** if a later step fails, the partial output is
  removed.

**Exit status:** 0 on success, 1 with a `provision_yaffs:` message
otherwise.

**Disk use:** the output is a dense 553,648,128-byte file, even from a
sparse base.

## Supported layouts

| Item | Supported | Status |
| --- | --- | --- |
| NAND image | The emulator's MT29F4G08 image (`src/peripheral/MT29F4G08.cpp`): 4096 blocks × 64 pages × 2048 data bytes, then 64 OOB bytes per page, 553,648,128 bytes in all. Interleaved 2112-byte raw dumps must be converted first. | Checked by size only |
| YAFFS input | `mkyaffs2image` output with `chunkSize 2048`, `spareSize 64`. Each 2112-byte record is a chunk plus a spare whose first 28 bytes are `struct yaffs_packed_tags2` (16 bytes of tags, 12 bytes of tag ECC) and the rest 0xFF. All-0xFF records are block padding. | Spare layout enforced. Validated with a little-endian x86-64 build of aleph-one yaffs2 at `27f18203551940abf35826a66978daf1b8124c6b`. |
| Big-endian (`convert`) output and other builders | Copied the same way, but not checked further | UNKNOWN; not validated |
| YAFFS semantics: tags, tag ECC, sequence numbers, object IDs | Not interpreted. Bytes are placed as given. | Not validated by the tool |

## What the output contains

YAFFS records go to consecutive pages starting at the first page of
`START-BLOCK`.

**Target pages:**
- Every target page must be fully erased in the base: 0xFF data and OOB.
  Otherwise nothing is written.
- There is no bad-block skipping. A factory bad-block marker in the target
  region refuses the run.
- Records must not run past the end of the device.

**For each non-padding record:**
- the 2048 data bytes;
- the 28 tag bytes at OOB `TAGS-OFFSET`;
- at OOB `ECC-OFFSET`, eight 4-byte entries, one per 256-byte sector. Each
  holds the NFC odd and even 11-bit parity values as two little-endian
  16-bit words. They are computed by `ComputeEccPair` (`src/cpu/nand_ecc.cpp`),
  the same code the emulated NFC uses.
- All other OOB bytes stay 0xFF.

Padding records leave their pages erased.

**Offsets:** `TAGS-OFFSET + 28` and `ECC-OFFSET + 32` must fit in the 64-byte
OOB, and the two ranges must not overlap.

## What has been validated for v241

| Placement | Status |
| --- | --- |
| Tags at OOB+4, ECC at OOB+32 | CONFIRMED. A checkpoint page written by v241 itself had its 28-byte packed tags at column 2052 (OOB+4) and the eight parity pairs at OOB+32. Independent calculation matched all eight sectors. The record is in the local provisioning notes, which are not yet in the repository. Boot pages use a different, packed 3-byte ECC format; this tool does not produce it. |
| Start block 132 | LIKELY: a working placement. It is not a reconstructed partition map. v241 started from it, created its tape and album files and reached its normal tape UI (`docs/nand-boot-investigation.md`, `docs/v241-nand-provenance-and-traversal.md`). |
| Any other offsets or start block | Accepted, and the byte layout is tested. Firmware acceptance is UNKNOWN. |

**Reproduction:** with the clean split NAND and the saved YAFFS
intermediate, block 132 and offsets 4/32, the tool reproduces the recorded
pristine provisioned image byte for byte (SHA-256 `08d6e6fb…c50880`). An
independent read-back matched every file and directory of the source tree:
it decoded the tags from the OOB, rebuilt the YAFFS objects and recomputed
every page's data ECC.

`mkyaffs2image` is GPL-2.0. It is built and run separately; none of its code
is part of this project.

## Tests

`provision_yaffs_tests` (Python; registered when Python 3 is found) use
synthetic data only. The base is a sparse image whose only erased pages are
blocks 2 and 3.

**Normal operation:** 66 records crossing a block boundary, two of them
padding.
- Output is checked for offsets 4/32 and 36/0: page data, tags, the rest of
  the OOB, and ECC computed independently in Python.
- Every byte outside the target pages equals the base.
- Both inputs are left unchanged.

**Malformed input and wrong dimensions:**
- argument count;
- non-decimal, negative and empty numbers;
- out-of-range block, oversized or overlapping offsets;
- missing inputs and a directory;
- bases of 0, 1000, 553,648,127 and 553,648,192 bytes;
- YAFFS images of 0, 1, 2048, 2111 and 2113 bytes;
- an image running past the end of the device;
- a non-erased target page, in data or OOB;
- spare data after the 28 tag bytes.

**Failure handling:**
- an existing output file, a dangling symbolic link and the base itself are
  refused and left untouched;
- every rejected run leaves no output.

The Python ECC is checked against the `nand_ecc_tests` vectors.

**Not tested:** removing a partial output after a write failure part-way
through, which is hard to induce portably.
