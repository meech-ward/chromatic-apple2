/* SPDX-License-Identifier: MIT */
#include "apple2.h"
#include <string.h>

static void softswitch(apple2_t *m, uint16_t address)
{
    switch (address) {
    case 0xc010: m->key_ready = false; break;
    case 0xc030:
        ++m->speaker_edges;
        m->speaker_level = !m->speaker_level;
        if (m->speaker) m->speaker(m->speaker_context,
                    m->cpu.total_cycle + m->cpu.cycle, m->speaker_level);
        break;
    case 0xc050: m->text_mode = false; break;
    case 0xc051: m->text_mode = true; break;
    case 0xc052: m->mixed_mode = false; break;
    case 0xc053: m->mixed_mode = true; break;
    case 0xc054: m->page_two = false; break;
    case 0xc055: m->page_two = true; break;
    case 0xc056: m->hires_mode = false; break;
    case 0xc057: m->hires_mode = true; break;
    default: break;
    }
}

uint8_t apple2_read(apple2_t *m, uint16_t address)
{
    if (address < sizeof(m->ram)) return m->ram[address];
    if (m->disk && address >= 0xc0e0 && address <= 0xc0ef)
        return disk2_io(m->disk, address, false, 0, m->cpu.total_cycle + m->cpu.cycle);
    if (m->disk && address >= 0xc600 && address <= 0xc6ff)
        return m->disk->boot_rom[address & 255];
    if (address <= 0xc0ff) {
        softswitch(m, address);
        if (address == 0xc000) return m->keyboard | (m->key_ready ? 0x80 : 0);
        if (address == 0xc01a) return m->text_mode ? 0x80 : 0;
        if (address == 0xc061) return m->buttons & 0x08 ? 0x80 : 0;
        if (address == 0xc062) return m->buttons & 0x04 ? 0x80 : 0;
        /* Chromatic extension: held controls, using the handheld button mask.
         * START=1 SELECT=2 B=4 A=8 UP=16 RIGHT=32 LEFT=64 DOWN=128. */
        if (address == 0xc0f0) return m->buttons;
        return 0;
    }
    if (address >= m->rom_base) return m->rom[address - m->rom_base];
    return 0xff;
}

void apple2_write(apple2_t *m, uint16_t address, uint8_t value)
{
    if (address < sizeof(m->ram)) m->ram[address] = value;
    else if (m->disk && address >= 0xc0e0 && address <= 0xc0ef)
        disk2_io(m->disk, address, true, value, m->cpu.total_cycle + m->cpu.cycle);
    else if (address <= 0xc0ff) softswitch(m, address);
}

static mii_cpu_state_t access_memory(mii_cpu_t *cpu, mii_cpu_state_t state)
{
    apple2_t *m = cpu->access_param;
    if (state.w) apple2_write(m, state.addr, state.data);
    else state.data = apple2_read(m, state.addr);
    return state;
}

bool apple2_init(apple2_t *m, const uint8_t *rom, size_t rom_size)
{
    if (!m || !rom || (rom_size != 256 && rom_size != 8192 &&
                       rom_size != 12288 && rom_size != 16384)) return false;
    memset(m, 0, sizeof(*m));
    m->rom = rom;
    m->rom_size = rom_size;
    m->rom_base = (uint16_t)(65536u - rom_size);
    apple2_reset(m, true);
    return true;
}

void apple2_reset(apple2_t *m, bool clear_ram)
{
    if (clear_ram) {
        memset(m->ram, 0, sizeof(m->ram));
        memset(m->ram + 0x400, 0xa0, 0x800);
    }
    memset(&m->cpu, 0, sizeof(m->cpu));
    m->cpu.S = 0xff;
    m->cpu.access = access_memory;
    m->cpu.access_param = m;
    m->state = mii_cpu_init(&m->cpu);
    m->key_ready = false;
    m->text_mode = true;
    m->page_two = false;
    m->mixed_mode = false;
    m->hires_mode = false;
    m->speaker_edges = 0;
    m->speaker_level = false;
    if (m->speaker) m->speaker(m->speaker_context, 0, false);
}

void apple2_run(apple2_t *m, uint32_t instructions)
{
    if (!instructions) return;
    m->cpu.instruction_run = instructions - 1;
    m->state = mii_cpu_run(&m->cpu, m->state);
    if (m->disk) disk2_tick(m->disk, m->cpu.total_cycle + m->cpu.cycle);
    if (m->speaker) m->speaker(m->speaker_context,
                    m->cpu.total_cycle + m->cpu.cycle, m->speaker_level);
}

bool apple2_key(apple2_t *m, uint8_t ascii)
{
    if (m->key_ready) return false;
    m->keyboard = ascii & 0x7f;
    m->key_ready = true;
    return true;
}

void apple2_text_line(const apple2_t *m, unsigned row, char output[41])
{
    if (row >= 24) { output[0] = 0; return; }
    const unsigned base = (m->page_two ? 0x800 : 0x400) +
                          ((row & 7) << 7) + (row >> 3) * 40;
    for (unsigned col = 0; col < 40; ++col) {
        uint8_t value = m->ram[base + col] & 0x7f;
        if (value < 32) value += 64;
        output[col] = value <= 126 ? value : '.';
    }
    output[40] = 0;
}

void apple2_attach_disk(apple2_t *m, disk2_t *disk) { m->disk = disk; }

void apple2_attach_speaker(apple2_t *m, apple2_speaker_fn notify, void *context)
{
    m->speaker = notify;
    m->speaker_context = context;
    if (notify) notify(context, m->cpu.total_cycle + m->cpu.cycle, m->speaker_level);
}
