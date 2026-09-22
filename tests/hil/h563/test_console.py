from pathlib import Path
import pytest

pytestmark = pytest.mark.hil


@pytest.mark.parametrize('transport', ['uart', 'usb', 'tcp'])
def test_commands(board, transport):
    stream = board.uart if transport == 'uart' else board.connect(transport)
    board.query(stream, 'help', 'list all commands')
    board.query(stream, 'board timer 200000', 'Timer fired')
    board.query(stream, 'health status', 'Watchdog running')
    board.query(stream, 'health fault', 'No retained failure')
    board.query(stream, 'boot status', 'Slot A: confirmed')


def test_network_and_reconnect(board):
    board.run('ping', ['ping', '-c', '2', '-W', '2', '-s', '1400', board.ip])
    for _ in range(3):
        tcp = board.connect('tcp')
        board.query(tcp, 'board timer 10000', 'Timer fired')
        tcp.close()


def test_reset_and_usb_reconnect(board):
    usb = board.connect('usb')
    board.query(usb, 'board timer 10000', 'Timer fired')
    usb.close()
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()
    usb = board.connect('usb')
    board.query(usb, 'health status', 'Watchdog running')


def test_version_identity(board):
    import json
    import re

    version = json.loads((Path(board.config['build']) / 'lib/util/version.json').read_text())
    build = 'local' if version['build'] == 0xffffffff else str(version['build'])
    expected = f"Application {version['major']}.{version['minor']}.{build}; Git {version['commit']}"
    if version['dirty']:
        expected += ' dirty'
    board.query(board.uart, 'board version', re.escape(expected))
    boot = json.loads((Path(board.config['build']) / 'lib/util/boot_version.json').read_text())
    transcript = (board.output / 'uart-1.log').read_text(errors='replace')
    assert f"DaveOS bootloader {boot['major']}.{boot['minor']}" in transcript
    build = 'local' if boot['build'] == 0xffffffff else str(boot['build'])
    assert f"Build {build}; Git {boot['commit']}" in transcript
