"""Check interactive dispatch, explicit shutdown, and EOF without shutdown."""
import subprocess
import sys

executable = sys.argv[1]
result = subprocess.run(
    [executable], input='help\nconsole echo "Hello World"\nconsole exit\n',
    text=True, capture_output=True, timeout=5, check=True)
if sys.argv[2] == 'enabled':
    assert 'core/command: console:' in result.stdout, result.stdout
    assert 'console/Echo: Hello World' in result.stdout, result.stdout
    assert 'console/Exit: Exiting' in result.stdout, result.stdout
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
