"""SSD1306 I2C transport checks; visual output still needs human confirmation."""
import re
import time

import pytest
import build_config

pytestmark = pytest.mark.hil


def test_display(hil_config, request):
    build_config.require(hil_config['build'], 'ssd1306')
    board = request.getfixturevalue('board')
    def counters():
        match = board.query(board.uart, 'i2c stats',
                            r'I2C1: reads=0 writes=(\d+) completed=(\d+) failed=(\d+) timeouts=(\d+)')
        return tuple(int(match[i]) for i in range(1, 5))

    # Factory boot starts the status view automatically; one initialization
    # plus periodic frames should already have occurred during health checks.
    assert counters()[0] >= 34
    board.query(board.uart, 'display live off', 'Live display off')
    time.sleep(.25)  # Let any accepted page sequence finish; no cancellation.
    # Scans and manual commands now run without competing automatic frames.
    board.query(board.uart, 'i2c scan', r'I2C1.*scan|I2C1.*ACK', timeout=10)
    board.uart.watch_for(r'30 .*\*', timeout=5)
    board.uart.watch_for(r'I2C scan complete', timeout=5)
    before = counters()

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
    after = counters()
    assert after[0] - before[0] == 258
    assert after[2:] == (111, 0)  # Only the 111 unoccupied scan addresses NACK.
    # Manual commands paused live updates; check it stays paused across ticks.
    time.sleep(1.2)
    assert counters() == after
    board.query(board.uart, 'display live on', 'Live display on')
    time.sleep(3.25)
    board.query(board.uart, 'display live off', 'Live display off')
    time.sleep(.25)
    resumed = counters()
    assert resumed[0] - after[0] in (48, 64)  # Three or four complete 1 Hz frames.
    assert resumed[2:] == after[2:]
    board.query(board.uart, 'display live on', 'Live display on')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
