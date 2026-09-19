"""Check actual stdout/stderr and exit codes, including logging-disabled builds."""
import subprocess
import sys

binary, logging = sys.argv[1:]
success = subprocess.run([binary], capture_output=True, text=True, check=True)
assert not success.stderr, success.stderr
expected = '[000:00:00:00.001] I ' + 'hello.Poll'.ljust(22) + ': Hello\n'
assert success.stdout == (expected if logging == 'true' else ''), repr(success.stdout)
failure = subprocess.run([binary, 'fail'], capture_output=True, text=True)
assert failure.returncode == 1
assert failure.stdout == ''
assert failure.stderr == 'DaveOS: initialization_failed\n', failure.stderr
