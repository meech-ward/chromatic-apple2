#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit/check the original Chromatic input, vertical paddle and click helper.

BASIC reserves $9000 upward with HIMEM:36864 before creating arrays. The helper
uses $9000 for input, $9003 for sound, and $03C0-$03C8 for event, paddle, key,
previous B, old paddle, pause edge, moved flag, loop end and tone delay. It stays
below DOS at $9600. It neither borrows Applesoft zero page nor uses ROM calls.

Held UP/DOWN override queued cursor/W/S keys; opposing directions cancel.
Paddle pixels occupy x=0, y=P..P+5. The lores address tables handle interleaved
screen memory. Read/modify/write touches just the selected color nibble and
preserves white balls. Repainting every call heals a vacated stationary paddle.
The small sound entry toggles the emulated Apple speaker, as the old game did.
Six-byte JMP stubs at $0300 preserve short CALL operands in the hot BASIC loop.
"""
import argparse
from pathlib import Path

BASE = 0x9000
EV, P, K, B, OP, PE, MV, END, TONE = range(0x3c0, 0x3c9)
code = []; labels = {}; fixes = []; absolute_fixes = []; asm = []
def emit(op, *args, desc=''):
    asm.append(f'{BASE + len(code):04x}: {desc}')
    code.append(op); code.extend(args)
def imm(op, n, desc): emit(op, n, desc=desc)
def abs_(op, n, desc): emit(op, n & 255, n >> 8, desc=desc)
def ref(op, name, desc, offset=0):
    absolute_fixes.append((len(code) + 1, name, offset)); emit(op, 0, 0, desc=desc)
def label(name): labels[name] = BASE + len(code); asm.append(name + ':')
def branch(op, name, desc): fixes.append((len(code) + 1, name)); emit(op, 0, desc=desc)

ref(0x4c, 'input', 'JMP input')
ref(0x4c, 'sound', 'JMP sound')
label('input')
imm(0xa9, 0, 'LDA #0')
for n, name in ((EV, 'event'), (K, 'key'), (PE, 'pause_edge'), (MV, 'moved')):
    abs_(0x8d, n, 'STA ' + name)
abs_(0xad, 0xc000, 'LDA keyboard')
branch(0x10, 'nokey', 'BPL nokey')
imm(0x29, 0x7f, 'AND #$7F')
imm(0xc9, 3, 'CMP #3 ; let Applesoft receive Ctrl-C')
branch(0xf0, 'nokey', 'BEQ nokey ; leave break strobe pending')
abs_(0x8d, K, 'STA key')
abs_(0x2c, 0xc010, 'BIT keyboard_strobe')
label('nokey')
abs_(0xad, 0xc062, 'LDA B_button')
emit(0xaa, desc='TAX')
abs_(0x4d, B, 'EOR previous_B')
imm(0x29, 0x80, 'AND #$80')
branch(0xf0, 'direction', 'BEQ direction')
abs_(0x8e, B, 'STX previous_B')
emit(0x8a, desc='TXA')
branch(0x10, 'direction', 'BPL direction')
abs_(0x8d, PE, 'STA pause_edge')
abs_(0xee, EV, 'INC event')
label('direction')
abs_(0xad, 0xc0f0, 'LDA held_buttons')
imm(0x29, 0x90, 'AND #$90 ; UP/DOWN')
imm(0xc9, 0x90, 'CMP #$90')
branch(0xf0, 'done', 'BEQ done ; opposed neutral')
imm(0xc9, 0x10, 'CMP #$10')
branch(0xf0, 'up', 'BEQ up')
imm(0xc9, 0x80, 'CMP #$80')
branch(0xf0, 'down', 'BEQ down')
abs_(0xad, K, 'LDA key')
for key in (11, 87):
    imm(0xc9, key, 'CMP #' + str(key)); branch(0xf0, 'up', 'BEQ up')
imm(0xc9, 10, 'CMP #10')
branch(0xf0, 'down', 'BEQ down')
imm(0xc9, 83, 'CMP #83')
branch(0xd0, 'done', 'BNE done')
label('down')
abs_(0xad, P, 'LDA paddle')
imm(0xc9, 34, 'CMP #34')
branch(0xf0, 'done', 'BEQ done')
emit(0x18, desc='CLC'); imm(0x69, 2, 'ADC #2')
imm(0xc9, 35, 'CMP #35')
branch(0x90, 'commit', 'BCC commit')
imm(0xa9, 34, 'LDA #34')
branch(0xd0, 'commit', 'BNE commit')
label('up')
abs_(0xad, P, 'LDA paddle')
branch(0xf0, 'done', 'BEQ done')
imm(0xc9, 2, 'CMP #2')
branch(0xb0, 'subtract', 'BCS subtract')
imm(0xa9, 0, 'LDA #0')
ref(0x4c, 'commit', 'JMP commit')
label('subtract')
emit(0x38, desc='SEC'); imm(0xe9, 2, 'SBC #2')
label('commit')
abs_(0xae, P, 'LDX paddle')
abs_(0x8e, OP, 'STX old_paddle')
abs_(0x8d, P, 'STA paddle')
abs_(0xee, MV, 'INC moved'); abs_(0xee, EV, 'INC event')
imm(0xa9, 1, 'LDA #1 ; original magenta background')
ref(0x20, 'paint', 'JSR paint ; erase previous paddle')
label('done')
abs_(0xae, P, 'LDX paddle')
imm(0xa9, 6, 'LDA #6 ; original blue paddle')
ref(0x20, 'paint', 'JSR paint')
emit(0x60, desc='RTS')

label('paint')
ref(0x8d, 'ink_low', 'STA ink_low+1', 1)
for _ in range(4): emit(0x0a, desc='ASL A')
ref(0x8d, 'ink_high', 'STA ink_high+1', 1)
emit(0x8a, desc='TXA'); emit(0x18, desc='CLC'); imm(0x69, 6, 'ADC #6')
abs_(0x8d, END, 'STA loop_end')
label('pixel')
ref(0xbd, 'low_table', 'LDA low_table,X')
ref(0x8d, 'sample', 'STA sample+1', 1); ref(0x8d, 'store', 'STA store+1', 1)
ref(0xbd, 'high_table', 'LDA high_table,X')
ref(0x8d, 'sample', 'STA sample+2', 2); ref(0x8d, 'store', 'STA store+2', 2)
label('sample'); abs_(0xad, 0, 'LDA patched_screen_address')
emit(0xa8, desc='TAY'); emit(0x8a, desc='TXA'); emit(0x4a, desc='LSR A ; odd y in carry')
branch(0x90, 'lower', 'BCC lower')
emit(0x98, desc='TYA'); imm(0x29, 0xf0, 'AND #$F0')
imm(0xc9, 0xf0, 'CMP #$F0 ; preserve white ball')
branch(0xf0, 'next_pixel', 'BEQ next_pixel')
emit(0x98, desc='TYA'); imm(0x29, 15, 'AND #$0F ; preserve neighbor')
label('ink_high'); imm(0x09, 0, 'ORA #patched_high_color')
ref(0x4c, 'store', 'JMP store')
label('lower')
emit(0x98, desc='TYA'); imm(0x29, 15, 'AND #$0F')
imm(0xc9, 15, 'CMP #15 ; preserve white ball')
branch(0xf0, 'next_pixel', 'BEQ next_pixel')
emit(0x98, desc='TYA'); imm(0x29, 0xf0, 'AND #$F0 ; preserve neighbor')
label('ink_low'); imm(0x09, 0, 'ORA #patched_low_color')
label('store'); abs_(0x8d, 0, 'STA patched_screen_address')
label('next_pixel')
emit(0xe8, desc='INX'); abs_(0xec, END, 'CPX loop_end')
branch(0xd0, 'pixel', 'BNE pixel'); emit(0x60, desc='RTS')

label('sound'); imm(0xa2, 16, 'LDX #16')
label('edge'); abs_(0xad, 0xc030, 'LDA speaker_toggle')
abs_(0xac, TONE, 'LDY tone_delay')
label('delay'); emit(0x88, desc='DEY'); branch(0xd0, 'delay', 'BNE delay')
emit(0xca, desc='DEX'); branch(0xd0, 'edge', 'BNE edge'); emit(0x60, desc='RTS')
addresses = [0x400 + (((y // 2) & 7) << 7) + ((y // 2) >> 3) * 40 for y in range(40)]
label('low_table'); code.extend(a & 255 for a in addresses)
label('high_table'); code.extend(a >> 8 for a in addresses)
for i, name in fixes:
    distance = labels[name] - (BASE + i + 1)
    assert -128 <= distance <= 127, (i, name, distance)
    code[i] = distance & 255
for i, name, offset in absolute_fixes:
    address = labels[name] + offset; code[i:i+2] = [address & 255, address >> 8]
assert BASE + len(code) <= 0x9600
assert TONE < 0x3d0

def data_lines():
    return [str(2000 + offset // 24 * 10) + ' DATA ' + ','.join(map(str, code[offset:offset+24]))
            for offset in range(0, len(code), 24)]
def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembly', action='store_true')
    parser.add_argument('--check', type=Path, metavar='BASIC_FILE')
    args = parser.parse_args()
    if args.check:
        lines = args.check.read_text().splitlines()
        actual = [line for line in lines if line.split(maxsplit=1) and
                  line.split(maxsplit=1)[0].isdigit() and 2000 <= int(line.split(maxsplit=1)[0]) < 2300]
        if actual != data_lines(): parser.exit(1, 'BREAKOUT DATA differs from helper source\n')
        loader = f'26 RESTORE:FOR I=0 TO {len(code)-1}:READ V:POKE 36864+I,V:NEXT I'
        stub = '27 POKE 768,76:POKE 769,0:POKE 770,144:POKE 771,76:POKE 772,3:POKE 773,144'
        if loader not in lines or '24 HIMEM:36864' not in lines or stub not in lines:
            parser.exit(1, 'BREAKOUT helper loader/reserved memory does not match\n')
        print(f'PASS BREAKOUT helper DATA ({len(code)} original 6502 bytes)')
    elif args.assembly: print('\n'.join(asm))
    else: print('\n'.join(data_lines()))
if __name__ == '__main__': main()
