/* SPDX-License-Identifier: MIT */
#include "video_link.h"
#include "video_delta.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include <stdatomic.h>

static spi_device_handle_t display;
static a2_video_delta_t delta;
static _Atomic uint32_t transferred_bytes, full_frames;

void video_link_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num=23, .miso_io_num=-1, .sclk_io_num=18,
        .quadwp_io_num=-1, .quadhd_io_num=-1,
        .max_transfer_sz=A2_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO));
    const spi_device_interface_config_t device = {
        .clock_speed_hz=1000000, .mode=0, .spics_io_num=5, .queue_size=1,
        .command_bits=8, .address_bits=16, .cs_ena_posttrans=1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &display));
    delta.valid=false;
}

esp_err_t video_link_frame(const uint8_t frame[A2_FRAME_BYTES], bool force_full)
{
    a2_video_plan_t plan;
    a2_video_delta_plan(&delta, frame, force_full, &plan);
    if (!plan.count) return ESP_OK;
    esp_err_t result=spi_device_acquire_bus(display, portMAX_DELAY);
    if (result == ESP_OK) {
        for (unsigned i=0; i<plan.count; ++i) {
            const a2_video_span_t span=plan.spans[i];
            /* Hardware prefixes 01, offset high, offset low in one CS window. */
            spi_transaction_t pixels={
                .cmd=1, .addr=span.offset, .length=span.length*8,
                .tx_buffer=frame+span.offset,
            };
            /* Wait on queued DMA completion so the transfer yields the CPU. */
            result=spi_device_transmit(display, &pixels);
            if (result != ESP_OK) break;
            atomic_fetch_add(&transferred_bytes, span.length+3);
        }
        spi_device_release_bus(display);
    }
    a2_video_delta_complete(&delta, frame, result==ESP_OK);
    if (result==ESP_OK && plan.full_frame) atomic_fetch_add(&full_frames, 1);
    return result;
}

uint32_t video_link_bytes(void)
{
    return atomic_load(&transferred_bytes);
}

uint32_t video_link_full_frames(void)
{
    return atomic_load(&full_frames);
}
