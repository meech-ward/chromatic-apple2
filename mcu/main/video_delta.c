/* SPDX-License-Identifier: MIT */
#include "video_delta.h"
#include <string.h>

static void full_frame(a2_video_plan_t *plan)
{
    plan->spans[0] = (a2_video_span_t){0, A2_FRAME_BYTES};
    plan->count = 1;
    plan->bytes = A2_FRAME_BYTES;
    plan->full_frame = true;
}

void a2_video_delta_plan(const a2_video_delta_t *state,
                        const uint8_t frame[A2_FRAME_BYTES], bool force_full,
                        a2_video_plan_t *plan)
{
    plan->count = plan->bytes = 0;
    plan->full_frame = false;
    if (force_full || !state->valid) {
        full_frame(plan);
        return;
    }
    for (unsigned offset = 0; offset < A2_FRAME_BYTES; offset += 4) {
        if (!memcmp(frame + offset, state->previous + offset, 4)) continue;
        a2_video_span_t *last = plan->count ? &plan->spans[plan->count - 1] : NULL;
        if (last && offset - (last->offset + last->length) <= 16) {
            plan->bytes += offset + 4 - (last->offset + last->length);
            last->length = offset + 4 - last->offset;
        } else {
            if (plan->count == A2_VIDEO_MAX_SPANS) {
                full_frame(plan);
                return;
            }
            plan->spans[plan->count++] = (a2_video_span_t){offset, 4};
            plan->bytes += 4;
        }
    }
    /* Dense scenes are cheaper as one transaction than many small transfers. */
    if (plan->bytes >= A2_FRAME_BYTES * 3 / 4) full_frame(plan);
}

void a2_video_delta_complete(a2_video_delta_t *state,
                            const uint8_t frame[A2_FRAME_BYTES], bool success)
{
    state->valid = success;
    if (success) memcpy(state->previous, frame, A2_FRAME_BYTES);
}
