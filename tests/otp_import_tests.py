import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/import_otp_dump.py"
spec = importlib.util.spec_from_file_location("otp_import", TOOL)
otp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(otp)


class ImportTests(unittest.TestCase):
    def test_little_endian_and_sparse_zero(self):
        pages = otp.parse_dump("...\n!0x10 0000000000000001 0000000000000002\n0x11 00\n")
        self.assertEqual(pages[0x10], struct.pack("<QQ", 1, 2))
        self.assertEqual(pages[0x11], bytes(16))

    def test_malformed_entries_rejected(self):
        for text in ("0x200 00", "0x10 00\n0x10 00", "junk", "0x10 123 0000000000000000", "0x10 0000000000000000 10000000000000000", "..."):
            with self.subTest(text=text), self.assertRaises(ValueError):
                otp.parse_dump(text)

    def test_override_is_explicit_and_bounded(self):
        self.assertEqual(otp.parse_dump("0x10 123 456", {0x10: bytes(16)})[0x10], bytes(16))
        with self.assertRaises(ValueError):
            otp.parse_dump("0x10 00", {0x11: bytes(16)})
        with self.assertRaises(ValueError):
            otp.parse_dump("0x10 00", {0x10: bytes(15)})

    def test_image_ecc_and_preservation(self):
        base = bytes([0xA5]) * 8192
        page = struct.pack("<QQ", 1, 0)
        result = otp.make_image(base, {0x10: page, 0x100: page})
        self.assertEqual(len(result), 8192)
        self.assertEqual(result[0x10 * 16:0x11 * 16], page)
        self.assertEqual(result[0xE2 * 16:0xE2 * 16 + 2], bytes((0x83, 0)))
        self.assertEqual(result[0x1E0 * 16:0x1E0 * 16 + 2], bytes((0x83, 0)))
        self.assertEqual(result[:16], base[:16])
        self.assertEqual(base, bytes([0xA5]) * 8192)

    def test_ecc_matches_device_vectors(self):
        # The same vectors are checked against OTP::CalculateEcc in otp_storage_tests.
        vectors = {0: 0x00, 1: 0x83, 1 << 63: 0xC7, (1 << 64) - 1: 0xFF, 0x0123456789ABCDEF: 0x9C,
                   struct.unpack("<Q", b"ADSP-BF5")[0]: 0xDF}
        for value, ecc in vectors.items():
            with self.subTest(value=hex(value)):
                self.assertEqual(otp.ecc64(value), ecc)

    def test_bad_image_inputs(self):
        for base, pages in ((bytes(8191), {}), (bytes(8192), {512: bytes(16)}), (bytes(8192), {0xE0: bytes(16)}), (bytes(8192), {0: bytes(17)})):
            with self.subTest(pages=pages), self.assertRaises(ValueError):
                otp.make_image(base, pages)


class CommandLineTests(unittest.TestCase):
    # Synthetic inputs only: a patterned base image and a two-page text dump.
    BASE = bytes((i * 7 + 3) & 0xFF for i in range(8192))
    SOURCE = "...\n!0x10 0000000000000001 0000000000000002\n0x11 00\n"

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.root = Path(self.dir.name)
        self.base = self.root / "base.bin"
        self.base.write_bytes(self.BASE)
        self.source = self.root / "source.txt"
        self.source.write_text(self.SOURCE, encoding="utf-8")
        self.output = self.root / "out.bin"

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, *extra, source=None, base=None, output=None):
        command = [sys.executable, "-I", str(TOOL), str(source or self.source), str(output or self.output),
                   "--base", str(base or self.base), *extra]
        return subprocess.run(command, capture_output=True, text=True, timeout=60)

    def assert_failed(self, result, message):
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("OTP import failed:", result.stderr)
        self.assertIn(message, result.stderr)
        self.assertFalse(self.output.exists(), "a failed import leaves no output")

    def test_valid_import(self):
        result = self.run_tool()
        self.assertEqual(result.returncode, 0, result.stderr)
        image = self.output.read_bytes()
        self.assertEqual(len(image), 8192)
        self.assertEqual(image[0x100:0x110], struct.pack("<QQ", 1, 2))
        self.assertEqual(image[0x110:0x120], bytes(16))
        self.assertEqual(image[:0x100], self.BASE[:0x100])
        self.assertEqual(self.base.read_bytes(), self.BASE, "the base image is not modified")

    def test_existing_output_is_refused(self):
        self.output.write_bytes(b"keep")
        result = self.run_tool()
        self.assertEqual(result.returncode, 1)
        self.assertIn("OTP import failed:", result.stderr)
        self.assertEqual(self.output.read_bytes(), b"keep")

    def test_missing_inputs(self):
        self.assert_failed(self.run_tool(source=self.root / "missing.txt"), "missing.txt")
        self.assert_failed(self.run_tool(base=self.root / "missing.bin"), "missing.bin")

    def test_truncated_and_oversized_base(self):
        for size in (0, 8191, 8193):
            with self.subTest(size=size):
                self.base.write_bytes(self.BASE[:size] if size <= 8192 else self.BASE + b"\0")
                self.assert_failed(self.run_tool(), "exactly 8192 bytes")

    def test_malformed_and_truncated_source(self):
        for text, message in (("0x10 0000000000000001 00000000000", "line 1"),  # cut mid-word
                              ("0x10 0000000000000001 0000000000000002\n0x1", "line 2"),  # cut mid-line
                              ("", "no OTP entries"),
                              ("\xff\xfe", "can't decode")):
            with self.subTest(text=text):
                self.source.write_text(text, encoding="latin-1")
                self.assert_failed(self.run_tool(), message)

    def test_malformed_override(self):
        for override in ("0x10", "0x10:1:2", "zz:0000000000000000:0000000000000000", "0x200:0000000000000000:0000000000000000"):
            with self.subTest(override=override):
                self.assert_failed(self.run_tool("--override-page", override), "expected PAGE:LOW64:HIGH64")


if __name__ == "__main__":
    unittest.main()
