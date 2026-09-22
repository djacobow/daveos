"""Read-only SPI1 DMA fault injection; never write card data or OTP."""
import json
import os
from pathlib import Path
import subprocess
import time

import pytest

import build_config
from sd_cases import spi_board

pytestmark = [
    pytest.mark.hil,
    pytest.mark.skipif(os.environ.get('DAVEOS_HIL_SD_FAULT') != '1',
                       reason='opt-in fault injection may require SD power cycling'),
]


def test_sd_dma_timeout_cleanup(spi_board, hil_config):
    board = spi_board
    build_config.require(hil_config['build'], 'sd-dma')
    board.query(board.uart, 'sd probe', 'SD ready', timeout=15)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    board.query(board.uart, 'sd reset', 'busy')
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')

    h755 = hil_config.get('board') == 'h755'
    family = 'stm32h7' if h755 else 'stm32h5'
    target = 'stm32h7x.cpu0' if h755 else 'stm32h5x.cpu'
    enable = 'DMA1_Stream2->CR = control' if h755 else 'GPDMA1_Channel2->CCR = kControl'
    injection = ('set {unsigned int}0x40020808 = 40' if h755 else
                 'set ((DMA_Channel_TypeDef*)0x40020150)->CTR2 = 0x409')
    registers = ((0x40020028, 0x40020040, 0x40013008, 0x40013010, 0x58020c14)
                 if h755 else
                 (0x400200e4, 0x40020164, 0x40013008, 0x40013010, 0x42020c14))
    header = Path(f'platform/{family}/spi_dma.h').resolve()
    line = next(i for i, text in enumerate(header.read_text().splitlines(), 1)
                if enable in text)
    script = board.output / 'dma-timeout.gdb'
    script.write_text(f'''set pagination off
set confirm off
target remote :{hil_config.get('gdb_port', 3333)}
hbreak {header}:{line}
commands
silent
if app::components.sd.hardware_.backend_.total_ < 512
 quit 1
end
# Change TX to the inactive SPI2 request before enabling it. RX and SPI wait
# for clocks that never arrive; the normal HAL deadline must clean up.
{injection}
printf "INJECTED\\n"
delete breakpoints
detach
quit
end
printf "ARMED\\n"
continue
''')
    output = board.output / 'dma-timeout-gdb.log'
    with output.open('w') as log:
        process = subprocess.Popen([hil_config['gdb'], '-q', '-batch',
                                    str(board.firmware / 'stm32-console.elf'), '-x', str(script)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while 'ARMED' not in output.read_text():
                assert process.poll() is None, output.read_text()
                assert time.monotonic() < deadline, output.read_text()
                time.sleep(.02)
            board.uart.drain()
            board.uart.send('sd probe')
            assert process.wait(timeout=10) == 0, output.read_text()
            board.control(f'if {{[{target} curstate] eq "halted"}} {{resume}}')
            board.uart.watch_for(r'SD probe failed at CMD17: timeout', timeout=10)
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            board.control(f'if {{[{target} curstate] eq "halted"}} {{resume}}')
    assert 'INJECTED' in output.read_text()
    board.query(board.uart, 'sd stats', r'SPI1 faulted=no; SD ready=no; last error=timeout')
    # Both channels disabled, DMA requests/IRQs off, CS high. No user buffers
    # are retained and no new SD command is issued while idle after failure.
    registers = board.control('format "%x %x %x %x %x" ' +
                              ' '.join(f'[mrw {address:#x}]' for address in registers))
    rx, tx, config, interrupts, pins = [int(x, 16) for x in registers.split()]
    assert (rx | tx) & (1 if h755 else 5) == 0
    assert config & 0xc000 == 0 and interrupts == 0
    assert pins & 0x4000
    board.query(board.uart, 'fs mount', 'Filesystem: not_ready')
    time.sleep(.3)
    board.query(board.uart, 'sd stats', r'SD ready=no; last error=timeout')
    board.query(board.uart, 'sd reset', 'SPI1 reset: ok; SD requires sd probe')
    board.query(board.uart, 'fs mount', 'Filesystem: not_ready')
    recovery = board.query(board.uart, 'sd probe',
                           r'SD (ready:.*|initialization failed.*|probe failed.*)', timeout=15)[0]
    recovered = recovery.startswith('SD ready:')
    if recovered:
        board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
        board.query(board.uart, 'fs ls', 'Filesystem request complete', timeout=15)
        board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    else:
        board.query(board.uart, 'sd stats', r'SD ready=no;')
        board.query(board.uart, 'fs mount', 'Filesystem: not_ready')
    (board.output / 'recovery.json').write_text(json.dumps(
        dict(controller_reset=True, card_reinitialized=recovered, response=recovery), indent=2))
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
