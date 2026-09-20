"""Portable Python tests run by default; hardware is always opt-in."""
import sys
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def pytest_addoption(parser):
    parser.addoption('--starter-board', action='append', default=[], choices=['host', 'h563', 'h755'])
    parser.addoption('--starter-work')
    parser.addoption('--test-binary')
    parser.addoption('--test-logging', default='enabled', choices=['enabled', 'disabled'])
    parser.addoption('--hil', action='store_true', help='Enable board-specific HIL; always mass-erase and program factory firmware')
    parser.addoption('--hil-config', help='Local HIL configuration TOML file')


def pytest_ignore_collect(collection_path, config):
    return collection_path == ROOT / 'tests/hil' and not config.getoption('--hil')
