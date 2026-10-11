"""GUI keyboard map (gui/ui.json "keycaps"): each key drives the same OP-1 control
as its on-screen button, keys and pins are unique, and every key name is one that
GLFWDisplay::MapKeyNameToGLFW understands."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / "gui/ui.json").read_text())
SOURCE = (ROOT / "gui/glfw_display.cpp").read_text()

# Intended keyboard shortcuts: key name -> on-screen button it duplicates.
EXPECTED = {"shift": "shift", "r": "rec", "space": "play", "escape": "stop"}


def supported_key_names():
    body = SOURCE[SOURCE.index("int GLFWDisplay::MapKeyNameToGLFW"):]
    body = body[:body.index("\n}\n")]
    return set(re.findall(r'keyName == "([^"]+)"', body))


def glfw_key(name):
    """Mirror of MapKeyNameToGLFW's result identity, enough to detect duplicates."""
    aliases = {"control": "ctrl", "return": "enter", "esc": "escape"}
    if len(name) == 1 and name.isalnum():
        return name.lower()
    return aliases.get(name, name)


class KeymapTests(unittest.TestCase):
    def test_keys_drive_their_buttons(self):
        keycaps = CONFIG["keycaps"]
        self.assertEqual(set(keycaps), set(EXPECTED))
        for key, button in EXPECTED.items():
            with self.subTest(key=key):
                self.assertEqual(keycaps[key], CONFIG["buttons"][button]["gpio"])

    def test_each_pin_belongs_to_exactly_one_button(self):
        for key, pin in CONFIG["keycaps"].items():
            owners = [name for name, b in CONFIG["buttons"].items() if b["gpio"] == pin]
            owners += [f"encoder {name}" for name, e in CONFIG.get("encoders", {}).items()
                       if pin in (e["clockwise_gpio"], e["counter_clockwise_gpio"])]
            with self.subTest(key=key):
                self.assertEqual(owners, [EXPECTED[key]])

    def test_keys_and_pins_are_unique(self):
        keys = [glfw_key(k) for k in CONFIG["keycaps"]]
        pins = [tuple(p) for p in CONFIG["keycaps"].values()]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(len(pins), len(set(pins)))

    def test_key_names_are_supported(self):
        supported = supported_key_names()
        self.assertIn("space", supported)
        for key in CONFIG["keycaps"]:
            with self.subTest(key=key):
                self.assertTrue(key in supported or (len(key) == 1 and key.isalnum()),
                                f"{key!r} is not mapped by MapKeyNameToGLFW")


if __name__ == "__main__":
    unittest.main()
