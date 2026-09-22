"""Fail if a consumer source transitively includes a forbidden project layer.

Usage: check_layering.py ROOT SOURCE FORBIDDEN[,FORBIDDEN...] -- COMPILER...
FORBIDDEN entries are repository-relative path prefixes such as core/platform/.
"""
from pathlib import Path
import subprocess
import sys

root, source, forbidden = Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3]
compiler = sys.argv[sys.argv.index('--') + 1:]
prefixes = [entry for entry in forbidden.split(',') if entry]
result = subprocess.run(
    [*compiler, '-std=c++20', '-M', '-I', str(root), source],
    capture_output=True, text=True)
if result.returncode:
    sys.exit(f'{source}: dependency scan failed\n{result.stderr}')
headers = set()
for token in result.stdout.replace('\\\n', ' ').split()[1:]:
    path = Path(token).resolve()
    if path.is_relative_to(root):
        headers.add(path.relative_to(root).as_posix())
violations = sorted(h for h in headers if any(h.startswith(p) for p in prefixes))
if violations:
    sys.exit(f'{source} must not include {", ".join(prefixes)}; found:\n  '
             + '\n  '.join(violations))
print(f'{Path(source).name}: {len(headers)} project headers, none forbidden')
