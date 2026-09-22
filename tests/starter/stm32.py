"""Build a separate Nucleo consumer against this checkout, without downloads."""
from pathlib import Path
import os
import json
import shutil
import subprocess
import sys

root, work = map(Path, sys.argv[1:3])
board = sys.argv[3]
# Optional starter directory under starters/ and its firmware name.
starter = sys.argv[4] if len(sys.argv) > 4 else 'stm32'
name = {'stm32': 'my-device', 'stm32_storage': 'my-storage'}[starter]
source = work / 'application'
shutil.copytree(root / 'starters' / starter, source, dirs_exist_ok=True)
checkout = source / 'subprojects/daveos'
if not checkout.exists():
    checkout.symlink_to(root, target_is_directory=True)
build = work / 'build'
env = os.environ.copy()
env['CCACHE_DIR'] = str(work / 'ccache')
setup = ['meson', 'setup', str(build), str(source), '--wrap-mode=nodownload',
         '--cross-file', str(source / 'meson/stm32.ini'), '-Dboard=' + board]
if (build / 'meson-private/coredata.dat').exists():
    setup.append('--reconfigure')
for command in (setup, ['meson', 'compile', '-C', str(build), '-j', '2']):
    subprocess.run(command, check=True, env=env)
plan = subprocess.run(['meson', 'compile', '-C', str(build), 'flash-plan'],
                      check=True, env=env, capture_output=True, text=True)
print(plan.stdout)
assert f'{name}.elf' in plan.stdout
assert ('stm32h755-sleep-m4.elf' in plan.stdout) == (board == 'h755')
assert (build / f'{name}.elf').is_file()
assert (build / f'{name}.bin').stat().st_size > 0
subprocess.run([sys.executable, '-B', str(root / 'tools/memory_report.py'),
                '--elf', str(build / f'{name}.elf'),
                '--output', str(build / f'{name}-memory.json')], check=True, env=env)
memory = json.loads((build / f'{name}-memory.json').read_text())
assert memory['reserved_heap_bytes'] == 0
assert memory['stack_bytes'] >= memory['minimum_stack_bytes']
