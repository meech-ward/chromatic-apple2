/* SPDX-License-Identifier: MIT */
#include "audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { int16_t pcm[32768]; size_t count; } capture;
static capture first, second;
static void emit(void *context, int16_t sample)
{
    capture *out = context;
    assert(out->count < sizeof(out->pcm) / sizeof(out->pcm[0]));
    assert(sample >= -4096 && sample <= 4096);
    out->pcm[out->count++] = sample;
}
int main(void)
{
    a2_audio_sampler a, b;
    a2_audio_sampler_reset(&a, 0, false);
    assert(a2_audio_sampler_update(&a, A2_AUDIO_CPU_HZ, false, emit, &first) == A2_AUDIO_RATE);
    assert(first.count == A2_AUDIO_RATE && a.phase == 0 && a.area == 0);
    for (size_t i = 0; i < first.count; ++i) assert(first.pcm[i] == 0);

    memset(&first, 0, sizeof(first));
    a2_audio_sampler_reset(&a, 0, false);
    assert(!a2_audio_sampler_update(&a, 16, true, emit, &first));
    assert(a2_audio_sampler_update(&a, 32, true, emit, &first) == 1);
    assert(first.count == 1 && first.pcm[0] == 999); /* Integrated partial sample. */
    a2_audio_sampler_update(&a, A2_AUDIO_CPU_HZ, true, emit, &first);
    assert(first.count == A2_AUDIO_RATE && first.pcm[first.count - 1] == 0); /* DC decays to silence. */

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    a2_audio_sampler_reset(&a, 1000000000000ULL, false);
    a2_audio_sampler_reset(&b, 1000000000000ULL, false);
    const uint64_t start = a.cycle;
    for (unsigned segment = 0; segment < 20; ++segment) {
        const bool before = segment & 1, after = !before;
        a2_audio_sampler_update(&a, start + (segment + 1) * 511, after, emit, &first);
        a2_audio_sampler_update(&b, start + segment * 511 + 100, before, emit, &second);
        a2_audio_sampler_update(&b, start + segment * 511 + 400, before, emit, &second);
        a2_audio_sampler_update(&b, start + (segment + 1) * 511, after, emit, &second);
    }
    assert(first.count > 300 && first.count == second.count);
    assert(!memcmp(first.pcm, second.pcm, first.count * sizeof(first.pcm[0])));
    bool positive = false, negative = false;
    for (size_t i = 0; i < first.count; ++i) {
        positive |= first.pcm[i] > 500;
        negative |= first.pcm[i] < -500;
    }
    assert(positive && negative);
    assert(!a2_audio_sampler_update(&a, 0, false, emit, &first));
    assert(a.cycle == 0 && a.phase == 0 && a.area == 0 && a.filtered == 0 && !a.level);
    a2_audio_sampler_reset(&a, UINT64_MAX - 32, false);
    assert(a2_audio_sampler_update(&a, UINT64_MAX, false, NULL, NULL) == 1);
    puts("PASS audio resampler: rational timing, integrated edge position, DC silence, segmentation invariance, signed pulses, resets and large timestamps");
}
