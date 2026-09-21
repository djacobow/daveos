"""Static audit parsing must distinguish direct allocator calls from pool names."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import memory_report


def test_symbols_include_absolute_stack_bounds():
    table = memory_report.symbols('''20001000 ? _sstack
20020000 A _estack
00000000 A _Min_Heap_Size
08001234 T operator new(unsigned int)
         U not_defined
''')
    assert table['_estack'] - table['_sstack'] == 0x1f000
    assert table['_Min_Heap_Size'] == 0
    assert 'not_defined' not in table


def test_allocation_calls_include_tail_calls_without_confusing_lwip_pools():
    sites = memory_report.call_sites('''08001000 <startup>:
 8001000: f000 f801 bl 8002000 <malloc>
08003000 <malloc>:
 8003000: f000 b800 b.w 8004000 <_malloc_r>
08004000 <_malloc_r>:
 8004000: d001 beq.n 8004006 <_malloc_r+0x6>
08005000 <poll>:
 8005000: f000 f800 bl 8006000 <memp_malloc>
 8005004: 4798 blx r3
 8005006: f000 f800 bl 8007000 <operator new(unsigned int)>
''')
    assert [(s['caller'], s['callee']) for s in sites] == [
        ('startup', 'malloc'), ('malloc', '_malloc_r'), ('poll', 'operator new(unsigned int)')]
