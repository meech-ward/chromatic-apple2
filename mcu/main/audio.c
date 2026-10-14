/* SPDX-License-Identifier: MIT */
#include "audio.h"
#include <string.h>

enum { SPEAKER_GAIN = 1024 };

void a2_audio_sampler_reset(a2_audio_sampler *s, uint64_t cycle, bool level)
{
    memset(s, 0, sizeof(*s));
    s->initialized = true;
    s->cycle = cycle;
    s->level = level;
    s->previous_input = level ? SPEAKER_GAIN : -SPEAKER_GAIN;
}

size_t a2_audio_sampler_update(a2_audio_sampler *s, uint64_t cycle,
                              bool level, a2_audio_emit_fn emit, void *context)
{
    if (!s->initialized || cycle < s->cycle ||
        cycle - s->cycle > UINT64_MAX / A2_AUDIO_RATE) {
        a2_audio_sampler_reset(s, cycle, level);
        return 0;
    }
    uint64_t remaining = (cycle - s->cycle) * A2_AUDIO_RATE;
    size_t produced = 0;
    while (remaining) {
        uint32_t part = A2_AUDIO_CPU_HZ - s->phase;
        if (remaining < part) part = (uint32_t)remaining;
        s->area += s->level ? (int32_t)part : -(int32_t)part;
        s->phase += part;
        remaining -= part;
        if (s->phase == A2_AUDIO_CPU_HZ) {
            const int32_t average = (int32_t)((int64_t)s->area * SPEAKER_GAIN / A2_AUDIO_CPU_HZ);
            s->filtered = average - s->previous_input + s->filtered * 255 / 256;
            s->previous_input = average;
            if (emit) emit(context, (int16_t)s->filtered);
            ++produced;
            s->phase = 0;
            s->area = 0;
        }
    }
    s->cycle = cycle;
    s->level = level;
    return produced;
}

#ifndef A2_AUDIO_PORTABLE_ONLY
#include "apple2.h"
#include "shell_link.h"
#include <stdatomic.h>
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum { RING_FRAMES = 2048, DMA_FRAMES = 128, DMA_BUFFERS = 4,
       PRIME_FRAMES = DMA_FRAMES * 4 };
static int16_t ring[RING_FRAMES];
static a2_audio_sampler sampler;
static atomic_uint written, consumed, samples, overflows, underruns, resets;
static atomic_bool discard, armed;
static atomic_int driver_error;
static i2s_chan_handle_t channel;
static bool started;

static void emit_sample(void *unused, int16_t value)
{
    (void)unused;
    const unsigned w = atomic_load_explicit(&written, memory_order_relaxed);
    const unsigned r = atomic_load_explicit(&consumed, memory_order_acquire);
    atomic_fetch_add_explicit(&samples, 1, memory_order_relaxed);
    if (w - r >= RING_FRAMES) {
        atomic_fetch_add_explicit(&overflows, 1, memory_order_relaxed);
        return;
    }
    ring[w & (RING_FRAMES - 1)] = value;
    atomic_store_explicit(&written, w + 1, memory_order_release);
}

static void speaker_event(void *unused, uint64_t cycle, bool level)
{
    (void)unused;
    if (!cycle || (sampler.initialized && cycle < sampler.cycle)) {
        a2_audio_sampler_reset(&sampler, cycle, level);
        atomic_store_explicit(&discard, true, memory_order_release);
        atomic_fetch_add_explicit(&resets, 1, memory_order_relaxed);
    } else a2_audio_sampler_update(&sampler, cycle, level, emit_sample, NULL);
}

static void fill_output(bool *primed, int16_t output[DMA_FRAMES * 2])
{
    /* The consumer alone owns its read index and priming state. Reset merely
     * requests a discard, so it cannot race an index update from this task.
     */
    const bool reset = atomic_exchange_explicit(&discard, false, memory_order_acq_rel);
    unsigned r = atomic_load_explicit(&consumed, memory_order_relaxed);
    const unsigned w = atomic_load_explicit(&written, memory_order_acquire);
    if (reset) { r = w; *primed = false; }
    const unsigned available = w - r;
    if (!*primed && available >= PRIME_FRAMES) *primed = true;
    if (*primed && available < DMA_FRAMES) {
        *primed = false;
        r = w; /* Drop the old partial block before collecting a new cushion. */
        atomic_fetch_add_explicit(&underruns, 1, memory_order_relaxed);
    }
    if (!*primed) memset(output, 0, DMA_FRAMES * 2 * sizeof(*output));
    else {
        for (unsigned i = 0; i < DMA_FRAMES; ++i) {
            const int16_t sample = ring[r++ & (RING_FRAMES - 1)];
            output[i * 2] = output[i * 2 + 1] = sample;
        }
    }
    atomic_store_explicit(&consumed, r, memory_order_release);
}

static void audio_task(void *unused)
{
    (void)unused;
    int16_t output[DMA_FRAMES * 2];
    int64_t next_probe = 0;
    bool primed = false;
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (now >= next_probe) {
            next_probe = now + 500000;
            uint8_t flags = 0;
            const bool ready = shell_link_diag(15, &flags) && (flags & 1) && !(flags & 16);
            if (!ready) atomic_store(&armed, false);
            else if (!atomic_load(&armed)) atomic_store(&armed, shell_link_audio_arm(true));
        }
        fill_output(&primed, output);
        size_t sent = 0;
        const esp_err_t error = i2s_channel_write(channel, output, sizeof(output), &sent, 20);
        atomic_store(&driver_error, error == ESP_OK && sent != sizeof(output) ? ESP_FAIL : error);
        if (error != ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool audio_start(struct apple2 *machine)
{
    if (!machine || started) return false;
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_SLAVE);
    config.dma_desc_num = DMA_BUFFERS;
    config.dma_frame_num = DMA_FRAMES;
    config.auto_clear_after_cb = true;
    esp_err_t error = i2s_new_channel(&config, &channel, NULL);
    bool enabled = false;
    if (error != ESP_OK) goto failed;
    const i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(A2_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = GPIO_NUM_33, .ws = GPIO_NUM_25,
                     .dout = GPIO_NUM_27, .din = I2S_GPIO_UNUSED},
    };
    error = i2s_channel_init_std_mode(channel, &standard);
    if (error != ESP_OK) goto failed;
    static const int16_t silence[DMA_BUFFERS * DMA_FRAMES * 2] = {0};
    size_t loaded = 0;
    error = i2s_channel_preload_data(channel, silence, sizeof(silence), &loaded);
    if (error != ESP_OK) goto failed;
    if (loaded != sizeof(silence)) { error = ESP_ERR_INVALID_SIZE; goto failed; }
    error = i2s_channel_enable(channel);
    if (error != ESP_OK) goto failed;
    enabled = true;
    if (xTaskCreate(audio_task, "apple2_audio", 3072, NULL, 3, NULL) != pdPASS) {
        error = ESP_ERR_NO_MEM;
        goto failed;
    }
    started = true;
    apple2_attach_speaker(machine, speaker_event, NULL);
    return true;
failed:
    atomic_store(&driver_error, error);
    if (enabled) (void)i2s_channel_disable(channel);
    if (channel) (void)i2s_del_channel(channel);
    channel = NULL;
    return false;
}

void audio_status(a2_audio_status *status)
{
    if (!status) return;
    const unsigned r = atomic_load_explicit(&consumed, memory_order_acquire);
    const unsigned w = atomic_load_explicit(&written, memory_order_acquire);
    const unsigned buffered = w - r;
    *status = (a2_audio_status){
        .samples = atomic_load(&samples), .overflows = atomic_load(&overflows),
        .underruns = atomic_load(&underruns), .resets = atomic_load(&resets),
        .buffered = buffered > RING_FRAMES ? RING_FRAMES : buffered,
        .error = atomic_load(&driver_error), .armed = atomic_load(&armed),
    };
}
#endif
