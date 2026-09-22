"""The resolved configuration Meson records beside the console firmware."""
import json
from pathlib import Path

import pytest


def load(build):
    """Board, bootloader, logging and the resolved (expanded) feature list."""
    path = Path(build) / 'examples/stm32_console/build-config.json'
    if not path.is_file():
        pytest.fail(f'{path} is missing; configure the STM32 console with -Dexamples=true')
    return json.loads(path.read_text())


def features(build):
    return set(load(build)['features'])


def require(build, *names, reason=''):
    """Skip the calling test unless every named feature was built."""
    missing = [name for name in names if name not in features(build)]
    if missing:
        pytest.skip(f'requires features {",".join(missing)}' + (f'; {reason}' if reason else ''))
