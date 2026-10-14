#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the original game on the real Applesoft ROM/CPU/Disk II with a disposable disk."""
import argparse
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--rom', required=True)
parser.add_argument('--disk-rom', required=True)
parser.add_argument('--disk-image', required=True)
parser.add_argument('--sanitize', action='store_true')
args = parser.parse_args()
base = Path(__file__).resolve().parents[1]
work = base / 'build/host'
work.mkdir(parents=True, exist_ok=True)
disk = work / 'breakout-test.dsk'
shutil.copyfile(args.disk_image, disk)
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
         '-DMII_RP2350=0', '-DMII_65C02_DIRECT_ACCESS=1', '-Imain', '-Imain/vendor/mii']
if 'clang' in subprocess.run(['cc', '--version'], capture_output=True, text=True, check=True).stdout:
    flags.append('-Wno-unterminated-string-initialization')
if args.sanitize:
    flags.append('-fsanitize=address,undefined')
subprocess.run(['cc', *flags, 'main/apple2.c', 'main/disk2.c', 'main/vendor/mii/mii_65c02.c',
                'tests/basic_game_test.c', '-lm', '-o', str(work / 'basic_game_test')],
               cwd=base, check=True, timeout=30)
subprocess.run([str(work / 'basic_game_test'), str(Path(args.rom).resolve()),
                str(Path(args.disk_rom).resolve()), str(disk),
                str(base.parent / 'basic/BREAKOUT.bas')], cwd=base, check=True, timeout=120)
