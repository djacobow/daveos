import binascii
import socket
import struct
import threading
import time

import pytest
import ota

pytestmark = [pytest.mark.hil, pytest.mark.slow]


def test_rejection_and_bidirectional_update(board):
    image = (board.firmware / 'application.ota').read_bytes()
    # Factory startup leaves the upload service application-disabled.
    with pytest.raises((ConnectionRefusedError, socket.timeout)):
        with socket.create_connection((board.ip, 1001), timeout=1):
            pass
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=5) as sock:
        assert ota.request(sock, 1, image[:128])['status'] == 'ok'
        deadline = time.monotonic() + 10
        while not ota.request(sock, 2)['ready']:
            assert time.monotonic() < deadline, 'OTA not ready'
            time.sleep(.02)
        chunk = image[128:1152]
        payload = struct.pack('<II', 0, binascii.crc32(chunk) ^ 1) + chunk
        assert ota.request(sock, 3, payload)['status'] == 'checksum_error'
        payload = struct.pack('<II', 0, binascii.crc32(chunk)) + chunk
        assert ota.request(sock, 3, payload)['status'] == 'ok'
    deadline = time.monotonic() + 5
    while True:
        match = board.query(board.uart, 'ota status', r'OTA (\w+)')
        if match.group(1) == 'failed':
            break
        assert time.monotonic() < deadline, 'disconnect did not abort OTA'
        time.sleep(.05)
    board.query(board.uart, 'boot status', 'Slot A: confirmed')
    for destination in ('B', 'A'):
        board.query(board.uart, 'ota enable', 'OTA enabled')
        tcp = board.connect('tcp')
        stop = threading.Event()
        results, failures = [], []

        def monitor():
            try:
                while not stop.is_set():
                    before = time.monotonic()
                    board.query(tcp, 'board timer 10000', 'Timer fired', timeout=2)
                    results.append(time.monotonic() - before)
                    stop.wait(.2)
            except Exception as error:
                failures.append(error)

        worker = threading.Thread(target=monitor)
        worker.start()
        try:
            with socket.create_connection((board.ip, 1001), timeout=30) as sock:
                ota.upload(sock, image)
                stop.set()
                worker.join(3)
                assert not worker.is_alive() and results and not failures, failures
                tcp.close()
                board.uart.drain()
                assert ota.request(sock, 5)['status'] == 'ok'
        finally:
            stop.set()
            worker.join(3)
            tcp.close()
        board.ready(destination, trial=True)
        print(f'OTA ->{destination}: {len(results)} concurrent timers, max {max(results):.3f}s')


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
