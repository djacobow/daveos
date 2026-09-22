"""Heap-denial and stack-watermark checks shared by the two Nucleo suites."""
import json
import re
from pathlib import Path


def inspect_memory(board, name, slot='A', deny_probe=False, setup=''):
    dump = board.output / f'{name}-stack.bin'
    probe = '''
set $allocation = (void*)_sbrk(16)
if $allocation != (void*)-1 || daveos_heap_attempts != 1 || daveos_heap_last_request != 16
 quit 1
end
''' if deny_probe else ''
    board.prepare_execution(f'''
{setup}
printf "MEMORY bottom=%u top=%u minimum=%u attempts=%u\\n", (unsigned int)&_sstack, (unsigned int)&_estack, (unsigned int)&_Min_Stack_Size, daveos_heap_attempts
if daveos_heap_attempts != 0
 quit 1
end
dump binary memory {dump} &_sstack &_estack
{probe}''', slot=slot)
    log = (board.output / 'prepare.log').read_text()
    match = re.search(r'MEMORY bottom=(\d+) top=(\d+) minimum=(\d+) attempts=(\d+)', log)
    assert match, log
    bottom, top, minimum, attempts = map(int, match.groups())
    data = dump.read_bytes()
    assert len(data) == top - bottom >= minimum
    assert data[:32] == b'\xa5' * 32, 'stack reached the static-data boundary'
    unused = 0
    while unused < len(data) and data[unused:unused + 4] == b'\xa5' * 4:
        unused += 4
    result = dict(stack_bytes=len(data), observed_peak_bytes=len(data) - unused,
                  untouched_bytes=unused, heap_attempts=attempts,
                  note='Paint measures overwritten bytes, not every possible SP excursion or future worst case.')
    assert 0 < result['observed_peak_bytes'] < len(data)
    (board.output / f'{name}-memory.json').write_text(json.dumps(result, indent=2) + '\n')
    print(name, result)
    return result


def check_memory_policy(board):
    inspect_memory(board, 'startup')
    for transport in ['uart', 'usb', 'tcp']:
        stream = board.uart if transport == 'uart' else board.connect(transport)
        board.query(stream, 'board help', 'reset - Reset')
        board.query(stream, 'board stats', r'Overflow events=\d+ timers=\d+')
        board.query(stream, 'board timer 10000', 'Timer fired')
        board.query(stream, 'health crc "123456789"', 'cbf43926')
        if transport != 'uart':
            stream.close()
    board.run('memory-ping', ['ping', '-c', '2', '-W', '2', '-s', '1400', board.ip])
    inspect_memory(board, 'commands', deny_probe=True)
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')

    family = board.config.get('board', 'h563')
    if board.config.get('bootloader', family == 'h563'):
        boot = Path(board.config['build']).resolve() / 'apps/bootloader' / family / 'bootloader.elf'
        resume_m4 = ('monitor targets stm32h7x.cpu1\nmonitor resume\n'
                     'monitor targets stm32h7x.cpu0') if family == 'h755' else ''
        board.uart.drain()
        inspect_memory(board, 'bootloader', setup=f"""file {boot}
monitor reset halt
{resume_m4}
hbreak Enter
continue
delete breakpoints""")
        board.ready()
