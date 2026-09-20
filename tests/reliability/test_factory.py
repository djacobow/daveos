"""Factory image bounds, address mapping, checksums, and programming plans."""
from pathlib import Path
import struct
import json
import subprocess
import tempfile
import sys
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import factory
import flash


class FactoryTests(unittest.TestCase):
    def setUp(self):
        self.layout = dict(boot_reservation=32768, sector_size=8192, write_size=16,
                           metadata=[0x08008000, 0x08108000], slots=[0x0800a000, 0x0810a000],
                           slot_size=984 * 1024, product=0x563, revision=1)
        self.version = dict(major=0, minor=1, build=0xffffffff, commit='abc123', dirty=True)
        self.boot = struct.pack('<II', 0x200a0000, 0x08000009) + bytes(17)
        self.app = struct.pack('<II', 0x200a0000, 0x0800a009) + bytes(100001)

    def test_roundtrip_sparse_hex(self):
        parts = factory.regions(self.boot, self.app, self.layout, self.version)
        memory, upper = {}, 0
        for line in factory.intel_hex(parts).splitlines():
            data = bytes.fromhex(line[1:])
            self.assertEqual(sum(data) & 255, 0)
            length, offset, kind = struct.unpack_from('>BHB', data)
            self.assertEqual(len(data), length + 5)
            if kind == 4:
                upper = int.from_bytes(data[4:6], 'big') << 16
            elif kind == 0:
                for i, byte in enumerate(data[4:-1]):
                    self.assertNotIn(upper + offset + i, memory)
                    memory[upper + offset + i] = byte
        self.assertEqual(memory, {address + i: byte for address, data in parts
                                  for i, byte in enumerate(data)})
        self.assertFalse(any(0x0810a000 <= at < 0x08200000 for at in memory))
        self.assertEqual(parts[1][1], parts[3][1])

    def test_reject_bad_vectors_bounds_and_layout(self):
        for app in (b'', self.app[:4], self.app + bytes(self.layout['slot_size']),
                    struct.pack('<II', 0x200a0000, 0x08000009) + self.app[8:]):
            with self.assertRaises(ValueError):
                factory.regions(self.boot, app, self.layout, self.version)
        with self.assertRaises(ValueError):
            factory.regions(self.boot, self.app, dict(self.layout, boot_reservation=24576), self.version)

    def test_factory_programming_erases_before_writing(self):
        cube = flash.command('cubeprogrammer', 'programmer', 'stm32h5', [Path('factory.hex')], '', True)
        self.assertLess(cube.index('-e'), cube.index('-d'))
        self.assertEqual(cube[cube.index('-e') + 1], 'all')
        ocd = flash.command('openocd', 'openocd', 'stm32h5', [Path('factory.hex')], '', True)
        self.assertLess(ocd.index('stm32h5x mass_erase 0'),
                        ocd.index('flash write_image erase "factory.hex"'))
        self.assertNotIn('-e', flash.command('cubeprogrammer', 'programmer', 'stm32h5', [], ''))

    def test_paired_package_uses_generated_layout_and_version(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            a = bytearray(self.app)
            b = bytearray(a)
            struct.pack_into('<I', b, 4, 0x0810a009)
            (root / 'a.bin').write_bytes(a)
            (root / 'b.bin').write_bytes(b)
            (root / 'layout.json').write_text(json.dumps(self.layout))
            (root / 'version.json').write_text(json.dumps(self.version))
            subprocess.run([sys.executable, '-B', str(Path(factory.__file__).with_name('package.py')),
                            '--a', str(root / 'a.bin'), '--b', str(root / 'b.bin'),
                            '--layout', str(root / 'layout.json'), '--version', str(root / 'version.json'),
                            '--output', str(root / 'image.ota')], check=True, capture_output=True)
            data = (root / 'image.ota').read_bytes()
            self.assertEqual(struct.unpack_from('<II', data, 24), tuple(self.layout['slots']))
            self.assertEqual(struct.unpack_from('<III', data, 48), (0, 1, 0xffffffff))
            self.assertEqual(data[60:66], b'abc123')
            self.assertEqual(data[101], 1)


if __name__ == '__main__':
    unittest.main()
