#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build portable emulator tests in ignored build/host, then run with time limits."""
import argparse
from pathlib import Path
import subprocess
import sys
import shutil

parser = argparse.ArgumentParser()
parser.add_argument('--rom', help='Optional private ROM to boot and type PRINT 2+2')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--disk-rom', help='Optional private 256-byte slot6 ROM')
parser.add_argument('--disk-image', help='Optional private DOS-order .dsk; only an ignored copy is modified')
args = parser.parse_args()
base = Path(__file__).resolve().parents[1]
work = base / 'build/host'
work.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(base / 'tools/embed_rom.py'), '--output', str(work / 'diagnostic_image.c')], check=True)
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-DMII_RP2350=0', '-DMII_65C02_DIRECT_ACCESS=1', '-Imain', '-Imain/vendor/mii']
version = subprocess.run(['cc', '--version'], capture_output=True, text=True, check=True).stdout
if 'clang' in version:
    # Upstream opcode names intentionally fill four bytes without a trailing NUL.
    flags.append('-Wno-unterminated-string-initialization')
if args.sanitize:
    flags.append('-fsanitize=address,undefined')
sources = ['main/apple2.c', 'main/disk2.c', 'main/vendor/mii/mii_65c02.c']
subprocess.run(['cc', *flags, *sources, str(work / 'diagnostic_image.c'), 'tests/apple2_test.c',
                '-o', str(work / 'apple2_test')], cwd=base, check=True, timeout=30)
subprocess.run([str(work / 'apple2_test')], cwd=base, check=True, timeout=20)
subprocess.run(['cc', *flags, 'main/disk2.c', 'tests/disk2_test.c', '-o', str(work / 'disk2_test')],
               cwd=base, check=True, timeout=30)
subprocess.run([str(work / 'disk2_test')], cwd=base, check=True, timeout=20)
if args.rom:
    subprocess.run(['cc', *flags, *sources, 'tests/rom_probe.c', '-o', str(work / 'rom_probe')],
                   cwd=base, check=True, timeout=30)
    subprocess.run([str(work / 'rom_probe'), str(Path(args.rom).resolve())],
                   cwd=base, check=True, timeout=20)
if args.disk_rom or args.disk_image:
    if not (args.rom and args.disk_rom and args.disk_image):
        parser.error('--rom, --disk-rom and --disk-image are required together for DOS tests')
    copy = work / 'disk-qa.dsk'
    shutil.copyfile(args.disk_image, copy)
    subprocess.run(['cc', *flags, *sources, 'tests/dos_probe.c', '-o', str(work / 'dos_probe')],
                   cwd=base, check=True, timeout=30)
    subprocess.run([str(work / 'dos_probe'), str(Path(args.rom).resolve()),
                    str(Path(args.disk_rom).resolve()), str(copy)], cwd=base, check=True, timeout=45)
