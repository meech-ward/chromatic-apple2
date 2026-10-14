#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess

base=Path(__file__).resolve().parents[1]
work=base/'build/host'
work.mkdir(parents=True,exist_ok=True)
source=(base/'main/main.c').read_text()
begin=source.index('static handheld_ui_event_t sample_physical_buttons(')
end=source.index('\nstatic void input_task(',begin)
(work/'menu_input_impl.inc').write_text(source[begin:end])
program=work/'menu_input_test'
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-Imain','-I../storage','-Ibuild/host',
                'main/handheld_ui.c','tests/menu_input_test.c','-o',str(program)],
               cwd=base,check=True,timeout=30)
subprocess.run([str(program)],cwd=base,check=True,timeout=20)
