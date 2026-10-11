#!/usr/bin/env python3
"""Convert an explicitly supplied textual OTP dump to the emulator's file layout."""

import argparse
from pathlib import Path
import re
import struct


IMAGE_SIZE = 512 * 16
PAGE_LINE = re.compile(r"!?0x([0-9a-fA-F]+)\s+([0-9a-fA-F]+)(?:\s+([0-9a-fA-F]+))?")


def parse_dump(text, overrides=None):
    overrides = overrides or {}
    pages = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line == "...":
            continue
        match = PAGE_LINE.fullmatch(line)
        if not match:
            raise ValueError(f"line {number}: invalid OTP entry")
        page = int(match[1], 16)
        if not 0 <= page < 512 or page in pages:
            raise ValueError(f"line {number}: duplicate or out-of-range page")
        if page in overrides:
            pages[page] = overrides[page]
        elif match[2] == "00" and match[3] is None:
            pages[page] = bytes(16)
        elif len(match[2]) == 16 and match[3] is not None and len(match[3]) == 16:
            pages[page] = struct.pack("<QQ", int(match[2], 16), int(match[3], 16))
        else:
            raise ValueError(f"line {number}: expected two exact 64-bit words; supply an explicit override if justified")
        if len(pages[page]) != 16:
            raise ValueError(f"page {page:#x}: override must contain 16 bytes")
    if not pages:
        raise ValueError("no OTP entries")
    if set(overrides) - set(pages):
        raise ValueError("override refers to a page absent from the source")
    return pages


def ecc64(value):
    # Mirror OTP::CalculateEcc's current emulator storage encoding.
    parity = 0
    data_bit = 0
    overall = 0
    for position in range(1, 73):
        if position & (position - 1) == 0:
            continue
        bit = (value >> data_bit) & 1
        data_bit += 1
        if bit:
            parity ^= position
            overall ^= 1
        if data_bit == 64:
            break
    overall ^= parity.bit_count() & 1
    return parity | (overall << 7)


def make_image(base, pages):
    if len(base) != IMAGE_SIZE:
        raise ValueError("base OTP image must be exactly 8192 bytes")
    if any(not 0 <= page < 512 or len(data) != 16 for page, data in pages.items()):
        raise ValueError("invalid OTP page")
    # ECC pages are synthesized, not accepted as overlapping source entries.
    if any(0xE0 <= page <= 0xFF or 0x1E0 <= page <= 0x1FF for page in pages):
        raise ValueError("source entries overlap ECC storage pages")
    image = bytearray(base)
    for page, data in pages.items():
        image[page * 16:(page + 1) * 16] = data
        low, high = struct.unpack("<QQ", data)
        ecc_page = 0xE0 + page // 8 if page < 256 else 0x1E0 + (page - 256) // 8
        offset = ecc_page * 16 + (page % 8) * 2
        image[offset:offset + 2] = bytes((ecc64(low), ecc64(high)))
    return bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--override-page", action="append", default=[], metavar="PAGE:LOW64:HIGH64")
    args = parser.parse_args()
    try:
        if args.source.stat().st_size > 65536:
            raise ValueError("text dump exceeds 64 KiB")
        if args.base.stat().st_size != IMAGE_SIZE:
            raise ValueError("base OTP image must be exactly 8192 bytes")
        overrides = {}
        for override in args.override_page:
            fields = override.split(":")
            try:
                page = int(fields[0], 0) if len(fields) == 3 else -1
            except ValueError:
                page = -1
            if not 0 <= page < 512 or page in overrides or not all(re.fullmatch(r"[0-9a-fA-F]{16}", word) for word in fields[1:]):
                raise ValueError(f"invalid or duplicate --override-page {override!r}; expected PAGE:LOW64:HIGH64 with 16 hex digits per word")
            overrides[page] = struct.pack("<QQ", int(fields[1], 16), int(fields[2], 16))
        pages = parse_dump(args.source.read_text(encoding="utf-8"), overrides)
        image = make_image(args.base.read_bytes(), pages)
        with args.output.open("xb") as output:
            try:
                output.write(image)
            except OSError:
                args.output.unlink()  # never leave a partial image behind
                raise
        print(f"Wrote {len(image)} bytes; imported {len(pages)} pages")
        print("Explicit overrides:", ", ".join(hex(page) for page in overrides) or "none")
        print("Unspecified pages preserved from base; source and base were not changed")
    except (ValueError, OSError) as error:
        parser.exit(1, f"OTP import failed: {error}\n")


if __name__ == "__main__":
    main()
