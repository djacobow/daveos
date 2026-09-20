import struct
import pytest

pytestmark = pytest.mark.hil


@pytest.mark.parametrize('kind,name,instruction,r0,enables,pc,cfsr_bit', [
    (3, 'usage_fault', 0xde00, 0, 0x70000, 0x20000400, 1 << 16),
    (2, 'bus_fault', 0x6800, 0xfffffffc, 0x70000, 0x20000400, 1 << 9),
    (1, 'memory_fault', 0xde00, 0, 0x70000, 0x08fff000, 1),
    (0, 'hard_fault', 0xde00, 0, 0x30000, 0x20000400, 1 << 16),
], ids=['usage', 'bus', 'memory', 'hard'])
def test_exception_capture(board, kind, name, instruction, r0, enables, pc, cfsr_bit):
    board.uart.drain()
    data = board.fault_experiment(f'''hbreak app::Health::Heartbeat
continue
delete breakpoints
if $primask != 0
 quit 1
end
set {{unsigned int}}0xe000ed24 = {enables}
set {{unsigned short}}0x20000400 = {instruction}
set $r0 = {r0}
jump *{pc:#x}''')
    frame = struct.unpack_from('<8I', data, 84)
    assert struct.unpack_from('<I', data, 12)[0] == kind
    assert frame[6] == pc and frame[7] & (1 << 24)
    assert struct.unpack_from('<I', data, 116)[0] & cfsr_bit
    if kind == 0:
        assert struct.unpack_from('<I', data, 120)[0] & (1 << 30)
    assert data[140:172].split(b'\0')[0].decode() == name
    board.ready()
    board.query(board.uart, 'health fault', f'Retained failure: {name}')
