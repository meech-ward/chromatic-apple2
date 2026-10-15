#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create private Wi-Fi and SSH settings without putting passwords in shell history."""
import argparse
import getpass
import json
import os
from pathlib import Path
import secrets

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build/device-local.json')
    parser.add_argument('--wifi-mode', choices=('station', 'hotspot'), default='station',
                        help='join an existing network (default) or create a private hotspot')
    args = parser.parse_args()
    if args.output.exists() or args.output.is_symlink():
        parser.error('Output already exists; choose a new path or edit your existing private file.')
    hotspot = args.wifi_mode == 'hotspot'
    ssid = input('Hotspot network name: ' if hotspot else 'Wi-Fi network name: ')
    password = getpass.getpass('Hotspot password (blank to generate one): ' if hotspot
                               else 'Wi-Fi password (blank for an open network): ')
    if hotspot and not password:
        password = secrets.token_urlsafe(24)
    ssh_password = getpass.getpass('SSH password (blank to generate one): ')
    if not 1 <= len(ssid.encode()) <= 32:
        parser.error('Wi-Fi network name must contain 1..32 UTF-8 bytes.')
    if hotspot and (not 8 <= len(password) <= 63 or
                    any(ord(c) < 32 or ord(c) > 126 for c in password)):
        parser.error('Hotspot password must contain 8..63 printable ASCII characters.')
    if len(password.encode()) > 64:
        parser.error('Wi-Fi password exceeds 64 UTF-8 bytes.')
    if ssh_password and not 8 <= len(ssh_password.encode()) <= 64:
        parser.error('SSH password must contain 8..64 UTF-8 bytes.')
    values = dict(wifi_mode=args.wifi_mode, wifi_ssid=ssid, wifi_password=password,
                  api_token=secrets.token_urlsafe(32),
                  ssh_password=ssh_password or secrets.token_urlsafe(24))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w') as output:
        json.dump(values, output, indent=2)
        output.write('\n')
    print(f'Saved private settings to {args.output}.')
    print('Your SSH password is in that file. Keep it and provisioned firmware private.')


if __name__ == '__main__':
    main()
