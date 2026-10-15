#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read ignored device settings; emit secrets only into the private build directory."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--input', default='')
parser.add_argument('--output', required=True)
args = parser.parse_args()
values = dict(wifi_mode='station', wifi_ssid='', wifi_password='', api_token='', ssh_password='')
if args.input:
    try:
        settings = json.loads(Path(args.input).read_text())
        if not isinstance(settings, dict):
            raise ValueError('settings must be an object')
        for key in values:
            value = settings.get(key, values[key])
            if not isinstance(value, str) or '\0' in value:
                raise ValueError('settings must be strings without NUL bytes')
            values[key] = value
    except (OSError, ValueError):
        parser.error('cannot read valid private settings; use tools/configure.py')
    placeholders = {'YOUR_WIFI_NETWORK', 'YOUR_WIFI_PASSWORD',
                    'GENERATE_A_UNIQUE_RANDOM_TOKEN_AT_LEAST_32_CHARACTERS',
                    'CHOOSE_A_PRIVATE_PASSWORD'}
    if any(value in placeholders for value in values.values()):
        parser.error('example placeholders are not credentials; use tools/configure.py')
    if values['wifi_mode'] not in ('station', 'hotspot'):
        parser.error('wifi_mode must be station or hotspot')
    if values['wifi_mode'] == 'hotspot' and (
            not 8 <= len(values['wifi_password']) <= 63 or
            any(ord(c) < 32 or ord(c) > 126 for c in values['wifi_password'])):
        parser.error('hotspot wifi_password must contain 8..63 printable ASCII characters')
    if not 1 <= len(values['wifi_ssid'].encode()) <= 32:
        parser.error('wifi_ssid must contain 1..32 UTF-8 bytes')
    if len(values['wifi_password'].encode()) > 64:
        parser.error('wifi_password exceeds 64 bytes')
    if not 32 <= len(values['api_token']) <= 128:
        parser.error('api_token must contain 32..128 characters')
    if any(ord(c) < 33 or ord(c) > 126 for c in values['api_token']):
        parser.error('api_token must contain printable ASCII without spaces')
    if values['ssh_password'] and not 8 <= len(values['ssh_password'].encode()) <= 64:
        parser.error('ssh_password must contain 8..64 UTF-8 bytes')
lines = ['/* PRIVATE BUILD INPUT: never publish this file or a provisioned binary. */',
         '#pragma once', f'#define DEVICE_WIFI_ENABLED {int(bool(args.input))}',
         f'#define DEVICE_WIFI_AP {int(values["wifi_mode"] == "hotspot")}',
         f'#define DEVICE_SSH_ENABLED {int(bool(args.input and values["ssh_password"]))}']
for key in ('wifi_ssid', 'wifi_password', 'api_token', 'ssh_password'):
    lines.append(f'#define DEVICE_{key.upper()} {json.dumps(values[key], ensure_ascii=False)}')
output = Path(args.output)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text('\n'.join(lines) + '\n')
output.chmod(0o600)
