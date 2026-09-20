"""HIL never runs without explicit selection, configuration and factory flashing."""
import fcntl
import json
import hashlib
from pathlib import Path
import re
import subprocess
import tomllib

import pytest

from board import Board, ROOT


@pytest.fixture(scope='session')
def hil_config(request):
    if not request.config.getoption('--hil'):
        pytest.fail('HIL requires --hil')
    path = request.config.getoption('--hil-config')
    if not path:
        pytest.fail('HIL requires --hil-config with UART, USB, build and GDB paths')
    if hasattr(request.config, 'workerinput') or request.config.getoption('numprocesses', default=0):
        pytest.fail('One board requires serial test execution; do not use pytest-xdist')
    config = tomllib.loads(Path(path).read_text())
    for key in ('uart', 'usb', 'build', 'gdb'):
        if key not in config:
            pytest.fail(f'Missing HIL configuration: {key}')
    build = Path(config['build']).resolve()
    options = json.loads((build / 'meson-info/intro-buildoptions.json').read_text())
    options = {item['name']: item['value'] for item in options}
    for key, value in dict(board='h563', bootloader=True, networking=True, usb_console=True, tcp_console=True).items():
        if options.get(key) != value:
            pytest.fail(f'HIL build requires {key}={value}')
    if options.get('otp_programming', False):
        pytest.fail('Automated HIL forbids real OTP programming; use -Dotp_programming=false')
    # Hold ownership before building/flashing, through every teardown.
    output = ROOT / 'build/hil'
    output.mkdir(parents=True, exist_ok=True)
    with (output / 'board.lock').open('w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pytest.fail('Another HIL session owns this board')
        with (output / 'build.log').open('w') as log:
            subprocess.run(['meson', 'compile', '-C', str(build)], check=True, stdout=log, stderr=subprocess.STDOUT)
        for name in ('factory.hex', 'stm32-console.elf', 'stm32-console-b.elf', 'application.ota'):
            if not (build / 'examples/stm32_console' / name).is_file():
                pytest.fail(f'Missing firmware artifact: {name}')
        firmware = build / 'examples/stm32_console'
        manifest = dict(config=config, artifacts={name: hashlib.sha256((firmware / name).read_bytes()).hexdigest()
            for name in ('factory.hex', 'stm32-console.elf', 'stm32-console-b.elf', 'application.ota')})
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        yield config


@pytest.fixture
def board(hil_config, request):
    output = ROOT / 'build/hil' / re.sub(r'[^a-zA-Z0-9_.-]', '_', request.node.nodeid)
    output.mkdir(parents=True, exist_ok=True)
    device = Board(hil_config, output)
    try:
        device.factory()
        yield device
    finally:
        try:
            # Release any debugger halt; leave the installed image running.
            device.control('reset run')
        finally:
            device.close()
