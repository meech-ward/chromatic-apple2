/* SPDX-License-Identifier: MIT */
#include "video_link.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
static spi_device_handle_t display;

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
        .cs_ena_posttrans=1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &device, &display));
}

esp_err_t video_link_frame(const uint8_t frame[A2_FRAME_BYTES])
{
    esp_err_t result=spi_device_acquire_bus(display, portMAX_DELAY);
    if (result != ESP_OK) return result;
    spi_transaction_t header={
        .flags=SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length=24, .tx_data={1,0,0,0},
    };
    result=spi_device_polling_transmit(display, &header);
    if (result == ESP_OK) {
        spi_transaction_t pixels={.length=A2_FRAME_BYTES*8, .tx_buffer=frame};
        result=spi_device_polling_transmit(display, &pixels);
    }
    spi_device_release_bus(display);
    return result;
}
