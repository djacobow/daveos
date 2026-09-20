"""One H563 fixture owns programming, consoles and debugger access."""
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
        if 'stm32h5x' not in target:
            raise RuntimeError('OpenOCD must target the configured H563')
        self.uart = self.connect('uart')
        image = tcl_word(str(self.firmware / 'factory.hex'))
        self.control(f'reset halt; stm32h5x mass_erase 0; flash write_image {image}; verify_image {image}')
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
