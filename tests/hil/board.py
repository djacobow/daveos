"""One board fixture owns programming, consoles and debugger access."""
from contextlib import contextmanager
from pathlib import Path
import socket
import subprocess
import sys
import time

from watcher import LineDecoder, WatcherGroup

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from flash import tcl_word


class RecordedLines(LineDecoder):
    def __init__(self, path):
        super().__init__()
        self.path = path

    def feed(self, data):
        with self.path.open('ab') as log:
            log.write(data)
        return super().feed(data)


class Board:
    def __init__(self, config, output):
        self.config = config
        self.output = output
        (output / "openocd.log").write_text("")
        self.firmware = Path(config['build']).resolve() / 'examples/stm32_console'
        self.group = WatcherGroup(colorize=False)
        self.sequence = 0
        self.uart = None
        self.ip = None

    def control(self, command):
        script = f'if {{[catch {{{command}}} result]}} {{return "FAILED: $result"}}; return "OK: $result"'
        with socket.create_connection(('localhost', self.config.get('tcl_port', 6666)), timeout=60) as sock:
            sock.sendall(script.encode() + b'\x1a')
            data = b''
            while not data.endswith(b'\x1a'):
                chunk = sock.recv(65536)
                if not chunk:
                    raise RuntimeError('OpenOCD closed its control connection')
                data += chunk
        text = data[:-1].decode()
        with (self.output / 'openocd.log').open('a') as log:
            log.write(command + '\n' + text + '\n')
        if not text.startswith('OK:'):
            raise RuntimeError(text)
        return text[3:]

    def connect(self, transport):
        self.sequence += 1
        name = f'{transport}-{self.sequence}'
        transcript = self.output / f'{name}.log'
        transcript.write_bytes(b'')
        stream = self.group.watcher(name, decoder_factory=lambda: RecordedLines(transcript))
        if transport == 'tcp':
            return stream.socket((self.ip, 1000), timeout=3)
        return stream.serial(self.config[transport], 1000000, timeout=.1)

    def query(self, stream, command, pattern, timeout=5):
        stream.drain()
        stream.send(raw=command.encode() + b'\r')
        return stream.watch_for(pattern, timeout=timeout)

    def ready(self, slot='A', trial=False):
        if self.config.get('board', 'h563') == 'h755' and not self.config.get('bootloader', False):
            self.uart.watch_for('DaveOS STM32H755 M7; type help', timeout=35)
            self.ip = self.uart.watch_for(r'ready, link .*IP ((?!0\.0\.0\.0)\d+\.\d+\.\d+\.\d+)', timeout=15)[1]
            # Survive a complete watchdog interval before declaring ready.
            time.sleep(5)
            self.query(self.uart, 'health status', 'Watchdog running')
            return
        pattern = (rf'(?P<boot>Boot slot {slot} \({"trial" if trial else "confirmed"}\), CRC verified)'
                   r'|ready, link .*IP (?P<ip>(?!0\.0\.0\.0)\d+\.\d+\.\d+\.\d+)'
                   r'|(?P<healthy>Healthy for five seconds; image confirmed)')
        seen = set()
        deadline = time.monotonic() + 35
        while len(seen) != 3:
            match = self.uart.watch_for(pattern, timeout=max(0, deadline - time.monotonic()))
            seen.add(match.lastgroup)
            if match.lastgroup == 'ip':
                self.ip = match.group('ip')
        self.query(self.uart, 'boot status', f'Slot {slot}: confirmed')

    def factory(self):
        # No reuse option: even a single selected test starts from factory state.
        target = self.control('capture {targets}')
        family = self.config.get('board', 'h563')
        expected = 'stm32h5x' if family == 'h563' else 'stm32h7x'
        if expected not in target:
            raise RuntimeError(f'OpenOCD must target the configured {family}')
        self.uart = self.connect('uart')
        if family == 'h563':
            image = tcl_word(str(self.firmware / 'factory.hex'))
            self.control(f'reset halt; stm32h5x mass_erase 0; flash write_image {image}; verify_image {image}')
        elif self.config.get('bootloader', False):
            image = tcl_word(str(self.firmware / 'factory.hex'))
            self.control(f'targets stm32h7x.cpu0; reset halt; flash erase_address 0x08000000 0x200000; flash write_image {image}; verify_image {image}')
        else:
            # H755 standalone factory state includes both the sleeping M4 and M7.
            m4 = tcl_word(str(Path(self.config['build']).resolve() / 'platform/stm32/nucleo/h755/CM4/stm32h755-sleep-m4.elf'))
            m7 = tcl_word(str(self.firmware / 'stm32-console.elf'))
            self.control(f'targets stm32h7x.cpu0; reset halt; flash erase_address 0x08000000 0x200000; flash write_image {m4}; verify_image {m4}; flash write_image {m7}; verify_image {m7}')
        # Flash erase does not clear retained RAM; start with no old fault record.
        self.control('mww 0x20000000 0')
        self.uart.drain()
        self.control('reset run')
        self.ready()

    @contextmanager
    def external_uart(self):
        """Exclusive lease for byte-level stress/debugger regression helpers."""
        self.uart.close()
        try:
            yield
        finally:
            self.uart = self.connect('uart')

    def run(self, name, args, timeout=60):
        with (self.output / f'{name}.log').open('w') as log:
            subprocess.run(args, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                           timeout=timeout, check=True)

    def close(self):
        self.group.close()

    def stub(self, name, assembly):
        """Assemble a RAM-only test injection and return its binary and labels."""
        source = self.output / f'{name}.s'
        obj = self.output / f'{name}.o'
        binary = self.output / f'{name}.bin'
        prefix = str(Path(self.config['gdb']).resolve()).removesuffix('gdb')
        source.write_text('.syntax unified\n.cpu cortex-m33\n.thumb\n.text\n' + assembly + '\n')
        self.run(name + '-as', [prefix + 'as', '-mcpu=cortex-m33', '-mthumb', str(source), '-o', str(obj)])
        self.run(name + '-objcopy', [prefix + 'objcopy', '-O', 'binary', str(obj), str(binary)])
        symbols = subprocess.check_output([prefix + 'nm', '-n', '--defined-only', str(obj)], text=True)
        labels = {line.split()[2]: 0x20000400 + int(line.split()[0], 16)
                  for line in symbols.splitlines() if len(line.split()) == 3}
        return binary, labels

    @staticmethod
    def task_progress_fault(module, task):
        """Locate the intended task without assuming module registration order.

        Use GDB expressions only: the ARM GDB may lack Python, and calling a
        target strcmp while halted would disturb the watchdog experiment.
        """
        def matches(pointer, text):
            return ' && '.join(f'{pointer}[{i}] == {byte}'
                               for i, byte in enumerate(text.encode('ascii') + b'\0'))
        condition = matches('$progress_module', module) + ' && ' + matches('$progress_task', task)
        return f'''set $progress_index = 0
set $progress_matches = 0
set $progress_match = -1
while $progress_index < app::application.scheduler_.task_count_
 set $progress_module = app::application.scheduler_.tasks_._M_elems[$progress_index].module_name
 set $progress_task = app::application.scheduler_.tasks_._M_elems[$progress_index].name
 if {condition}
  set $progress_match = $progress_index
  set $progress_matches = $progress_matches + 1
 end
 set $progress_index = $progress_index + 1
end
if $progress_matches != 1
 quit 1
end
set var app::application.scheduler_.tasks_._M_elems[$progress_match].completed = 0'''

    def prepare_execution(self, script, slot='A'):
        """Arrange an injected fault/hang, then detach and leave it running."""
        commands = self.output / 'prepare.gdb'
        commands.write_text(f'''set pagination off
set confirm off
target remote :{self.config.get('gdb_port', 3333)}
hbreak app::Health::Heartbeat
continue
delete breakpoints
{script}
detach
quit
''')
        elf = 'stm32-console.elf' if slot == 'A' else 'stm32-console-b.elf'
        self.run('prepare', [self.config['gdb'], '-q', '-batch', str(self.firmware / elf), '-x', str(commands)])
        self.control('resume')

    def phy_power(self, enabled):
        """Change the PHY BCR power-down bit through the real MDIO driver."""
        operation = '& ~0x0800' if enabled else '| 0x0800'
        self.prepare_execution(f'''set $result = HAL_ETH_ReadPHYRegister(&'(anonymous namespace)::eth', '(anonymous namespace)::phy'.DevAddr, 0, (unsigned int*)0x20000440)
if $result != 0
 quit 1
end
set $result = HAL_ETH_WritePHYRegister(&'(anonymous namespace)::eth', '(anonymous namespace)::phy'.DevAddr, 0, (*(unsigned int*)0x20000440) {operation})
if $result != 0
 quit 1
end''')

    def fault_experiment(self, script, slot='A', frame_valid=True):
        import binascii
        import struct
        record = self.output / 'retained.bin'
        commands = self.output / 'experiment.gdb'
        commands.write_text(f'''set pagination off
set confirm off
target remote :{self.config.get('gdb_port', 3333)}
{script}
dump binary memory {record} 0x20000000 0x20000100
if *(unsigned short*)$pc != 0xbe00
 quit 1
end
set $pc = $pc + 2
detach
quit
''')
        elf = 'stm32-console.elf' if slot == 'A' else 'stm32-console-b.elf'
        self.run('gdb', [self.config['gdb'], '-q', '-batch', str(self.firmware / elf), '-x', str(commands)], 30)
        data = record.read_bytes()
        assert len(data) == 256 and bool(data[70]) == frame_valid
        assert struct.unpack_from('<3I', data) == (0x46534f44, 1, 256)
        assert binascii.crc32(data[:252]) == struct.unpack_from('<I', data, 252)[0]
        self.control('resume')
        return data
