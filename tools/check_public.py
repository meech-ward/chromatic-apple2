#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check publishable source files for private artifacts and broken local links."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_DIRS = {'build', '.git', 'private', 'managed_components', '__pycache__'}
FORBIDDEN_SUFFIXES = {'.rom', '.dsk', '.do', '.po', '.nib', '.woz', '.img', '.bin',
                      '.elf', '.uf2', '.fs', '.map', '.eml', '.pem', '.key', '.log', '.pyc'}
FORBIDDEN_NAMES = {'sdkconfig', 'sdkconfig.old', 'device-local.json', 'rom_image.c'}
TEST_CONFIG = 'mcu/main/ssh/tests/stubs/private_config.h'


def source_files(root):
    result = subprocess.run(['git', 'ls-files', '--cached', '--others', '--exclude-standard',
                             '-z', '--', '.'], cwd=root, capture_output=True, check=True)
    return sorted(set(Path(p.decode()) for p in result.stdout.split(b'\0') if p))


def problems(root, files, secrets=()):
    failures = []
    for relative in files:
        path = root / relative
        name = relative.as_posix()
        if path.is_symlink():
            failures.append(f'{name}: symlinks are not part of the source package')
            continue
        if any(p in FORBIDDEN_DIRS for p in relative.parts) or path.suffix.lower() in FORBIDDEN_SUFFIXES:
            failures.append(f'{name}: generated/private artifact')
            continue
        if (path.name in FORBIDDEN_NAMES or path.name.startswith('.env') and path.name != '.env.example'
                or path.name.endswith('.local.json')
                or path.name == 'private_config.h' and name != TEST_CONFIG):
            failures.append(f'{name}: private configuration or generated input')
            continue
        try:
            data = path.read_bytes()
            content = data.decode('utf-8')
        except (OSError, UnicodeError):
            failures.append(f'{name}: expected a readable UTF-8 source file')
            continue
        if '\0' in content:
            failures.append(f'{name}: binary content in source package')
        if re.search(r'/(?:Users|home)/[A-Za-z0-9_.-]+/', content):
            failures.append(f'{name}: machine-specific home path')
        if any(secret.encode() in data for secret in secrets if len(secret) >= 8):
            failures.append(f'{name}: matches a private setting (value withheld)')
        if path.suffix.lower() == '.md':
            for target in re.findall(r'!?\[[^\]]*\]\(([^\s)]+)\)', content):
                target = target.strip('<>')
                url = urlsplit(target)
                if url.scheme or target.startswith('#') or not url.path:
                    continue
                linked = (path.parent / unquote(url.path)).resolve()
                if not linked.is_relative_to(root.resolve()) or not linked.exists():
                    failures.append(f'{name}: broken local link {target}')
    for required in ['LICENSE', 'THIRD_PARTY_NOTICES.md', 'mcu/licenses/MII-MIT.txt',
                     'mcu/licenses/Apple2JS-MIT.txt', 'storage/vendor/littlefs/LICENSE.md',
                     'mcu/main/ssh/vendor/staticnet/LICENSE']:
        if not (root / required).is_file():
            failures.append(f'{required}: missing license notice')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--secrets-from', type=Path,
                        help='optional private JSON to compare without displaying its values')
    args = parser.parse_args()
    values = ()
    if args.secrets_from:
        try:
            settings = json.loads(args.secrets_from.read_text())
            values = [settings.get(k, '') for k in ('api_token', 'ssh_password', 'wifi_password', 'wifi_ssid')]
            if not all(isinstance(v, str) for v in values):
                raise ValueError('expected strings')
        except (OSError, ValueError, AttributeError):
            parser.error('cannot read private settings for comparison')
    try:
        files = source_files(ROOT)
    except (OSError, subprocess.CalledProcessError):
        parser.error('run from a Git checkout; only publishable Git files are checked')
    if Path('README.md') not in files or Path('mcu/main/main.c') not in files:
        parser.error('no complete project source set found; check Git ignore rules and checkout location')
    failures = problems(ROOT, files, values)
    if failures:
        for failure in failures:
            print(failure, file=sys.stderr)
        raise SystemExit(1)
    print(f'PASS: {len(files)} source files; no forbidden artifacts, private-setting matches or broken local links.')


if __name__ == '__main__':
    main()
