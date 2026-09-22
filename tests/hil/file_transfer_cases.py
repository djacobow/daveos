"""Opt-in temporary SD files; no installation, existing-file replacement or OTP writes."""
import io
import os
import socket
import time
import uuid

import pytest
import build_config
import files
from memory_cases import inspect_memory

pytestmark = [pytest.mark.hil, pytest.mark.skipif(
    os.environ.get('DAVEOS_HIL_FILE_TRANSFER') != '1',
    reason='set DAVEOS_HIL_FILE_TRANSFER=1 to authorize temporary SD files')]


@pytest.fixture
def files_board(hil_config, request):
    build_config.require(hil_config['build'], 'file-transfer')
    return request.getfixturevalue('board')


def test_file_transfer(files_board):
    board = files_board
    path = '/daveos-transfer-' + uuid.uuid4().hex[:12] + '.ota'
    partial = path + '.partial'
    package = (board.firmware / 'application.ota').read_bytes()

    def connect():
        return socket.create_connection((board.ip, 1002), timeout=60)

    def absent(name):
        for _ in range(20):
            result = board.query(board.uart, f'fs read "{name}"', r'Filesystem: no_file|status busy')
            if 'no_file' in result[0]:
                return
            time.sleep(.2)
        pytest.fail('Transfer did not release the filesystem after disconnect')

    board.query(board.uart, 'sd probe', 'SD ready', timeout=20)
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    with connect() as sock, pytest.raises(OSError, match='read_only'):
        files.upload(sock, io.BytesIO(package), path)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
    with connect() as sock:
        files.upload(sock, io.BytesIO(package), path)
    with connect() as sock, pytest.raises(OSError, match='exists'):
        files.upload(sock, io.BytesIO(b'must not replace'), path)
    for mode in ('rw', 'ro'):
        if mode == 'ro':
            board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
            board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
        output = io.BytesIO()
        with connect() as sock:
            files.download(sock, path, output)
        assert output.getvalue() == package
        (board.output / f'download-{mode}.ota').write_bytes(output.getvalue())
    # The newly uploaded package must pass the real SD OTA validation path.
    board.query(board.uart, f'ota init "{path}"', 'OTA SD prepared:', timeout=60)
    with connect() as sock, pytest.raises(OSError, match='busy'):
        files.download(sock, path, io.BytesIO())
    board.query(board.uart, 'ota disable', 'OTA disabled')
    time.sleep(.1)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
    with connect() as sock:
        files.request(sock, 1, partial.encode(), 1024, 0)
        files.request(sock, 2, b'partial')
        board.query(board.uart, 'fs unmount', 'status busy')
        board.query(board.uart, 'board timer 10000', 'Timer fired')
        with pytest.raises(OSError):
            with connect() as second:
                files.request(second, 4, path.encode())
    absent(partial)
    with connect() as sock:
        files.request(sock, 1, partial.encode(), 3, 0)
        files.request(sock, 2, b'abc')
        with pytest.raises(OSError, match='checksum_error'):
            files.request(sock, 3)
    absent(partial)
    # An idle client must not hold the filesystem indefinitely. The timeout
    # reply arrives only after the partial file has been closed and removed.
    with connect() as sock:
        files.request(sock, 1, partial.encode(), 100, 0)
        header = files.HEADER.unpack(files.receive(sock, files.HEADER.size))
        assert header[:4] == (files.MAGIC, files.VERSION, 9, 0)
    absent(partial)
    # Exercise only a unique directory created by this test.
    directory = path + '.dir'
    child = directory + '/hello world.txt'
    with connect() as sock:
        files.mutate(sock, 'mkdir', directory)
        assert list(files.listing(sock, directory)) == []
        files.upload(sock, io.BytesIO(b'hello directory'), child)
        assert list(files.listing(sock, directory)) == [('hello world.txt', False, 15)]
    with connect() as sock, pytest.raises(OSError, match='denied'):
        files.mutate(sock, 'rmdir', directory)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount', 'Filesystem mounted read-only')
    with connect() as sock:
        assert list(files.listing(sock, directory)) == [('hello world.txt', False, 15)]
    for operation, target in (('rm', child), ('mkdir', directory + '/new'), ('rmdir', directory)):
        with connect() as sock, pytest.raises(OSError, match='read_only'):
            files.mutate(sock, operation, target)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'fs mount rw', 'Filesystem mounted read-write')
    with connect() as sock:
        files.mutate(sock, 'rm', child)
        assert list(files.listing(sock, directory)) == []
        files.mutate(sock, 'rmdir', directory)
        files.mutate(sock, 'rm', path)

    absent(path)
    board.query(board.uart, 'fs unmount', 'Filesystem unmounted')
    board.query(board.uart, 'health status', 'Watchdog running')
    board.query(board.uart, 'health fault', 'No retained failure')
    inspect_memory(board, 'file-transfer')
