/* SPDX-License-Identifier: MIT */
#include "shell_link.h"
#include <string.h>
#ifndef FPGA_UART_BAUD
#define FPGA_UART_BAUD 115200
#endif

uint8_t shell_protocol_crc(const uint8_t *bytes, size_t length)
{
    uint8_t crc = 0xff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x1d : 0));
    }
    return crc;
}

static bool readable_window(uint16_t address, size_t count)
{
    return count > 0 &&
        ((address <= 0x7fff && count <= 0x8000u - address) ||
         (address >= 0xa000 && address <= 0xbfff && count <= 0xc000u - address));
}

bool shell_protocol_request_valid(uint8_t operation, uint16_t address, uint8_t value)
{
    switch (operation) {
    case 0: return value == 0 && readable_window(address, 1);
    case 1:
        return (address == 0 && (value == 0 || value == 0x0a)) || address == 0x2000 ||
            (address == 0x3000 && value == 0) || (address == 0x4000 && value <= 3);
    case 2: return address == 0 && value == 0;
    case 3: return address <= 33 && value == 0;
    case 4: return address >= 0xa000 && address <= 0xbfff;
    case 5: return value <= SHELL_MAX_READ && readable_window(address, value);
    case 6:
        return (address >= 0x4000 && address <= 0x7fff) ||
            (address == 0 && value == 0xf0) || (address == 0xaa && value == 0x98) ||
            (address == 0x555 && value == 0x55) ||
            (address == 0xaaa && (value == 0xaa || value == 0x90 || value == 0xa0 || value == 0x80));
    case 7:
        return value >= 1 && value <= SHELL_MAX_WRITE && address >= 0x4000 &&
            address <= 0x7fff && value <= 0x8000u - address;
    case 8: return address == 0 && value <= 1;
    default: return false;
    }
}

static bool request_data_valid(uint8_t operation, uint16_t address, uint8_t value,
                                const void *data, size_t data_size)
{
    return shell_protocol_request_valid(operation, address, value) &&
        (operation == 7 ? data && data_size == value : data_size == 0);
}

size_t shell_protocol_encode_packet(uint8_t token, uint8_t operation,
                                    uint16_t address, uint8_t value,
                                    const void *data, size_t data_size,
                                    uint8_t *output, size_t capacity)
{
    if (!output || !request_data_valid(operation, address, value, data, data_size) ||
        capacity < 9 + data_size) return 0;
    const uint8_t bytes[8] = {0x8f, 0x20, 5 + data_size, token, operation,
                              address >> 8, address & 255, value};
    memcpy(output, bytes, sizeof(bytes));
    if (data_size) memcpy(output + 8, data, data_size);
    output[8 + data_size] = shell_protocol_crc(output, 8 + data_size);
    return 9 + data_size;
}

bool shell_protocol_encode(uint8_t token, uint8_t operation, uint16_t address,
                           uint8_t value, uint8_t output[9])
{
    return operation != 7 &&
        shell_protocol_encode_packet(token, operation, address, value, NULL, 0, output, 9) == 9;
}

void shell_protocol_parser_reset(shell_parser_t *parser)
{
    parser->used = parser->expected = 0;
}

bool shell_protocol_feed(shell_parser_t *parser, uint8_t byte, shell_frame_t *frame)
{
    if (!parser->used) {
        if (byte == 0x8f) parser->bytes[parser->used++] = byte;
        return false;
    }
    parser->bytes[parser->used++] = byte;
    if (parser->used == 3) {
        if (byte > SHELL_MAX_PAYLOAD) {
            shell_protocol_parser_reset(parser);
            if (byte == 0x8f) parser->bytes[parser->used++] = byte;
            return false;
        }
        parser->expected = byte + 4;
    }
    if (!parser->expected || parser->used < parser->expected) return false;
    const bool valid = shell_protocol_crc(parser->bytes, parser->used) == 0;
    if (valid) {
        frame->channel = parser->bytes[1];
        frame->length = parser->bytes[2];
        memcpy(frame->payload, parser->bytes + 3, frame->length);
    }
    shell_protocol_parser_reset(parser);
    if (!valid && byte == 0x8f) parser->bytes[parser->used++] = byte;
    return valid;
}

bool shell_protocol_matches(const shell_frame_t *frame, uint8_t token,
                            uint16_t address, size_t expected_data)
{
    if (!frame || !expected_data || expected_data > SHELL_MAX_READ ||
        frame->channel != 0x0a || frame->length < 5 || frame->payload[0] != token ||
        frame->payload[2] != (uint8_t)(address >> 8) ||
        frame->payload[3] != (uint8_t)address || frame->payload[1] > 5) return false;
    return frame->payload[1] != 0 ? frame->length == 5 : frame->length == 4 + expected_data;
}

bool shell_protocol_write_matches(const shell_frame_t *frame, uint8_t token,
                                  uint16_t address, size_t count)
{
    return count >= 1 && count <= SHELL_MAX_WRITE &&
        shell_protocol_matches(frame, token, address, 1) && frame->payload[4] == count;
}

#ifndef SHELL_LINK_PROTOCOL_ONLY
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>

static uint8_t token;
static SemaphoreHandle_t request_lock;
static _Thread_local char last_error[80];

void shell_link_init(void)
{
    request_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(request_lock ? ESP_OK : ESP_ERR_NO_MEM);
    const uart_config_t config = {
        .baud_rate = FPGA_UART_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, 9, 10, -1, -1));
}

static bool request_packet(uint8_t operation, uint16_t address, uint8_t value,
                           const void *data, size_t data_size,
                           uint8_t *output, size_t expected_data)
{
    uint8_t tx[SHELL_MAX_REQUEST_FRAME];
    if (!request_data_valid(operation, address, value, data, data_size)) {
        strcpy(last_error, "Local operation/address/count rejected"); return false;
    }
    if (!request_lock || xSemaphoreTake(request_lock, pdMS_TO_TICKS(1500)) != pdTRUE) {
        strcpy(last_error, "UART request lock unavailable"); return false;
    }
    bool success = false;
    const size_t tx_size = shell_protocol_encode_packet(++token, operation, address,
                                    value, data, data_size, tx, sizeof(tx));
    if (!tx_size) {
        strcpy(last_error, "UART request encoding failed"); goto done;
    }
    if (uart_flush_input(UART_NUM_1) != ESP_OK) {
        strcpy(last_error, "UART input flush failed"); goto done;
    }
    if (uart_write_bytes(UART_NUM_1, tx, tx_size) != (int)tx_size) {
        strcpy(last_error, "UART write failed"); goto done;
    }
    shell_parser_t parser = {0};
    shell_frame_t frame;
    uint8_t bytes[64];
    const int64_t deadline = esp_timer_get_time() + 1000000;
    int64_t last_received = esp_timer_get_time();
    while (esp_timer_get_time() < deadline) {
        int count = uart_read_bytes(UART_NUM_1, bytes, 1, pdMS_TO_TICKS(2));
        size_t buffered = 0;
        if (count == 1 && uart_get_buffered_data_len(UART_NUM_1, &buffered) == ESP_OK && buffered) {
            if (buffered > sizeof(bytes)-1) buffered = sizeof(bytes)-1;
            const int more = uart_read_bytes(UART_NUM_1, bytes+1, buffered, 0);
            if (more > 0) count += more;
        }
        if (count > 0) last_received = esp_timer_get_time();
        else if (esp_timer_get_time() - last_received > 20000) shell_protocol_parser_reset(&parser);
        for (int i = 0; i < count; ++i) {
            if (shell_protocol_feed(&parser, bytes[i], &frame) &&
                (operation == 7 ? shell_protocol_write_matches(&frame, token, address, value) :
                 shell_protocol_matches(&frame, token, address, expected_data))) {
                if (frame.payload[1]) {
                    snprintf(last_error, sizeof(last_error), "FPGA status=%u op=%u address=%04x",
                             frame.payload[1], operation, address);
                    goto done;
                }
                if (output) memcpy(output, frame.payload + 4, expected_data);
                last_error[0] = 0;
                success = true;
                goto done;
            }
        }
    }
    snprintf(last_error, sizeof(last_error), "FPGA timeout op=%u address=%04x", operation, address);
done:
    xSemaphoreGive(request_lock);
    return success;
}

static bool request(uint8_t operation, uint16_t address, uint8_t value,
                    uint8_t *output, size_t expected_data)
{
    return request_packet(operation, address, value, NULL, 0, output, expected_data);
}

bool shell_link_read(uint16_t address, uint8_t *value)
{
    if (!value) { strcpy(last_error, "Read output is NULL"); return false; }
    return request(0, address, 0, value, 1);
}
bool shell_link_read_block(uint16_t address, uint8_t *output, size_t count)
{
    if (!output || count < 1 || count > SHELL_MAX_READ) {
        strcpy(last_error, "Bulk read requires output and count 1..32"); return false;
    }
    return request(5, address, count, output, count);
}
bool shell_link_mapper_write(uint16_t address, uint8_t value)
{
    return request(1, address, value, NULL, 1);
}
bool shell_link_ram_write(uint16_t address, uint8_t value)
{
    return request(4, address, value, NULL, 1);
}
bool shell_link_diag(uint16_t address, uint8_t *value)
{
    if (!value) { strcpy(last_error, "Diagnostic output is NULL"); return false; }
    return request(3, address, 0, value, 1);
}
bool shell_link_release(void) { return request(2, 0, 0, NULL, 1); }
bool shell_link_flash_command(uint16_t address, uint8_t value)
{
    return request(6, address, value, NULL, 1);
}
bool shell_link_flash_write_block(uint16_t address, const void *data, size_t count)
{
    if (!data || count < 1 || count > SHELL_MAX_WRITE) {
        strcpy(last_error, "Raw flash write requires data and count 1..32"); return false;
    }
    return request_packet(7, address, (uint8_t)count, data, count, NULL, 1);
}
bool shell_link_audio_arm(bool enable)
{
    uint8_t echoed = 0xff;
    if (!request(8, 0, enable ? 1 : 0, &echoed, 1)) return false;
    if (echoed != (enable ? 1 : 0)) {
        strcpy(last_error, "Audio arm response value mismatch"); return false;
    }
    return true;
}
const char *shell_link_error(void) { return last_error; }
#endif
