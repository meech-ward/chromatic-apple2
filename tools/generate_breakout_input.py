#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Emit or verify the original 6502 input/paddle helper embedded in BREAKOUT.

The labelled instruction definitions below are the source of truth. This small
emitter has no assembler dependency. Run this file to print the BASIC DATA lines;
--assembly prints an annotated listing; --check verifies a BASIC source file.

$0300-$03BF: code; $03C0-$03C6: event, paddle, key, previous B, old paddle,
pause edge and movement flag. The code deliberately stays below DOS vectors at
$03D0. BASIC initializes the byte paddle to 18 at each new game and mirrors it
into P after a move. No Applesoft variable layout or ROM entry point is used.

C0F0 supplies held Chromatic buttons. C000/C010 supply/clear ASCII keyboard
input; C062 supplies B. Held left/right take priority over queued arrow keys,
and opposing held directions cancel. Paddle writes affect only the upper
nibbles for lores row 37, leaving neighboring row 36 intact. White balls
are preserved during both erase and paint; painting on every call repairs the
paddle after a ball leaves it without another BASIC drawing operation.
"""
import argparse
from pathlib import Path

BASE=0x300
EV,P,K,B,OP,PE,MV = range(0x3c0,0x3c7)
code=[]; labels={}; fixes=[]
asm=[]
def emit(op,*args,desc=''):
    asm.append(f'{BASE+len(code):04x}: {desc}')
    code.append(op); code.extend(args)
def imm(op,n,desc): emit(op,n,desc=desc)
def abs_(op,n,desc): emit(op,n&255,n>>8,desc=desc)
def label(n): labels[n]=BASE+len(code);asm.append(n+':')
def branch(op,n,desc): fixes.append((len(code)+1,n));emit(op,0,desc=desc)

imm(0xa9,0,'LDA #0')
for n,name in ((EV,'event'),(K,'key'),(PE,'pause_edge'),(MV,'moved')):
    abs_(0x8d,n,'STA '+name)
abs_(0xad,0xc000,'LDA keyboard')
branch(0x10,'nokey','BPL nokey')
imm(0x29,0x7f,'AND #$7F')
abs_(0x8d,K,'STA key')
abs_(0x2c,0xc010,'BIT keyboard_strobe')
label('nokey')
abs_(0xad,0xc062,'LDA B_button')
emit(0xaa,desc='TAX')
abs_(0x4d,B,'EOR previous_B')
imm(0x29,0x80,'AND #$80')
branch(0xf0,'direction','BEQ direction')
abs_(0x8e,B,'STX previous_B')
emit(0x8a,desc='TXA')
branch(0x10,'direction','BPL direction')
abs_(0x8d,PE,'STA pause_edge')
abs_(0xee,EV,'INC event')
label('direction')
abs_(0xad,0xc0f0,'LDA held_buttons')
imm(0x29,0x60,'AND #$60')
imm(0xc9,0x60,'CMP #$60')
branch(0xf0,'done','BEQ done ; opposed neutral')
imm(0xc9,0x40,'CMP #$40')
branch(0xf0,'left','BEQ left')
imm(0xc9,0x20,'CMP #$20')
branch(0xf0,'right','BEQ right')
abs_(0xad,K,'LDA key')
imm(0xc9,8,'CMP #8')
branch(0xf0,'left','BEQ left')
imm(0xc9,65,'CMP #65')
branch(0xf0,'left','BEQ left')
imm(0xc9,21,'CMP #21')
branch(0xf0,'right','BEQ right')
imm(0xc9,68,'CMP #68')
branch(0xd0,'done','BNE done')
label('right')
abs_(0xad,P,'LDA paddle')
imm(0xc9,33,'CMP #33')
branch(0xf0,'done','BEQ done')
emit(0x18,desc='CLC')
imm(0x69,2,'ADC #2')
imm(0xc9,34,'CMP #34')
branch(0x90,'commit','BCC commit')
imm(0xa9,33,'LDA #33')
branch(0xd0,'commit','BNE commit')
label('left')
abs_(0xad,P,'LDA paddle')
imm(0xc9,1,'CMP #1')
branch(0xf0,'done','BEQ done')
imm(0xc9,3,'CMP #3')
branch(0xb0,'subtract','BCS subtract')
imm(0xa9,1,'LDA #1')
branch(0xd0,'commit','BNE commit')
label('subtract')
emit(0x38,desc='SEC')
imm(0xe9,2,'SBC #2')
label('commit')
abs_(0xae,P,'LDX paddle')
abs_(0x8e,OP,'STX old_paddle')
abs_(0x8d,P,'STA paddle')
abs_(0xee,MV,'INC moved')
abs_(0xee,EV,'INC event')
imm(0xa0,6,'LDY #6 ; six lores paddle cells')
label('erase_paddle')
abs_(0xbd,0x550,'LDA $0550,X ; y37 shares byte with y36')
imm(0xc9,0xf0,'CMP #$F0 ; retain a white ball')
branch(0xb0,'next_erase','BCS next_erase')
imm(0x29,0x0f,'AND #$0F ; preserve lower (y36) nibble')
abs_(0x9d,0x550,'STA $0550,X')
label('next_erase')
emit(0xe8,desc='INX')
emit(0x88,desc='DEY')
branch(0xd0,'erase_paddle','BNE erase_paddle')
label('done')
abs_(0xae,P,'LDX paddle')
imm(0xa0,6,'LDY #6')
label('draw_paddle')
abs_(0xbd,0x550,'LDA $0550,X')
imm(0x09,0xd0,'ORA #$D0 ; paddle color 13, preserve lower nibble')
abs_(0x9d,0x550,'STA $0550,X')
emit(0xe8,desc='INX')
emit(0x88,desc='DEY')
branch(0xd0,'draw_paddle','BNE draw_paddle')
emit(0x60,desc='RTS')
for i,name in fixes:
    distance=labels[name]-(BASE+i+1)
    assert -128<=distance<=127,(i,name,distance)
    code[i]=distance&255
assert len(code)<=0xc0,len(code)
def data_lines():
    return [str(2000 + offset // 24 * 10) + " DATA " +
            ",".join(map(str, code[offset:offset + 24]))
            for offset in range(0, len(code), 24)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assembly", action="store_true")
    parser.add_argument("--check", type=Path, metavar="BASIC_FILE")
    args = parser.parse_args()
    if args.check:
        lines = args.check.read_text().splitlines()
        actual = [line for line in lines if line.split(maxsplit=1) and
                  line.split(maxsplit=1)[0].isdigit() and
                  2000 <= int(line.split(maxsplit=1)[0]) < 2100]
        if actual != data_lines():
            parser.exit(1, "BREAKOUT DATA differs from the input helper source\n")
        loader = f"26 RESTORE:FOR I=0 TO {len(code)-1}:READ V:POKE 768+I,V:NEXT I"
        if loader not in lines:
            parser.exit(1, "BREAKOUT helper loader has the wrong code length\n")
        print(f"PASS BREAKOUT helper DATA ({len(code)} original 6502 bytes)")
    elif args.assembly:
        print("\n".join(asm))
    else:
        print("\n".join(data_lines()))


if __name__ == "__main__":
    main()
