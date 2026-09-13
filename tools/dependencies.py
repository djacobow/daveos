#!/usr/bin/env python3
"""Fetch verified test-only sources into the disposable build directory."""
import hashlib
from pathlib import Path
import sys
import tarfile
import urllib.request

VERSION = "3.16.0"
SHA256 = "0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34"
URL = f"https://github.com/catchorg/Catch2/archive/refs/tags/v{VERSION}.tar.gz"


def main():
    root = Path(__file__).resolve().parents[1]
    destination = root / "build" / "deps" / f"catch2-{VERSION}"
    destination.mkdir(parents=True, exist_ok=True)
    archive = destination / "source.tar.gz"
    if not archive.exists():
        try:
            with urllib.request.urlopen(URL, timeout=60) as response:
                data = response.read()
        except OSError as error:
            sys.exit(f"Cannot download Catch2: {error}. Retry with network access.")
        if hashlib.sha256(data).hexdigest() != SHA256:
            sys.exit("Catch2 archive checksum mismatch")
        archive.write_bytes(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        sys.exit("Cached Catch2 archive checksum mismatch")
    # Extract only explicit files; never trust archive paths for filesystem writes.
    with tarfile.open(archive, "r:gz") as source:
        for relative in ("extras/catch_amalgamated.cpp", "extras/catch_amalgamated.hpp", "LICENSE.txt"):
            target = destination / Path(relative).name
            if not target.exists():
                member = source.extractfile(f"Catch2-{VERSION}/{relative}")
                if member is None:
                    sys.exit(f"Missing Catch2 source: {relative}")
                target.write_bytes(member.read())
    print(destination)


if __name__ == "__main__":
    main()
