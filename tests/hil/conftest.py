"""HIL never runs without explicit selection, configuration and factory flashing."""
import fcntl
import json
import hashlib
from pathlib import Path
import re
import subprocess
import tomllib

import pytest

import build_config
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
    board_name = config.get('board', 'h563')
    if board_name not in ('h563', 'h755'):
        pytest.fail('HIL supports h563 or h755')
    bootloader = config.get('bootloader', board_name == 'h563')
    for key, value in dict(board=board_name, bootloader=bootloader).items():
        if options.get(key) != value:
            pytest.fail(f'HIL build requires {key}={value}')
    # meson/profiles/hil.ini selects these (with bootloader=true).
    required = {'usb', 'net', 'tcp'} | ({'ota'} if bootloader else set())
    missing = required - build_config.features(build)
    if missing:
        pytest.fail(f'HIL build requires features {",".join(sorted(missing))}; '
                    'configure with --cross-file meson/profiles/hil.ini')
    if options.get('otp_programming', False):
        pytest.fail('Automated HIL forbids real OTP programming; use -Dotp_programming=false')
    if config.get('probe_serial') and options.get('probe_serial') != config['probe_serial']:
        pytest.fail('Meson probe_serial must match HIL probe_serial')
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
        names = ('factory.hex', 'stm32-console.elf', 'stm32-console-b.elf', 'application.ota') if config.get('bootloader', board_name == 'h563') else ('stm32-console.elf',)
        firmware = build / 'examples/stm32_console'
        images = {name: firmware / name for name in names}
        if config.get('bootloader', board_name == 'h563'):
            images['bootloader.elf'] = build / 'boot' / board_name / 'bootloader.elf'
        if board_name == 'h755':
            images['stm32h755-sleep-m4.elf'] = build / 'platform/stm32/nucleo/h755/CM4/stm32h755-sleep-m4.elf'
        for image in images.values():
            if not image.is_file():
                pytest.fail(f'Missing firmware artifact: {image}')
        manifest = dict(config=config, build=build_config.load(build),
            artifacts={name: hashlib.sha256(image.read_bytes()).hexdigest()
            for name, image in images.items()})
        (output / 'manifest.json').write_text(json.dumps(manifest, indent=2))
        yield config


@pytest.fixture
def board(hil_config, request):
    output = ROOT / 'build/hil' / re.sub(r'[^a-zA-Z0-9_.-]', '_', request.node.nodeid)
    output.mkdir(parents=True, exist_ok=True)
    selected = hil_config.get('board', 'h563')
    if selected not in request.node.path.parts:
        pytest.skip(f'requires {request.node.path.parent.name} board')
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
