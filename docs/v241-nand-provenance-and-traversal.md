# v241 immutable inputs and loaded-code traversal (2026-10-08)

## Result

**Latest continuation:** the narrow observed 2D MDMA0 extension now clears the
following `018D4736` boundary by completing before its pre-IDLE DONE poll.
A fresh clone of the same frozen inputs displays the normal tape UI. The CPU
then stops at `019F18F4`, with the byte at `FF907DF6` still 1; its producer/wake
condition is **UNKNOWN**. See the finite 2D MDMA section of
`pll-idle-awake-bypass-checkpoint.md`. No further device/timing work followed.

**Current fix:** completed NAND Random Data Read availability is now modeled
and regressed. Fresh frozen-image trials transfer correct OOB ECC, preserve
entry 113 as `0072`, and advance `113 -> 114`. The walker returns normally;
the first new boundary is a two-dimensional MDMA0 completion wait at IDLE
`018D4736`. Tape UI is not yet observed. See the fix-verification section below.

**Construction follow-up (pre-fix):** root cause **D — emulator data-path corruption**
is now established for this current-image cycle. NAND contains FAT entry 113
as `72 00`; a blocked Random Data Read of OOB leaves stale ECC comparison data,
and guest instruction `FFA07CE2` flips the high byte to `80`. See the construction
follow-up below. No fix was implemented. Fresh pristine clones stop earlier at
a guest database assertion, before constructing this FAT table.

**CONFIRMED before the fix:** the initialized NAND reproduced a software cycle, not
the previously inferred finite walk to `0xFFFF`. Starting at index 3, the guest
executes `3 -> 4 -> ... -> 113 -> 32882 (0x8072) -> 0 -> 0`. Two fresh
identically instrumented processes reproduced all 113 lookups through the first
self-cycle. Stop condition 4 (the finite-traversal premise is false) was reached.
The display retained **R.00241**, not tape UI. No new unsupported IDLE/hardware
boundary was observed. No hardware behavior, guest code, or guest table was changed.

Investigation baseline: parent `44925ea`, branch `wip/pll-idle-timing`, Bcore
`37de8e8` on `wip/op1-timing`. The existing dirty production tree/build was used;
this is not a claim about an otherwise clean checkout. A separate diagnostic
executable reused the existing libraries and GUI objects. Production sources,
the original executable, and existing NAND/OTP contents were preserved.

## SHA-256 input identities

Paths below are relative to the repository unless marked `/tmp/opencode/`.
Every split NAND is 553,648,128 bytes; OTP binaries are 8,192 bytes.

| Input/artifact | SHA-256 |
| --- | --- |
| `build/nand-working241.img` | `c94057cb8000aa9516242c426eeb5c69df03300fd9c4e0f19893c6ef374d5a42` |
| `build/otp-tolsi.bin` | `06c993496c3aba43ca345c62a2caa27b69d68e160cad8c29196152991a238c5a` |
| `TE-OP-1-flash-dump-fw241-boot-ok-formated-fw-MT29F4G08ABBDA@BGA63_2131.BIN.zip` | `b36a6d29359ed1c0dda2385da5ac8f18c0e0b5816909583777aa459ecc0a0f2c` |
| ZIP member `MT29F4G08ABBDA@BGA63_2131_without_3627_BB.BIN`; also `build/nandflash.img`, `build/nand-diagnostic.img` | `154b591055ffb60405db62d635277a720c4366e102ed5d6518a48a32f59584c2` |
| `build/nand-clean-split.img` | `45b56ebeeefd9baf763994a9e5ae0b51ba35dbbfaeeb593e1c170c26c43627b5` |
| `build/nand-split-diagnostic.img`, `build/nand-before-reset.img` | `85842d77c8e57174f1c3e048d8c187cfce2d7feacc814028cef8a2f7060d272e` |
| `build/nand-content241-experiment.img` | `f91d9462bac845689be4bebb39adbb12be158cd74b490760d72940c8d48a0567` |
| `build/nand-content241-register-ecc.img` (already run/mutated) | `0dcc07e4cb389b8e3f2cd3b8c4c4b030635c76f16dbd086a82649e8b5fb37c8b` |
| `build/op1_241.op1` | `bf1e0a638c0dda489828f466877498545cf38542e78b43f9a9979895ca48d3ab` |
| `build/content241-yaffs.img` | `519b50b3647dc7c34d299074b8a40028d157b3108e5088f5795e76de1c6adf13` |
| `build/tolsi-otp-source.txt` | `34736c84247500db30862037739791eb48c69a3d83d08a5643d671d9257ac082` |
| `build/otp-before-provisioning.bin` | `860d63589f99351690047e8750ce7f1f3180831440dc5add2b18d4100dc6fdb7` |
| `build/otp.bin` (generic, not selected by these trials) | `cc5cc00c8026bf734acdd328e8fbd03c4ee6516a6646e7bc3a2eba46b5bc1bc9` |
| `build/provision_yaffs` | `10bdfb1f1c99449720bec655f498894de2c88444552104a73c6862a8394e880e` |
| `build/yaffs2-tools/utils/mkyaffs2image` | `76c880db45705b32bc49f38bedc847cf1f1b35771396fb8b3796ed47a8ee9d43` |
| `tools/provision_yaffs.cpp` | `0403ad31b2893eeda49694ca4822db7e3528aeee3dcd8677fd3d7eba485a910a` |
| `src/cpu/nand_ecc.cpp` | `3246686ec069e12414f228253dfba1d6e790dc527a2afb37630c3821fb7845dd` |
| `src/cpu/nand_ecc.h` | `049deb281d511918d97d72132ddca408224c8ca78f87028e8a3a693c517ea4c6` |
| `tools/import_otp_dump.py` | `d2ce3540d230233e9d5d7e6d6acfb99fe0aa64173d5f1218c6c51edc0b19249c` |

Content-tree manifest identity: sort regular files by relative pathname, concatenate
`<file SHA-256><two spaces><relative path><newline>` as UTF-8, then SHA-256 that text.
`build/firmware241/`: 107 regular files, manifest hash
`b61a3d6c435bb67081a23c2f29607b288b8f37bb078f4425f8e6689bafb84633`.
`build/yaffs-root241/`: 105 regular files, manifest hash
`360312966d17bd47fc8702aaddc14fd9ec4972a567a1006277bd305b2975cb11`.
These counts exclude archive directory members. Per-file hashes and mtimes are
retained locally in `/tmp/opencode/provenance-inventory.json`.

### Surviving trial clones

All surviving `build/*.otp.bin` trial copies have the same hash as `otp-tolsi.bin`.
NAND hashes below are **post-run** identities, not automatically launch identities.

The following `.nand.img` prefixes all hash to the current base
`c94057cb8000aa9516242c426eeb5c69df03300fd9c4e0f19893c6ef374d5a42`:

- `pll-idle-foundation-v241`, `pll-awake-bypass-v241`
- `nfc-reset-wake-v241`, `nfc-page-read-wake-v241`
- `dma2-reconstruct-v241`, `dma2-nfc-completion-v241`, `dma2-nfc-followup-v241`
- `post-pll-boundary-v241`, `post-pll-nfc-rdrdy-v241`
- `post-nfc-pio-handshake-v241`, `post-nfc-pio-handshake-no-count-v241`
- `post-nfc-pio-mdma-v241`, `post-nfc-pio-mdma2-v241`, `post-nfc-pio-mdma3-v241`

These prefixes hash to the common longer-trial final state
`8fab436075e2f2dea24d748ee9b56befc7a4aeef563d2b650cf08de635f75597`:
`post-nfc-pio-mdma-long-v241`, `post-nfc-pio-mdma-ui-v241`,
`post-nfc-helper-context-v241`, `post-nfc-helper-object-v241`, and
`post-nfc-mdma-65535-ab-v241`.

| Other surviving `.nand.img` prefix | SHA-256 |
| --- | --- |
| `post-nfc-pio-mdma4-v241` | `1713c96821103349626339e8085f531a9f4bcf896326d024dafd636e15387bc9` |
| `controls-baseline` | `a8b82c21a09b87672c9fce06b963c776bc4e8e39cece1b6694dae16cf9464d58` |
| `controls-corrected`, `controls-mcp18-fullrate` | `743768a751379ba6d732ef1bb63c44feb81d60e5cd322842f1ad670720d83300` |
| `controls-paced` | `e6b7b39b0adb78022177e02586f9fe4e2397640b4198f7b8b20f19744133e87b` |
| `controls-final` | `45a85acf4f72b754dbbb61179b7e96096d0656353d966fc2fb4efd40e84f536d` |
| `timing-existing-d10` | `929ac73d96d411c82a81f8dd32ba264b6847c09b66cfb044dddbc6d18936d4dc` |
| `timing-existing-d1`, `timing-probe-d1`, `timing-counterfactual-d1-timer1` | `4156245fa40bce71a0796f8cb8b74f6aa4060082033ae909daf1f3a0600f9fb2` |
| `timing-probe-d10` | `317c5314c0f887398465aabc69677112a66e0a26cc391c30dc641361ab9aafd3` |
| `shadow-coverage-v241` | `0cb0ac055b7d5247af7c41d6af53a679029d0290d020dacc63b0d4c7968006e9` |
| `shadow-coverage-final-v241` | `880fa42b956f669482ec34be02bacf27454eecb8b8b2cc3dcc7ab01b56d6ffaa` |
| `idle-clock-followup-v241` | `7db0e51ddb16e441977426d5ae32a72009e0897e84f1b5cf5eb1c931d2d71981` |

**CONFIRMED:** no `boot-content241-ui-check` NAND/OTP clone survives in the
inventoried build artifacts; its logs/screenshots do. The pre-mutation control
clones are different initialized states. None can be proven byte-identical to
the old successful tape-UI input/output: that run recorded no input hash.

**LIKELY:** the recent boot-trial prefixes used the same post-mutation base.
`/tmp/opencode/boot_trials.py` exclusively cloned `nand-working241.img` and
`otp-tolsi.bin`, and their creation chronology is after the base's last content
mtime, `2026-10-08T01:21:17.736292Z` (October 7 21:21 local). Historical launch
hashes were not recorded. Equal current hashes prove surviving content identity,
not every historical launch state. The new trials below establish launch identity
directly, and reproduce the older long-trial final NAND hash.

## Reconstruction and mutability

**CONFIRMED:** streaming the ZIP donor as 262,144 records of 2,112 bytes and
concatenating all 2,048-byte data portions followed by all 64-byte OOB portions
produces exactly the clean split hash above. The independently computed donor
hash equals the surviving interleaved `nandflash.img`. The older split diagnostic
hash differs from the fresh clean conversion and must not substitute for it.

**CONFIRMED:** pristine populated v241 NAND can be regenerated from the preserved
clean split and exact saved YAFFS intermediate. This does not regenerate the
later initialized working state. Two exclusive new outputs produced the same hash:

```sh
build/provision_yaffs build/nand-clean-split.img build/content241-yaffs.img \
  /tmp/opencode/nand-pristine-provisioned.img 132 4 32
# Repeat using a different, nonexistent output name.
```

Output SHA-256:
`08d6e6fbee173d83e71627597942e90524eca8ee4ebdbf4d325119af75c50880`.
Both new outputs remained unrun. No existing image was overwritten.

OTP import was repeated with an exclusive new output:

```sh
python3 tools/import_otp_dump.py build/tolsi-otp-source.txt \
  /tmp/opencode/otp-reconstructed.bin --base build/otp-before-provisioning.bin \
  --override-page 0xdf:204547414e454554:5245454e49474e45
```

The output matched `otp-tolsi.bin` exactly. The explicit malformed-source page
correction remains **LIKELY**, as documented in `local-working-setup.md`.

Two fresh `mkyaffs2image` invocations from the current staging tree produced
`e21317115d15d58d5b08ab503b583d1b2461f3866e602f1526eb039875c409e9`,
identical to each other but different from the saved intermediate. All 345
differing bytes were within object-header `yst_atime` fields at record offsets
280..283. The builder copies `stat.st_atime` (`utils/mkyaffs2image.c:313`).
Content hashes alone therefore do not guarantee a historical YAFFS image hash;
filesystem metadata must also be frozen, or the saved intermediate retained.
The original firmware extraction command/parser revision remains **UNKNOWN**.

**CONFIRMED:** the current base includes initialized/persisted guest state;
it is not a canonical pristine fixture. Relative to the newly regenerated
unrun populated NAND, it differs on 130,709 data pages (266,391,142 data bytes,
7,628,153 OOB bytes). The retained `nand-content241-register-ecc.img` also differs
on 130,508 data pages. Existing documentation records firmware-created tape/album
files; NAND page program/block erase persist to the selected file. The exact
history of every changed page is **UNKNOWN**. Recreating the current base hash
from sources plus guest initialization is not presently documented/deterministic.

## Immutable-base trials and probe

New exclusive reflink snapshots `/tmp/opencode/v241-frozen.nand.img` and
`v241-frozen.otp.bin` were hashed and set mode `0444`. They were never passed to
the emulator; each process used a new writable clone. Snapshot hashes equal the
current input identities above and were rechecked after each run. This is an
operationally frozen snapshot, not a claim of filesystem immutable attributes.

Temporary external files: `traversal_probe.cpp`, `build_probe.py`,
`traversal_trials.py`, `check_reconstruction.py`, and `provenance_inventory.py`
under `/tmp/opencode/`. A derived CPU wrapper observes existing state between
normal `Run()` calls, uses Bcore's disassembler, and snapshots ordinary SDRAM.
It writes no guest register/memory/MMIO. On a proven repeated next-index it
stops the host CPU loop before another guest iteration. No production source
was edited. The original GUI main, audio, USB, and device behavior were reused.
The diagnostic overhead is not a guest-timing measurement.

Environment: GPIO model 17, audio divisor 10, `OP1EMU_TRACE_CPU=1`,
`OP1EMU_EXPERIMENTAL_CLKIN_HZ=25000000`, `OP1EMU_SHADOW_TIMING=1`,
`OP1EMU_EXPERIMENTAL_PLL_AWAKE_BYPASS=1`, private `OP1EMU_OTP_FILE`, and
`TRAVERSAL_PREFIX`. No guest-driving sleep or timing constant was added.
The runner's process-wait timeout and screenshot checkpoints only observe/bound
the host process. It strips inherited `OP1EMU_*` settings before setting these.

| Trial | Probe binary SHA-256 | Stop evidence / host metadata |
| --- | --- | --- |
| `traversal-a` | `a7399055fca344cd8b0c8b6c601da0be3802306727f045400035c44ed3c80af5` | Initial coarse probe; 146.473 seconds total; repeated zero returns after the initial prefix |
| `traversal-b` | `3e87399b5fc9028169adeb0552ec58c5127c52cd04708bb354272f6ad25cb982` | Lookup 113; PC `0190A226`; packets 275,248,452; cycle at 110.791527 seconds; total process lifetime 125.753 seconds |
| `traversal-c` | same as B | Lookup 113; PC `0190A226`; packets 275,220,815; cycle at 111.429556 seconds; total process lifetime 125.532 seconds |

B and C used exactly the same executable and immutable launch hashes.
Every index transition and returned value matched. Their full table, remapping
arrays, and disassembly snapshots matched byte-for-byte. Packet counts differ
and are not treated as deterministic timing. CPU execution stopped at cycle
confirmation; the remaining host lifetime was screenshot capture/runner shutdown.
All three trial NANDs ended at `8fab4360...f75597` (full hash above), and OTPs
remained identical to the frozen input. Existing NAND/OTP contents were not changed.

Evidence retained locally as `/tmp/opencode/traversal-{a,b,c}.*`: exclusive
launch/result JSON manifests, logs, private NAND/OTP clones, code/table dumps,
and screenshots. C's `traversal-c-0124.png` visibly retains **R.00241**.

| Matching B/C SDRAM artifact | Bytes | SHA-256 |
| --- | --- | --- |
| `table.bin`, address `01082B98` | 131,072 | `53e036e70004a6c0fb504a92ebcce2d5292399c3a9f9c6580d4d8aecc1b8092a` |
| `map50.bin`, address `01022B88` | 262,144 | `0883a965ba3f3e24126d3057494fc54151069c2f46880175513de40c5fdfd30b` |
| `map54.bin`, address `01052B90` | 262,144 | `fc17a8d2407e9dd6c9e8b1d0d709052d0840ae3a5c23dad91577a39629376318` |

Dump lengths include the complete 16-bit index domain; they do not assert the
original allocations' sizes. Reachable indices used here remain within the
observed arrays. A's table snapshot also matches B/C.

## Function and traversal semantics (pre-fix)

The three originally quoted addresses are **return sites**, not function starts:

| Return site | Containing function, inclusive entry through RTS | Role |
| --- | --- | --- |
| `0190AF00` | `0190AC82..0190AFE4` | Directory traversal and callback invocation at `0190AEFE` |
| `0190A306` | `0190A2E8..0190A31C` | Callback calls chain walker at `0190A302`, accumulates returned count into object +`0x7C` |
| `0190A226` | `0190A202..0190A24A` | Chain walker calls next-index helper at `0190A222` |

Next-index helper: `0190A024..0190A124`. Observed object `00024544` has
format +`0x4C` = 1, bound +`0x40` = `0xFFFF`, table pointer +`0x58` =
`01082B98`, and forward/reverse mapping pointers +`0x50` / +`0x54` as above.
At this stage it was **LIKELY** to be a FAT16 cluster-chain traversal inside directory scanning:
the helper implements 12/16/32-bit formats, directory entries have 32-byte stride,
deleted marker `0xE5`, long-name attribute `0x0F`, and cluster fields at offsets
`0x1A`/`0x14`. Exact original symbol names/backing-file attribution are **UNKNOWN**.

**CONFIRMED observed transformation**, with flags R2=3:

1. Bit 1 maps the logical current index through the 32-bit +`0x54` array.
2. Format 1 reads a little-endian 16-bit entry from +`0x58` at `2 * index`.
3. An entry not below the observed bound `0xFFFF` is normalized to `-1`.
4. Bit 0 maps a nonnegative next index through +`0x50`; negative values are
   preserved by the mapping helper (`01909F54..01909F78`).
5. The walker stores the return at `0190A226`, exits only on returned `-1`,
   otherwise counts a discontinuity when next != current+1, assigns current=next,
   and branches back to `0190A210`. There is no cycle check in this loop.

Start: logical index **3**, stored at `[FP+0xC]`; active index is `[FP-8]`,
discontinuity count `[FP-4]`, frame `000243D0`. Reachable mapping entries are
identity entries, so the raw and effective chains match. The first 110 lookups
advance `3..112` to `4..113`; lookup 111 is `113 -> 32882`; lookup 112 is
`32882 -> 0`; lookup 113 is `0 -> 0`. This proves non-monotonic progression
and a reachable cycle. Total iterations to the terminator is **not finite** for
the captured graph, not 65,532 inferred sequential lookups. At the first cycle
return the discontinuity count is 2; another zero iteration would increment it.

Raw table entry 2 is `0xFFFF`, but entry 2 is never reached from start 3.
The existence of that terminator and a sequential table prefix does not prove
global termination. This corrects the earlier checkpoint interpretation.

**CONFIRMED decoded termination path, not executed in these trials:** if the
helper returned `-1`, `0190A228..0190A22C` would branch to `0190A244`, load the
discontinuity count, `unlink` at `0190A246`, and `rts` at `0190A24A` to caller
`0190A306`. That callback would accumulate the count, return to `0190AF00`, and
continue directory scanning. No first *observed* PC after completion exists,
because this invocation never completed. There is no evidence here of reaching
UI initialization or another hardware wait after this walker.

## Next boundary and verification (provenance phase)

The construction follow-up below establishes the origin of the value and proposes
a narrowly scoped Random Data Read readiness fix. No table patch or guest-iteration
bypass is justified. The earlier broader **UNKNOWN** classification is superseded
for the current-image cycle; pristine startup remains a separate unresolved result.

All nine existing focused tests passed after this investigation:

```sh
ctest --test-dir build -R '^(pll_idle_tests|execution_accounting_tests|sport_audio_tests|audio_ring_tests|pcm_sample_tests|mcp23017_tests|nand_ecc_tests|otp_import_tests|usb_reset_tests)$' --output-on-failure
```

Only documentation was changed in the repository for that provenance task. No files were
staged or committed; private images/dumps/binaries remain local.

## Construction follow-up: current-image root cause D

### Allocation and population

**CONFIRMED:** function `0190A508..0190A56E` loads the selected FAT. At
`0190A52A` it calls allocator entry `01A35FA4` with R0=`00018000` (98,304 bytes:
512 bytes/sector × 192 FAT sectors). The return at `0190A52E` is R0=`01082B98`;
instruction `0190A530` stores it in object `00024544` +`0x58`.

The loader invokes `0190A408..0190A506` at `0190A562` with object R0,
destination R1=`01082B98`, logical sector R2=8, and sector count 192 on the
stack. This is a direct file/sector read into the table, not decompression or
table synthesis. The mapping arrays are consulted later during traversal, not
while populating this entry.

Three fresh current-image clones observed this ordering at the watched halfword
`01082C7A` (`table + 2*113`):

1. `0000 -> 0072`: raw NAND page data arrives through DMA3. The CPU block starts
   at `FFA07DA4`, writes DMA3 CONFIG at `FFA07DBE`, and starts NFC page DMA at
   `FFA07DC8`. There is no guest scalar store for the initial halfword: the
   existing DMA implementation writes the transfer into guest memory.
2. `0072 -> 8072`: guest ECC correction writes a single byte at `01082C7B`.
   The exact instruction is **`FFA07CE2 B[P0] = R6`**, in the NAND read-completion
   handler entered at `FFA07950`. The enclosing block starts at `FFA07CA4` and
   returns to `FFA07C6A`. The value is corrupted after the correct direct read.

Immediately before the erroneous byte store, the decoded block establishes:

```text
byte displacement = 0xE3 (227)
P0 = 01082B98 + 0xE3 = 01082C7B
old byte = 00
correction mask = 80
R6 = old byte XOR mask = 80
FFA07CE2 B[P0] = R6
```

Observed block-entry registers include R1=`7`, R2=`7FF`, R3=`B`, R4=`4F8`,
R5=`0027C4F8`, R6=`0007071F`, P0=`01082B98`, P1=`FF80296C`, and
P5=`FF80298C`. Scratch `[P1-4]`=`04F804F8` is interpreted as the stored ECC;
`[P5-4]`=`041803E7` is the newly calculated ECC. Their packed 22-bit XOR is
the observed syndrome `0007071F`, which looks like a single-bit error at byte
227, bit 7. The correction routine itself consistently follows that syndrome.

### Exact backing file and bytes

**CONFIRMED:** the backing YAFFS object is **769 (`0x301`)**, parent object 513
(`user`). Its header at physical page 222385 names **`tape_c.raw`**, file size
402,653,184 bytes. The path is `/yaffs2/user/tape_c.raw`. This guest-created
initialized file is not one of the saved stock `content/` files.

Its boot sector is in file chunk 1 at NAND page 223524. Primary FAT sector 8
begins at file byte 4096, YAFFS chunk 3, physical NAND **page 223868**, block
**3497**, page **60** within that block. The selected page's tags contain
sequence 7277 (`0x1C6D`), object `0x301`, chunk 3, and byte count 2048.

| Entry | FAT-relative byte offset | File byte offset | Source |
| --- | --- | --- | --- |
| 0 | 0 | 4096 | Page 223868, column 0: `00 00` |
| 113 | 226 | 4322 | Page 223868, column 226: **`72 00`**, i.e. next cluster 114 |
| `0x8072` (32882) | 65764 | 69860 | File chunk 35, offset 228; no allocated chunk-35 page for object 769 survives in the frozen image; captured table value is `00 00` |

Entry 113's low byte is at absolute split-NAND offset **458,481,890**; its high
byte is at 458,481,891. The surrounding bytes are `71 00 72 00 73 00 74 00`.
The table's unallocated region reading as zero is consistent with a sparse file
hole, but the exact zero-fill implementation is **LIKELY**, not independently
traced here. There is no physical NAND page to name for absent chunk 35.

OOB columns **2080..2083** contain `E7 03 18 04`, the first ECC pair
`03E7/0418`. Their split-image offset is **551,198,496**. Independent bit/byte
parity calculation matched **all eight** 256-byte data ECC pairs to the stored
OOB pairs on this page. Flipping byte 227 bit 7 would instead produce
`04F8/04F8`, precisely the erroneous scratch pair used by firmware.

Localized image comparisons:

| Input/state | Page 223868 columns 224..231 | First OOB ECC pair |
| --- | --- | --- |
| Preserved current NAND / frozen current base | `7100720073007400` | `e7031804` |
| Regenerated pristine populated NAND | `ffffffffffffffff` | `ffffffff` |
| Clean split donor NAND | `ffffffffffffffff` | `ffffffff` |
| Both pristine clones at their earlier stop | `ffffffffffffffff` | `ffffffff` |

The saved YAFFS content image has **no `tape_c.raw` object header**. It therefore
has no corresponding file/FAT entry to compare; arbitrary occurrences of `72 80`
elsewhere are not relevant provenance. The physical target page in the pristine
and donor inputs is erased, not a pre-existing bad FAT page.

### FAT interpretation

**CONFIRMED:** this is a FAT16 representation within `tape_c.raw`. The BPB has
OEM `MSDOS5.0`, filesystem label `FAT16   `, signature `55 AA`, bytes/sector=512,
sectors/cluster=16, reserved sectors=8, FAT copies=2, root entries=512,
sectors/FAT=192, and total sectors=786366. Root directory size is 32 sectors;
data starts at sector 424; cluster count is **49121 (`0xBFE1`)**. Firmware parses
these BPB fields and selects its 16-bit format (object +`0x4C`=1) for this count.
Full standards conformance is not claimed: the observed zero reserved entry 0
is noncanonical, and exact OP-1 virtual-volume conventions remain **UNKNOWN**.

`0x8072` is numerically a possible data-cluster index within this volume; it is
not a reserved/bad-cluster marker. It is nevertheless wrong for entry 113,
whose stored value is `0x0072`. Its destination entry is free/zero; cluster 0
is reserved and is not a valid next data cluster. This gives the reachable
self-cycle when the guest fails to reject zero.

FAT16 convention treats `FFF8..FFFF` as end-of-chain. The observed helper only
normalizes values at/above **`FFFF`** to `-1`; this is narrower than the standard
EOC range. The exact intended treatment of other EOC encodings in this firmware
is **UNKNOWN**. That discrepancy is not causal for this `0072 -> 8072` mutation.

### First proven data-path divergence

After reading the full valid data page, firmware issues Random Data Read:
`05`, column bytes `40 07`, then `E0` at **`FFA07BA6`**. Column `0x740` (1856)
selects the final 192 data bytes followed by 64 OOB bytes. DMA3 is configured
for **128 16-bit elements (256 bytes)** into `FF802888`; its ECC comparison area
at destination +`0xE0` (`FF802968`) should therefore receive page OOB +32.

The observational NAND wrapper records after `E0`:

```text
page=223868 column=1856 dataOffset=1856
pageReadDataReady=1
raw page-buffer OOB ECC=E7/03/18/04
MT29F4G08::IsDataReady()=0
```

**CONFIRMED emulator defect:** `MT29F4G08::HandleCommand` correctly selects the
column for `CMD_RANDOM_READ2`, but `IsDataReady()` only handles ordinary
`CMD_READ2`, READ ID, and STATUS. It returns false for the valid random-read
command. `NFC::PageReadDMAReady()` requires that predicate, so the subsequent
OOB DMA request is blocked. The wrapper sees the four ordinary 512-byte page
reads and **no** random-read 256-byte transfer. The guest continues with the
incorrect/stale comparison area, while the fresh NFC ECC values match storage.

The first proven readiness divergence is the false predicate after `E0`;
the first incorrect write to this target entry is `FFA07CE2`. Correct NAND
column/page data, original byte order, and independently matching stored versus
calculated ECC rule out persisted `0x8072` bytes as this entry's source.
Whether other stale-completion/interrupt conditions also need attention is
**UNKNOWN**; no general DMA or timing conclusion is drawn.

Micron's 4Gb/8Gb/16Gb x8 datasheet, Rev. B 2/07, p.22, **RANDOM DATA READ
05h-E0h**, specifies selecting a new column and outputting data on the same
already-read page. Local evidence: `/tmp/opencode/mt29fxg08xaa.txt:1054-1062`.
This supports a readiness correction, not a new array-transfer delay.

**Decision gate: D.** Current NAND contains correct data and matching ECC;
the emulator denies a valid random-data transfer and firmware consequently
mutates RAM using wrong ECC comparison bytes. This is not evidence that guest
firmware intentionally constructs a cycle or that the preserved FAT entry is
already corrupt. Stop at diagnosis; **no fix was implemented**.

Smallest proposed next fix: recognize completed, in-buffer `CMD_RANDOM_READ2`
data availability in `MT29F4G08::IsDataReady()` without fabricating latency or
bypassing DMA. Add a deterministic regression for `00/address/30` completion,
`05/40/07/E0`, a 256-byte NFC/DMA read spanning data tail and OOB, and matching
ECC comparison bytes. Reobserve before claiming that this alone reaches UI.

### Pristine/current reproduction and limits

External observation files are under `/tmp/opencode/`: `construction_probe.cpp`,
`build_construction_probe.py`, `construction_trials.py`, and
`localize_fat_source.py`. A derived CPU compares the watched SDRAM halfword at
normal block boundaries; a derived NAND wrapper logs existing operations and
returns the original values unchanged. Disassembly identifies the sole target
byte store in the observed block. No guest instruction/register/table/device
behavior was patched. Allocation returns dynamically identify the table address.

| Fresh clone | Input | Outcome | Host process lifetime |
| --- | --- | --- | --- |
| `construction-current-a` | Frozen current hash | Correct `0072`, then store to `8072`; cycle lookup 113 | 125.638 s |
| `construction-current-b` | Same | Same allocation/write/cycle; correct fresh ECC MMRs and wrong comparison scratch | 125.580 s |
| `construction-current-c` | Same | Same; explicit random-read ready=0 and valid raw OOB captured | 125.670 s |
| `construction-pristine-a` | Pristine hash `08d6e6fb...50880` | Stops before table allocation at IDLE `018AF8C0`, resume `018AF8C2` | 62.827 s |
| `construction-pristine-b` | Same | Same earlier stop; guest diagnostic `..\src\db2\db2.cpp:287 dst != -1`, caller `019046F8` | 62.953 s |

Both pristine runs stop with the same guest IDLE/assertion path, at packets
120,330,730 (A) and 120,241,228 (B), before any `0190A508` FAT table allocation
is observed. Packet counts are observational, not deterministic timing. Thus
whether this cycle would reproduce **after successful pristine initialization**
remains **UNKNOWN**. No table value or missing cycle is inferred from an unreached
phase. The earlier stop is logged as unsupported IDLE by the emulator, but the
observed assertion/terminal IDLE is not established to be a genuine hardware wait.
No bypass was introduced to reach the later FAT phase.

All three current clones finish at the prior `8fab4360...f75597` hash. Both
pristine clones finish at
`a327afebb7633ce34fb9502b38336fc1fa43321dc27805d00ef028bbd040b71a`.
OTP remains unchanged in all five. Every new run used an exclusive fresh clone,
and preserved bases were hash-checked before/after; original `build/op1emu`
also remains unchanged. Diagnostic executable versions differ only in added
observations; each launch/result manifest records its exact hash/environment.
Current A/B/C independently reproduce the claimed `0072 -> 8072` divergence;
the earlier pristine stop is independently reproduced by A/B.

Logs, screenshots, manifests, FAT-object/BPB dumps, and disassemblies remain
local as `/tmp/opencode/construction-*`. Existing unrelated dirty work and
clean Bcore were preserved. Only this provenance document and the checkpoint
document changed in the repository. `git diff --check` was run. **No production
code changed, no tests were run for this follow-up, and nothing was staged or
committed.**

## Fix verification: preserve random-read data availability

### Exact-device semantics and minimal change

**CONFIRMED:** Micron `m60a_4gb_8gb_16gb_ecc_nand.pdf`, Rev. Q 04/14,
p.57, RANDOM DATA READ (05h-E0h), is the exact-device reference retained as
`micron_technology_micts06228-1-1759217.pdf`; extracted text is local
`/tmp/opencode/micts06228-exact.txt:2959-2997`.

- The command changes the column address of the selected cache register and
  enables data output from the last selected die. It is accepted when RDY=1,
  ARDY=1 (also RDY=1/ARDY=0 during cache reads, not modeled by this change).
- The required sequence is `05h`, **two** column cycles, then `E0h`.
- The same loaded cache register remains selected; this is not another array
  transfer. Figure 34 shows readiness staying high: no new R/B# busy interval.
- `E0h` puts the die into data-output mode, which persists until another valid
  command. Physical data output must wait at least **tWHR** after `E0h`.
- Functional readability therefore does not require another array completion.
  **UNKNOWN/unmodeled:** pin-level tWHR enforcement and cycle-accurate bus timing.
  No timing constant, host sleep, or zero-hardware-latency claim was introduced.

The already-correct state was loaded `pageBuffer`, `pageReadDataReady=true`,
and column-selected `dataOffset`. The defect was the omission of confirmed
`CMD_RANDOM_READ2` from `IsDataReady()`, which made NFC's generic readiness gate
reject the ordinary DMA3 request.

The production change is confined to `MT29F4G08.cpp/.h`: a small random-command
validity flag records a loaded, nonbusy page at `05h`, validates the exact two
column cycles and in-buffer column at `E0h`, and permits confirmed random output
in `IsDataReady()`. Scalar and bulk reads reject pending/invalid random sequences.
Malformed-sequence rejection is an explicit emulator validity policy, not a claim
about undefined chip behavior. Reset/normal commands clear this validity state.
No NFC, DMA, ECC, CPU, scheduler, SPORT, or timer behavior was changed.

### Deterministic regression

`tests/nand_random_read_cases.h` adds a firmware-free sparse data/OOB fixture
invoked by `execution_accounting_tests.cpp`. It tests:

- ordinary page busy/completion semantics, including normal reads after a rejected
  random command;
- rejection without a loaded page, during pending array read, without `05h`,
  with zero/one/three column cycles, before confirm, and beyond the page/OOB;
- valid `05/40/07/E0` readiness without a new operation/deadline;
- exact 256-byte span from column 1856, crossing data/OOB at 2048;
- OOB `E7 03 18 04`, cursor exhaustion, final-byte selection, reselecting after
  exhaustion, and reset invalidation;
- a FAT-like first 256 bytes containing `72 00`, whose production NFC parity
  calculates `03E7/0418` matching stored OOB;
- the ordinary awake DMA3 NFC path with 128 16-bit elements: stale comparison
  word `04F804F8` is replaced by `041803E7`, all bytes match, DONE is set, and
  current count reaches zero. No firmware PC is encoded in the regression.

The helper header keeps this new case independently stageable without importing
the existing unrelated test/CMake changes. All nine established focused tests
passed. The exact selectively staged source snapshot was reconstructed from
`44925ea` in `/tmp/opencode/random-read-stage`; its emulator library built, and
`execution_accounting_tests` plus `pll_idle_tests` passed independently. Other
seven focused targets belong to the pre-existing local dirty build, and their
results are not misrepresented as clean-snapshot targets. The clean baseline GUI
still depends on an unrelated unstaged cqueue include fix; the production library
and changed regression do not require it.

### Frozen-input live result

Both `random-read-fixed-v241` and `random-read-followup-v241` used fresh writable
clones of unchanged frozen NAND hash
`c94057cb8000aa9516242c426eeb5c69df03300fd9c4e0f19893c6ef374d5a42` and OTP
hash `06c993496c3aba43ca345c62a2caa27b69d68e160cad8c29196152991a238c5a`.
The external observational executable hash was
`dc5ee099dbce7e40890307c24634fb025e9eadd9ff751f17eec0139b3c63f014`.
The inherited dirty host/device implementation is the same diagnostic baseline;
only the NAND random-read fix changes production behavior for these trials.

**CONFIRMED:** at page 223868/column 1856, `E0` now reports ready=1 and the
NAND/DMA payload contains exactly 256 bytes. At offset 224, ECC is
`E7/03/18/04`. The former watched transition `0072 -> 8072` does not occur.
At lookup 111, current=`00000071`, next=`00000072`, table113=`0072`, proving
**113 -> 114**. The walker returns to **`0190A306`** and directory scanning
continues; several subsequent callbacks also complete. Later the allocation is
reused by unrelated code, so address `01082C7A` is not claimed to remain a FAT
entry indefinitely. The correct value is verified during its FAT-table lifetime.

The initial 180.667-second observation established the correction but no terminal
boundary. Its final private NAND hash was
`3445868239547c6911e211c4f45d1e82fd39733286afa95beba812ef48b5cc65`.
The fresh extended follow-up stopped guest execution at the **first unsupported
IDLE**, approximately **280.795 host seconds** after CPU start; total process
lifetime including screenshot/shutdown was **282.777 seconds**. Packet count
was **1,134,125,974** (observational, not deterministic timing).

### Next explicit boundary — stop here

- IDLE **`018D4736`**, resume `018D4738`, RETS=`018D46C6`.
- P2=`FFC00F28`: MDMA0 destination IRQ_STATUS; pre-IDLE read returned `0008`
  (RUN, DONE clear). Resume code tests bit 0 (`DMA_DONE`).
- At `018D4724`, MDMA0 source CONFIG was enabled from `0014` to `0015`.
- At `018D472A`, destination CONFIG was enabled from `0096` to `0097`.
- **CONFIRMED configuration decode:** 16-bit, two-dimensional source/destination
  transfer, with destination completion interrupt. The existing narrow finite
  IDLE-completion predicate explicitly excludes two-dimensional transfers.
- SIC masks=`10104000/00100300`, ISR=`0/3`, IWR=`00014000/00010000`;
  CEC IMASK/IPEND/ILAT=`7FDF/8000/0`.
- No eligible known wake or deterministic deadline was available. Exact transfer
  parameters, wake configuration consequences, and required next model behavior
  are **UNKNOWN** and were not investigated/fixed in this checkpoint.

**R.00241 remains displayed; tape UI did not appear before this stop.** No
continuation through this new boundary occurred. The follow-up private NAND hash
is `d05cf4ca9ef49ff420d678b93ec72d0c184556ffb8a969fcbf416f5200ae463b`;
OTP remained unchanged. Base inputs were verified unchanged after both runs.

Logs, manifests, screenshots, raw random-read span, and read-only diagnostics are
local under `/tmp/opencode/random-read-{fixed,followup}-v241.*`. No proprietary
binary/image contents are included in the checkpoint. Only the NAND fix, new
regression/include/call, and these two evidence documents are selected for commit.
