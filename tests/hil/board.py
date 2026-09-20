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

    def fault_experiment(self, script, slot='A'):
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
        assert len(data) == 256 and data[70]
        assert struct.unpack_from('<3I', data) == (0x46534f44, 1, 256)
        assert binascii.crc32(data[:252]) == struct.unpack_from('<I', data, 252)[0]
        self.control('resume')
        return data
