import socket
import struct

import pytest
import ota

pytestmark = pytest.mark.hil


def test_latched_task_failure(board):
    board.uart.drain()
    data = board.fault_experiment('''hbreak app::Health::Heartbeat
continue
delete breakpoints
set var app::application.scheduler_.tasks_._M_elems[0].completed = 0
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
