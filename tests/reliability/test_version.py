"""Version tool contract: clean/dirty identity, CI/local build and components."""
import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[2]


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], text=True).strip()


@pytest.mark.parametrize('component,symbol', [('application', 'kApplicationVersion'), ('bootloader', 'kBootloaderVersion')])
def test_stamp(tmp_path, component, symbol):
    repo = tmp_path / 'source'
    repo.mkdir()
    git(repo, 'init', '-q')
    git(repo, 'config', 'user.name', 'Version Test')
    git(repo, 'config', 'user.email', 'version@example.invalid')
    (repo / 'input').write_text('one')
    git(repo, 'add', 'input')
    git(repo, 'commit', '-qm', 'initial')
    output, metadata = tmp_path / 'stamp.h', tmp_path / 'stamp.json'
    args = [sys.executable, '-B', str(ROOT / 'tools/version.py'), '--root', str(repo),
            '--component', component, '--major', '7', '--minor', '9',
            '--output', str(output), '--json', str(metadata)]
    subprocess.run(args, check=True)
    version = json.loads(metadata.read_text())
    assert version == dict(major=7, minor=9, build=0xffffffff, commit=git(repo, 'rev-parse', 'HEAD'), dirty=False)
    assert symbol in output.read_text()
    before = output.stat().st_mtime_ns
    subprocess.run(args, check=True)
    assert output.stat().st_mtime_ns == before
    (repo / 'input').write_text('two')
    subprocess.run(args + ['--build', '12345'], check=True)
    version = json.loads(metadata.read_text())
    assert version['dirty'] and version['build'] == 12345
    assert '12345u' in output.read_text()


@pytest.mark.parametrize('build', ['-1', '4294967296', 'nope'])
def test_reject_invalid_build(tmp_path, build):
    result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/version.py'),
        '--root', str(tmp_path), '--major', '0', '--minor', '1', '--build', build,
        '--output', str(tmp_path / 'stamp.h')], capture_output=True)
    assert result.returncode != 0
    assert not (tmp_path / 'stamp.h').exists()


@pytest.mark.parametrize('mismatch', [None, 'build', 'commit', 'package'])
def test_ci_identity_verification(tmp_path, mismatch):
    import struct

    util = tmp_path / 'lib/util'
    firmware = tmp_path / 'examples/stm32_console'
    util.mkdir(parents=True)
    firmware.mkdir(parents=True)
    app = dict(major=7, minor=9, build=123, commit='1' * 40, dirty=True)
    boot = dict(app, major=2, minor=3)
    if mismatch == 'build':
        boot['build'] += 1
    if mismatch == 'commit':
        boot['commit'] = '2' * 40
    (util / 'version.json').write_text(json.dumps(app))
    (util / 'boot_version.json').write_text(json.dumps(boot))
    header = bytearray(128)
    struct.pack_into('<3I', header, 48, app['major'], app['minor'], 124 if mismatch == 'package' else 123)
    header[60:100] = app['commit'].encode()
    header[101] = 1
    (firmware / 'application.ota').write_bytes(header)
    result = subprocess.run([sys.executable, '-B', str(ROOT / 'tools/verify_version.py'),
                             '--build', str(tmp_path), '--number', '123'], capture_output=True)
    assert (result.returncode == 0) == (mismatch is None)
