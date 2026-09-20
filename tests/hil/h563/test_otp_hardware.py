"""Read-only real OTP qualification. Never send setter or lock commands.

The DUT may be provisioned. Do not assert erased data, zero locks, a particular
serial, or that an unfamiliar record is ours. Write/failure tests belong in the
host/flash emulators. Initial fuse write qualification is deliberate manual work.
"""
import json
from pathlib import Path

import pytest

pytestmark = pytest.mark.hil


@pytest.fixture
def hardware_otp_board(hil_config, request):
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    if not any(o['name'] == 'otp_backend' and o['value'] == 'h563' for o in options):
        pytest.skip('requires real h563 backend with provisioning disabled')
    return request.getfixturevalue('board')


def scan(board):
    records = []
    for index in range(32):
        match = board.query(board.uart, f'otp_hw inspect {index}',
                            rf'block {index}: virgin (\d+) programmed (\d+) unreadable (\d+) locked (yes|no) crc ([0-9a-f]{{8}})')
        result = match.groups()
        assert sum(int(n) for n in result[:3]) == 32
        records.append(result)
    return records


def test_read_only_scan_and_reset(hardware_otp_board):
    board = hardware_otp_board
    board.query(board.uart, 'otp status', 'backend h563-otp-readonly, ready yes')
    board.query(board.uart, 'otp serial', r'serial: .*')
    before = scan(board)
    assert scan(board) == before
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()
    assert scan(board) == before
    board.query(board.uart, 'health fault', 'No retained failure')
    (board.output / 'otp-audit.json').write_text(json.dumps(before, indent=2) + '\n')
