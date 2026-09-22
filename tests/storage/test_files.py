"""File-transfer client framing, bounded streaming and response validation."""
import binascii
import importlib.util
import io
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location('files', Path(__file__).resolve().parents[2] / 'tools/files.py')
files = importlib.util.module_from_spec(spec)
spec.loader.exec_module(files)


class Peer:
    def __init__(self, data=b'', corrupt=False, reject=False):
        self.data = data
        self.received = bytearray()
        self.output = bytearray()
        self.offset = 0
        self.corrupt = corrupt
        self.reject = reject
        self.requests = []

    def sendall(self, frame):
        magic, version, op, length, arg0, arg1 = files.HEADER.unpack(frame[:24])
        assert (magic, version) == (files.MAGIC, 1)
        assert length == len(frame) - 24 and length <= 1024
        payload = frame[24:]
        self.requests.append(op)
        status, reply, offset, crc = 0, b'', 0, 0
        if self.reject:
            status = 5
        elif op == 1:
            self.total, self.expected = arg0, arg1
            offset = arg0
        elif op == 2:
            assert arg0 == len(self.received)
            self.received.extend(payload)
            offset = len(self.received)
            crc = binascii.crc32(self.received)
        elif op == 3:
            assert len(self.received) == self.total
            offset, crc = self.total, self.expected
        elif op == 4:
            offset = len(self.data)
        elif op == 5:
            reply = self.data[self.offset:self.offset + 1024]
            self.offset += len(reply)
            offset = self.offset
            crc = binascii.crc32(self.data[:self.offset])
        if self.corrupt and op in (2, 5):
            crc ^= 1
        self.output.extend(files.HEADER.pack(files.MAGIC, 1, status, len(reply), offset, crc) + reply)

    def recv(self, count):
        count = min(count, 7)  # Every response is fragmented.
        data = bytes(self.output[:count])
        del self.output[:count]
        return data


@pytest.mark.parametrize('size', [0, 1, 1023, 1024, 1025, 8197])
def test_round_trip(size):
    data = bytes(i % 256 for i in range(size))
    peer = Peer()
    assert files.upload(peer, io.BytesIO(data), '/new image.ota') == (size, binascii.crc32(data))
    assert peer.received == data
    assert peer.requests[0] == 1 and peer.requests[-1] == 3
    peer = Peer(data)
    output = io.BytesIO()
    assert files.download(peer, '/image.ota', output) == (size, binascii.crc32(data))
    assert output.getvalue() == data
    assert peer.requests[-1] == 5  # Includes final close/CRC acknowledgment.


@pytest.mark.parametrize('upload', [False, True])
def test_reject_existing_and_corrupt_data(upload):
    def run(peer):
        if upload:
            files.upload(peer, io.BytesIO(b'abc'), '/x')
        else:
            files.download(peer, '/x', io.BytesIO())
    with pytest.raises(OSError, match='exists'):
        run(Peer(reject=True))
    with pytest.raises(ValueError, match='acknowledgment|CRC mismatch'):
        run(Peer(b'abc', corrupt=True))


def test_disconnect():
    with pytest.raises(OSError, match='disconnected'):
        files.receive(Peer(), 24)


class DirectoryPeer(Peer):
    def __init__(self, entries=()):
        super().__init__()
        self.entries = iter(entries)

    def sendall(self, frame):
        _, _, op, length, arg0, arg1 = files.HEADER.unpack(frame[:24])
        assert not arg0 and not arg1
        self.requests.append(op)
        data = b''
        if op == 7:
            assert length == 0
            data = next(self.entries, b'')
        else:
            assert op in (6, 8, 9, 10)
            assert frame[24:] == '/folder'.encode()
        self.output.extend(files.HEADER.pack(files.MAGIC, 1, 0, len(data), 0, 0) + data)


def test_listing_and_mutations():
    peer = DirectoryPeer([files.struct.pack('<2I', 1, 0) + b'child',
                          files.struct.pack('<2I', 0, 123) + 'café.txt'.encode()])
    assert list(files.listing(peer, '/folder')) == [('child', True, 0), ('café.txt', False, 123)]
    assert peer.requests == [6, 7, 7, 7]
    for op in ('rm', 'mkdir', 'rmdir'):
        files.mutate(peer, op, '/folder')
    assert peer.requests[-3:] == [8, 9, 10]
    assert list(files.listing(DirectoryPeer(), '/folder')) == []


@pytest.mark.parametrize('entry', [b'x', files.struct.pack('<2I', 2, 0) + b'x',
                                  files.struct.pack('<2I', 0, 0) + b'a/b'])
def test_invalid_directory_entry(entry):
    with pytest.raises(ValueError):
        list(files.listing(DirectoryPeer([entry]), '/folder'))


@pytest.mark.parametrize('path', ['', 'x' * 256, 'a\0b'])
def test_invalid_remote_path(path):
    with pytest.raises(ValueError, match='Remote path'):
        files.mutate(DirectoryPeer(), 'rm', path)


def test_upload_rewinds_before_checksum_and_transfer():
    source = io.BytesIO(b'complete file')
    source.read(5)
    peer = Peer()
    assert files.upload(peer, source, '/file') == (13, binascii.crc32(b'complete file'))
    assert peer.received == b'complete file'
