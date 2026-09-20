import sys
import pytest
from board import ROOT

pytestmark = [pytest.mark.hil, pytest.mark.slow]


def test_uart_bursts(board):
    with board.external_uart():
        board.run('uart-stress', [sys.executable, str(ROOT / 'tests/hil/h563/uart_stress.py'),
                  board.config['uart'], '--repeat', '2', '--output', str(board.output / 'uart-stress')], timeout=120)
