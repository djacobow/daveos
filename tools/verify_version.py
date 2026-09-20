#!/usr/bin/env python3
"""Verify that a CI build number reaches both stamps and the OTA header."""
import argparse
import json
from pathlib import Path
import struct


def verify(build, number):
    identities = [json.loads((build / 'util' / name).read_text())
                  for name in ('version.json', 'boot_version.json')]
    for identity in identities:
        if identity['build'] != number:
            raise ValueError(f"wrong build number: {identity['build']}, expected {number}")
        if len(identity['commit']) != 40 or identity['commit'] == '0' * 40:
            raise ValueError('missing Git commit identity')
    if identities[0]['commit'] != identities[1]['commit']:
        raise ValueError('application and bootloader built from different commits')
    package = build / 'examples/stm32_console/application.ota'
    data = package.read_bytes()
    app = identities[0]
    if struct.unpack_from('<3I', data, 48) != (app['major'], app['minor'], number):
        raise ValueError('OTA version differs from the generated application stamp')
    if data[60:101].split(b'\0')[0].decode() != app['commit'] or bool(data[101]) != app['dirty']:
        raise ValueError('OTA Git identity differs from the application stamp')
    print(f'Application, bootloader and OTA build {number}: identity verified')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--number', type=int, required=True)
    args = parser.parse_args()
    try:
        verify(args.build, args.number)
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'Version: {error}\n')


if __name__ == '__main__':
    main()
