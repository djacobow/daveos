import socket
import struct

import pytest
import ota

pytestmark = pytest.mark.hil


def test_latched_task_failure(board):
    board.uart.drain()
    data = board.fault_experiment(f'''hbreak app::Health::Heartbeat
continue
delete breakpoints
{board.task_progress_fault('health', 'Heartbeat')}
continue''')
    assert struct.unpack_from('<I', data, 12)[0] == 4
    assert b'task_progress' in data and b'Heartbeat' in data
    board.ready()
    board.query(board.uart, 'health fault', 'task_progress health.Heartbeat')


@pytest.mark.slow
def test_startup_trial_rollback(board):
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=30) as sock:
        ota.upload(sock, (board.firmware / 'application.ota').read_bytes())
    board.uart.drain()
    data = board.fault_experiment('''monitor reset halt
hbreak SystemClock_Config
continue
delete breakpoints
if $primask != 0
 quit 1
end
jump *Default_Handler''', slot='B')
    assert struct.unpack_from('<I', data, 12)[0] == 4
    assert b'iwdg_early_warning' in data
    board.uart.watch_for(r'Boot slot B \(trial\)', timeout=5)
    board.ready()
    board.query(board.uart, 'health fault', 'iwdg_early_warning')


def test_interrupt_masked_hang_resets_without_frame(board):
    board.uart.drain()
    board.prepare_execution('set $primask = 1\nset $pc = Default_Handler')
    board.ready()
    board.query(board.uart, 'health fault', 'No retained failure')
