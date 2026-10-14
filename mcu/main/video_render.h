/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define A2_FRAME_BYTES 11520
/* Text page starts at its first byte ($0400 or $0800), at least 1024 bytes. */
void a2_render_page(const uint8_t *text_page, bool flash_phase,
                    uint8_t foreground, uint8_t output[A2_FRAME_BYTES]);
void a2_render_text(const uint8_t *ram, bool page_two, bool flash_phase,
                    uint8_t foreground, uint8_t output[A2_FRAME_BYTES]);
/* RAM has 48 KiB. HGR is monochrome, center-sampled from 280x192 to 160x144;
 * byte bit 7 (the NTSC color phase) is intentionally ignored. Lores uses the
 * sixteen palette indices directly. Mixed mode keeps the bottom four text rows.
 */
void a2_render_screen(const uint8_t *ram, bool text_mode, bool page_two,
                      bool mixed_mode, bool hires_mode, bool flash_phase,
                      uint8_t foreground, uint8_t output[A2_FRAME_BYTES]);
