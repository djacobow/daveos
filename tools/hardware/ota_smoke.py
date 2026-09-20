#!/usr/bin/env python3
"""H563 TCP OTA hardware regression: replaces both application slots.

Requires an OTA-capable, confirmed image, working DHCP Ethernet and ST-LINK UART.
Tests CRC rejection/disconnect, then updates both slots, requests each reboot,
and manually confirms each trial. The final image runs in the original slot.
Serial and TCP access require pyserial and an otherwise idle console connection.
"""
import argparse
import binascii
import socket
import struct
import sys
import threading
import time
from pathlib import Path
import serial
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import ota
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--host', required=True)
parser.add_argument('--image', type=Path, required=True)
parser.add_argument('--uart', required=True)
parser.add_argument('--slot', choices=('A', 'B'), default='A')
args = parser.parse_args()
IP = args.host
image = args.image.read_bytes()
UART = args.uart

def report(s):
    print(s, flush=True)

def query(port, cmd, expect, timeout=5):
    port.write(cmd.encode() + b'\r')
    data = b''
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        data += port.read(8192)
        if expect.encode() in data:
            report(data.decode('ascii'))
            return
    raise RuntimeError((cmd, data.decode(errors='replace')))

def monitor(stop, results):
    try:
        with socket.create_connection((IP, 1000), timeout=3) as sock:
            sock.settimeout(0.1)
            while not stop.is_set():
                start = time.monotonic()
                sock.sendall(b'board timer 10000\n')
                data = b''
                while b'Timer fired' not in data:
                    if time.monotonic() - start > 2:
                        raise RuntimeError('console stalled during OTA')
                    try:
                        chunk = sock.recv(8192)
                        if not chunk:
                            raise RuntimeError('console disconnected during OTA')
                        data += chunk
                    except socket.timeout:
                        pass
                data.decode('ascii')
                results.append(time.monotonic() - start)
                stop.wait(0.2)
    except Exception as e:
        results.append(e)
with serial.Serial(UART, 1000000, timeout=0.1) as port:
    end = time.monotonic() + 1
    while time.monotonic() < end:
        port.read(8192)
    query(port, 'boot status', f'Slot {args.slot}: confirmed')
    query(port, 'ota disable', 'OTA disabled')
    time.sleep(0.1)
    try:
        s = socket.create_connection((IP, 1001), timeout=1)
        s.close()
        raise RuntimeError('OTA listening while disabled')
    except (ConnectionRefusedError, socket.timeout):
        pass
    query(port, 'ota enable', 'OTA enabled')
    time.sleep(0.1)
    with socket.create_connection((IP, 1001), timeout=3) as s:
        assert ota.request(s, 1, image[:128])['status'] == 'ok'
        while not ota.request(s, 2)['ready']:
            time.sleep(0.02)
        data = image[128:1152]
        assert ota.request(s, 3, struct.pack('<II', 0, binascii.crc32(data) ^ 1) + data)['status'] == 'checksum_error'
        assert ota.request(s, 3, struct.pack('<II', 0, binascii.crc32(data)) + data)['status'] == 'ok'
    time.sleep(0.5)
    query(port, 'ota status', 'failed')
    query(port, 'boot status', f'Slot {args.slot}: confirmed')
    report('PASS: disabled listener, bad-chunk rejection, disconnect abort')
    other = 'B' if args.slot == 'A' else 'A'
    for source, destination in [(args.slot, other), (other, args.slot)]:
        if source == other:
            query(port, 'ota enable', 'OTA enabled')
            time.sleep(0.1)
        stop = threading.Event()
        results = []
        thread = threading.Thread(target=monitor, args=(stop, results))
        thread.start()
        try:
            with socket.create_connection((IP, 1001), timeout=30) as sock:
                start = time.monotonic()
                ota.upload(sock, image, progress=report)
                stop.set()
                thread.join(3)
                assert results and (not any((isinstance(x, Exception) for x in results))), results
                report(f'PASS: {source}->{destination} installed in {time.monotonic() - start:.2f}s; {len(results)} concurrent console timer checks; max latency {max(results):.3f}s')
                response = ota.request(sock, 5)
                assert response['status'] == 'ok', response
        finally:
            stop.set()
            thread.join(3)
        text = b''
        end = time.monotonic() + 12
        while time.monotonic() < end:
            text += port.read(8192)
            if f'Boot slot {destination} (trial), CRC verified'.encode() in text and b'ready, link 100M' in text:
                break
        report(text.decode('ascii'))
        assert f'Boot slot {destination} (trial), CRC verified'.encode() in text
        query(port, 'boot status', f'Slot {destination}: trial')
        query(port, 'ota status', 'disabled')
        query(port, 'boot confirm', 'Image confirmed')
        query(port, 'boot status', f'Slot {destination}: confirmed')
    report('PASS: bidirectional TCP OTA, optional reboot, manual confirmation, console stays live')
