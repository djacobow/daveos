"""Shared H563/H755 OTA checks, called only by factory-provisioned HIL tests."""
import binascii
import socket
import struct
import threading
import time

import pytest
import ota
from memory_cases import inspect_memory

def check_rejection_and_bidirectional_update(board):
    image = (board.firmware / 'application.ota').read_bytes()
    # Factory startup leaves the upload service application-disabled.
    with pytest.raises((ConnectionRefusedError, socket.timeout)):
        with socket.create_connection((board.ip, 1001), timeout=1):
            pass
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=5) as sock:
        assert ota.request(sock, 1, image[:128])['status'] == 'ok'
        deadline = time.monotonic() + 10
        while True:
            state = ota.request(sock, 2)
            if state['ready']:
                break
            assert state['state'] != 'failed', state
            assert time.monotonic() < deadline, state
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
                source = 'A' if destination == 'B' else 'B'
                inspect_memory(board, f'upload-from-{source}', slot=source)
                board.uart.drain()
                assert ota.request(sock, 5)['status'] == 'ok'
        finally:
            stop.set()
            worker.join(3)
            tcp.close()
        board.ready(destination, trial=True)
        inspect_memory(board, f'boot-{destination}', slot=destination)
        summary = f'OTA ->{destination}: {len(results)} concurrent timers, max {max(results):.3f}s'
        print(summary)
        with (board.output / 'ota-timing.log').open('a') as log:
            log.write(summary + '\n')
