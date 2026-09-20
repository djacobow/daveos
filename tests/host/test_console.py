"""Check interactive dispatch, explicit shutdown, and EOF without shutdown."""
import re
import subprocess
import pytest


def test_output(request):
    executable = request.config.getoption("--test-binary")
    if not executable:
        pytest.skip("Meson supplies --test-binary")
    result = subprocess.run(
        [executable], input='help\nconsole echo "Hello World"\nconsole exit extra\nconsole exit\n',
        text=True, capture_output=True, timeout=5, check=True)
    if request.config.getoption('--test-logging') == 'enabled':
        for context, message in [('core.command', 'console:'),
                                 ('console.Echo', 'Hello World'),
                                 ('console.Exit', 'Exiting')]:
            assert f'{context:<22}: {message}' in result.stdout, result.stdout
        assert '(status invalid_argument)' in result.stdout, result.stdout
        for line in result.stdout.splitlines():
            assert re.match(r'^\[\d{3}:\d{2}:\d{2}:\d{2}\.\d{3}\] [DIWEF] .{22}: ', line), line
    else:
        assert not result.stdout, result.stdout
    # EOF is deliberately not an exit signal. Close stdin and verify it stays alive,
    # then terminate externally for cleanup; a timeout is the expected observation.
    process = subprocess.Popen([executable], stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        try:
            process.wait(timeout=0.3)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise AssertionError(f'EOF stopped console: {process.returncode}')
    finally:
        process.terminate()
        process.communicate(timeout=5)
