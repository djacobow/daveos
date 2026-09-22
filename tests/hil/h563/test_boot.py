"""Hardware journal rollover and CRC rejection start from real factory firmware."""
import binascii
import json
from pathlib import Path
import socket
import struct

import pytest
import ota
from ota_helpers import wait_status
from flash import tcl_word

pytestmark = [pytest.mark.hil, pytest.mark.slow]


def test_journal_rollover(board):
    layout = json.loads((Path(board.config['build']) / 'apps/bootloader/h563/layout.json').read_text())
    records = layout['sector_size'] // 256
    header = (board.firmware / 'application.ota').read_bytes()[:128]
    board.query(board.uart, 'ota enable', 'OTA enabled')
    # Each begin durably invalidates B; abort never advances the install counter.
    # Cross both A->B and B->A, including reclaiming a previously full sector.
    with socket.create_connection((board.ip, 1001), timeout=10) as sock:
        for _ in range(2 * records + 1):
            assert ota.request(sock, 1, header)['status'] == 'ok'
            wait_status(sock, lambda s: s['ready'])
            assert ota.request(sock, 4)['status'] == 'ok'
            wait_status(sock, lambda s: s['state'] == 'failed')
    valid = []
    for bank, address in enumerate(layout['metadata']):
        dump = board.output / f'metadata-{bank}.bin'
        board.control(f'dump_image {tcl_word(str(dump))} {address:#x} {layout["sector_size"]}')
        data = dump.read_bytes()
        sequences = []
        for at in range(0, len(data), 256):
            record = data[at:at + 256]
            if struct.unpack_from('<I', record)[0] != 0x4a534f44:
                continue
            assert binascii.crc32(record[:236]) == struct.unpack_from('<I', record, 236)[0]
            sequence = struct.unpack_from('<Q', record, 8)[0]
            assert struct.unpack_from('<IQI', record, 240) == (0x454e4f44, sequence, 0xbab1b0bb)
            assert struct.unpack_from('<Q', record, 16)[0] == 1
            assert struct.unpack_from('<I', record, 40)[0] == 4  # A confirmed
            sequences.append(sequence)
        valid.append(sequences)
    assert max(valid[0]) == 2 * records + 2
    assert max(valid[1]) == 2 * records
    assert len(valid[0]) == 2  # Old A records were physically erased.
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()


def test_both_images_invalid_reset_loop(board):
    # Install an eligible B first, then invalidate both payloads. Neither a
    # confirmed A nor a pending B may bypass flash CRC verification.
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=30) as sock:
        ota.upload(sock, (board.firmware / 'application.ota').read_bytes())
    layout = json.loads((Path(board.config['build']) / 'apps/bootloader/h563/layout.json').read_text())
    board.control('reset halt')
    for address in layout['slots']:
        board.control(f'flash erase_address {address:#x} {layout["sector_size"]}')
    board.uart.drain()
    board.control('reset run')
    for _ in range(3):
        board.uart.watch_for('No bootable image: checksum_error; resetting', timeout=10)
    # Repair via the same full factory programming path and verify recovery.
    board.uart.close()
    board.factory()
