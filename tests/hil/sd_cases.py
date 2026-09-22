"""Optional read-only SPI A SD fixture; never writes or formats card sectors."""
import json
import re
from pathlib import Path

import pytest

from memory_cases import inspect_memory

pytestmark = pytest.mark.hil


@pytest.fixture
def spi_board(hil_config, request):
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    if not any(o['name'] == 'spi_sd_probe' and o['value'] for o in options):
        pytest.skip('requires -Dspi_sd_probe=true and an SD card on the selected board SPI1 pins')
    return request.getfixturevalue('board')


def test_read_only_sd_inspection(spi_board, hil_config):
    board = spi_board
    for _ in range(5):
        board.query(board.uart, 'sd probe', r'SD ready: SPI1 1000000 Hz, OCR 0x[0-9a-f]+', timeout=15)
    counters = board.query(board.uart, 'sd stats',
                           r'SPI1 IRQs=(\d+) polls=(\d+) DMA chunks=(\d+) bytes=(\d+)')
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    dma = any(
        o['name'] == 'spi_sd_dma' and o['value'] for o in options)
    assert int(counters[1]) > 0 and int(counters[2]) > 0
    if dma:
        assert int(counters[3]) >= 30 and int(counters[4]) == int(counters[3]) * 512
    else:
        assert int(counters[3]) == int(counters[4]) == 0
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')

    transcript = '\n'.join(path.read_text() for path in board.output.glob('uart-*.log'))
    assert 'SD probe failed' not in transcript
    assert transcript.count('SD inspection complete: CRC verified, repeated reads match') == 5
    assert len(re.findall(r'SD capacity: \d+ MiB', transcript)) == 5
    assert transcript.count('SD CID: manufacturer') == 5
    assert transcript.count('SD LBA 0: 3 CRC-checked reads at 250000 Hz match') == 5
    assert transcript.count('SD LBA 0: 3 CRC-checked reads at 1000000 Hz match slow baseline') == 5
    assert 'SD partition' in transcript or 'SD layout:' in transcript


def test_read_only_filesystem(spi_board, hil_config):
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    if not any(o['name'] == 'fatfs' and o['value'] for o in options):
        pytest.skip('requires -Dfatfs=true')
    board = spi_board
    board.query(board.uart, 'sd probe', r'SD ready: SPI1 1000000 Hz', timeout=15)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only', timeout=15)
    board.query(board.uart, 'sd probe', r'busy', timeout=5)
    board.query(board.uart, 'fs ls', 'Filesystem request complete', timeout=30)
    board.query(board.uart, 'fs read /daveos-nonexistent-test.txt', 'Filesystem: no_file', timeout=15)
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only', timeout=15)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    inspect_memory(board, 'filesystem')


def test_create_file_opt_in(spi_board, hil_config):
    """Only writes when the operator supplies a new path explicitly."""
    import os
    name = os.environ.get('DAVEOS_HIL_SD_WRITE_PATH')
    if not name:
        pytest.skip('set DAVEOS_HIL_SD_WRITE_PATH to authorize new-file creation')
    assert re.fullmatch(r'[A-Za-z0-9_-]+\.txt', name), 'use a simple root-level .txt name'
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    if not any(o['name'] == 'fatfs' and o['value'] for o in options):
        pytest.skip('requires -Dfatfs=true')
    board = spi_board
    text = 'DaveOS SD write test: create, sync, close, remount, and verify.'
    board.query(board.uart, 'sd probe', r'SD ready: SPI1 1000000 Hz', timeout=15)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    board.query(board.uart, f'fs create "{name}" "{text}"', 'Filesystem: write_protected')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
    result = board.query(board.uart, f'fs create "{name}" "{text}"',
                         r'Filesystem( request complete|:.*)', timeout=30)
    assert result[1] == ' request complete', result[0]
    board.query(board.uart, f'fs create "{name}" "must not replace"', 'Filesystem: exists', timeout=15)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted', timeout=15)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    board.query(board.uart, f'fs read "{name}"', 'Filesystem request complete', timeout=15)
    transcript = '\n'.join(path.read_text() for path in board.output.glob('uart-*.log'))
    rows = re.findall(r'fs\.Poll\s+: [0-9a-f]{8}  ([0-9a-f]+)  ', transcript)
    assert bytes.fromhex(''.join(rows)) == text.encode()
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    board.query(board.uart, f'fs rm "{name}"', 'Filesystem: write_protected')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
    board.query(board.uart, 'fs rm /', r'Filesystem: (denied|invalid_name)')
    result = board.query(board.uart, f'fs rm "{name}"',
                         r'Filesystem( request complete|:.*)', timeout=30)
    assert result[1] == ' request complete', result[0]
    board.query(board.uart, f'fs rm "{name}"', 'Filesystem: no_file')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    board.query(board.uart, f'fs read "{name}"', 'Filesystem: no_file')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    inspect_memory(board, 'filesystem-write')


def _card_package(board, path):
    """Read the operator's package through the text-only file preview command."""
    import binascii
    import struct

    def read(offset, count):
        log = board.output / 'uart-1.log'
        start = log.stat().st_size
        board.query(board.uart, f'fs read "{path}" {offset} {count}',
                    'Filesystem request complete', timeout=15)
        text = log.read_bytes()[start:].decode(errors='replace')
        data = bytearray()
        for at, hex_bytes in re.findall(r'fs\.Poll\s+: ([0-9a-f]{8})  ([0-9a-f]+)  ', text):
            assert int(at, 16) == offset + len(data)
            data += bytes.fromhex(hex_bytes)
        assert len(data) == count
        return data

    header = read(0, 128)
    assert binascii.crc32(header[:124]) == struct.unpack_from('<I', header, 124)[0]
    size = struct.unpack_from('<I', header, 104)[0]
    assert 128 < size < 2 * 1024 * 1024
    package = header
    while len(package) < size:
        package += read(len(package), min(4096, size - len(package)))
    (board.output / 'card-package.ota').write_bytes(package)
    return bytes(package)


def _package_images(package):
    """Independent reconstruction, including both directions of relocation."""
    import binascii
    import struct
    fields = struct.unpack_from('<12I', package)
    magic, version, header_size, _, _, size, address_a, address_b, crc_a, crc_b, encoding, block_size = fields
    assert (magic, version, header_size, encoding, block_size) == (0x50534f44, 1, 128, 0, 1024)
    images = [bytearray(), bytearray()]
    cursor = 128
    while cursor < len(package):
        offset, length, patches = struct.unpack_from('<3I', package, cursor)
        cursor += 12
        assert offset == len(images[0]) and 0 < length <= 1024 and patches <= 256
        codes = struct.unpack_from(f'<{patches}H', package, cursor)
        cursor += 2 * patches
        block = package[cursor:cursor + length]
        assert len(block) == length
        cursor += length
        relocated = bytearray(block)
        previous = -1
        for code in codes:
            at = (code & 255) * 4
            assert not code & 0x7f00 and previous < at and at + 4 <= length
            previous = at
            value = struct.unpack_from('<I', relocated, at)[0]
            delta = address_b - address_a
            value = value - delta if code & 0x8000 else value + delta
            struct.pack_into('<I', relocated, at, value & 0xffffffff)
        images[0] += block
        images[1] += relocated
    assert cursor == len(package) and len(images[0]) == len(images[1]) == size
    assert binascii.crc32(images[0]) == crc_a and binascii.crc32(images[1]) == crc_b
    return dict(A=bytes(images[0]), B=bytes(images[1]))


def test_sd_foreign_update_opt_in(spi_board, hil_config):
    """Reject an operator-supplied package for a different product, read-only."""
    import os
    import struct
    path = os.environ.get('DAVEOS_HIL_SD_FOREIGN_PATH')
    if not path:
        pytest.skip('set DAVEOS_HIL_SD_FOREIGN_PATH to a foreign-board package')
    assert '"' not in path and '\r' not in path and '\n' not in path
    board = spi_board
    board.query(board.uart, 'sd probe', 'SD ready', timeout=15)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    package = _card_package(board, path)
    product = 0x755 if hil_config.get('board') == 'h755' else 0x563
    assert struct.unpack_from('<I', package, 12)[0] != product
    board.query(board.uart, f'ota init "{path}"', 'OTA SD failed: incompatible')
    board.query(board.uart, 'ota install', 'not_running')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')


def test_sd_update_opt_in(spi_board, hil_config):
    """Install an operator-copied package A->B->A; never write SD files/OTP."""
    import hashlib
    import os
    import socket
    import threading
    import time
    import ota
    from flash import tcl_word

    path = os.environ.get('DAVEOS_HIL_SD_UPDATE_PATH')
    if not path:
        pytest.skip('copy application.ota to SD and set DAVEOS_HIL_SD_UPDATE_PATH')
    if not hil_config.get('bootloader', False):
        pytest.skip('requires bootloader=true')
    assert '"' not in path and '\r' not in path and '\n' not in path
    board = spi_board
    expected_package = None
    expected_images = None

    def flash_hash(label):
        dump = board.output / f'{label}.bin'
        # A full H563 read can exceed OpenOCD's per-request socket timeout.
        # Keep every request bounded while still comparing all main flash.
        data = bytearray()
        part = board.output / f'{label}-part.bin'
        for offset in range(0, 0x200000, 0x40000):
            board.control(f'dump_image {tcl_word(str(part))} {0x08000000 + offset:#x} 0x40000')
            chunk = part.read_bytes()
            assert len(chunk) == 0x40000
            data += chunk
        dump.write_bytes(data)
        return hashlib.sha256(data).hexdigest()

    results = []
    for index, destination in enumerate(('B', 'A')):
        board.query(board.uart, 'sd probe', 'SD ready', timeout=15)
        board.query(board.uart, 'ota install', 'not_running')
        board.query(board.uart, 'ota init /missing.ota', 'read-only mount')
        board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
        board.query(board.uart, f'ota init "{path}"', 'read-only mount')
        board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
        board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
        board.query(board.uart, 'ota init /daveos-missing.ota', 'OTA SD failed: not_found')
        board.query(board.uart, 'fs ls', 'Filesystem request complete', timeout=30)
        if expected_package is None:
            expected_package = _card_package(board, path)
            expected_images = _package_images(expected_package)
        before = flash_hash(f'before-validation-{index}')
        board.query(board.uart, f'ota init "{path}"', 'OTA SD validating:')
        board.query(board.uart, 'fs unmount', 'busy')
        board.query(board.uart, 'ota init /other.ota', 'busy')
        board.uart.watch_for(rf'OTA SD prepared: .* package {len(expected_package)} slot {destination}', timeout=45)
        assert flash_hash(f'after-validation-{index}') == before
        board.query(board.uart, 'ota enable', 'OTA enabled')
        time.sleep(.05)
        with socket.create_connection((board.ip, 1001), timeout=3) as sock:
            assert ota.request(sock, 1, expected_package[:128])['status'] == 'busy'
            assert ota.request(sock, 4)['status'] == 'busy'
        board.query(board.uart, 'ota status', 'OTA SD prepared: ok')
        if index == 0:
            board.query(board.uart, 'ota disable', 'OTA disabled')
            board.uart.watch_for('OTA SD failed: rejected', timeout=5)
            board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
            board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
            board.query(board.uart, f'ota init "{path}"', 'OTA SD prepared:', timeout=45)

        tcp = board.connect('tcp')
        stop = threading.Event()
        latencies, failures = [], []

        def monitor():
            try:
                while not stop.is_set():
                    started = time.monotonic()
                    board.query(tcp, 'board timer 10000', 'Timer fired', timeout=2)
                    latencies.append(time.monotonic() - started)
                    stop.wait(.05)
            except Exception as error:
                failures.append(str(error))

        thread = threading.Thread(target=monitor)
        thread.start()
        started = time.monotonic()
        try:
            board.query(board.uart, 'ota install', 'OTA SD done: ok', timeout=120)
        finally:
            stop.set()
            thread.join(3)
            tcp.close()
        assert not thread.is_alive() and latencies and not failures, failures
        results.append(dict(destination=destination, seconds=time.monotonic()-started,
                            timer_count=len(latencies), maximum_timer_ms=max(latencies)*1000))
        expected = expected_images[destination]
        address = (0x08140000 if destination == 'B' else 0x08040000) if hil_config.get('board') == 'h755' else (0x0810a000 if destination == 'B' else 0x0800a000)
        dump = board.output / f'installed-{destination}.bin'
        board.control(f'dump_image {tcl_word(str(dump))} {address:#x} {len(expected)}')
        assert dump.read_bytes() == expected
        board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
        board.query(board.uart, 'health status', 'Watchdog running')
        board.query(board.uart, 'health fault', 'No retained failure')
        if index == 0:
            # Only the factory image is guaranteed to match the build's ELF;
            # the operator's SD package can be an older compatible build.
            inspect_memory(board, 'sd-update')
        board.uart.drain()
        board.uart.send('board reset')
        board.ready(destination, trial=True)
    (board.output / 'sd-update-results.json').write_text(json.dumps(results, indent=2))
