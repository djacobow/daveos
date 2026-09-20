"""H755 M7 fault frames and IWDG1 recovery; no OTP/option-byte writes."""
import struct
import pytest

pytestmark = pytest.mark.hil


@pytest.mark.parametrize('floating', [False, True], ids=['basic', 'floating-point'])
def test_fault_capture(board, floating):
    # Execute a fault from ITCM. Avoid flash mutation and cacheable test code.
    fp = 'set {unsigned int}0x00000400 = 0x0a10ee00\nset {unsigned short}0x00000404 = 0xde00' if floating else 'set {unsigned short}0x00000400 = 0xde00'
    pc = 0x404 if floating else 0x400
    data = board.fault_experiment(f"""hbreak app::Health::Heartbeat
continue
delete breakpoints
set {{unsigned int}}0xe000ed24 = 0x70000
{fp}
set $r0 = 0x12345678
jump *0x00000400""")
    assert struct.unpack_from('<I', data, 12)[0] == 3
    assert struct.unpack_from('<I', data, 84)[0] == 0x12345678
    assert struct.unpack_from('<I', data, 108)[0] == pc
    if floating:
        assert not (struct.unpack_from('<I', data, 132)[0] & 0x10)
    board.ready()
    board.query(board.uart, 'health fault', 'Retained failure: usage_fault')
    board.query(board.uart, 'health clear', 'Retained failure cleared')


def test_latched_task_failure(board):
    board.uart.drain()
    board.prepare_execution('set var app::application.scheduler_.tasks_._M_elems[0].completed = 0')
    board.ready()
    board.query(board.uart, 'health fault', 'task_progress health.Heartbeat')
    board.query(board.uart, 'health fault', 'Exception frame unavailable')


def test_interrupt_masked_hang(board):
    board.uart.drain()
    board.prepare_execution('set $primask = 1\nset $pc = Default_Handler')
    board.ready()
    board.query(board.uart, 'health fault', 'No retained failure')


def test_initialization_hang(board):
    board.uart.drain()
    board.prepare_execution('''monitor reset halt
monitor targets stm32h7x.cpu1
monitor resume
monitor targets stm32h7x.cpu0
hbreak SystemClock_Config
continue
delete breakpoints
set $pc = Default_Handler''')
    board.ready()
    board.query(board.uart, 'health fault', 'No retained failure')


@pytest.mark.parametrize('kind,name,instruction,r0,enables,pc,cfsr', [
    (2, 'bus_fault', 0x6800, 0xfffffffc, 0x70000, 0x400, 1 << 9),
    (1, 'memory_fault', 0xde00, 0, 0x70000, 0x40000000, 1),
    (0, 'hard_fault', 0xde00, 0, 0x30000, 0x400, 1 << 16),
])
def test_other_faults(board, kind, name, instruction, r0, enables, pc, cfsr):
    data = board.fault_experiment(f"""hbreak app::Health::Heartbeat
continue
delete breakpoints
set {{unsigned int}}0xe000ed24 = {enables}
set {{unsigned short}}0x00000400 = {instruction}
set $r0 = {r0}
jump *{pc:#x}""")
    assert struct.unpack_from('<I', data, 12)[0] == kind
    assert struct.unpack_from('<I', data, 108)[0] == pc
    assert struct.unpack_from('<I', data, 116)[0] & cfsr
    board.ready()
    board.query(board.uart, 'health fault', f'Retained failure: {name}')


def test_initialization_failure(board):
    board.uart.drain()
    board.prepare_execution('set $pc = app_init_failed')
    board.ready()
    board.query(board.uart, 'health fault', 'initialization hardware.')


def test_debugger_pause(board):
    import time
    board.control('halt')
    try:
        time.sleep(6)  # Longer than the five-second watchdog timeout.
    finally:
        board.control('resume')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
