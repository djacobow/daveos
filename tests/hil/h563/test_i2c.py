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
