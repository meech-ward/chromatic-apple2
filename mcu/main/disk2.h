/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DISK2_TRACK_BYTES = 4096, DISK2_NIB_BYTES = 6656, DISK2_TRACKS = 35 };
typedef bool (*disk2_read_track_fn)(void *context, unsigned track, uint8_t output[4096]);
typedef bool (*disk2_write_track_fn)(void *context, unsigned track, const uint8_t data[4096]);

typedef struct {
    uint8_t sectors[DISK2_TRACK_BYTES];
    uint8_t nibbles[DISK2_NIB_BYTES];
    disk2_read_track_fn read_track;
    disk2_write_track_fn write_track;
    void *context;
    const uint8_t *boot_rom;
    int cached_track;
    int half_track[2];
    uint8_t phase[2];
    uint8_t selected;
    uint8_t latch;
    uint8_t write_latch;
    uint32_t position;
    uint64_t last_cycle;
    uint64_t motor_off_cycle;
    bool motor;
    bool q6;
    bool q7;
    bool data_ready;
    bool dirty;
    bool error;
    uint32_t read_count;
    uint32_t write_count;
    uint32_t flush_count;
    uint16_t flush_mask;
} disk2_t;

bool disk2_init(disk2_t *disk, const uint8_t boot_rom[256],
                disk2_read_track_fn read_track, disk2_write_track_fn write_track,
                void *context);
bool disk2_flush(disk2_t *disk);
void disk2_tick(disk2_t *disk, uint64_t cycles);
uint8_t disk2_io(disk2_t *disk, uint16_t address, bool write, uint8_t value, uint64_t cycles);
void disk2_encode_sector(const uint8_t input[256], uint8_t output[343]);
bool disk2_decode_sector(const uint8_t input[343], uint8_t output[256]);
