"""Hardware opt-in must not override pytest's explicit file exclusions."""
from pathlib import Path
import shutil
import subprocess
import sys


def test_hil_respects_ignore(tmp_path):
    root = Path(__file__).resolve().parents[2]
    suite = tmp_path / 'tests/hil/h755'
    suite.mkdir(parents=True)
    shutil.copyfile(root / 'tests/conftest.py', tmp_path / 'tests/conftest.py')
    for name in ('display', 'sd'):
        (suite / f'test_{name}.py').write_text(f'def test_{name}(): pass\n')
    result = subprocess.run(
        [sys.executable, '-B', '-m', 'pytest', '--hil', '--collect-only', '-q',
         'tests/hil/h755', '--ignore=tests/hil/h755/test_sd.py'],
        cwd=tmp_path, capture_output=True, text=True, check=True)
    assert 'test_display.py::test_display' in result.stdout
    assert 'test_sd.py::' not in result.stdout
