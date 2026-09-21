#!/usr/bin/env python3
"""Emit fixed H563/H755 layouts and check the final bootloader code-size limit."""
import argparse
import json
from pathlib import Path
import re
import struct

BASE = 0x08000000
BANK_SIZE = 1024 * 1024
SECTOR = 8192
BOOT_RESERVATION = 32 * 1024
OTP_EMULATOR_BASE = BASE + BANK_SIZE
OTP_EMULATOR_SIZE = SECTOR
assert OTP_EMULATOR_BASE % SECTOR == 0
assert OTP_EMULATOR_SIZE <= BOOT_RESERVATION


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
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--board", choices=["h563", "h755"], default="h563")
    parser.add_argument("--source-linker", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--check", type=Path)
    args = parser.parse_args()
    size = flash_size(args.probe) if args.probe else None
    reserve = BOOT_RESERVATION
    if size is not None and size > reserve:
        parser.error(f"bootloader probe ({size}) exceeds fixed reservation ({reserve})")
    if args.check:
        actual = flash_size(args.check)
        if actual > reserve:
            parser.error(f"final bootloader ({actual}) exceeds fixed reservation ({reserve})")
        print(f"Bootloader {actual} bytes; reservation {reserve} bytes (code-size limit)")
        return
    directory = args.directory
    directory.mkdir(parents=True, exist_ok=True)
    sector = 128 * 1024 if args.board == 'h755' else SECTOR
    prefix = sector if args.board == 'h755' else reserve
    write_size = 32 if args.board == 'h755' else 16
    product = 0x755 if args.board == 'h755' else 0x563
    slots = [BASE + prefix + sector, BASE + BANK_SIZE + prefix + sector]
    metadata = [BASE + prefix, BASE + BANK_SIZE + prefix]
    layout = {"boot_size": size, "boot_reservation": reserve, "boot_erase_reservation": prefix, "slot_size": BANK_SIZE - prefix - sector,
              "slots": slots, "metadata": metadata, "sector_size": sector, "write_size": write_size,
              "product": product, "revision": 1,
              "otp_emulator_base": OTP_EMULATOR_BASE, "otp_emulator_size": OTP_EMULATOR_SIZE if args.board == "h563" else 0}
    if args.board == "h755":
        layout["m4_address"] = BASE + BANK_SIZE
        layout["m4_reservation"] = sector
    (directory / "layout.json").write_text(json.dumps(layout, indent=2) + "\n")
    header = ("#pragma once\n#include \"boot/flash.h\"\nnamespace daveos::boot {\n"
              "inline constexpr Layout kLayout{{"
              + ",".join(hex(x) for x in slots) + "},{"
              + ",".join(hex(x) for x in metadata)
              + f"}}, {layout['slot_size']}, {sector}, {write_size}, {product}, 1, {5000000 if args.board == 'h755' else 1000000}}};\n"
              )
    if args.board == 'h563':
        header += (f"inline constexpr std::uint32_t kOtpEmulatorBase = {OTP_EMULATOR_BASE:#x};\n"
                   f"inline constexpr std::uint32_t kOtpEmulatorSize = {OTP_EMULATOR_SIZE};\n"
                   "static_assert(kOtpEmulatorBase % 8192 == 0);\n"
                   "static_assert(kOtpEmulatorBase + kOtpEmulatorSize <= kLayout.metadata[1]);\n")
    header += "}\n"
    (directory / "layout.h").write_text(header)
    source = args.source_linker.read_text()
    (directory / "boot.ld").write_text(linker(source, BASE, reserve))
    for name, origin in zip(("slot_a.ld", "slot_b.ld"), slots):
        (directory / name).write_text(linker(source, origin, layout["slot_size"]))
    print(f"Bootloader limit {reserve}; erase reservation {prefix}; each app {layout['slot_size']}")


if __name__ == "__main__":
    main()
