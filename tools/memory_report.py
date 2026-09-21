#!/usr/bin/env python3
"""Report linked stack bounds and allocation paths, not just allocator names.

Direct call sites are evidence of possible use, not proof that paths execute.
Indirect calls and third-party private allocators require separate inspection.
Runtime heap-attempt counters and stack painting complement this static report.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

ALLOCATORS = {'malloc', 'calloc', 'realloc', 'aligned_alloc', 'memalign', 'sbrk',
              '_malloc_r', '_calloc_r', '_realloc_r', '_memalign_r', '_sbrk', '_sbrk_r'}


def allocator(name):
    return name in ALLOCATORS or name.startswith(('operator new(', 'operator new[]('))


def symbols(text):
    result = {}
    for line in text.splitlines():
        match = re.match(r'^([0-9a-fA-F]+)\s+[A-Za-z?]\s+(.+)$', line)
        if match:
            result[match[2]] = int(match[1], 16)
    return result


def call_sites(text):
    caller = None
    result = []
    for line in text.splitlines():
        function = re.match(r'^[0-9a-fA-F]+ <(.+)>:', line)
        if function:
            caller = function[1]
        # Include tail calls as well as BL. Ignore local branches within a body.
        call = re.search(r'^\s*([0-9a-fA-F]+):.*\s(?:blx?|b)(?:\.[nw])?\s+[0-9a-fA-F]+ <(.+)>', line)
        if call and caller and allocator(call[2]):
            result.append(dict(caller=caller, callee=call[2], address=int(call[1], 16)))
    return result


def report(elf, nm, objdump):
    table = symbols(subprocess.check_output([nm, '-n', '-C', '--defined-only', str(elf)], text=True))
    disassembly = subprocess.check_output([objdump, '-d', '-C', str(elf)], text=True)
    for name in ('_sstack', '_estack', '_Min_Stack_Size', '_Min_Heap_Size'):
        if name not in table:
            raise ValueError(f'missing memory policy symbol: {name}')
    capacity = table['_estack'] - table['_sstack']
    if capacity < table['_Min_Stack_Size'] or table['_Min_Heap_Size'] != 0:
        raise ValueError('expected zero heap and sufficient minimum stack headroom')
    return dict(elf=str(elf), stack_bottom=table['_sstack'], stack_top=table['_estack'],
                stack_bytes=capacity, minimum_stack_bytes=table['_Min_Stack_Size'],
                reserved_heap_bytes=table['_Min_Heap_Size'],
                heap_attempt_counter=table.get('daveos_heap_attempts'),
                allocators=sorted(name for name in table if allocator(name)),
                allocation_calls=call_sites(disassembly),
                caveat='Linked paths may be dormant. This report does not prove absence of allocation attempts; verify the _sbrk policy and runtime counters.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf', type=Path, required=True)
    parser.add_argument('--nm', default='arm-none-eabi-nm')
    parser.add_argument('--objdump', default='arm-none-eabi-objdump')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = report(args.elf, args.nm, args.objdump)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{args.elf.name}: stack {result["stack_bytes"]} bytes '
          f'(minimum {result["minimum_stack_bytes"]}); heap reservation 0; '
          f'{len(result["allocation_calls"])} linked allocation call sites')


if __name__ == '__main__':
    main()
