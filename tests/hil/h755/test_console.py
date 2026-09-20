"""H755 standalone parity. Every case provisions both cores from factory state."""
import binascii
import pytest

pytestmark = pytest.mark.hil


@pytest.mark.parametrize('transport', ['uart', 'usb', 'tcp'])
def test_commands(board, transport):
    stream = board.uart if transport == 'uart' else board.connect(transport)
    board.query(stream, 'help', 'list all commands')
    board.query(stream, 'board version', 'Application ')
    board.query(stream, 'board timer 200000', 'Timer fired')
    board.query(stream, 'board led 1 toggle', 'LED 1')
    board.query(stream, 'board button', 'Button')
    board.query(stream, 'health status', 'Watchdog running')
    board.query(stream, 'health fault', 'No retained failure')
    for text in ['', '123456789', 'odd length', 'x' * 117]:
        expected = binascii.crc32(text.encode())
        board.query(stream, f'health crc "{text}"', f'CRC32 {expected:08x} .*agree')


def test_network_and_reset(board):
    board.run('ping', ['ping', '-c', '2', '-W', '2', '-s', '1400', board.ip])
    for _ in range(3):
        tcp = board.connect('tcp')
        board.query(tcp, 'board timer 10000', 'Timer fired')
        tcp.close()
    board.uart.drain()
    board.uart.send('board reset')
    board.ready()
    usb = board.connect('usb')
    board.query(usb, 'health status', 'Watchdog running')
