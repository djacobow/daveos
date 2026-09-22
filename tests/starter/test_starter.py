"""Opt-in consumer builds; Meson and CI select the relevant board."""
from pathlib import Path
import subprocess
import sys

import pytest


@pytest.mark.parametrize('board, starter', [('host', 'application'),
                                            ('h563', 'stm32'), ('h755', 'stm32'),
                                            ('h563', 'stm32_storage'),
                                            ('h755', 'stm32_storage')])
def test_starter(board, starter, request):
    if board not in request.config.getoption('--starter-board'):
        pytest.skip('select with --starter-board')
    root = Path(__file__).resolve().parents[2]
    work = Path(request.config.getoption('--starter-work') or root / 'build/starter-pytest').resolve() / board
    if starter == 'stm32_storage':
        work = work.with_name(f'{board}-{starter}')
    helper = 'starter.py' if board == 'host' else 'stm32.py'
    args = [sys.executable, '-B', str(Path(__file__).with_name(helper)), str(root), str(work)]
    if board != 'host':
        args += [board, starter]
    subprocess.run(args, check=True)
