/* SPDX-License-Identifier: MIT */
#include "ssh_terminal.h"
#include "ssh_transport.h"
#include "../control.h"
#include "private_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/platform_util.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

#ifndef DEVICE_SSH_ENABLED
#define DEVICE_SSH_ENABLED 0
#endif
#ifndef DEVICE_SSH_PASSWORD
#define DEVICE_SSH_PASSWORD ""
#endif
static const char *TAG = "apple2_ssh";
enum { SCREEN_REFRESH_MS = 40 };
static TaskHandle_t task;
static char snapshot[1024], last_frame[24][40], pending_output[2048];
static size_t output_length, output_position;
static bool force_frame, key_pending, replay_pending, skip_lf;
static uint8_t pending_key, replay_byte, escape[16];
static unsigned escape_size;
static uint32_t escape_at;

static esp_err_t host_seed(uint8_t seed[32]) {
    nvs_handle_t handle;
    esp_err_t error = nvs_open("a2_ssh", NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    size_t size = 32;
    error = nvs_get_blob(handle, "host_seed", seed, &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        esp_fill_random(seed, 32);
        error = nvs_set_blob(handle, "host_seed", seed, 32);
        if (error == ESP_OK) error = nvs_commit(handle);
    } else if (error == ESP_OK && size != 32) error = ESP_ERR_INVALID_SIZE;
    nvs_close(handle);
    return error;
}
static void output(const char *text, size_t length) {
    if (length <= sizeof(pending_output)-output_length) {
        memcpy(pending_output+output_length, text, length); output_length += length;
    }
}
static void send_output(void) {
    if (output_position < output_length)
        output_position += ssh_transport_write((const uint8_t *)pending_output+output_position,
                                                output_length-output_position);
    if (output_position == output_length) output_position = output_length = 0;
}
static bool screen(void) {
    if (output_length || !control_terminal_text(snapshot, sizeof(snapshot))) return false;
    if (force_frame) {
        static const char start[] = "\033]0;Apple II - Ctrl] opens menu\007\033[2J";
        output(start, sizeof(start)-1);
    }
    const char *cursor = snapshot;
    for (unsigned row = 0; row < 24; ++row) {
        char line[40]; memset(line, ' ', sizeof(line));
        unsigned column = 0;
        while (*cursor && *cursor != '\n') {
            if (column < 40) line[column++] = *cursor >= 32 && *cursor <= 126 ? *cursor : ' ';
            ++cursor;
        }
        if (*cursor == '\n') ++cursor;
        if (force_frame || memcmp(line, last_frame[row], sizeof(line))) {
            char position[20]; int length = snprintf(position, sizeof(position), "\033[%u;1H", row+1);
            output(position, (size_t)length); output(line, sizeof(line));
            memcpy(last_frame[row], line, sizeof(line));
        }
    }
    if (force_frame) {
        unsigned columns, rows; ssh_transport_terminal_size(&columns, &rows);
        if (columns >= 40 && rows >= 25) {
            static const char help[] = "\033[25;1HCtrl] MENU | Arrows | Enter | Esc back";
            output(help, sizeof(help)-1);
        }
    }
    if (output_length) output("\033[24;40H", 8);
    force_frame = false;
    return true;
}
static void queue_key(uint8_t key) { pending_key = key; key_pending = true; }
static void decode(uint8_t byte, uint32_t now) {
    if (escape_size) {
        if (escape_size == 1 && byte != '[' && byte != 'O') {
            escape_size = 0; replay_byte = byte; replay_pending = true; queue_key(27); return;
        }
        if (escape_size == sizeof(escape)) { escape_size = 0; return; }
        escape[escape_size++] = byte; escape_at = now;
        if (escape_size >= 3 && byte >= 0x40 && byte <= 0x7e) {
            escape_size = 0;
            if (byte == 'A') queue_key(11);
            else if (byte == 'B') queue_key(10);
            else if (byte == 'C') queue_key(21);
            else if (byte == 'D') queue_key(8);
        }
        return;
    }
    if (byte == 27) { escape[0] = byte; escape_size = 1; escape_at = now; return; }
    if (byte == '\n' && skip_lf) { skip_lf = false; return; }
    skip_lf = byte == '\r';
    if (byte == '\r' || byte == '\n') byte = 13;
    if (byte == 127) byte = 8;
    if ((byte >= 32 && byte <= 126) || byte == 3 || byte == 8 || byte == 10
            || byte == 11 || byte == 13 || byte == 21 || byte == 29) queue_key(byte);
}
static void input(uint32_t now) {
    if (ssh_transport_take_interrupt()) {
        escape_size = 0; replay_pending = false; queue_key(3);
    }
    if (key_pending) {
        if (!control_terminal_key(pending_key)) return;
        key_pending = false;
    }
    if (escape_size && (uint32_t)(now-escape_at) >= 100) {
        bool standalone = escape_size == 1; escape_size = 0;
        if (standalone) { queue_key(27); return; }
    }
    for (unsigned count = 0; count < 32 && !key_pending; ++count) {
        uint8_t byte;
        if (replay_pending) { byte = replay_byte; replay_pending = false; }
        else if (!ssh_transport_read(&byte, 1)) break;
        decode(byte, now);
    }
}
static bool sync_network(bool *enabled) {
    bool ready = network_ready();
    if (ready != *enabled) {
        if (!ssh_transport_set_enabled(ready)) return false;
        *enabled = ready;
    }
    return ready;
}
static void ssh_task(void *unused) {
    (void)unused;
    while (!network_ready()) vTaskDelay(pdMS_TO_TICKS(250));
    uint8_t seed[32]; esp_err_t error = host_seed(seed);
    bool ready = error == ESP_OK && ssh_transport_init(seed, DEVICE_SSH_PASSWORD);
    mbedtls_platform_zeroize(seed, sizeof(seed));
    if (!ready || !ssh_transport_set_enabled(true)) {
        ESP_LOGE(TAG, "SSH initialization failed (%d)", (int)error);
        task = NULL; vTaskDelete(NULL); return;
    }
    ESP_LOGI(TAG, "SSH ready: user apple, port22, Ctrl] opens menu");
    uint32_t session = 0, refresh_at = 0, eof_at = 0;
    bool enabled = true;
    for (;;) {
        if (!sync_network(&enabled)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        ssh_transport_poll();
        uint32_t now = (uint32_t)(esp_timer_get_time()/1000);
        if (ssh_transport_connected()) {
            if (session != ssh_transport_session_id()) {
                session = ssh_transport_session_id(); force_frame = true;
                output_position = output_length = 0; escape_size = 0;
                key_pending = replay_pending = skip_lf = false; eof_at = 0; refresh_at = now-SCREEN_REFRESH_MS;
            }
            input(now); send_output();
            bool refreshed = false;
            if ((uint32_t)(now-refresh_at) >= SCREEN_REFRESH_MS && !output_length) {
                refreshed = screen(); if (refreshed) refresh_at = now;
            }
            if (ssh_transport_input_ended() && !key_pending && !replay_pending && !escape_size) {
                if (!eof_at) eof_at = now;
                else if ((uint32_t)(now-eof_at) >= 200 && refreshed && !output_length)
                    ssh_transport_disconnect();
            } else eof_at = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
void ssh_terminal_start(void) {
    if (task || !DEVICE_SSH_ENABLED) return;
    size_t length = strlen(DEVICE_SSH_PASSWORD);
    if (length < 8 || length > 64) { ESP_LOGE(TAG, "SSH password needs8..64bytes"); return; }
    /* Short input/snapshot work takes precedence over the emulator; the task
     * yields for 10 ms after every poll so emulation and video keep running. */
    if (xTaskCreatePinnedToCore(ssh_task, "apple2_ssh", 12*1024, NULL, 5, &task, 0) != pdPASS)
        ESP_LOGE(TAG, "SSH task allocation failed");
}
