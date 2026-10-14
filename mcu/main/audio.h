/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { A2_AUDIO_RATE = 32768, A2_AUDIO_CPU_HZ = 1023000 };
typedef void (*a2_audio_emit_fn)(void *context, int16_t sample);
typedef struct {
    uint64_t cycle;
    uint32_t phase;
    int32_t area, previous_input, filtered;
    bool initialized, level;
} a2_audio_sampler;

void a2_audio_sampler_reset(a2_audio_sampler *sampler, uint64_t cycle, bool level);
/* Level changes at cycle; samples integrate the preceding level over each
 * rational sample interval. A quiet DC-blocking filter models AC coupling.
 * A backwards timestamp discards the partial sample and starts a new timeline.
 */
size_t a2_audio_sampler_update(a2_audio_sampler *sampler, uint64_t cycle,
                              bool level, a2_audio_emit_fn emit, void *context);

typedef struct {
    uint32_t samples, overflows, underruns, resets, buffered;
    int error;
    bool armed;
} a2_audio_status;
struct apple2;
/* Bind before launching CPU tasks, or while holding the machine lock. */
bool audio_start(struct apple2 *machine);
void audio_status(a2_audio_status *status);
