#!/usr/bin/env python3
"""Build factory HEX: bootloader, confirmed A, metadata, plus M4 on H755.

Program with a main-flash mass erase first. Unlisted bytes (including bank 2's
placeholder and application slot, except the fixed H755 M4) remain erased; OTP is never part of this file.
"""
import argparse
import binascii
import json
from pathlib import Path
import struct

BASE = 0x08000000
BANK_SIZE = 1024 * 1024


def metadata(application, layout, version):
    record = bytearray(256)
    wide = layout.get('write_size', 16) == 32
    struct.pack_into('<IIQQ', record, 0, 0x4a534f44, 2 if wide else 1, 1, 1)
    struct.pack_into('<Q8I', record, 24, 1, len(application),
                     binascii.crc32(application), 4, layout['product'],
                     layout['revision'], version['major'], version['minor'], version['build'])
    commit = version['commit'].encode('ascii')
    if len(commit) > 40 or b'\0' in commit:
        raise ValueError('invalid version commit identifier')
    record[64:64 + len(commit)] = commit
    record[105] = int(version['dirty'])
    # Empty slot still has the default local-build version sentinel.
    struct.pack_into('<I', record, 156, 0xffffffff)
    crc_at, commit_at = (220, 224) if wide else (236, 240)
    struct.pack_into('<I', record, crc_at, binascii.crc32(record[:crc_at]))
    struct.pack_into('<IQI', record, commit_at, 0x454e4f44, 1, 0xbab1b0bb)
    return bytes(record)


def regions(bootloader, application, layout, version, m4=None):
    reserve = layout['boot_reservation']
    sector = layout['sector_size']
    h7 = layout.get('product') == 0x755
    prefix = 128 * 1024 if h7 else reserve
    expected_sector, word = (128 * 1024, 32) if h7 else (8192, 16)
    if (reserve != 32768 or sector != expected_sector or layout['write_size'] != word or
            layout['metadata'] != [BASE + prefix, BASE + BANK_SIZE + prefix] or
            layout['slots'] != [BASE + prefix + sector, BASE + BANK_SIZE + prefix + sector] or
            layout['slot_size'] != BANK_SIZE - prefix - sector):
        raise ValueError('unexpected factory layout')
    if not 8 <= len(bootloader) <= reserve or not 8 <= len(application) <= layout['slot_size']:
        raise ValueError('image is empty, truncated, or exceeds its flash reservation')
    for image, address in ((bootloader, BASE), (application, layout['slots'][0])):
        stack, entry = struct.unpack_from('<II', image)
        if (not 0x20000800 < stack <= (0x20020000 if h7 else 0x200a0000) or stack % 8 or not entry & 1 or
                not address <= (entry & ~1) < address + len(image)):
            raise ValueError(f'invalid vectors for image at {address:#010x}')
    record = metadata(application, layout, version)
    extra = []
    if h7:
        if m4 is None or not 8 <= len(m4) <= sector:
            raise ValueError('H755 factory image requires the fixed M4 image')
        stack, entry = struct.unpack_from('<II', m4)
        if not (0x10000000 < stack <= 0x10048000 and stack % 8 == 0 and entry & 1 and
                BASE + BANK_SIZE <= (entry & ~1) < BASE + BANK_SIZE + len(m4)):
            raise ValueError('invalid M4 vectors')
        extra = [(BASE + BANK_SIZE, m4)]
    elif m4 is not None:
        raise ValueError('M4 image is only valid for H755')
    return sorted(extra + [(BASE, bootloader), (layout['slots'][0], application),
                   *[(address, record) for address in layout['metadata']]])


def hex_record(kind, address, data):
    payload = struct.pack('>BHB', len(data), address, kind) + data
    return ':' + (payload + bytes([-sum(payload) & 255])).hex().upper() + '\n'


def intel_hex(parts):
    output = []
    upper = None
    for address, data in parts:
        # Encode sparse data as 16-byte Intel HEX records; the programmer owns
        # device-specific flash-word packing. Do not materialize erased gaps.
        for offset in range(0, len(data), 16):
            at = address + offset
            if at >> 16 != upper:
                upper = at >> 16
                output.append(hex_record(4, 0, struct.pack('>H', upper)))
            output.append(hex_record(0, at & 0xffff, data[offset:offset + 16]))
    output.append(hex_record(1, 0, b''))
    return ''.join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('bootloader', 'application', 'layout', 'version', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument("--m4", type=Path)
    args = parser.parse_args()
    try:
        layout = json.loads(args.layout.read_text())
        version = json.loads(args.version.read_text())
        parts = regions(args.bootloader.read_bytes(), args.application.read_bytes(), layout, version, args.m4.read_bytes() if args.m4 else None)
        args.output.write_text(intel_hex(parts))
    except (ValueError, KeyError, struct.error) as error:
        parser.error(str(error))
    print(f'Factory image: confirmed A at {layout["slots"][0]:#010x}; main-flash mass erase required')


if __name__ == '__main__':
    main()
