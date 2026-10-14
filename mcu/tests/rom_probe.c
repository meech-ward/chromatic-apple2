/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include <stdio.h>
#include <stdlib.h>

static apple2_t machine;
static uint8_t rom[16384];

static void dump(void)
{
    char line[41];
    printf("PC=%04x cycles=%llu key=%u\n", machine.cpu.PC,
           (unsigned long long)machine.cpu.total_cycle, machine.key_ready);
    for (unsigned row = 0; row < 24; ++row) {
        apple2_text_line(&machine, row, line);
        puts(line);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 3;
    const size_t size = fread(rom, 1, sizeof(rom), file);
    fclose(file);
    if (!apple2_init(&machine, rom, size)) return 4;
    apple2_run(&machine, 2000000);
    puts("BEFORE INPUT");
    dump();
    const char *command = "PRINT 2+2\r";
    for (unsigned i = 0; command[i]; ++i) {
        if (!apple2_key(&machine, command[i])) {
            puts("KEYBOARD LATCH NOT CONSUMED"); return 5;
        }
        apple2_run(&machine, 100000);
    }
    puts("AFTER INPUT");
    dump();
    bool arithmetic_visible = false;
    for (unsigned row = 0; row < 24; ++row) {
        char line[41];
        apple2_text_line(&machine, row, line);
        if (line[0] == '4' && line[1] == ' ') arithmetic_visible = true;
    }
    if (!arithmetic_visible) { puts("FAIL BASIC arithmetic result not visible"); return 6; }
    puts("PASS private-ROM BASIC PRINT 2+2 produced 4");
    return 0;
}
