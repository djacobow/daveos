#!/usr/bin/env python3
"""Upload a DaveOS package over its dedicated binary TCP port (never UART)."""
import argparse
import binascii
from pathlib import Path
import socket
import struct
import time

MAGIC = 0x54534f44


def receive(sock, size):
    result = bytearray()
    while len(result) < size:
        data = sock.recv(size - len(result))
        if not data:
            raise RuntimeError("target disconnected; a new upload must restart from the beginning")
        result.extend(data)
    return result


def request(sock, operation, payload=b""):
    sock.sendall(struct.pack("<4I", MAGIC, 1, operation, len(payload)) + payload)
    magic, version, response, size = struct.unpack("<4I", receive(sock, 16))
    if (magic, version, response, size) != (MAGIC, 1, operation | 0x80000000, 72):
        raise RuntimeError("invalid protocol response")
    data = receive(sock, size)
    result, phase, ready, offset, total, maximum = struct.unpack_from("<6I", data)
    return {"result": result, "phase": phase, "ready": bool(ready), "offset": offset,
            "total": total, "maximum": maximum,
            "status": bytes(data[24:56]).split(b"\0", 1)[0].decode("ascii"),
            "state": bytes(data[56:72]).split(b"\0", 1)[0].decode("ascii")}


def upload(sock, package, reboot=False, poll_interval=0.02, progress=print):
    if len(package) < 128 or binascii.crc32(package[:124]) != struct.unpack_from("<I", package, 124)[0]:
        raise ValueError("invalid package header CRC")
    if struct.unpack_from("<I", package, 104)[0] != len(package):
        raise ValueError("package size mismatch")
    response = request(sock, 1, package[:128])
    if response["result"]:
        raise RuntimeError("upload rejected: " + response["status"])
    payload = memoryview(package)[128:]
    offset = 0
    last_percent = -1
    while True:
        response = request(sock, 2)
        if response["state"] == "done":
            if offset != len(payload):
                raise RuntimeError("target completed before all data was sent")
            break
        if response["state"] in ("failed", "disabled", "idle"):
            raise RuntimeError("upload failed: " + response["status"])
        if response["ready"]:
            if response["offset"] != offset or not 0 < response["maximum"] <= 1024:
                raise RuntimeError("unexpected target offset/chunk limit")
            chunk = payload[offset:offset + response["maximum"]]
            message = struct.pack("<II", offset, binascii.crc32(chunk)) + chunk.tobytes()
            for attempt in range(3):
                accepted = request(sock, 3, message)
                if not accepted["result"]:
                    break
                if accepted["status"] != "checksum_error" or attempt == 2:
                    raise RuntimeError("chunk rejected: " + accepted["status"])
            offset += len(chunk)
            percent = offset * 100 // len(payload)
            if percent != last_percent:
                progress(f"Sent {percent}% ({offset}/{len(payload)} package bytes)")
                last_percent = percent
        else:
            time.sleep(poll_interval)
    progress("Installed; flash CRC verified and installation committed")
    if reboot:
        response = request(sock, 5)
        if response["result"]:
            raise RuntimeError("installed, reboot declined: " + response["status"])
        progress("Reboot accepted; next boot is a trial requiring application confirmation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("image", type=Path)
    parser.add_argument("--port", type=int, default=1001)
    parser.add_argument("--reboot", action="store_true")
    args = parser.parse_args()
    try:
        with socket.create_connection((args.host, args.port), timeout=30) as sock:
            upload(sock, args.image.read_bytes(), args.reboot)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, f"OTA: {error}\n")


if __name__ == "__main__":
    main()
