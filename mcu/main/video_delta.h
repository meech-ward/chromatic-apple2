/* SPDX-License-Identifier: MIT */
#pragma once
#include "video_render.h"

#define A2_VIDEO_MAX_SPANS 64

typedef struct {
    uint16_t offset, length;
} a2_video_span_t;

typedef struct {
    a2_video_span_t spans[A2_VIDEO_MAX_SPANS];
    unsigned count, bytes;
    bool full_frame;
} a2_video_plan_t;

typedef struct {
    uint8_t previous[A2_FRAME_BYTES];
    bool valid;
} a2_video_delta_t;

/* Zero-initialize state. Spans are ordered, bounded and aligned to four bytes
 * for SPI DMA. Short gaps are included to avoid extra transaction overhead. */
void a2_video_delta_plan(const a2_video_delta_t *state,
                        const uint8_t frame[A2_FRAME_BYTES], bool force_full,
                        a2_video_plan_t *plan);
/* Commit only after all spans succeed. Any failure invalidates the reference;
 * the next plan then replaces the entire device image, including partial writes. */
void a2_video_delta_complete(a2_video_delta_t *state,
                            const uint8_t frame[A2_FRAME_BYTES], bool success);
