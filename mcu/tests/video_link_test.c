/* SPDX-License-Identifier: MIT */
#include "video_link.h"
#include "driver/spi_master.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(4) uint8_t frame[A2_FRAME_BYTES];
static uint8_t device_image[A2_FRAME_BYTES];
static unsigned transactions, fail_at, releases, expected_bytes;
static bool acquired, acquire_failure;
static int handle;

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *config, int dma)
{
    assert(host==SPI3_HOST && dma==SPI_DMA_CH_AUTO);
    assert(config->mosi_io_num==23 && config->miso_io_num==-1 && config->sclk_io_num==18);
    assert(config->max_transfer_sz==A2_FRAME_BYTES);
    return ESP_OK;
}

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *device)
{
    assert(host==SPI3_HOST && config->clock_speed_hz==1000000 && config->mode==0);
    assert(config->spics_io_num==5 && config->queue_size==1);
    /* The ESP-IDF MSB-first command/address phases emit 01, offset high, low. */
    assert(config->command_bits==8 && config->address_bits==16);
    assert(config->cs_ena_posttrans==1);
    *device=&handle;
    return ESP_OK;
}

esp_err_t spi_device_acquire_bus(spi_device_handle_t device, unsigned timeout)
{
    assert(device==&handle && timeout==0xffffffffu && !acquired);
    if (acquire_failure) return ESP_FAIL;
    acquired=true;
    return ESP_OK;
}

void spi_device_release_bus(spi_device_handle_t device)
{
    assert(device==&handle && acquired);
    acquired=false;
    ++releases;
}

esp_err_t spi_device_transmit(spi_device_handle_t device, spi_transaction_t *transaction)
{
    assert(device==&handle && acquired);
    assert(!transaction->flags && transaction->cmd==1);
    assert(transaction->length && !(transaction->length%32));
    assert(!((uintptr_t)transaction->tx_buffer%4));
    const unsigned offset=transaction->addr, bytes=transaction->length/8;
    assert(offset<A2_FRAME_BYTES && bytes<=A2_FRAME_BYTES-offset);
    assert(transaction->tx_buffer==frame+offset);
    ++transactions;
    if (transactions==fail_at) {
        /* Include an unknown partial write before a reported driver error. */
        memcpy(device_image+offset, transaction->tx_buffer, bytes/2);
        return ESP_FAIL;
    }
    memcpy(device_image+offset, transaction->tx_buffer, bytes);
    expected_bytes+=bytes+3;
    return ESP_OK;
}

static void update(bool force)
{
    assert(video_link_frame(frame, force)==ESP_OK);
    assert(!memcmp(frame, device_image, sizeof(frame)) && !acquired);
    assert(video_link_bytes()==expected_bytes);
}

int main(void)
{
    video_link_init();
    update(false);
    assert(transactions==1 && video_link_full_frames()==1);
    update(false);
    assert(transactions==1); /* Identical frames send no transactions. */
    frame[5]=0x12;
    update(false);
    assert(transactions==2 && video_link_full_frames()==1);
    assert(video_link_bytes()==A2_FRAME_BYTES+3+4+3);
    frame[1]=0x34; frame[A2_FRAME_BYTES-1]=0x56;
    fail_at=transactions+2;
    assert(video_link_frame(frame, false)==ESP_FAIL && !acquired);
    update(false);
    assert(video_link_full_frames()==2); /* Partial failure requires full repair. */
    acquire_failure=true;
    unsigned old_releases=releases;
    assert(video_link_frame(frame, true)==ESP_FAIL);
    assert(releases==old_releases && !acquired);
    acquire_failure=false;
    update(false);
    assert(video_link_full_frames()==3);
    update(true);
    assert(video_link_full_frames()==4);
    puts("PASS video SPI: command/offset framing, DMA alignment, wire counts and full repair after errors");
}
