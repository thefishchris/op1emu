"""Boot ROM selection and errors in the headless runner and the GUI (docs/boot-rom.md).

Usage: bootrom_cli_tests.py OP1EMU_HEADLESS OP1EMU UI_JSON

Uses a synthetic ROM image only. The GUI checks run without a display: the
boot ROM is validated before any window opens.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HEADLESS, GUI, CONFIG = (Path(p) for p in sys.argv[1:4]) if len(sys.argv) > 3 else (None, None, None)
del sys.argv[1:]

ROM_SIZE = 32768


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('OP1EMU_') and k not in ('DISPLAY', 'WAYLAND_DISPLAY')}
    env.update(extra)
    return env


class BootRomCli(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='op1-bootrom-cli-')
        self.root = Path(self.dir.name)
        self.nand = self.root / 'source.nand'
        self.nand.write_bytes(bytes([255]) * 128)
        self.script = self.root / 'input.txt'
        self.script.write_text('quit\n')
        # JUMP.S 0 everywhere: the reset vector spins in place.
        self.rom = self.root / 'synthetic.rom'
        self.rom_bytes = bytes([0x00, 0x20]) * (ROM_SIZE // 2)
        self.rom.write_bytes(self.rom_bytes)

    def tearDown(self):
        self.dir.cleanup()

    def headless(self, *extra, **env):
        command = [str(HEADLESS), str(self.nand), '--ui-config', str(CONFIG), '--input-script', str(self.script), *extra]
        result = subprocess.run(command, cwd=self.root, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                timeout=60, env=clean_env(**env))
        return result.returncode, result.stdout + result.stderr

    def gui(self, **env):
        result = subprocess.run([str(GUI), str(self.nand), '--nand-snapshot'], cwd=self.root, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=60, env=clean_env(**env))
        return result.returncode, result.stdout + result.stderr

    def test_headless_requires_a_rom_to_boot(self):
        code, output = self.headless()
        self.assertEqual(code, 1, output)
        self.assertIn('No boot ROM image', output)
        self.assertIn('--boot-rom', output)

    def test_headless_option_and_environment(self):
        code, output = self.headless('--boot-rom', str(self.rom))
        self.assertEqual(code, 0, output)
        self.assertIn('unvalidated revision', output)
        code, output = self.headless(OP1EMU_BOOTROM_FILE=str(self.rom))
        self.assertEqual(code, 0, output)
        code, output = self.headless('--boot-rom', str(self.rom), OP1EMU_BOOTROM_FILE=str(self.root / 'missing.rom'))
        self.assertEqual(code, 0, '--boot-rom overrides OP1EMU_BOOTROM_FILE: ' + output)
        self.assertEqual(self.rom.read_bytes(), self.rom_bytes, 'the ROM image is never modified')

    def test_headless_rejects_invalid_images(self):
        short = self.root / 'short.rom'
        short.write_bytes(self.rom_bytes[:100])
        for args, message in ((['--boot-rom', str(self.root / 'missing.rom')], 'does not exist'),
                              (['--boot-rom', str(short)], 'is 100 bytes'),
                              (['--boot-rom', str(self.root)], 'not a regular file'),
                              (['--boot-rom', ''], 'empty path')):
            with self.subTest(message=message):
                code, output = self.headless(*args)
                self.assertEqual(code, 1, output)
                self.assertIn('OP1EMU_BOOTROM_FILE: boot ROM image', output)
                self.assertIn(message, output)
                self.assertIn('32768', output)
        self.assertFalse((self.root / 'missing.rom').exists())

    def test_headless_read_only_image(self):
        self.rom.chmod(0o444)
        code, output = self.headless('--boot-rom', str(self.rom))
        self.assertEqual(code, 0, output)
        self.assertEqual(self.rom.read_bytes(), self.rom_bytes)

    def test_gui_requires_a_rom_before_opening_a_window(self):
        code, output = self.gui()
        self.assertEqual(code, 1, output)
        self.assertIn('No boot ROM image', output)

    def test_gui_rejects_an_invalid_image(self):
        short = self.root / 'short.rom'
        short.write_bytes(self.rom_bytes[:4])
        code, output = self.gui(OP1EMU_BOOTROM_FILE=str(short))
        self.assertEqual(code, 1, output)
        self.assertIn('OP1EMU_BOOTROM_FILE: boot ROM image', output)
        self.assertIn('is 4 bytes', output)


if __name__ == '__main__':
    if HEADLESS is None:
        sys.exit('usage: bootrom_cli_tests.py OP1EMU_HEADLESS OP1EMU UI_JSON')
    unittest.main()
