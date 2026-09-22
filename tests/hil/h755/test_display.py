"""SSD1306 I2C transport checks; visual output still needs human confirmation."""
import re

import pytest
import build_config

pytestmark = pytest.mark.hil


def test_display(hil_config, request):
    build_config.require(hil_config['build'], 'ssd1306')
    board = request.getfixturevalue('board')
    # Scan first; initialization is deliberately an explicit later command.
    board.query(board.uart, 'i2c scan', r'I2C1.*scan|I2C1.*ACK', timeout=10)
    board.uart.watch_for(r'30 .*\*', timeout=5)
    board.uart.watch_for(r'I2C scan complete', timeout=5)
    def update(stream, command):
        # Logs are broadcast: synchronize on this command's echo before its
        # completion, so an old completion from the other console cannot pass.
        board.query(stream, command, re.escape('> ' + command))
        match = stream.watch_for(r'SSD1306 update complete|status busy|SSD1306: .*', timeout=5)
        assert match[0] == 'SSD1306 update complete'

    update(board.uart, 'display init')
    usb = board.connect('usb')
    for _ in range(5):
        update(board.uart, 'display pattern')
        update(board.uart, 'display clear')
        update(usb, 'display text "DaveOS H755"')
    board.query(board.uart, 'i2c stats', r'I2C1: reads=0 writes=258 completed=370 failed=111 timeouts=0')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
