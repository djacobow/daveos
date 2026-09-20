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
