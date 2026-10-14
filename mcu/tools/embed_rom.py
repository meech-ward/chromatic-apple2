#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate build-only ROM data, using an original diagnostic by default."""
import argparse
from pathlib import Path


def diagnostic():
    image = bytearray([0xEA] * 256)
    program = bytearray()
    labels, patches = {}, []

    def emit(*values):
        program.extend(values)

    def label(name):
        labels[name] = len(program)

    def branch(opcode, target):
        emit(opcode, 0)
        patches.append((len(program) - 1, target, False))

    def jump(opcode, target):
        emit(opcode, 0, 0)
        patches.append((len(program) - 2, target, True))

    # Reset, stack, text output through actual 6502 memory writes.
    emit(0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA2, 0x00)
    label('text')
    emit(0xBD, 0xA0, 0xFF)
    branch(0xF0, 'math')
    emit(0x9D, 0x00, 0x04, 0xE8)
    branch(0xD0, 'text')
    label('math')
    emit(0x18, 0xA9, 7, 0x69, 5, 0xC9, 12)
    branch(0xD0, 'fail')
    emit(0x8D, 0x01, 0x02)
    # Decimal 45 + 55 = 00 with carry; tests real CPU decimal arithmetic.
    emit(0xF8, 0x18, 0xA9, 0x45, 0x69, 0x55, 0xC9, 0)
    branch(0xD0, 'fail')
    branch(0x90, 'fail')
    emit(0xD8)
    jump(0x20, 'subroutine')
    emit(0xA9, 0xA2, 0x8D, 0x00, 0x02)
    label('keyboard')
    emit(0xAD, 0x00, 0xC0)
    branch(0x10, 'keyboard')
    emit(0x8D, 0x80, 0x04, 0x2C, 0x10, 0xC0)
    jump(0x4C, 'keyboard')
    label('subroutine')
    emit(0x48, 0xA9, 0x5A, 0x8D, 0x02, 0x02, 0x68, 0x60)
    label('fail')
    emit(0xA9, 0xEE, 0x8D, 0x00, 0x02)
    jump(0x4C, 'fail')
    for offset, target, absolute in patches:
        if absolute:
            address = 0xFF00 + labels[target]
            program[offset:offset + 2] = address.to_bytes(2, 'little')
        else:
            distance = labels[target] - offset - 1
            assert -128 <= distance < 128
            program[offset] = distance & 255
    assert len(program) <= 0xA0
    image[:len(program)] = program
    message = b'CHROMATIC APPLE II CPU / TEXT PROOF\0'
    image[0xA0:0xA0 + len(message)] = message
    for offset in (0xFA, 0xFC, 0xFE):
        image[offset:offset + 2] = b'\x00\xff'
    return bytes(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    parser.add_argument('--input', default='')
    parser.add_argument('--disk-rom', default='')
    args = parser.parse_args()
    if args.input:
        data = Path(args.input).read_bytes()
        if len(data) not in (8192, 12288, 16384):
            parser.error('User ROM must be 8192, 12288 or 16384 bytes')
        kind = 'external-user-rom'
    else:
        data, kind = diagnostic(), 'original-diagnostic'
    lines = ['/* Build-generated data. User-supplied ROM retains its own rights. */',
             '#include "rom_image.h"', 'const uint8_t a2_rom_image[] = {']
    for offset in range(0, len(data), 16):
        lines.append('    ' + ','.join(f'0x{x:02x}' for x in data[offset:offset + 16]) + ',')
    lines += ['};', f'const size_t a2_rom_size = {len(data)};',
              f'const char a2_rom_kind[] = "{kind}";', '']
    disk_rom = Path(args.disk_rom).read_bytes() if args.disk_rom else b''
    if disk_rom and len(disk_rom) != 256:
        parser.error('Disk II slot ROM must be 256 bytes')
    lines.append('const uint8_t a2_disk_rom[] = {')
    if disk_rom:
        for offset in range(0, len(disk_rom), 16):
            lines.append('    ' + ','.join(f'0x{x:02x}' for x in disk_rom[offset:offset + 16]) + ',')
    else:
        lines.append('    0,')
    lines += ['};', f'const size_t a2_disk_rom_size = {len(disk_rom)};', '']
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('\n'.join(lines))


if __name__ == '__main__':
    main()
