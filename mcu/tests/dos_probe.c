/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include "disk2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static apple2_t machine;
static disk2_t disk;
static uint8_t rom[16384], slot_rom[256];
static FILE *image;
static bool read_track(void *context, unsigned track, uint8_t output[4096])
{
    (void)context;
    return !fseek(image, track * 4096, SEEK_SET) && fread(output, 1, 4096, image) == 4096;
}
static bool write_track(void *context, unsigned track, const uint8_t input[4096])
{
    (void)context;
    return !fseek(image, track * 4096, SEEK_SET) && fwrite(input, 1, 4096, image) == 4096 &&
           !fflush(image) && !fsync(fileno(image));
}
static void screen(void)
{
    char line[41];
    printf("PC=%04x cycles=%llu track=%d position=%u reads=%u writes=%u flushes=%u error=%u\n",
           machine.cpu.PC, (unsigned long long)machine.cpu.total_cycle,
           disk.cached_track, disk.position, disk.read_count, disk.write_count, disk.flush_count, disk.error);
    for (unsigned row = 0; row < 24; ++row) {
        apple2_text_line(&machine, row, line); puts(line);
    }
    fflush(stdout);
}
static bool type(const char *text)
{
    for (unsigned i = 0; text[i]; ++i) {
        for (unsigned waits = 0; machine.key_ready && waits < 50; ++waits)
            apple2_run(&machine, 100000);
        if (!apple2_key(&machine, text[i])) return false;
        apple2_run(&machine, 30000);
    }
    apple2_run(&machine, 2000000);
    return true;
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    FILE *input = fopen(argv[1], "rb");
    if (!input || fread(rom, 1, sizeof(rom), input) != sizeof(rom)) return 3;
    fclose(input);
    input = fopen(argv[2], "rb");
    if (!input || fread(slot_rom, 1, sizeof(slot_rom), input) != sizeof(slot_rom)) return 4;
    fclose(input);
    image = fopen(argv[3], "r+b");
    if (!image || !apple2_init(&machine, rom, sizeof(rom)) ||
        !disk2_init(&disk, slot_rom, read_track, write_track, NULL)) return 5;
    apple2_attach_disk(&machine, &disk);
    for (unsigned i = 0; i < 12; ++i) {
        apple2_run(&machine, 2000000);
        printf("BOOT_STEP %u pc=%04x track=%d disk_error=%u\n", i, machine.cpu.PC, disk.cached_track, disk.error);
        fflush(stdout);
    }
    screen();
    if (!type("CATALOG\r")) return 6;
    screen();
    if (!type("NEW\r10 PRINT \"CHROMATIC DISK PASS\"\rSAVE CHROMATIC QA\r")) return 7;
    apple2_run(&machine, 6000000);
    if (!disk2_flush(&disk)) {
        printf("FLUSH FAILED mask=%04x dirty=%u motor=%u q6=%u q7=%u\n", disk.flush_mask, disk.dirty, disk.motor, disk.q6, disk.q7);
        screen();
        FILE *debug = fopen("build/host/failed-track.nib", "wb");
        if (debug) { fwrite(disk.nibbles, 1, sizeof(disk.nibbles), debug); fclose(debug); }
        return 8;
    }
    screen();
    const unsigned persisted_flushes = disk.flush_count;
    fclose(image);
    image = fopen(argv[3], "r+b");
    if (!image || !disk2_init(&disk, slot_rom, read_track, write_track, NULL)) return 11;
    apple2_reset(&machine, true);
    apple2_run(&machine, 6000000);
    puts("REOPENED IMAGE AND REBOOTED");
    if (!type("NEW\rLOAD CHROMATIC QA\rRUN\r")) return 9;
    screen();
    bool found = false;
    char line[41];
    for (unsigned row = 0; row < 24; ++row) {
        apple2_text_line(&machine, row, line);
        if (strstr(line, "CHROMATIC DISK PASS")) found = true;
    }
    fclose(image);
    if (!found || disk.error || !persisted_flushes) return 10;
    puts("PASS DOS boot/CATALOG/SAVE, image close/reopen and reboot, LOAD/RUN with real slot ROM and file persistence");
    return 0;
}
