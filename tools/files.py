#!/usr/bin/env python3
"""Basic file and directory operations over DaveOS TCP port 1002."""
import argparse
import binascii
from pathlib import Path
import socket
import struct

MAGIC = 0x46534F44
VERSION = 1
CHUNK = 1024
HEADER = struct.Struct('<6I')
STATUSES = ('ok', 'invalid', 'busy', 'not_ready', 'read_only', 'exists',
            'not_found', 'io_error', 'checksum_error', 'timeout', 'cleanup_failed', 'denied')


def receive(sock, count):
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise OSError('File service disconnected')
        data.extend(chunk)
    return bytes(data)


def request(sock, op, payload=b'', arg0=0, arg1=0):
    sock.sendall(HEADER.pack(MAGIC, VERSION, op, len(payload), arg0, arg1) + payload)
    magic, version, status, length, offset, crc = HEADER.unpack(receive(sock, HEADER.size))
    if magic != MAGIC or version != VERSION or length > CHUNK:
        raise ValueError('Invalid file-service response')
    data = receive(sock, length)
    if status:
        name = STATUSES[status] if status < len(STATUSES) else f'unknown status {status}'
        raise OSError(f'File service: {name}')
    return data, offset, crc


def upload(sock, source, destination):
    """Stream a seekable binary file; target never replaces an existing path."""
    source.seek(0)
    size, crc = 0, 0
    while chunk := source.read(CHUNK):
        size += len(chunk)
        crc = binascii.crc32(chunk, crc)
    if size > 0xffffffff:
        raise ValueError('Files must fit in 32 bits')
    source.seek(0)
    path = remote_path(destination)
    data, total, initial_crc = request(sock, 1, path, size, crc)
    if data or total != size or initial_crc:
        raise ValueError('Invalid upload size acknowledgment')
    offset, running = 0, 0
    while chunk := source.read(CHUNK):
        data, next_offset, reported_crc = request(sock, 2, chunk, offset)
        offset += len(chunk)
        running = binascii.crc32(chunk, running)
        if data or next_offset != offset or reported_crc != running:
            raise ValueError('Invalid upload acknowledgment')
    data, stored, reported_crc = request(sock, 3)
    if data or stored != size or reported_crc != crc:
        raise ValueError('Upload size/CRC mismatch')
    return size, crc


def download(sock, source, destination):
    """Stream to a binary file and verify the final size and CRC acknowledgment."""
    path = remote_path(source)
    data, size, initial_crc = request(sock, 4, path)
    if data or initial_crc:
        raise ValueError('Invalid download header')
    offset, crc = 0, 0
    while True:
        data, reported_offset, reported_crc = request(sock, 5)
        offset += len(data)
        crc = binascii.crc32(data, crc)
        if reported_offset != offset or reported_crc != crc or offset > size:
            raise ValueError('Download size/CRC mismatch')
        if not data:
            if offset != size:
                raise ValueError('Truncated download')
            return size, crc
        destination.write(data)


def remote_path(path):
    encoded = path.encode('utf-8')
    if not encoded or len(encoded) > 255 or b'\0' in encoded:
        raise ValueError('Remote path must be 1..255 UTF-8 bytes without NUL')
    return encoded


def empty_reply(reply):
    if reply != (b'', 0, 0):
        raise ValueError('Invalid metadata acknowledgment')


def listing(sock, path='/'):
    """Yield (name, is_directory, size); consume fully or close the connection."""
    empty_reply(request(sock, 6, remote_path(path)))
    while True:
        data, offset, crc = request(sock, 7)
        if offset or crc:
            raise ValueError('Invalid directory response')
        if not data:
            return
        if len(data) < 9:
            raise ValueError('Truncated directory entry')
        kind, size = struct.unpack('<2I', data[:8])
        name = data[8:].decode('utf-8')
        if kind > 1 or '\0' in name or '/' in name or '\\' in name:
            raise ValueError('Invalid directory entry')
        yield name, bool(kind), size


def mutate(sock, operation, path):
    op = {'rm': 8, 'mkdir': 9, 'rmdir': 10}[operation]
    empty_reply(request(sock, op, remote_path(path)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--port', type=int, default=1002)
    parser.add_argument('--timeout', type=float, default=60)
    commands = parser.add_subparsers(dest='operation', required=True)
    for name in ('put', 'get'):
        command = commands.add_parser(name)
        command.add_argument('source')
        command.add_argument('destination')
    for name in ('ls', 'rm', 'mkdir', 'rmdir'):
        command = commands.add_parser(name)
        if name == 'ls':
            command.add_argument('path', nargs='?', default='/')
        else:
            command.add_argument('path')
    args = parser.parse_args()
    try:
        with socket.create_connection((args.host, args.port), timeout=args.timeout) as sock:
            if args.operation == 'put':
                with Path(args.source).open('rb') as source:
                    size, crc = upload(sock, source, args.destination)
            elif args.operation == 'get':
                target = Path(args.destination)
                # Exclusive create protects existing host files as well.
                with target.open('xb') as output:
                    try:
                        size, crc = download(sock, args.source, output)
                    except BaseException:
                        output.close()
                        target.unlink()
                        raise
            elif args.operation == 'ls':
                for name, directory, size in listing(sock, args.path):
                    print(f'{"d" if directory else "f"} {size:10d} {name}')
            else:
                mutate(sock, args.operation, args.path)
        if args.operation in ('put', 'get'):
            print(f'{args.operation}: {size} bytes, CRC32 {crc:08x}')
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
