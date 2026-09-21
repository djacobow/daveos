"""Build a separate Nucleo consumer against this checkout, without downloads."""
from pathlib import Path
import os
import json
import shutil
import subprocess
import sys

root, work = map(Path, sys.argv[1:3])
board = sys.argv[3]
source = work / 'application'
shutil.copytree(root / 'starters/stm32', source, dirs_exist_ok=True)
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
assert 'my-device.elf' in plan.stdout
assert ('stm32h755-sleep-m4.elf' in plan.stdout) == (board == 'h755')
assert (build / 'my-device.elf').is_file()
assert (build / 'my-device.bin').stat().st_size > 0
subprocess.run([sys.executable, '-B', str(root / 'tools/memory_report.py'),
                '--elf', str(build / 'my-device.elf'),
                '--output', str(build / 'my-device-memory.json')], check=True, env=env)
memory = json.loads((build / 'my-device-memory.json').read_text())
assert memory['reserved_heap_bytes'] == 0
assert memory['stack_bytes'] >= memory['minimum_stack_bytes']
