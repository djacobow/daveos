"""Independent host reference package for the C++ streaming/flash tests."""
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from package import build_package
from factory import metadata

a = bytearray((i % 251 for i in range(997)))
b = bytearray(a)
struct.pack_into("<I", a, 0, 0x401)
struct.pack_into("<I", b, 0, 0xc01)
struct.pack_into("<I", a, 500, 0xffff9000)
struct.pack_into("<I", b, 500, 0xffff8800)
Path(sys.argv[1]).write_bytes(build_package(a, b, (1024, 3072), 1, 1))
Path(sys.argv[2]).write_bytes(b)
Path(sys.argv[3]).write_bytes(metadata(a, {'product': 1, 'revision': 1},
    {'major': 2, 'minor': 3, 'build': 0xffffffff, 'commit': 'abc123', 'dirty': True}))

# A separate multi-block image exercises streaming across block/patch headers.
if len(sys.argv) > 4:
    large_a = bytearray((i % 251 for i in range(5003)))
    large_b = bytearray(large_a)
    for offset in (0, 1020, 1024, 2048, 4096):
        value = struct.unpack_from('<I', large_a, offset)[0]
        struct.pack_into('<I', large_b, offset, (value + 0x3000) & 0xffffffff)
    value = struct.unpack_from('<I', large_a, 2044)[0]
    struct.pack_into('<I', large_b, 2044, (value - 0x3000) & 0xffffffff)
    Path(sys.argv[4]).write_bytes(build_package(large_a, large_b, (4096, 16384), 1, 1))
    Path(sys.argv[5]).write_bytes(large_b)
