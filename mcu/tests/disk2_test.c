/* SPDX-License-Identifier: MIT */
#include "disk2.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t image[35 * 4096], expected[4096], written_nibbles[6656], boot[256];
static disk2_t disk;
static unsigned writes, write_attempts;
static bool fail_writes;
static bool read_track(void *context, unsigned track, uint8_t output[4096])
{
    (void)context;
    memcpy(output, image + track * 4096, 4096); return true;
}
static bool write_track(void *context, unsigned track, const uint8_t input[4096])
{
    (void)context;
    ++write_attempts;
    if (fail_writes) return false;
    memcpy(image + track * 4096, input, 4096); ++writes; return true;
}

int main(void)
{
    uint8_t source[256], decoded[256], encoded[343];
    uint32_t random = 1;
    for (unsigned trial = 0; trial < 256; ++trial) {
        for (unsigned i = 0; i < 256; ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            source[i] = random;
        }
        disk2_encode_sector(source, encoded);
        assert(disk2_decode_sector(encoded, decoded));
        assert(!memcmp(source, decoded, 256));
        encoded[342] = encoded[342] == 0x96 ? 0x97 : 0x96;
        assert(!disk2_decode_sector(encoded, decoded));
        encoded[0] = 0;
        assert(!disk2_decode_sector(encoded, decoded));
    }
    for (unsigned i = 0; i < sizeof(image); ++i) image[i] = i ^ (i >> 8);
    assert(disk2_init(&disk, boot, read_track, write_track, NULL));
    disk2_io(&disk, 0xc0e9, false, 0, 0);
    assert(disk2_io(&disk, 0xc0ec, false, 0, 32) == 0xff);
    assert(disk2_io(&disk, 0xc0ec, false, 0, 33) == 0);
    assert(disk2_io(&disk, 0xc0ec, false, 0, 17 * 32) == 0xd5);
    memcpy(expected, image, 4096);
    memset(expected, 0x5a, 256);
    memcpy(disk.sectors, expected, 4096);
    disk2_encode_sector(expected, disk.nibbles + 38);
    memcpy(written_nibbles, disk.nibbles, sizeof(written_nibbles));
    // Restore the initial track, then replay a whole revolution through Q6/Q7.
    assert(disk2_init(&disk, boot, read_track, write_track, NULL));
    disk2_io(&disk, 0xc0e9, false, 0, 0);
    disk2_io(&disk, 0xc0ef, false, 0, 0);
    for (unsigned i = 0; i < sizeof(written_nibbles); ++i) {
        disk2_io(&disk, 0xc0ed, true, written_nibbles[i], i * 32);
        disk2_io(&disk, 0xc0ec, false, 0, i * 32);
    }
    disk2_io(&disk, 0xc0ee, false, 0, sizeof(written_nibbles) * 32);
    assert(disk.dirty);
    disk2_io(&disk, 0xc0ea, false, 0, 220000);
    disk2_io(&disk, 0xc0ea, true, 0, 220001);
    assert(disk.selected == 0 && disk.dirty && writes == 0 && write_attempts == 0);
    disk2_io(&disk, 0xc0eb, false, 0, 220002);
    assert(disk.selected == 1 && !disk.dirty && writes == 1 && write_attempts == 1);
    assert(disk2_flush(&disk));
    assert(writes == 1 && !memcmp(image, expected, 4096));
    disk2_io(&disk, 0xc0e8, false, 0, 300000);
    disk2_tick(&disk, 300000 + 1023000);
    assert(!disk.motor);
    assert(disk2_init(&disk, boot, read_track, write_track, NULL));
    disk.dirty = true; /* The initialized nibble track is a valid dirty track. */
    fail_writes = true;
    disk2_io(&disk, 0xc0eb, false, 0, 0);
    assert(disk.selected == 0 && disk.dirty && disk.error && write_attempts == 2 && writes == 1);
    disk2_io(&disk, 0xc0ea, false, 0, 0);
    assert(disk.selected == 0 && disk.dirty && write_attempts == 2);
    fail_writes = false;
    disk2_io(&disk, 0xc0eb, false, 0, 0);
    assert(disk.selected == 1 && !disk.dirty && write_attempts == 3 && writes == 2);
    assert(disk2_init(&disk, boot, read_track, NULL, NULL));
    disk2_io(&disk, 0xc0ed, false, 0, 0);
    assert(disk2_io(&disk, 0xc0ee, false, 0, 0) == 0x80);
    printf("PASS GCR62 randomized roundtrips/checksum, nibble timing, Q6/Q7 writes, same-drive no-flush, drive-change flush/failure, persistence callback, motor-off, write protect; disk=%zu bytes\n", sizeof(disk));
    return 0;
}
