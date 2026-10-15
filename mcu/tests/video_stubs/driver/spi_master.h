/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
typedef void *spi_device_handle_t;
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    int clock_speed_hz, mode, spics_io_num, queue_size, cs_ena_posttrans;
    uint8_t command_bits, address_bits;
} spi_device_interface_config_t;
typedef struct {
    uint32_t flags;
    uint16_t cmd;
    uint64_t addr;
    size_t length;
    const void *tx_buffer;
} spi_transaction_t;
#define SPI3_HOST 3
#define SPI_DMA_CH_AUTO 3
esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *device);
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned timeout);
void spi_device_release_bus(spi_device_handle_t device);
esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *transaction);
