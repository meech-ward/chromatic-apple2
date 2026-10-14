/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include "rom_image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static apple2_t machine;
static uint8_t large_rom[8192];

int main(void)
{
    assert(!apple2_init(&machine, a2_rom_image, 100));
    assert(apple2_init(&machine, a2_rom_image, a2_rom_size));
    assert(sizeof(machine) < 50000);
    apple2_run(&machine, 2000);
    assert(machine.ram[0x200] == 0xa2);
    assert(machine.ram[0x201] == 12);
    assert(machine.ram[0x202] == 0x5a);
    char line[41];
    apple2_text_line(&machine, 0, line);
    assert(strstr(line, "CHROMATIC APPLE II CPU / TEXT PROOF") == line);
    assert(apple2_key(&machine, 'K'));
    assert(!apple2_key(&machine, 'X'));
    apple2_run(&machine, 100);
    assert(!machine.key_ready);
    apple2_text_line(&machine, 1, line);
    assert(line[0] == 'K');
    uint8_t opcode = apple2_read(&machine, 0xff00);
    apple2_write(&machine, 0xff00, 0);
    assert(apple2_read(&machine, 0xff00) == opcode);
    machine.buttons=0x68; /* A + held left/right are independent of key latch. */
    assert(apple2_read(&machine,0xc0f0)==0x68);
    assert(apple2_read(&machine,0xc061)==0x80);
    assert(apple2_read(&machine,0xc062)==0);
    apple2_write(&machine,0xc0f0,0);
    assert(apple2_read(&machine,0xc0f0)==0x68);
    machine.buttons=0;
    apple2_write(&machine, 0x428, '8' | 0x80);
    apple2_text_line(&machine, 8, line);
    assert(line[0] == '8');
    apple2_read(&machine, 0xc055);
    apple2_write(&machine, 0xbd0, 'P' | 0x80);
    apple2_text_line(&machine, 23, line);
    assert(line[0] == 'P');
    apple2_reset(&machine, false);
    assert(!machine.page_two && machine.ram[0x428] == ('8' | 0x80));
    memset(large_rom, 0xea, sizeof(large_rom));
    memcpy(large_rom + sizeof(large_rom) - a2_rom_size, a2_rom_image, a2_rom_size);
    assert(apple2_init(&machine, large_rom, sizeof(large_rom)));
    apple2_run(&machine, 2000);
    assert(machine.ram[0x200] == 0xa2);
    printf("PASS 65C02 reset, binary/decimal arithmetic, branches, JSR/stack, text mapping, keyboard strobe, ROM protection; machine=%zu bytes\n", sizeof(machine));
    return 0;
}
