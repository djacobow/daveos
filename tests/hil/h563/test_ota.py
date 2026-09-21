import binascii
import socket
import struct
import threading
import time

import pytest
import ota

pytestmark = [pytest.mark.hil, pytest.mark.slow]


def test_rejection_and_bidirectional_update(board):
    from update_cases import check_rejection_and_bidirectional_update
    check_rejection_and_bidirectional_update(board)


@pytest.mark.parametrize('failure', ['timeout', 'disable', 'reset', 'link'])
def test_interrupted_upload_restarts_from_zero(board, failure):
    from ota_helpers import wait_status

    image = (board.firmware / 'application.ota').read_bytes()
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=5) as sock:
        assert ota.request(sock, 1, image[:128])['status'] == 'ok'
        wait_status(sock, lambda s: s['ready'])
        chunk = image[128:1152]
        assert ota.request(sock, 3, struct.pack('<II', 0, binascii.crc32(chunk)) + chunk)['status'] == 'ok'
        wait_status(sock, lambda s: s['ready'] and s['offset'] == len(chunk))
        if failure == 'timeout':
            result = wait_status(sock, lambda s: s['state'] == 'failed', timeout=40)
            assert result['status'] == 'timeout'
        elif failure == 'disable':
            board.query(board.uart, 'ota disable', 'OTA disabled')
            deadline = time.monotonic() + 5
            with pytest.raises((OSError, RuntimeError)):
                while True:
                    assert time.monotonic() < deadline, 'disabled listener stayed open'
                    ota.request(sock, 2)
                    time.sleep(.02)
        elif failure == 'reset':
            board.uart.drain()
            board.uart.send('board reset')
            board.ready()
        else:
            # Real PHY link loss, without requiring a person to unplug a cable.
            board.phy_power(False)
            try:
                deadline = time.monotonic() + 5
                while True:
                    match = board.query(board.uart, 'ota status', r'OTA (\w+)')
                    if match.group(1) == 'failed':
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.05)
            finally:
                board.phy_power(True)
            board.uart.watch_for(r'ready, link .*IP (?!0\.0\.0\.0)', timeout=20)
    board.query(board.uart, 'boot status', 'Slot A: confirmed')
    board.query(board.uart, 'ota enable', 'OTA enabled')
    # New sessions discard the old partial transfer; they cannot resume offset.
    deadline = time.monotonic() + 10
    while True:
        try:
            fresh = socket.create_connection((board.ip, 1001), timeout=1)
            break
        except OSError:
            assert time.monotonic() < deadline
            time.sleep(.05)
    with fresh:
        assert ota.request(fresh, 1, image[:128])['status'] == 'ok'
        result = wait_status(fresh, lambda s: s['ready'])
        assert result['offset'] == 0
        assert ota.request(fresh, 4)['status'] == 'ok'


def test_replace_pending_image_before_reboot(board):
    from ota_helpers import wait_status

    image = (board.firmware / 'application.ota').read_bytes()
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=30) as sock:
        ota.upload(sock, image)
        # A second begin must invalidate the previously complete pending B.
        assert ota.request(sock, 1, image[:128])['status'] == 'ok'
        wait_status(sock, lambda s: s['ready'])
        assert ota.request(sock, 4)['status'] == 'ok'
        wait_status(sock, lambda s: s['state'] == 'failed')
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()  # B is now incomplete, so the old confirmed A must boot.
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=30) as sock:
        ota.upload(sock, image)
        ota.upload(sock, image)  # Repeated successful installs without reboot.
        board.uart.drain()
        assert ota.request(sock, 5)['status'] == 'ok'
    board.ready('B', trial=True)
