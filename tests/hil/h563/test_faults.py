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


@pytest.mark.parametrize('stack,limit,error', [
    (0x2009d000, 0, 0),
    (0x2009d010, 0x2009d000, 1 << 20),
    (0x60000000, 0, 1 << 12),
], ids=['psp', 'stack-limit', 'unreadable-psp'])
def test_process_stack_capture(board, stack, limit, error):
    # Valid storage is unused stack space, not .bss; the third case is unmapped.
    invalid = bool(error)
    binary, labels = board.stub('psp', f'''ldr r0, ={limit:#x}
msr psplim, r0
ldr r0, ={stack:#x}
msr psp, r0
movs r0, #2
msr control, r0
isb
fault: udf #0
''')
    board.uart.drain()
    data = board.fault_experiment(f'''hbreak app::Health::Heartbeat
continue
delete breakpoints
set {{unsigned int}}0xe000ed24 = 0x70000
restore {binary} binary 0x20000400
jump *0x20000400''', frame_valid=not invalid)
    assert struct.unpack_from('<I', data, 132)[0] & 4  # EXC_RETURN selects PSP.
    if invalid:
        assert struct.unpack_from('<I', data, 116)[0] & error
        assert data[84:116] == bytes(32)
    else:
        assert struct.unpack_from('<I', data, 136)[0] == stack - 32
        assert struct.unpack_from('<I', data, 108)[0] == labels['fault']
    board.ready()
    board.query(board.uart, 'health fault', 'Retained failure:')


def test_fault_resets_without_debugger(board):
    import binascii
    from flash import tcl_word

    board.uart.drain()
    # Wait for a RAM flag so core debugging is off before the actual fault.
    binary, labels = board.stub('unattended', '''ldr r0, =0x20000440
wait: ldr r1, [r0]
cmp r1, #0
beq wait
ldr r0, =0xe000edf0
ldr r1, [r0]
ldr r0, =0x20000444
str r1, [r0]
fault: udf #0
''')
    board.prepare_execution(f'''set {{unsigned int}}0xe000ed24 = 0x70000
restore {binary} binary 0x20000400
set {{unsigned int}}0x20000440 = 0
set $pc = 0x20000400''')
    try:
        # Keep ST-LINK physically connected, but disable core debug and prevent
        # OpenOCD polling until the CPU has captured the fault and rebooted.
        board.control('poll off; mww 0xe000edf0 0xa05f0000; mww 0x20000440 1')
        board.ready()
        board.query(board.uart, 'health fault', 'Retained failure: usage_fault')
    finally:
        board.control('poll on')
    debug = board.output / 'debug-state.bin'
    # Keep this evidence beside the injection flag in reserved scratch RAM;
    # ordinary stack space is painted again during the recovery boot.
    board.control(f'dump_image {tcl_word(str(debug))} 0x20000444 4')
    assert not (struct.unpack('<I', debug.read_bytes())[0] & 1)
    record = board.output / 'unattended-retained.bin'
    board.control(f'dump_image {tcl_word(str(record))} 0x20000000 256')
    data = record.read_bytes()
    assert data[70]
    assert struct.unpack_from('<I', data, 12)[0] == 3
    assert struct.unpack_from('<I', data, 108)[0] == labels['fault']
    assert binascii.crc32(data[:252]) == struct.unpack_from('<I', data, 252)[0]
