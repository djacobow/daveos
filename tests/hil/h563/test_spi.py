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
        pytest.skip('requires -Dspi_sd_probe=true and an SD card on SPI A')
    return request.getfixturevalue('board')


def test_read_only_sd_inspection(spi_board):
    board = spi_board
    for _ in range(5):
        board.query(board.uart, 'sd probe', r'SD ready: SPI1 1000000 Hz, OCR 0x[0-9a-f]+', timeout=15)
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
