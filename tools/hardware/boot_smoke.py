#!/usr/bin/env python3
"""Exercise an already-programmed H563 factory image; resets but never flashes.

Requires pyserial, ST-LINK UART, application USB CDC, and Ethernet with DHCP.
Keep the UART open to capture every boot and avoid stale ST-LINK receive data.
"""
import argparse
import re
import socket
import subprocess
import time

import serial


def receive(read, predicate, timeout=10):
    data = b''
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data += read()
        except socket.timeout:
            continue
        text = data.decode('ascii')
        if predicate(text):
            return text
    raise RuntimeError(f'Timed out waiting for response:\n{data.decode("ascii")}')


def timer(port):
    port.write(b'board timer 200000\r')
    return receive(lambda: port.read(8192), lambda text: 'Timer fired' in text, 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--uart', required=True)
    parser.add_argument('--usb', required=True)
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--slot', choices=('A', 'B'), default='A')
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error('--repeat must be positive')
    with serial.Serial(args.uart, 1000000, timeout=.1) as uart:
        # Drain both host and probe buffering before the first observed reset.
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline:
            uart.read(8192)
        for cycle in range(args.repeat):
            uart.write(b'\rboard reset\r')
            text = receive(lambda: uart.read(8192), lambda text:
                           f'Boot slot {args.slot} (confirmed), CRC verified' in text and
                           re.search(r'ready, link .*IP (?!0\.0\.0\.0)(\d+\.\d+\.\d+\.\d+)', text))
            address = re.search(r'ready, link .*IP (\d+\.\d+\.\d+\.\d+)', text).group(1)
            print(f'Cycle {cycle + 1}:\n{text}', flush=True)
            print(timer(uart), flush=True)
            deadline = time.monotonic() + 10
            while True:
                try:
                    usb = serial.Serial(args.usb, 1000000, timeout=.1)
                    break
                except (OSError, serial.SerialException):
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(.1)
            with usb:
                print(timer(usb), flush=True)
            with socket.create_connection((address, 1000), timeout=3) as tcp:
                tcp.settimeout(.2)
                tcp.sendall(b'help\nboard timer 200000\n')
                print(receive(lambda: tcp.recv(8192), lambda text:
                              'list all commands' in text and 'Timer fired' in text, 3), flush=True)
            subprocess.run(['ping', '-c', '1', '-W', '2', '-s', '1400', address], check=True)
        print(f'PASS: {args.repeat} CRC-verified boots, UART/USB/TCP commands, timers, DHCP and ping')


if __name__ == '__main__':
    main()
