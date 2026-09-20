#!/usr/bin/env python3
"""Measure a complete H563 bootloader probe and emit the shared flash layout."""
import argparse
import json
from pathlib import Path
import re
import struct

BASE = 0x08000000
BANK_SIZE = 1024 * 1024
SECTOR = 8192
BOOT_RESERVATION = 32 * 1024


def flash_size(path):
    data = Path(path).read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise ValueError("expected little-endian ELF32")
    phoff = struct.unpack_from("<I", data, 28)[0]
    entsize, count = struct.unpack_from("<HH", data, 42)
    end = BASE
    for i in range(count):
        kind, _, _, physical, size, _, _, _ = struct.unpack_from("<8I", data, phoff + i * entsize)
        if kind == 1 and size and BASE <= physical < BASE + BANK_SIZE:
            end = max(end, physical + size)
    if end == BASE:
        raise ValueError("no flash load segments")
    return end - BASE


def linker(source, origin, length):
    output, count = re.subn(r"(FLASH\s*\(rx\)\s*:\s*)ORIGIN\s*=\s*0x[0-9a-fA-F]+,\s*LENGTH\s*=\s*\w+",
        rf"\g<1>ORIGIN = 0x{origin:08x}, LENGTH = {length}", source)
    if count != 1:
        raise ValueError("expected one FLASH memory definition")
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--source-linker", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    size = flash_size(args.probe)
    reserve = BOOT_RESERVATION
    if size > reserve:
        parser.error(f"bootloader probe ({size}) exceeds fixed reservation ({reserve})")
    if args.check:
        actual = flash_size(args.check)
        if actual > reserve:
            parser.error(f"final bootloader ({actual}) exceeds fixed reservation ({reserve})")
        print(f"Bootloader {actual} bytes; reservation {reserve} bytes ({reserve // SECTOR} sectors)")
        return
    directory = args.directory
    directory.mkdir(parents=True, exist_ok=True)
    slots = [BASE + reserve + SECTOR, BASE + BANK_SIZE + reserve + SECTOR]
    metadata = [BASE + reserve, BASE + BANK_SIZE + reserve]
    layout = {"boot_size": size, "boot_reservation": reserve, "slot_size": BANK_SIZE - reserve - SECTOR,
              "slots": slots, "metadata": metadata, "sector_size": SECTOR, "write_size": 16,
              "product": 0x563, "revision": 1}
    (directory / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    header = ("#pragma once\n#include \"boot/flash.h\"\nnamespace daveos::boot {\n"
              "inline constexpr Layout kLayout{{"
              + ",".join(hex(x) for x in slots) + "},{"
              + ",".join(hex(x) for x in metadata)
              + f"}}, {layout['slot_size']}, 8192, 16, 0x563, 1}};\n}}\n")
    (directory / "layout.h").write_text(header)
    source = args.source_linker.read_text()
    (directory / "boot.ld").write_text(linker(source, BASE, reserve))
    for name, origin in zip(("slot_a.ld", "slot_b.ld"), slots):
        (directory / name).write_text(linker(source, origin, layout["slot_size"]))
    print(f"Measured bootloader probe: {size} bytes; reserved {reserve}; each app {layout['slot_size']}")


if __name__ == "__main__":
    main()
