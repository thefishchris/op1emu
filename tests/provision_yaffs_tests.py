"""provision_yaffs with synthetic inputs (docs/yaffs-provisioning.md).

Usage: provision_yaffs_tests.py PROVISION_YAFFS WORK_DIR

The base NAND images are sparse files in a temporary directory under WORK_DIR.
Output pages are checked against an independent Python ECC calculation, and
every byte outside the target pages must equal the base.
"""

import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(sys.argv[1]) if len(sys.argv) > 1 else None
WORK = Path(sys.argv[2]) if len(sys.argv) > 2 else Path.cwd()
del sys.argv[1:]

PAGE, OOB, PAGES_PER_BLOCK, BLOCKS = 2048, 64, 64, 4096
PAGES = BLOCKS * PAGES_PER_BLOCK
DATA_SIZE = PAGES * PAGE
IMAGE_SIZE = DATA_SIZE + PAGES * OOB
RECORD = PAGE + OOB
START = 2  # target block in the synthetic base; blocks 2 and 3 are erased
ERASED_PAGES = range(START * PAGES_PER_BLOCK, (START + 2) * PAGES_PER_BLOCK)


def parity(value):
    return bin(value).count("1") & 1


def sector_ecc(sector):
    """BF52x NFC parity pair for one 256-byte sector, as (odd, even) 11-bit values.

    Column parities come from the XOR of all bytes; line parity k covers the
    bytes whose index has bit k set (odd) or clear (even).
    """
    folded = 0
    for byte in sector:
        folded ^= byte
    odd = parity(folded & 0xAA) | parity(folded & 0xCC) << 1 | parity(folded & 0xF0) << 2
    even = parity(folded & 0x55) | parity(folded & 0x33) << 1 | parity(folded & 0x0F) << 2
    for k in range(8):
        set_bits = clear_bits = 0
        for index, byte in enumerate(sector):
            if index >> k & 1:
                set_bits ^= parity(byte)
            else:
                clear_bits ^= parity(byte)
        odd |= set_bits << (3 + k)
        even |= clear_bits << (3 + k)
    return odd, even


def expected_oob(data, tags, tags_offset, ecc_offset):
    oob = bytearray(b"\xff" * OOB)
    oob[tags_offset:tags_offset + 28] = tags
    for sector in range(8):
        odd, even = sector_ecc(data[sector * 256:(sector + 1) * 256])
        oob[ecc_offset + sector * 4:ecc_offset + sector * 4 + 4] = (odd | even << 16).to_bytes(4, "little")
    return bytes(oob)


def make_base(path, dirty=None):
    """A sparse image whose only erased pages are blocks START and START+1."""
    with open(path, "wb") as image:
        image.truncate(IMAGE_SIZE)
        first, count = ERASED_PAGES.start, len(ERASED_PAGES)
        image.seek(first * PAGE)
        image.write(b"\xff" * (count * PAGE))
        image.seek(DATA_SIZE + first * OOB)
        image.write(b"\xff" * (count * OOB))
        if dirty is not None:
            image.seek(dirty)
            image.write(b"\x00")


def make_record(index):
    data = bytes((index * 31 + i * 7 + (i >> 8)) & 0xFF for i in range(PAGE))
    tags = bytes((index + i * 13) & 0xFF for i in range(28))
    return data, tags, data + tags + b"\xff" * (OOB - 28)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as file:
        for chunk in iter(lambda: file.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


class ProvisionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory(prefix="provision-yaffs-", dir=WORK)
        cls.root = Path(cls.dir.name)
        cls.base = cls.root / "base.img"
        make_base(cls.base)
        cls.base_hash = sha256(cls.base)
        # 66 records cross a block boundary; records 3 and 65 are block padding.
        cls.records = [make_record(i) for i in range(66)]
        cls.records[3] = cls.records[65] = (b"\xff" * PAGE, b"\xff" * 28, b"\xff" * RECORD)
        cls.raw = cls.root / "content.yaffs2"
        cls.raw.write_bytes(b"".join(record for _, _, record in cls.records))
        cls.raw_hash = sha256(cls.raw)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_tool(self, *args):
        return subprocess.run([str(TOOL), *map(str, args)], capture_output=True, text=True, timeout=240)

    def assert_rejected(self, result, message, output):
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(os.path.lexists(output), "a rejected run leaves no output")

    def check_output(self, output, tags_offset, ecc_offset):
        self.assertEqual(output.stat().st_size, IMAGE_SIZE)
        first = START * PAGES_PER_BLOCK
        with open(output, "rb") as image:
            for i, (data, tags, record) in enumerate(self.records):
                image.seek((first + i) * PAGE)
                page_data = image.read(PAGE)
                image.seek(DATA_SIZE + (first + i) * OOB)
                page_oob = image.read(OOB)
                if record == b"\xff" * RECORD:
                    self.assertEqual(page_data + page_oob, record, f"padding record {i} stays erased")
                else:
                    self.assertEqual(page_data, data, f"record {i} data")
                    self.assertEqual(page_oob, expected_oob(data, tags, tags_offset, ecc_offset), f"record {i} OOB")
        # Every byte outside the provisioned pages equals the base.
        n = len(self.records)
        skip = [(first * PAGE, (first + n) * PAGE), (DATA_SIZE + first * OOB, DATA_SIZE + (first + n) * OOB)]
        with open(self.base, "rb") as base, open(output, "rb") as out:
            position = 0
            for start, end in skip + [(IMAGE_SIZE, IMAGE_SIZE)]:
                while position < start:
                    size = min(1 << 22, start - position)
                    self.assertEqual(base.read(size), out.read(size), f"bytes at {position} unchanged")
                    position += size
                base.seek(end)
                out.seek(end)
                position = end
        self.assertEqual(sha256(self.base), self.base_hash, "the base image is not modified")
        self.assertEqual(sha256(self.raw), self.raw_hash, "the YAFFS image is not modified")

    def test_ecc_reference_vectors(self):
        # The same vectors as nand_ecc_tests, which cover ComputeEccPair.
        sector = bytearray(256)
        self.assertEqual(sector_ecc(sector), (0, 0))
        self.assertEqual(sector_ecc(b"\xff" * 256), (0, 0))
        sector[0] = 0x01
        self.assertEqual(sector_ecc(sector), (0, 0x7FF))
        sector[0] = 0x04
        self.assertEqual(sector_ecc(sector), (2, 0x7FD))
        sector[0] = 0x05
        self.assertEqual(sector_ecc(sector), (2, 2))

    def test_provision_v241_offsets(self):
        output = self.root / "out-4-32.img"
        result = self.run_tool(self.base, self.raw, output, START, 4, 32)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Provisioned 66 records (64 written, 2 erased) at block 2", result.stdout)
        try:
            self.check_output(output, 4, 32)
        finally:
            output.unlink()

    def test_provision_other_offsets(self):
        # The tool places tags and ECC wherever told; only 4/32 is guest-confirmed.
        output = self.root / "out-36-0.img"
        result = self.run_tool(self.base, self.raw, output, START, 36, 0)
        self.assertEqual(result.returncode, 0, result.stderr)
        try:
            self.check_output(output, 36, 0)
        finally:
            output.unlink()

    def test_existing_output_is_never_overwritten(self):
        output = self.root / "existing.img"
        output.write_bytes(b"keep")
        result = self.run_tool(self.base, self.raw, output, START, 4, 32)
        self.assertEqual(result.returncode, 1)
        self.assertIn("already exists", result.stderr)
        self.assertEqual(output.read_bytes(), b"keep")
        link = self.root / "dangling.img"
        link.symlink_to(self.root / "nowhere.img")
        result = self.run_tool(self.base, self.raw, link, START, 4, 32)
        self.assertIn("already exists", result.stderr)
        self.assertFalse((self.root / "nowhere.img").exists(), "a dangling symlink target is not created")
        self.assertEqual(self.run_tool(self.base, self.raw, self.base, START, 4, 32).returncode, 1)
        self.assertEqual(sha256(self.base), self.base_hash)
        output.unlink()
        link.unlink()

    def test_invalid_arguments(self):
        output = self.root / "bad-args.img"
        for args, message in (((START, 4), "usage"),
                              ((START, 4, 32, 1), "usage"),
                              (("2x", 4, 32), "START-BLOCK must be a decimal integer"),
                              (("-1", 4, 32), "START-BLOCK must be a decimal integer"),
                              (("", 4, 32), "START-BLOCK must be a decimal integer"),
                              ((START, "0x4", 32), "TAGS-OFFSET must be a decimal integer"),
                              ((4096, 4, 32), "below 4096"),
                              ((START, 37, 0), "must fit in the 64-byte OOB"),
                              ((START, 0, 33), "must fit in the 64-byte OOB"),
                              ((START, 4, 16), "overlap")):
            with self.subTest(args=args):
                self.assert_rejected(self.run_tool(self.base, self.raw, output, *args), message, output)

    def test_missing_and_wrong_size_inputs(self):
        output = self.root / "bad-input.img"
        self.assert_rejected(self.run_tool(self.root / "missing.img", self.raw, output, START, 4, 32), "does not exist", output)
        self.assert_rejected(self.run_tool(self.base, self.root / "missing.yaffs2", output, START, 4, 32), "does not exist", output)
        self.assert_rejected(self.run_tool(self.base, self.root, output, START, 4, 32), "not a regular file", output)
        for size in (0, 1000, IMAGE_SIZE - 1, IMAGE_SIZE + OOB):
            with self.subTest(base_size=size):
                bad = self.root / f"base-{size}.img"
                with open(bad, "wb") as image:
                    image.truncate(size)
                self.assert_rejected(self.run_tool(bad, self.raw, output, START, 4, 32), "expected 553648128", output)
                bad.unlink()
        for size in (0, 1, PAGE, RECORD - 1, RECORD + 1):
            with self.subTest(raw_size=size):
                bad = self.root / f"raw-{size}.yaffs2"
                bad.write_bytes(self.records[0][2][:size] if size <= RECORD else self.records[0][2] + b"\xff")
                self.assert_rejected(self.run_tool(self.base, bad, output, START, 4, 32), "multiple of 2112", output)
                bad.unlink()

    def test_image_past_device_end(self):
        output = self.root / "past-end.img"
        self.assert_rejected(self.run_tool(self.base, self.raw, output, BLOCKS - 1, 4, 32), "more than the pages", output)

    def test_target_not_erased(self):
        output = self.root / "not-erased.img"
        first = START * PAGES_PER_BLOCK
        # A programmed byte in the data of the last target page, then in the OOB of the first.
        for offset, page in (((first + 65) * PAGE + 100, first + 65), (DATA_SIZE + first * OOB, first)):
            with self.subTest(page=page):
                dirty = self.root / "dirty-base.img"
                make_base(dirty, dirty=offset)
                self.assert_rejected(self.run_tool(dirty, self.raw, output, START, 4, 32), f"not erased at page {page}", output)
                dirty.unlink()
        # Block 0 of the synthetic base is all zero, as programmed data would be.
        self.assert_rejected(self.run_tool(self.base, self.raw, output, 0, 4, 32), "not erased at page 0", output)

    def test_unsupported_spare_layout(self):
        output = self.root / "spare.img"
        data, tags, record = self.records[5]
        bad = self.root / "bad-spare.yaffs2"
        bad.write_bytes(self.records[0][2] + data + tags + b"\x00" + b"\xff" * 35)
        self.assert_rejected(self.run_tool(self.base, bad, output, START, 4, 32), "record 1 has spare data after the 28-byte packed tags", output)
        bad.unlink()


if __name__ == "__main__":
    if TOOL is None or not TOOL.is_file():
        sys.exit("usage: provision_yaffs_tests.py PROVISION_YAFFS WORK_DIR")
    unittest.main()
