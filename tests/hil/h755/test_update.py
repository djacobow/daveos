"""H755 A/B boot/OTA tests; the fixed M4 image must survive every upload."""
import hashlib
import pytest
from flash import tcl_word
from update_cases import check_rejection_and_bidirectional_update

pytestmark = [pytest.mark.hil, pytest.mark.slow]


def test_bidirectional_update(board):
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    def fixed_images(name):
        values = []
        for bank in range(2):
            path = board.output / f'{name}-{bank}.bin'
            board.control(f'dump_image {tcl_word(str(path))} {0x08000000 + bank * 0x100000:#x} 0x20000')
            values.append(hashlib.sha256(path.read_bytes()).hexdigest())
        return values
    before = fixed_images('before')
    check_rejection_and_bidirectional_update(board)
    assert fixed_images('after') == before


def install_confirmed(board, slot, full_metadata=False):
    import binascii
    import json
    import struct
    from pathlib import Path
    import factory

    build = Path(board.config['build'])
    layout = json.loads((build / 'apps/bootloader/h755/layout.json').read_text())
    version = json.loads((build / 'lib/util/version.json').read_text())
    application = (board.firmware / ('stm32-console-b.bin' if slot else 'stm32-console.bin')).read_bytes()
    record = bytearray(factory.metadata(application, layout, version))
    if slot:
        record[120:216] = record[24:120]
        record[24:120] = bytes(96)
        struct.pack_into('<I', record, 60, 0xffffffff)
    def encoded(sequence):
        copy = bytearray(record)
        struct.pack_into('<Q', copy, 8, sequence)
        struct.pack_into('<Q', copy, 228, sequence)
        struct.pack_into('<I', copy, 220, binascii.crc32(copy[:220]))
        return copy
    parts = [(layout['slots'][slot], application)]
    for bank, address in enumerate(layout['metadata']):
        count = layout['sector_size'] // 256 if full_metadata and bank != slot else 1
        parts.append((address, b''.join(encoded(i + 1) for i in range(count))))
    image = board.output / 'confirmed.hex'
    image.write_text(factory.intel_hex(sorted(parts)))
    board.control(f'reset halt; flash write_image erase {tcl_word(str(image))}; verify_image {tcl_word(str(image))}')
    board.uart.drain()
    board.control('reset run')
    board.ready('AB'[slot])
    return layout


def test_factory_slot_b(board):
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    install_confirmed(board, 1)
    for transport in ['uart', 'usb', 'tcp']:
        stream = board.uart if transport == 'uart' else board.connect(transport)
        board.query(stream, 'boot status', 'Slot B: confirmed')
        board.query(stream, 'board timer 10000', 'Timer fired')
        board.query(stream, 'health status', 'Watchdog running')
        if transport != 'uart':
            stream.close()


@pytest.mark.parametrize('slot', [0, 1], ids=['A', 'B'])
def test_runtime_journal_reclamation(board, slot):
    import binascii
    import socket
    import struct
    import time
    import ota
    from ota_helpers import wait_status

    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    layout = install_confirmed(board, slot, full_metadata=True)
    board.query(board.uart, 'ota enable', 'OTA enabled')
    console = board.connect('tcp')
    latencies = []
    with socket.create_connection((board.ip, 1001), timeout=5) as sock:
        assert ota.request(sock, 1, (board.firmware / 'application.ota').read_bytes()[:128])['status'] == 'ok'
        deadline = time.monotonic() + 10
        while True:
            before = time.monotonic()
            board.query(console, 'board timer 1000', 'Timer fired', timeout=.5)
            latencies.append(time.monotonic() - before)
            state = ota.request(sock, 2)
            if state['ready']:
                break
            assert state['status'] in ('ok', 'busy'), state
            assert time.monotonic() < deadline, state
        assert ota.request(sock, 4)['status'] == 'ok'
        wait_status(sock, lambda s: s['state'] == 'failed')
    console.close()
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
    dump = board.output / 'reclaimed.bin'
    board.control(f"dump_image {tcl_word(str(dump))} {layout['metadata'][1-slot]:#x} {layout['sector_size']}")
    data = dump.read_bytes()
    assert struct.unpack_from('<Q', data, 8)[0] == 513
    assert binascii.crc32(data[:220]) == struct.unpack_from('<I', data, 220)[0]
    assert data[256:] == b'\xff' * (len(data) - 256)
    assert max(latencies) < .1, latencies
    summary = f'Journal rollover in slot {"AB"[slot]}: {len(latencies)} timers; max {max(latencies):.3f}s'
    print(summary)
    (board.output / 'journal-timing.log').write_text(summary + '\n')


def upload_pending(board):
    import socket
    import ota
    board.query(board.uart, 'ota enable', 'OTA enabled')
    with socket.create_connection((board.ip, 1001), timeout=30) as sock:
        ota.upload(sock, (board.firmware / 'application.ota').read_bytes())


def test_trial_watchdog_rollback(board):
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    upload_pending(board)
    board.uart.drain()
    board.prepare_execution(f'''file {board.firmware / 'stm32-console-b.elf'}
monitor reset halt
monitor targets stm32h7x.cpu1
monitor resume
monitor targets stm32h7x.cpu0
hbreak SystemClock_Config
continue
delete breakpoints
set $pc = Default_Handler''')
    board.ready('A')
    board.query(board.uart, 'health status', 'Watchdog running')


def test_corrupt_confirmed_fallback(board):
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    upload_pending(board)
    board.uart.drain()
    board.uart.send('board reset')
    board.ready('B', trial=True)
    board.control('reset halt; flash erase_address 0x08140000 0x20000')
    board.uart.drain()
    board.control('reset run')
    board.ready('A')
    board.query(board.uart, 'health status', 'Watchdog running')


def test_no_bootable_image_factory_recovery(board):
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    upload_pending(board)
    board.control('reset halt; flash erase_address 0x08040000 0x20000; flash erase_address 0x08140000 0x20000')
    board.uart.drain()
    board.control('reset run')
    for _ in range(3):
        board.uart.watch_for('No bootable image: checksum_error; resetting', timeout=10)
    board.uart.close()
    board.factory()


def test_bootloader_watchdog(board):
    from pathlib import Path
    if not board.config.get('bootloader', False):
        pytest.skip('requires H755 bootloader build')
    boot = Path(board.config['build']).resolve() / 'apps/bootloader/h755/bootloader.elf'
    board.uart.drain()
    board.prepare_execution(f'''file {boot}
monitor reset halt
monitor targets stm32h7x.cpu1
monitor resume
monitor targets stm32h7x.cpu0
hbreak daveos::boot::Control::select
continue
delete breakpoints
set $pc = Default_Handler''')
    board.ready('A')
    board.query(board.uart, 'health status', 'Watchdog running')
