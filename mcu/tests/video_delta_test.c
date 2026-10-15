/* SPDX-License-Identifier: MIT */
#include "video_delta.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static a2_video_delta_t state;
static uint8_t frame[A2_FRAME_BYTES], device[A2_FRAME_BYTES + 2];
static a2_video_plan_t plan;

static void apply(unsigned count)
{
    unsigned bytes=0;
    assert(plan.count<=A2_VIDEO_MAX_SPANS && count<=plan.count);
    for (unsigned i=0; i<plan.count; ++i) {
        const a2_video_span_t span=plan.spans[i];
        assert(span.length && !(span.offset%4) && !(span.length%4));
        assert(span.offset+span.length<=A2_FRAME_BYTES);
        if (i) assert(plan.spans[i-1].offset+plan.spans[i-1].length<span.offset);
        bytes+=span.length;
        if (i<count) memcpy(device+1+span.offset, frame+span.offset, span.length);
    }
    assert(bytes==plan.bytes);
    assert(device[0]==0xa5 && device[A2_FRAME_BYTES+1]==0xa5);
}

static void successful_update(bool force)
{
    a2_video_delta_plan(&state, frame, force, &plan);
    apply(plan.count);
    assert(!memcmp(device+1, frame, A2_FRAME_BYTES));
    a2_video_delta_complete(&state, frame, true);
}

static void test_boundaries(void)
{
    memset(device, 0xa5, sizeof(device));
    successful_update(false);
    assert(plan.full_frame && plan.count==1 && plan.bytes==A2_FRAME_BYTES);
    successful_update(false);
    assert(!plan.count && !plan.bytes && !plan.full_frame);
    frame[0]=1; frame[A2_FRAME_BYTES-1]=2;
    successful_update(false);
    assert(plan.count==2 && plan.bytes==8 && !plan.full_frame);
    assert(plan.spans[0].offset==0 && plan.spans[1].offset==A2_FRAME_BYTES-4);
    frame[0]=3; frame[20]=3; /* Include sixteen unchanged bytes between blocks. */
    successful_update(false);
    assert(plan.count==1 && plan.spans[0].length==24);
    frame[0]=4; frame[24]=4;
    successful_update(false);
    assert(plan.count==2 && plan.bytes==8);
    successful_update(true);
    assert(plan.full_frame && plan.bytes==A2_FRAME_BYTES);
    /* Excess spans and dense changes both fall back to one bounded transfer. */
    for (unsigned i=0; i<=A2_VIDEO_MAX_SPANS; ++i) frame[i*32]^=0x55;
    successful_update(false);
    assert(plan.full_frame);
    memset(frame, 0xff, A2_FRAME_BYTES*3/4);
    successful_update(false);
    assert(plan.full_frame);
}

static uint32_t random_state=0x7812ab89;
static unsigned next_random(void)
{
    random_state^=random_state<<13;
    random_state^=random_state>>17;
    random_state^=random_state<<5;
    return random_state;
}

static void test_reconstruction_and_recovery(void)
{
    for (unsigned iteration=0; iteration<1000; ++iteration) {
        unsigned changes=next_random()%100;
        while (changes--) frame[next_random()%A2_FRAME_BYTES]^=(uint8_t)next_random();
        if (!(iteration%73)) memset(frame, (uint8_t)iteration, sizeof(frame));
        a2_video_delta_plan(&state, frame, false, &plan);
        if (iteration%17==0 && plan.count) {
            /* A failed transaction may have written some previous spans. */
            apply(plan.count/2);
            a2_video_delta_complete(&state, frame, false);
            assert(!state.valid);
            a2_video_delta_plan(&state, frame, false, &plan);
            assert(plan.full_frame && plan.count==1);
        }
        apply(plan.count);
        assert(!memcmp(device+1, frame, A2_FRAME_BYTES));
        a2_video_delta_complete(&state, frame, true);
    }
}

int main(void)
{
    test_boundaries();
    test_reconstruction_and_recovery();
    puts("PASS video delta: aligned bounded spans, full fallback, random reconstruction and failed-transfer recovery");
}
