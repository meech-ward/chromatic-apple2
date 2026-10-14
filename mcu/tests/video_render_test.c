/* SPDX-License-Identifier: MIT */
#include "video_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t ram[49152], output[A2_FRAME_BYTES + 2];
static uint8_t page[1024], reference[A2_FRAME_BYTES];
/* Documented text row starts, relative to $0400/$0800. The holes are not rows. */
static const unsigned row_offsets[24] = {
    0x000, 0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380,
    0x028, 0x0a8, 0x128, 0x1a8, 0x228, 0x2a8, 0x328, 0x3a8,
    0x050, 0x0d0, 0x150, 0x1d0, 0x250, 0x2d0, 0x350, 0x3d0
};

static unsigned get(unsigned x, unsigned y)
{
    assert(x < 160 && y < 144);
    const unsigned value = output[1 + y * 80 + x / 2];
    return x & 1 ? value & 15 : value >> 4;
}

static void check_bounds(void)
{
    assert(output[0] == 0x55 && output[sizeof(output) - 1] == 0x55);
}

static void screen(bool text, bool second, bool mixed, bool hires, bool flash,
                    uint8_t foreground)
{
    /* Every frame must overwrite its entire output, even when modes change. */
    memset(output, 0x55, sizeof(output));
    a2_render_screen(ram, text, second, mixed, hires, flash, foreground, output + 1);
    check_bounds();
}

static void test_text(void)
{
    memset(ram, 0xa0, sizeof(ram));
    memset(output, 0x55, sizeof(output));
    ram[0x400] = 0xc1; /* A, normal */
    ram[0x480] = 0x01; /* row 1, A inverse */
    ram[0x428] = 0x41; /* row 8, A flashing */
    ram[0x850] = 0xc1; /* page 2, row 16 */
    a2_render_text(ram, false, false, 12, output + 1);
    assert(get(0, 0) == 0 && get(1, 0) == 12 && get(2, 0) == 0);
    assert(get(0, 2) == 12 && get(1, 2) == 12 && get(2, 2) == 12);
    assert(get(0, 6) == 12 && get(1, 6) == 0 && get(3, 11) == 12);
    assert(get(1, 48) == 12 && get(3, 53) == 0);
    memcpy(reference, output + 1, A2_FRAME_BYTES);
    memcpy(page, ram + 0x400, sizeof(page));
    a2_render_page(page, false, 0xfc, output + 1);
    assert(!memcmp(reference, output + 1, A2_FRAME_BYTES));
    screen(true, false, true, true, false, 12);
    assert(!memcmp(reference, output + 1, A2_FRAME_BYTES));
    a2_render_text(ram, false, true, 12, output + 1);
    assert(get(1, 48) == 0 && get(3, 53) == 12);
    a2_render_text(ram, true, false, 15, output + 1);
    assert(get(1, 96) == 15 && get(0, 96) == 0);
    assert(get(1, 0) == 0);
    check_bounds();

    /* All encodings have a glyph; no character can index beyond the font. */
    for (unsigned raw = 0; raw <= 255; ++raw) {
        memset(page, raw, sizeof(page));
        a2_render_page(page, raw & 1, 15, output + 1);
        check_bounds();
    }
}

static void test_lores(void)
{
    memset(ram, 0, sizeof(ram));
    for (unsigned row = 0; row < 24; ++row) {
        for (unsigned col = 0; col < 40; ++col) {
            const unsigned top = (row + col) & 15;
            const unsigned bottom = (15 - row + col) & 15;
            ram[0x400 + row_offsets[row] + col] = top | (bottom << 4);
        }
    }
    screen(false, false, false, false, false, 12);
    for (unsigned y = 0; y < 144; ++y) {
        for (unsigned x = 0; x < 160; ++x) {
            const unsigned row = y / 6, col = x / 4;
            const unsigned expected = (y % 6 < 3 ? row + col : 15 - row + col) & 15;
            assert(get(x, y) == expected);
        }
    }
    /* Page two must use its own bytes, with the low nibble above the high. */
    memset(ram + 0x800, 0xa9, 1024);
    screen(false, true, false, false, false, 15);
    assert(get(0, 0) == 9 && get(159, 2) == 9);
    assert(get(0, 3) == 10 && get(159, 143) == 10);
}

static void test_hires(void)
{
    memset(ram, 0, sizeof(ram));
    /* Pixel order across a seven-bit byte boundary, and the rightmost pixel. */
    ram[0x2000] = 0x45; /* source x=0,2,6 */
    ram[0x2001] = 0x01; /* source x=7 */
    ram[0x2027] = 0x40; /* source x=279 */
    screen(false, false, false, true, false, 0xfc);
    assert(get(0, 0) == 12 && get(1, 0) == 12 && get(2, 0) == 0);
    assert(get(3, 0) == 12 && get(4, 0) == 12 && get(5, 0) == 0);
    assert(get(159, 0) == 12 && get(158, 0) == 0);
    /* Fixed addresses exercise the interleave, 64-line bands and final row. */
    static const struct { unsigned address, destination_y; } lines[] = {
        {0x2800, 1},  /* source row 2 */
        {0x3c00, 5},  /* 7 */
        {0x2080, 6},  /* 8 */
        {0x3f80, 47}, /* 63 */
        {0x2028, 48}, /* 64 */
        {0x3fa8, 95}, /* 127 */
        {0x2050, 96}, /* 128 */
        {0x3fd0, 143} /* 191 */
    };
    memset(ram + 0x2000, 0, 8192);
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
        memset(ram + lines[i].address, 0x7f, 40);
    screen(false, false, false, true, false, 15);
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); ++i)
        for (unsigned x = 0; x < 160; ++x)
            assert(get(x, lines[i].destination_y) == 15);
    assert(get(0, 2) == 0 && get(159, 142) == 0);

    /* The high bit changes NTSC color phase, not the monochrome bitmap. */
    memset(ram + 0x4000, 0x80, 8192);
    screen(false, true, false, true, false, 15);
    for (unsigned i = 0; i < A2_FRAME_BYTES; ++i) assert(output[i + 1] == 0);
    memset(ram + 0x4000, 0xff, 8192);
    screen(false, true, false, true, false, 15);
    for (unsigned i = 0; i < A2_FRAME_BYTES; ++i) assert(output[i + 1] == 0xff);
}

static void test_mixed(void)
{
    for (unsigned second = 0; second < 2; ++second) {
        for (unsigned hires = 0; hires < 2; ++hires) {
            memset(ram, 0xff, sizeof(ram));
            const unsigned base = second ? 0x800 : 0x400;
            for (unsigned row = 20; row < 24; ++row) {
                memset(ram + base + row_offsets[row], 0xa0, 40);
                ram[base + row_offsets[row]] = row == 23 ? 0x41 : 0xc1;
            }
            screen(false, second, true, hires, false, 12);
            for (unsigned x = 0; x < 160; ++x) assert(get(x, 119) == 15 - hires * 3);
            assert(get(1, 120) == 12 && get(0, 120) == 0);
            assert(get(3, 125) == 0 && get(159, 143) == 0);
            assert(get(1, 138) == 12 && get(3, 143) == 0);
            screen(false, second, true, hires, true, 12);
            assert(get(1, 138) == 0 && get(3, 143) == 12);
        }
    }
}

int main(void)
{
    test_text();
    test_lores();
    test_hires();
    test_mixed();
    puts("PASS page/text attributes, 40x48 lores palette/layout, 280x192 monochrome HGR interleave/scaling, both pages, mixed mode and render bounds");
    return 0;
}
