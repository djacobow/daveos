"""Optional PB8/PB9 MCP3425 fixture: address-only scan and one-shot reads."""
import json
import re
from pathlib import Path

import pytest

from memory_cases import inspect_memory

pytestmark = pytest.mark.hil


@pytest.fixture
def adc_board(hil_config, request):
    options = json.loads((Path(hil_config['build']) / 'meson-info/intro-buildoptions.json').read_text())
    if not any(o['name'] == 'i2c_adc_probe' and o['value'] for o in options):
        pytest.skip('requires -Di2c_adc_probe=true and MCP3425 on PB8/PB9')
    return request.getfixturevalue('board')


def test_mcp3425_scan_and_conversion(adc_board):
    board = adc_board
    for cycle in range(3):
        board.query(board.uart, 'i2c reset', 'I2C1 reset: ok')
        board.query(board.uart, 'i2c scan', 'I2C scan complete', timeout=10)
        for sample in range(10):
            result = board.query(board.uart, 'adc sample',
                                 r'MCP3425: raw=(-?\d+) voltage=(-?\d+) uV')
            code, uv = int(result[1]), int(result[2])
            assert -32768 <= code <= 32767
            assert uv == int(code * 125 / 2)
        board.query(board.uart, 'health status', 'Watchdog running')
        board.query(board.uart, 'health fault', 'No retained failure')
    board.query(board.uart, 'i2c stats', r'I2C1: reads=\d+ writes=30 completed=\d+ failed=\d+ timeouts=0')
    transcript = '\n'.join(path.read_text() for path in board.output.glob('uart-*.log'))
    rows = re.findall(r'i2c\.Tick\s+: 60 ([ *]{31})', transcript)
    assert len(rows) == 3
    assert all(row[16] == '*' for row in rows), rows
    assert 'scan aborted' not in transcript
    assert transcript.count('I2C1 ACK map') == 3
    inspect_memory(board, 'i2c')


def _interrupt_read(board):
    """Leave the actual ADC holding SDA low mid-read, not an MCU GPIO clamp.

    H563 nonsecure GPIOB/I2C1 register addresses follow the CMSIS header.
    Only PB8/PB9 mode/output bits change; no flash, card or OTP is written.
    Debugger halt freezes the watchdog as in the memory-inspection fixture.
    """
    script = [
        'halt',
        'set mode [mrw 0x42020400]',
        'set cr1 [mrw 0x40005400]',
        'mww 0x40005400 0',
        'mww 0x42020418 0x300',
        'mww 0x42020404 [expr {[mrw 0x42020404] | 0x300}]',
        'mww 0x42020400 [expr {($mode & ~0xf0000) | 0x50000}]',
        'after 1',
        'mww 0x42020418 0x2000000',  # START, SDA low while SCL high
        'after 1',
        'mww 0x42020418 0x1000000',
    ]
    for bit in range(7, -1, -1):
        script += [f'mww 0x42020418 {0x200 if (0xd1 >> bit) & 1 else 0x2000000}',
                   'after 1', 'mww 0x42020418 0x100', 'after 1',
                   'mww 0x42020418 0x1000000', 'after 1']
    script += ['mww 0x42020418 0x200', 'mww 0x42020418 0x100', 'after 1',
               'set ack [expr {([mrw 0x42020410] & 0x200) == 0}]',
               'mww 0x42020418 0x1000000', 'after 1', 'set held 0']
    # Look for a zero data bit; the echoed 0x08 config guarantees one even
    # when both conversion bytes happen to be 0xff. ACK complete data bytes.
    script += ['''for {set bit 0} {$bit < 24} {incr bit} {
        mww 0x42020418 0x100
        after 1
        if {([mrw 0x42020410] & 0x200) == 0} { set held 1; break }
        mww 0x42020418 0x1000000
        after 1
        if {($bit % 8) == 7} {
            mww 0x42020418 0x2000000
            mww 0x42020418 0x100
            after 1
            mww 0x42020418 0x1000000
            mww 0x42020418 0x200
            after 1
        }
    }''', 'mww 0x42020400 $mode', 'mww 0x40005400 $cr1',
               'set released [expr {([mrw 0x42020414] & 0x300) == 0x300}]',
               'set low [expr {([mrw 0x42020410] & 0x200) == 0}]',
               'format "%d %d %d %d" $ack $held $released $low']
    try:
        result = board.control('; '.join(script)).strip()
        assert result == '1 1 1 1', result
    finally:
        board.control('resume')


def test_mcp3425_interrupted_read_recovery(adc_board):
    board = adc_board
    board.query(board.uart, 'adc sample', r'MCP3425: raw=')
    for startup in (False, True):
        _interrupt_read(board)
        if startup:
            board.uart.drain()
            board.control('reset run')
            board.uart.watch_for('I2C1 reset: ok', timeout=10)
        else:
            board.query(board.uart, 'i2c reset', 'I2C1 reset: ok')
        board.query(board.uart, 'adc sample', r'MCP3425: raw=')
        board.query(board.uart, 'i2c scan', 'I2C scan complete', timeout=10)
        board.query(board.uart, 'health status', 'Watchdog running')
        board.query(board.uart, 'health fault', 'No retained failure')
