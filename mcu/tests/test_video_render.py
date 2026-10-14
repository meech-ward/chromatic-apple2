#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess

base = Path(__file__).resolve().parents[1]
work = base / 'build/host'
work.mkdir(parents=True, exist_ok=True)
program = work / 'video_render_test'
subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-Imain', 'main/video_render.c',
                'tests/video_render_test.c', '-o', str(program)], cwd=base, check=True, timeout=30)
subprocess.run([str(program)], cwd=base, check=True, timeout=20)
