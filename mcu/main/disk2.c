/*
 * SPDX-License-Identifier: MIT
 * 6-and-2 codec adapted from MII mii_dsk.c.
 * Copyright (C) 2023-2024 Michel Pollet <buserror@gmail.com>
 * Copyright (c) 2010-2021 Will Scullin and contributors (apple2js ancestry).
 * The byte-track controller and backend adapter are new for this port.
 * See SOURCE-MANIFEST.json and licenses/MII-MIT.txt, Apple2JS-MIT.txt.
 */
#include "disk2.h"
#include <string.h>

static const uint8_t gcr[64] = {
    0x96,0x97,0x9a,0x9b,0x9d,0x9e,0x9f,0xa6,0xa7,0xab,0xac,0xad,0xae,0xaf,0xb2,0xb3,
    0xb4,0xb5,0xb6,0xb7,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xcb,0xcd,0xce,0xcf,0xd3,
    0xd6,0xd7,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,0xe5,0xe6,0xe7,0xe9,0xea,0xeb,0xec,
    0xed,0xee,0xef,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf9,0xfa,0xfb,0xfc,0xfd,0xfe,0xff
};
static const uint8_t dos_order[16] = {0,7,14,6,13,5,12,4,11,3,10,2,9,1,8,15};

void disk2_encode_sector(const uint8_t input[256], uint8_t output[343])
{
    uint8_t values[344] = {0};
    int low = 85;
    for (int high = 257; high >= 0; --high) {
        uint8_t data = input[high & 255];
        uint8_t bits = values[low];
        bits = (bits << 1) | (data & 1); data >>= 1;
        bits = (bits << 1) | (data & 1); data >>= 1;
        values[86 + high] = data;
        values[low] = bits;
        if (--low < 0) low = 85;
    }
    uint8_t previous = 0;
    for (unsigned i = 0; i < 342; ++i) {
        output[i] = gcr[(previous ^ values[i]) & 63];
        previous = values[i];
    }
    output[342] = gcr[previous & 63];
}

bool disk2_decode_sector(const uint8_t input[343], uint8_t output[256])
{
    uint8_t reverse[256], low[86], previous = 0;
    memset(reverse, 0xff, sizeof(reverse));
    for (unsigned i = 0; i < 64; ++i) reverse[gcr[i]] = i;
    for (unsigned i = 0; i < 343; ++i) {
        if (reverse[input[i]] == 0xff) return false;
        const uint8_t value = reverse[input[i]] ^ previous;
        if (i < 86) low[85 - i] = value;
        else if (i < 342) output[i - 86] = value;
        else if (value != 0) return false;
        previous = value;
    }
    int index = 85;
    for (unsigned i = 0; i < 256; ++i) {
        output[i] = (output[i] << 1) | (low[index] & 1); low[index] >>= 1;
        output[i] = (output[i] << 1) | (low[index] & 1); low[index] >>= 1;
        if (--index < 0) index = 85;
    }
    return true;
}

static uint8_t un44(uint8_t first, uint8_t second)
{
    return ((first & 0x55) << 1) | (second & 0x55);
}

static void address44(uint8_t *target, uint8_t value)
{
    target[0] = (value >> 1) | 0xaa;
    target[1] = value | 0xaa;
}

static bool load_track(disk2_t *disk, unsigned track)
{
    if ((int)track == disk->cached_track) return !disk->error;
    if (!disk2_flush(disk)) return false;
    if (track >= DISK2_TRACKS || !disk->read_track(disk->context, track, disk->sectors)) {
        disk->error = true;
        return false;
    }
    memset(disk->nibbles, 0xff, sizeof(disk->nibbles));
    for (unsigned physical = 0; physical < 16; ++physical) {
        uint8_t *sector = disk->nibbles + physical * 416;
        sector[16] = 0xd5; sector[17] = 0xaa; sector[18] = 0x96;
        address44(sector + 19, 254);
        address44(sector + 21, track);
        address44(sector + 23, physical);
        address44(sector + 25, 254 ^ track ^ physical);
        sector[27] = 0xde; sector[28] = 0xaa; sector[29] = 0xeb;
        sector[35] = 0xd5; sector[36] = 0xaa; sector[37] = 0xad;
        disk2_encode_sector(disk->sectors + dos_order[physical] * 256, sector + 38);
        sector[381] = 0xde; sector[382] = 0xaa; sector[383] = 0xeb;
    }
    disk->cached_track = track;
    disk->error = false;
    return true;
}

static uint8_t track_byte(const disk2_t *disk, unsigned position)
{
    return disk->nibbles[position % DISK2_NIB_BYTES];
}

bool disk2_flush(disk2_t *disk)
{
    if (!disk->dirty) return true;
    if (!disk->write_track || disk->cached_track < 0) return false;
    uint16_t found = 0;
    uint8_t encoded[343], decoded[256];
    for (unsigned position = 0; position < DISK2_NIB_BYTES; ++position) {
        if (track_byte(disk, position) != 0xd5 || track_byte(disk, position + 1) != 0xaa ||
            track_byte(disk, position + 2) != 0x96) continue;
        const uint8_t volume = un44(track_byte(disk, position + 3), track_byte(disk, position + 4));
        const uint8_t track = un44(track_byte(disk, position + 5), track_byte(disk, position + 6));
        const uint8_t physical = un44(track_byte(disk, position + 7), track_byte(disk, position + 8));
        const uint8_t checksum = un44(track_byte(disk, position + 9), track_byte(disk, position + 10));
        if (physical > 15 || track != disk->cached_track || (volume ^ track ^ physical) != checksum)
            continue;
        for (unsigned gap = 14; gap < 80; ++gap) {
            const unsigned data = position + gap;
            if (track_byte(disk, data) != 0xd5 || track_byte(disk, data + 1) != 0xaa ||
                track_byte(disk, data + 2) != 0xad) continue;
            for (unsigned i = 0; i < sizeof(encoded); ++i) encoded[i] = track_byte(disk, data + 3 + i);
            if (disk2_decode_sector(encoded, decoded)) {
                memcpy(disk->sectors + dos_order[physical] * 256, decoded, sizeof(decoded));
                found |= 1u << physical;
            }
            break;
        }
    }
    disk->flush_mask = found;
    if (found != 0xffff || !disk->write_track(disk->context, disk->cached_track, disk->sectors)) {
        disk->error = true;
        return false;
    }
    disk->dirty = false;
    ++disk->flush_count;
    return true;
}

bool disk2_init(disk2_t *disk, const uint8_t boot_rom[256],
                disk2_read_track_fn read_track, disk2_write_track_fn write_track,
                void *context)
{
    if (!disk || !boot_rom || !read_track) return false;
    memset(disk, 0, sizeof(*disk));
    disk->boot_rom = boot_rom;
    disk->read_track = read_track;
    disk->write_track = write_track;
    disk->context = context;
    disk->cached_track = -1;
    return load_track(disk, 0);
}

void disk2_tick(disk2_t *disk, uint64_t cycles)
{
    if (disk->motor_off_cycle && cycles >= disk->motor_off_cycle) {
        if (!disk2_flush(disk)) return;
        disk->motor = false;
        disk->motor_off_cycle = 0;
    }
}

static void advance(disk2_t *disk, uint64_t cycles)
{
    if (!disk->motor || disk->selected != 0) { disk->last_cycle = cycles; return; }
    // Standard RWTS emits one complete nibble per Q6 load strobe. Preserve those
    // bytes rather than sampling a global 32-cycle boundary during the load.
    // This byte-track model intentionally does not emulate flux-level writes.
    if (disk->q7) { disk->last_cycle = cycles; return; }
    if (cycles < disk->last_cycle) disk->last_cycle = cycles;
    const uint64_t elapsed = cycles - disk->last_cycle;
    const uint32_t count = elapsed / 32;
    if (!count) return;
    if (!disk->q7) {
        disk->latch = disk->nibbles[(disk->position + count - 1) % DISK2_NIB_BYTES];
        disk->data_ready = true;
    }
    disk->position = (disk->position + count) % DISK2_NIB_BYTES;
    disk->last_cycle += (uint64_t)count * 32;
}

uint8_t disk2_io(disk2_t *disk, uint16_t address, bool write, uint8_t value, uint64_t cycles)
{
    advance(disk, cycles);
    const unsigned control = address & 15;
    if (control < 8 && (control & 1)) {
        static const int8_t delta[4][4] = {{0,1,2,-1},{-1,0,1,2},{-2,-1,0,1},{1,-2,-1,0}};
        const unsigned phase = control >> 1;
        int half = disk->half_track[disk->selected] + delta[disk->phase[disk->selected]][phase];
        if (half < 0) half = 0;
        if (half > 68) half = 68;
        if (disk->selected == 0 && !load_track(disk, half / 2)) return 0;
        disk->half_track[disk->selected] = half;
        disk->phase[disk->selected] = phase;
    } else switch (control) {
    case 8: disk->motor_off_cycle = cycles + 1023000; break;
    case 9: disk->motor = true; disk->motor_off_cycle = 0; disk->last_cycle = cycles; break;
    case 10: case 11:
        if ((control & 1) != disk->selected && disk2_flush(disk))
            disk->selected = control & 1;
        break;
    case 12: disk->q6 = false; break;
    case 13:
        disk->q6 = true;
        if (write) {
            disk->write_latch = value;
            if (disk->q7 && disk->motor && disk->selected == 0 && disk->write_track && !disk->error) {
                disk->nibbles[disk->position] = value;
                disk->position = (disk->position + 1) % DISK2_NIB_BYTES;
                disk->dirty = true;
                ++disk->write_count;
            }
        }
        break;
    case 14: disk->q7 = false; break;
    case 15: disk->q7 = true; break;
    default: break;
    }
    if (write) return 0;
    if (disk->q6 && !disk->q7) return disk->write_track ? 0 : 0x80;
    if (!disk->q6 && !disk->q7 && disk->motor && disk->selected == 0 && !disk->error) {
        const uint8_t result = disk->data_ready ? disk->latch : 0;
        disk->data_ready = false;
        ++disk->read_count;
        return result;
    }
    return 0;
}
