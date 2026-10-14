/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mii_65c02.h"
#include "disk2.h"

/* Nonblocking notification on C030 changes, run boundaries and cycle-zero reset.
 * Called under the caller's machine lock; the binding survives CPU reset. */
typedef void (*apple2_speaker_fn)(void *context, uint64_t cycles, bool level);

typedef struct apple2 {
    uint8_t ram[48 * 1024];
    const uint8_t *rom;
    size_t rom_size;
    uint16_t rom_base;
    uint8_t keyboard;
    bool key_ready;
    bool text_mode;
    bool page_two;
    bool mixed_mode;
    bool hires_mode;
    uint8_t buttons;
    uint32_t speaker_edges;
    bool speaker_level;
    apple2_speaker_fn speaker;
    void *speaker_context;
    disk2_t *disk;
    mii_cpu_t cpu;
    mii_cpu_state_t state;
} apple2_t;

bool apple2_init(apple2_t *machine, const uint8_t *rom, size_t rom_size);
void apple2_reset(apple2_t *machine, bool clear_ram);
void apple2_run(apple2_t *machine, uint32_t instructions);
uint8_t apple2_read(apple2_t *machine, uint16_t address);
void apple2_write(apple2_t *machine, uint16_t address, uint8_t value);
bool apple2_key(apple2_t *machine, uint8_t ascii);
void apple2_text_line(const apple2_t *machine, unsigned row, char output[41]);
void apple2_attach_disk(apple2_t *machine, disk2_t *disk);
void apple2_attach_speaker(apple2_t *machine, apple2_speaker_fn notify, void *context);
