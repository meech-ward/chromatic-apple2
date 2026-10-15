/* SPDX-License-Identifier: MIT */
#include "wifi_store.h"
#include "nvs.h"
#include <string.h>

#define WIFI_NAMESPACE "a2_wifi"
#define WIFI_KEY "profile"

static esp_err_t load_blob(uint8_t blob[WIFI_PROFILE_BLOB_BYTES])
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t length = WIFI_PROFILE_BLOB_BYTES;
    error = nvs_get_blob(handle, WIFI_KEY, blob, &length);
    nvs_close(handle);
    if (error == ESP_OK && length != WIFI_PROFILE_BLOB_BYTES)
        error = ESP_ERR_INVALID_SIZE;
    return error;
}

esp_err_t wifi_store_load(wifi_profile_t *profile)
{
    if (!profile) return ESP_ERR_INVALID_ARG;
    uint8_t blob[WIFI_PROFILE_BLOB_BYTES];
    const esp_err_t error = load_blob(blob);
    if (error != ESP_OK) return error;
    return wifi_profile_decode(blob, sizeof(blob), profile) ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t write_blob(const uint8_t *blob)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = blob ? nvs_set_blob(handle, WIFI_KEY, blob, WIFI_PROFILE_BLOB_BYTES) :
                   nvs_erase_key(handle, WIFI_KEY);
    if (!blob && error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static bool matches_blob(const uint8_t *expected)
{
    uint8_t actual[WIFI_PROFILE_BLOB_BYTES];
    const esp_err_t error = load_blob(actual);
    return expected ? error == ESP_OK && !memcmp(actual, expected, sizeof(actual)) :
                      error == ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t wifi_store_save(const wifi_profile_t *profile)
{
    uint8_t next[WIFI_PROFILE_BLOB_BYTES], previous[WIFI_PROFILE_BLOB_BYTES];
    if (!wifi_profile_encode(profile, next)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = load_blob(previous);
    const bool had_previous = error == ESP_OK;
    if (!had_previous && error != ESP_ERR_NVS_NOT_FOUND) return error;
    if (had_previous && !memcmp(previous, next, sizeof(next))) return ESP_OK;
    error = write_blob(next);
    if (error == ESP_OK && matches_blob(next)) return ESP_OK;
    if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    const uint8_t *restore = had_previous ? previous : NULL;
    /* A failed commit may still have taken effect. Verify restoration even when
     * the compensating write reports an error, rather than assuming either one.
     */
    (void)write_blob(restore);
    return matches_blob(restore) ? error : ESP_ERR_INVALID_STATE;
}

#define WIFI_SETUP_KEY "setup_key"

static bool legacy_setup_key_valid(const char *key)
{
    for (size_t i = 0; i < 12; ++i)
        if (!((key[i] >= '0' && key[i] <= '9') ||
              (key[i] >= 'A' && key[i] <= 'F'))) return false;
    return key[12] == 0;
}

static esp_err_t load_setup_key(char key[WIFI_SETUP_PASSWORD_BYTES], bool *legacy)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t length = WIFI_SETUP_PASSWORD_BYTES;
    error = nvs_get_blob(handle, WIFI_SETUP_KEY, key, &length);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    *legacy = length == 13 && legacy_setup_key_valid(key);
    if (!*legacy && (length != WIFI_SETUP_PASSWORD_BYTES ||
                     !wifi_setup_password_valid(key)))
        return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

esp_err_t wifi_store_setup_key(char output[WIFI_SETUP_PASSWORD_BYTES],
                               const char candidate[WIFI_SETUP_PASSWORD_BYTES])
{
    if (!output) return ESP_ERR_INVALID_ARG;
    char saved[WIFI_SETUP_PASSWORD_BYTES];
    bool legacy = false;
    esp_err_t error = load_setup_key(saved, &legacy);
    if (error == ESP_OK && !legacy) {
        memcpy(output, saved, sizeof(saved));
        return ESP_OK;
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return error;
    if (!wifi_setup_password_valid(candidate)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    error = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = nvs_set_blob(handle, WIFI_SETUP_KEY, candidate, sizeof(saved));
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) return error;
    error = load_setup_key(saved, &legacy);
    if (error != ESP_OK) return error;
    if (legacy || memcmp(saved, candidate, sizeof(saved))) return ESP_ERR_INVALID_RESPONSE;
    memcpy(output, saved, sizeof(saved));
    return ESP_OK;
}

#define WIFI_ENABLED_KEY "enabled"
#define WIFI_ENABLED_BYTES 2

static esp_err_t load_enabled_blob(uint8_t blob[WIFI_ENABLED_BYTES])
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) return error;
    size_t length = WIFI_ENABLED_BYTES;
    error = nvs_get_blob(handle, WIFI_ENABLED_KEY, blob, &length);
    nvs_close(handle);
    if (error == ESP_OK && length != WIFI_ENABLED_BYTES)
        error = ESP_ERR_INVALID_SIZE;
    return error;
}

esp_err_t wifi_store_load_enabled(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    uint8_t blob[WIFI_ENABLED_BYTES];
    const esp_err_t error = load_enabled_blob(blob);
    if (error != ESP_OK) return error;
    if (blob[0] != 1 || blob[1] > 1) return ESP_ERR_INVALID_RESPONSE;
    *enabled = blob[1] != 0;
    return ESP_OK;
}

static esp_err_t write_enabled_blob(const uint8_t *blob)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) return error;
    error = blob ? nvs_set_blob(handle, WIFI_ENABLED_KEY, blob, WIFI_ENABLED_BYTES) :
                   nvs_erase_key(handle, WIFI_ENABLED_KEY);
    if (!blob && error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error;
}

static bool matches_enabled_blob(const uint8_t *expected)
{
    uint8_t actual[WIFI_ENABLED_BYTES];
    const esp_err_t error = load_enabled_blob(actual);
    return expected ? error == ESP_OK && !memcmp(actual, expected, sizeof(actual)) :
                      error == ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t wifi_store_save_enabled(bool enabled)
{
    const uint8_t next[WIFI_ENABLED_BYTES] = {1, enabled ? 1 : 0};
    uint8_t previous[WIFI_ENABLED_BYTES];
    esp_err_t error = load_enabled_blob(previous);
    const bool had_previous = error == ESP_OK;
    if (!had_previous && error != ESP_ERR_NVS_NOT_FOUND) return error;
    if (had_previous && !memcmp(previous, next, sizeof(next))) return ESP_OK;
    error = write_enabled_blob(next);
    if (error == ESP_OK && matches_enabled_blob(next)) return ESP_OK;
    if (error == ESP_OK) error = ESP_ERR_INVALID_RESPONSE;
    const uint8_t *restore = had_previous ? previous : NULL;
    /* The failed commit may have persisted; prove the previous value/absence
     * through a fresh handle even if the compensating write also reports failure.
     */
    (void)write_enabled_blob(restore);
    return matches_enabled_blob(restore) ? error : ESP_ERR_INVALID_STATE;
}
