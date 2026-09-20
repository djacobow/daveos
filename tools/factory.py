#!/usr/bin/env python3
"""Build an H563 factory HEX: bootloader, confirmed A, redundant metadata.

Program with a main-flash mass erase first. Unlisted bytes (including bank 2's
placeholder and application slot) remain erased; OTP is never part of this file.
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
    struct.pack_into('<IIQQ', record, 0, 0x4a534f44, 1, 1, 1)
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
    struct.pack_into('<I', record, 236, binascii.crc32(record[:236]))
    struct.pack_into('<IQI', record, 240, 0x454e4f44, 1, 0xbab1b0bb)
    return bytes(record)


def regions(bootloader, application, layout, version):
    reserve = layout['boot_reservation']
    sector = layout['sector_size']
    if (reserve != 32768 or sector != 8192 or layout['write_size'] != 16 or
            layout['metadata'] != [BASE + reserve, BASE + BANK_SIZE + reserve] or
            layout['slots'] != [BASE + reserve + sector, BASE + BANK_SIZE + reserve + sector] or
            layout['slot_size'] != BANK_SIZE - reserve - sector):
        raise ValueError('unexpected H563 factory layout')
    if not 8 <= len(bootloader) <= reserve or not 8 <= len(application) <= layout['slot_size']:
        raise ValueError('image is empty, truncated, or exceeds its flash reservation')
    for image, address in ((bootloader, BASE), (application, layout['slots'][0])):
        stack, entry = struct.unpack_from('<II', image)
        if (not 0x20000800 < stack <= 0x200a0000 or stack % 8 or not entry & 1 or
                not address <= (entry & ~1) < address + len(image)):
            raise ValueError(f'invalid vectors for image at {address:#010x}')
    record = metadata(application, layout, version)
    return sorted([(BASE, bootloader), (layout['slots'][0], application),
                   *[(address, record) for address in layout['metadata']]])


def hex_record(kind, address, data):
    payload = struct.pack('>BHB', len(data), address, kind) + data
    return ':' + (payload + bytes([-sum(payload) & 255])).hex().upper() + '\n'


def intel_hex(parts):
    output = []
    upper = None
    for address, data in parts:
        # Include only programmed 16-byte flash words, not erased gaps.
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
    args = parser.parse_args()
    try:
        layout = json.loads(args.layout.read_text())
        version = json.loads(args.version.read_text())
        parts = regions(args.bootloader.read_bytes(), args.application.read_bytes(), layout, version)
        args.output.write_text(intel_hex(parts))
    except (ValueError, KeyError, struct.error) as error:
        parser.error(str(error))
    print(f'Factory image: confirmed A at {layout["slots"][0]:#010x}; main-flash mass erase required')


if __name__ == '__main__':
    main()
