#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import sys

base = Path(__file__).resolve().parents[1]
work = base / 'build/host'
work.mkdir(parents=True, exist_ok=True)
flags = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-Imain']
program = work / 'audio_test'
subprocess.run(['cc', *flags, '-DA2_AUDIO_PORTABLE_ONLY', 'main/audio.c',
                'tests/audio_test.c', '-o', str(program)], cwd=base, check=True, timeout=30)
subprocess.run([str(program)], check=True, timeout=20)
image = work / 'audio_diagnostic_image.c'
subprocess.run([sys.executable, str(base / 'tools/embed_rom.py'), '--output', str(image)], check=True)
program = work / 'audio_hook_test'
flags += ['-Wno-unused-parameter', '-Wno-unknown-warning-option', '-Wno-unterminated-string-initialization',
          '-DMII_RP2350=0', '-DMII_65C02_DIRECT_ACCESS=1', '-Imain/vendor/mii']
subprocess.run(['cc', *flags, 'main/apple2.c', 'main/disk2.c', 'main/vendor/mii/mii_65c02.c',
                str(image), 'tests/audio_hook_test.c', '-o', str(program)], cwd=base, check=True, timeout=30)
subprocess.run([str(program)], check=True, timeout=20)
