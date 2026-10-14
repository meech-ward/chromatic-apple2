/* SPDX-License-Identifier: MIT */
#include "video_render.h"
#include <string.h>

/* Original three-column, five-row glyphs. Four by six cells fit 40x24 text. */
#define G(a,b,c,d,e) ((a)<<12 | (b)<<9 | (c)<<6 | (d)<<3 | (e))
static const uint16_t glyphs[64] = {
    G(0,0,0,0,0), G(2,2,2,0,2), G(5,5,0,0,0), G(5,7,5,7,5),
    G(3,6,3,6,2), G(5,1,2,4,5), G(2,5,2,5,3), G(2,2,0,0,0),
    G(1,2,2,2,1), G(4,2,2,2,4), G(0,5,2,5,0), G(0,2,7,2,0),
    G(0,0,0,2,4), G(0,0,7,0,0), G(0,0,0,0,2), G(1,1,2,4,4),
    G(7,5,5,5,7), G(2,6,2,2,7), G(6,1,2,4,7), G(6,1,2,1,6),
    G(5,5,7,1,1), G(7,4,6,1,6), G(3,4,7,5,7), G(7,1,2,2,2),
    G(7,5,7,5,7), G(7,5,7,1,6), G(0,2,0,2,0), G(0,2,0,2,4),
    G(1,2,4,2,1), G(0,7,0,7,0), G(4,2,1,2,4), G(6,1,2,0,2),
    G(7,5,7,4,3), G(2,5,7,5,5), G(6,5,6,5,6), G(3,4,4,4,3),
    G(6,5,5,5,6), G(7,4,6,4,7), G(7,4,6,4,4), G(3,4,5,5,3),
    G(5,5,7,5,5), G(7,2,2,2,7), G(1,1,1,5,2), G(5,5,6,5,5),
    G(4,4,4,4,7), G(5,7,7,5,5), G(5,7,7,7,5), G(2,5,5,5,2),
    G(6,5,6,4,4), G(2,5,5,7,3), G(6,5,6,5,5), G(3,4,2,1,6),
    G(7,2,2,2,2), G(5,5,5,5,7), G(5,5,5,5,2), G(5,5,7,7,5),
    G(5,5,2,5,5), G(5,5,2,2,2), G(7,1,2,4,7), G(3,2,2,2,3),
    G(4,4,2,1,1), G(6,2,2,2,6), G(2,5,0,0,0), G(0,0,0,0,7)
};
#undef G

static void pixel(uint8_t *output, unsigned x, unsigned y, uint8_t color)
{
    const unsigned address = y * 80 + x / 2;
    if (x & 1) output[address] = (output[address] & 0xf0) | color;
    else output[address] = (output[address] & 0x0f) | (color << 4);
}

static unsigned text_row_offset(unsigned row)
{
    return (row & 7) * 128 + (row >> 3) * 40;
}

static void render_text_rows(const uint8_t *page, unsigned first_row,
                             bool flash_phase, uint8_t foreground,
                             uint8_t *output)
{
    for (unsigned row = first_row; row < 24; ++row) {
        const unsigned address = text_row_offset(row);
        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t raw = page[address + col];
            const bool inverse = raw < 0x40 || (raw < 0x80 && flash_phase);
            unsigned code = raw & (raw < 0x80 ? 0x3f : 0x7f);
            if (code < 32) code += 64;
            if (code >= 96) code -= 32;
            const uint16_t glyph = glyphs[code - 32];
            for (unsigned y = 0; y < 6; ++y) {
                for (unsigned x = 0; x < 4; ++x) {
                    const bool ink = x < 3 && y < 5 &&
                                     ((glyph >> ((4 - y) * 3 + 2 - x)) & 1);
                    pixel(output, col * 4 + x, row * 6 + y,
                          ink != inverse ? foreground : 0);
                }
            }
        }
    }
}

void a2_render_page(const uint8_t *text_page, bool flash_phase,
                    uint8_t foreground, uint8_t output[A2_FRAME_BYTES])
{
    memset(output, 0, A2_FRAME_BYTES);
    render_text_rows(text_page, 0, flash_phase, foreground & 15, output);
}

void a2_render_text(const uint8_t *ram, bool page_two, bool flash_phase,
                    uint8_t foreground, uint8_t output[A2_FRAME_BYTES])
{
    a2_render_page(ram + (page_two ? 0x800 : 0x400), flash_phase,
                   foreground, output);
}

static void render_lores(const uint8_t *page, unsigned height, uint8_t *output)
{
    /* Each text byte is two vertically stacked lores pixels. At this display
     * size a 40x48 lores pixel is exactly four pixels wide and three high.
     */
    for (unsigned y = 0; y < height; ++y) {
        const unsigned lores_y = y / 3;
        const unsigned address = text_row_offset(lores_y / 2);
        const unsigned shift = (lores_y & 1) * 4;
        for (unsigned col = 0; col < 40; ++col) {
            const uint8_t color = (page[address + col] >> shift) & 15;
            const uint8_t pair = (color << 4) | color;
            output[y * 80 + col * 2] = pair;
            output[y * 80 + col * 2 + 1] = pair;
        }
    }
}

static void render_hires(const uint8_t *page, unsigned height,
                         uint8_t foreground, uint8_t *output)
{
    /* Apple II Reference Manual, display memory: seven low bits per byte,
     * forty bytes per scanline, interleaved in eight-line/64-line groups.
     * https://www.applelogic.org/files/AIIREF.pdf
     * Sample at destination pixel centers; use no expanded source framebuffer.
     */
    for (unsigned y = 0; y < height; ++y) {
        const unsigned source_y = (y * 192 + 96) / 144;
        const unsigned address = (source_y & 7) * 1024 +
                                 ((source_y >> 3) & 7) * 128 +
                                 (source_y >> 6) * 40;
        for (unsigned x = 0; x < 160; ++x) {
            const unsigned source_x = (x * 280 + 140) / 160;
            const bool ink = (page[address + source_x / 7] >>
                              (source_x % 7)) & 1;
            pixel(output, x, y, ink ? foreground : 0);
        }
    }
}

void a2_render_screen(const uint8_t *ram, bool text_mode, bool page_two,
                      bool mixed_mode, bool hires_mode, bool flash_phase,
                      uint8_t foreground, uint8_t output[A2_FRAME_BYTES])
{
    const uint8_t *text_page = ram + (page_two ? 0x800 : 0x400);
    if (text_mode) {
        a2_render_page(text_page, flash_phase, foreground, output);
        return;
    }
    foreground &= 15;
    const unsigned height = mixed_mode ? 120 : 144;
    if (hires_mode)
        render_hires(ram + (page_two ? 0x4000 : 0x2000), height,
                      foreground, output);
    else
        render_lores(text_page, height, output);
    if (mixed_mode)
        render_text_rows(text_page, 20, flash_phase, foreground, output);
}
