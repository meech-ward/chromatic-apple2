/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include "rom_image.h"
#include <assert.h>
#include <stdio.h>
static apple2_t machine;
static unsigned calls;
static uint64_t last_cycle;
static bool last_level;
static void event(void *context, uint64_t cycle, bool level)
{
    assert(context == &calls);
    if (cycle) assert(cycle >= last_cycle);
    ++calls;
    last_cycle = cycle;
    last_level = level;
}
int main(void)
{
    assert(apple2_init(&machine, a2_rom_image, a2_rom_size));
    apple2_attach_speaker(&machine, event, &calls);
    assert(calls == 1 && last_cycle == 0 && !last_level);
    apple2_run(&machine, 2000);
    assert(calls == 2 && last_cycle > 0 && !last_level);
    const uint64_t cycle = last_cycle;
    apple2_read(&machine, 0xc030);
    assert(calls == 3 && last_cycle == cycle && last_level);
    apple2_write(&machine, 0xc030, 0);
    assert(calls == 4 && last_cycle == cycle && !last_level);
    assert(machine.speaker_edges == 2);
    apple2_reset(&machine, false);
    assert(calls == 5 && last_cycle == 0 && !last_level);
    apple2_run(&machine, 100);
    assert(calls == 6 && last_cycle > 0 && !last_level);
    apple2_attach_speaker(&machine, NULL, NULL);
    apple2_read(&machine, 0xc030);
    assert(calls == 6 && machine.speaker_edges == 1);
    uint8_t tone[256] = {0xad, 0x30, 0xc0, 0x4c, 0x00, 0xff};
    tone[252] = 0; tone[253] = 0xff;
    assert(apple2_init(&machine, tone, sizeof(tone)));
    calls = 0; last_cycle = 0;
    apple2_attach_speaker(&machine, event, &calls);
    apple2_run(&machine, 5000);
    assert(machine.speaker_edges > 2000 && calls == machine.speaker_edges + 2);
    puts("PASS optional speaker hook: C030 read/write levels, batch timestamps, reset binding and detach");
}
