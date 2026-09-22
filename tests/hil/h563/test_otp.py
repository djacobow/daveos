"""Bank-B emulator only; never programs physical OTP."""
import socket

import pytest
import build_config
import ota
from flash import tcl_word

pytestmark = pytest.mark.hil


@pytest.fixture
def otp_board(hil_config, request):
    build_config.require(hil_config['build'], 'otp-emulator')
    return request.getfixturevalue('board')


def serial(board, value, stream=None):
    stream = stream or board.uart
    # Processing is ordered on each source; the read follows the setter.
    stream.send(raw=f'otp serial set "{value}" "{value}"\r'.encode())
    board.query(stream, 'otp serial', f'serial: {value}')


def dump(board, name, address, size):
    path = board.output / name
    board.control(f'halt; dump_image {tcl_word(str(path))} {address:#x} {size}; resume')
    return path.read_bytes()


def test_emulator_commands_reset_and_factory(otp_board):
    board = otp_board
    board.query(board.uart, 'otp status', 'backend flash-emulator, ready yes, used 0, free 32')
    board.query(board.uart, 'otp serial', 'serial: unset')
    before = dump(board, 'before.bin', 0x08100000, 0x10000)
    serial(board, 'ABC 123')
    first = dump(board, 'first.bin', 0x08100000, 0x10000)
    assert first[:256] != before[:256]
    assert first[256:] == before[256:]
    board.query(board.uart, 'otp status', 'serial block 0, locked yes')
    for transport in ('uart', 'usb', 'tcp'):
        stream = board.uart if transport == 'uart' else board.connect(transport)
        serial(board, 'ABC 123', stream)
        board.query(stream, 'otp serial set "ABC 123" "wrong"', 'invalid_argument')
        if transport != 'uart':
            stream.close()
    assert dump(board, 'duplicate.bin', 0x08100000, 0x10000) == first
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()
    board.query(board.uart, 'otp serial', 'serial: ABC 123')
    board.query(board.uart, 'health fault', 'No retained failure')
    for i in range(1, 32):
        serial(board, f'SERIAL-{i:02}')
    board.query(board.uart, 'otp status', 'used 32, free 0')
    board.query(board.uart, 'otp serial set extra extra', 'status full')
    serial(board, 'SERIAL-31')
    final = dump(board, 'full.bin', 0x08100000, 0x10000)
    assert final[8192:] == before[8192:]
    # Every HIL run starts from factory; explicitly verify clearing as well.
    board.uart.close()
    board.factory()
    board.query(board.uart, 'otp serial', 'serial: unset')
    board.query(board.uart, 'otp status', 'used 0, free 32')


@pytest.mark.slow
def test_emulator_survives_bidirectional_ota(otp_board):
    board = otp_board
    serial(board, 'OTA-RETAIN')
    original = dump(board, 'otp-before.bin', 0x08100000, 8192)
    image = (board.firmware / 'application.ota').read_bytes()
    for destination in ('B', 'A'):
        board.query(board.uart, 'ota enable', 'OTA enabled')
        with socket.create_connection((board.ip, 1001), timeout=30) as sock:
            ota.upload(sock, image)
            board.uart.drain()
            assert ota.request(sock, 5)['status'] == 'ok'
        board.ready(destination, trial=True)
        board.query(board.uart, 'otp serial', 'serial: OTA-RETAIN')
        board.query(board.uart, 'otp status', 'serial block 0, locked yes')
        assert dump(board, f'otp-{destination}.bin', 0x08100000, 8192) == original
        board.query(board.uart, 'health fault', 'No retained failure')


def test_emulator_torn_lock_retries_next_cell(otp_board):
    import binascii
    import struct

    board = otp_board
    # Independently encode a complete record and a torn lock marker into fresh
    # emulator flash, then reset. This is deterministic reset/fault injection,
    # not a physical power-interruption claim and not an OTP fuse operation.
    header = struct.pack('<HH', 1, 3)
    payload = b'ABC' + b'\xff' * 53
    record = header + struct.pack('<I', binascii.crc32(header + payload)) + payload
    body = b'\xff\xff' + record[2:]

    def marker(magic, tag, extra=b''):
        prefix = struct.pack('<III', magic, 0, tag)
        return prefix + struct.pack('<I', binascii.crc32(prefix + extra))

    torn = (marker(0x3143504f, 1) + body + marker(0x3144504f, 1, body)
            + b'\x4f' + b'\xff' * 15)
    path = board.output / 'torn-lock.bin'
    path.write_bytes(torn)
    board.control(f'reset halt; flash write_image {tcl_word(str(path))} 0x08100000; verify_image {tcl_word(str(path))} 0x08100000')
    board.uart.drain()
    board.control('reset run')
    board.ready()
    board.query(board.uart, 'otp serial', 'serial: ABC')
    board.query(board.uart, 'otp status', 'serial block 0, locked no')
    board.query(board.uart, 'otp serial set ABC wrong', 'invalid_argument')
    assert dump(board, 'before-retry.bin', 0x08100000, 112) == torn
    serial(board, 'ABC')
    board.query(board.uart, 'otp status', 'serial block 0, locked yes')
    result = dump(board, 'after-retry.bin', 0x08100000, 8192)
    assert result[:112] == torn
    assert result[112:128] == marker(0x314c504f, struct.unpack_from('<I', record, 4)[0])
    assert result[128:] == b'\xff' * (8192 - 128)


@pytest.mark.slow
def test_emulator_writes_during_ota_in_both_banks(otp_board):
    import re

    board = otp_board
    image = (board.firmware / 'application.ota').read_bytes()
    current = 'CONCURRENT-START'
    serial(board, current)
    for destination in ('B', 'A'):
        attempts = []

        def progress(message):
            nonlocal current
            match = re.match(r'Sent (\d+)%', message)
            if not match or int(match[1]) not in (0, 30, 60, 90):
                return
            wanted = f'CONCURRENT-{destination}-{match[1]}'
            board.uart.send(raw=f'otp serial set {wanted} {wanted}\r'.encode())
            result = board.query(board.uart, 'otp serial', r'serial: (CONCURRENT-[A-Z0-9-]+)')[1]
            # Busy is allowed before touching the row; a successful write must
            # publish exactly the requested value. OTA must still complete.
            assert result in (current, wanted)
            attempts.append(result == wanted)
            current = result

        board.query(board.uart, 'ota enable', 'OTA enabled')
        with socket.create_connection((board.ip, 1001), timeout=30) as sock:
            ota.upload(sock, image, progress=progress)
            assert len(attempts) == 4
            print(f'OTA ->{destination}: {sum(attempts)}/4 simultaneous serial writes accepted')
            serial(board, current)
            board.uart.drain()
            assert ota.request(sock, 5)['status'] == 'ok'
        board.ready(destination, trial=True)
        board.query(board.uart, 'otp serial', f'serial: {current}')
        board.query(board.uart, 'health fault', 'No retained failure')
        serial(board, f'CONCURRENT-{destination}-CONFIRMED')
        current = f'CONCURRENT-{destination}-CONFIRMED'
