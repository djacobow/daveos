#!/usr/bin/env python3
"""Build a streamed DaveOS OTA package from independently linked A/B binaries.

The target applies block-local +/- slot-displacement word patches. Unsupported
link differences fail packaging; --fixed-slot provides the explicit fallback.
"""
import argparse
import binascii
from pathlib import Path
import struct

HEADER_SIZE = 128
BLOCK_SIZE = 1024
MAGIC = 0x50534f44


def crc(data):
    return binascii.crc32(data)


def build_package(a, b, addresses, product, revision, version=(0, 1, 0xffffffff),
                  commit="unknown", dirty=False, fixed_slot=None):
    if not a or not b or (fixed_slot is None and len(a) != len(b)):
        raise ValueError("relocatable images must be nonempty and have equal sizes; use --fixed-slot if needed")
    if len(commit) > 40 or not commit.isascii():
        raise ValueError("Git commit must be at most 40 ASCII characters")
    delta = (addresses[1] - addresses[0]) & 0xffffffff
    if not delta or delta == 0x80000000:
        raise ValueError("invalid slot displacement")
    base = a if fixed_slot in (None, 0) else b
    payload = bytearray()
    reconstructed = bytearray(base)
    for offset in range(0, len(base), BLOCK_SIZE):
        block = base[offset:offset + BLOCK_SIZE]
        patches = []
        if fixed_slot is None:
            other = b[offset:offset + BLOCK_SIZE]
            for at in range(0, len(block), 4):
                before, after = block[at:at + 4], other[at:at + 4]
                if before == after:
                    continue
                if len(before) != 4 or len(after) != 4:
                    raise ValueError("unaligned final relocation")
                old, new = int.from_bytes(before, "little"), int.from_bytes(after, "little")
                change = (new - old) & 0xffffffff
                if change == delta:
                    code = at // 4
                elif change == (-delta & 0xffffffff):
                    code = 0x8000 | at // 4
                else:
                    raise ValueError(f"unsupported relocation at {offset + at:#x}; use --fixed-slot")
                patches.append(code)
                struct.pack_into("<I", reconstructed, offset + at,
                                 (old - delta if code & 0x8000 else old + delta) & 0xffffffff)
        payload += struct.pack("<III", offset, len(block), len(patches))
        payload += struct.pack(f"<{len(patches)}H", *patches)
        payload += block
    if fixed_slot is None and reconstructed != b:
        raise ValueError("reconstructed image differs from independent slot B link")
    header = bytearray(HEADER_SIZE)
    fields = [MAGIC, 1, HEADER_SIZE, product, revision, len(base), *addresses,
              crc(a), crc(b), 0 if fixed_slot is None else fixed_slot + 1,
              BLOCK_SIZE, *version]
    struct.pack_into("<15I", header, 0, *fields)
    header[60:60 + len(commit)] = commit.encode("ascii")
    header[101] = bool(dirty)
    struct.pack_into("<I", header, 104, HEADER_SIZE + len(payload))
    struct.pack_into("<I", header, 124, crc(header[:124]))
    return bytes(header + payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("a", "b", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    for name in ("address-a", "address-b", "product", "revision"):
        parser.add_argument("--" + name, type=lambda s: int(s, 0))
    parser.add_argument("--layout", type=Path)
    parser.add_argument("--version", type=Path)
    parser.add_argument("--major", type=int, default=0)
    parser.add_argument("--minor", type=int, default=1)
    parser.add_argument("--build-number", type=int, default=0xffffffff)
    parser.add_argument("--commit", default="unknown")
    parser.add_argument("--dirty", action="store_true")
    parser.add_argument("--fixed-slot", choices=("a", "b"))
    args = parser.parse_args()
    if args.layout:
        import json
        layout = json.loads(args.layout.read_text())
        args.address_a, args.address_b = layout['slots']
        args.product, args.revision = layout['product'], layout['revision']
    if any(value is None for value in (args.address_a, args.address_b, args.product, args.revision)):
        parser.error('provide --layout or both addresses, product and revision')
    if args.version:
        import json
        version = json.loads(args.version.read_text())
        args.major, args.minor, args.build_number = version['major'], version['minor'], version['build']
        args.commit, args.dirty = version['commit'], version['dirty']
    try:
        package = build_package(args.a.read_bytes(), args.b.read_bytes(),
            (args.address_a, args.address_b), args.product, args.revision,
            (args.major, args.minor, args.build_number), args.commit, args.dirty,
            None if args.fixed_slot is None else (0 if args.fixed_slot == "a" else 1))
    except (ValueError, struct.error) as error:
        parser.error(str(error))
    args.output.write_bytes(package)
    print(f"Wrote {len(package)} bytes to {args.output}")


if __name__ == "__main__":
    main()
