/* SPDX-License-Identifier: MIT */
#include "wifi_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool present, staged_present, opened;
static uint8_t committed[WIFI_PROFILE_BLOB_BYTES], staged[WIFI_PROFILE_BLOB_BYTES];
static size_t stored_length = WIFI_PROFILE_BLOB_BYTES;
static int opens, commit_calls, fail_open, fail_commit, corrupt_read;
static bool failed_commit_persists, rollback_fails;

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "a2_wifi") && !opened);
    ++opens;
    if (opens == fail_open) return ESP_FAIL;
    if (mode == NVS_READONLY && !present) return ESP_ERR_NVS_NOT_FOUND;
    opened = true;
    staged_present = present;
    memcpy(staged, committed, sizeof(staged));
    *handle = mode;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    (void)handle;
    assert(opened);
    opened = false;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *value, size_t *length)
{
    (void)handle;
    assert(opened && !strcmp(key, "profile"));
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < stored_length) return ESP_ERR_NVS_INVALID_LENGTH;
    *length = stored_length;
    memcpy(value, committed, stored_length);
    if (opens == corrupt_read) ((uint8_t *)value)[0] ^= 1;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(opened && handle == NVS_READWRITE && !strcmp(key, "profile"));
    assert(length == sizeof(staged));
    if (rollback_fails && commit_calls >= fail_commit) return ESP_FAIL;
    memcpy(staged, value, sizeof(staged));
    staged_present = true;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(opened && handle == NVS_READWRITE && !strcmp(key, "profile"));
    if (rollback_fails && commit_calls >= fail_commit) return ESP_FAIL;
    staged_present = false;
    return present ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && handle == NVS_READWRITE);
    ++commit_calls;
    const bool fail = commit_calls == fail_commit;
    if (!fail || failed_commit_persists) {
        present = staged_present;
        memcpy(committed, staged, sizeof(committed));
        stored_length = sizeof(committed);
    }
    return fail ? ESP_FAIL : ESP_OK;
}

static void reset(void)
{
    assert(!opened);
    present = false;
    opens = commit_calls = fail_open = fail_commit = corrupt_read = 0;
    failed_commit_persists = rollback_fails = false;
    stored_length = sizeof(committed);
    memset(committed, 0, sizeof(committed));
}

static void expect_profile(const wifi_profile_t *expected)
{
    wifi_profile_t actual;
    assert(wifi_store_load(&actual) == ESP_OK);
    assert(actual.mode == expected->mode && !strcmp(actual.ssid, expected->ssid));
    assert(!strcmp(actual.password, expected->password));
}

int main(void)
{
    const wifi_profile_t old = {.mode = WIFI_PROFILE_STATION,
        .ssid = "old network", .password = "old-password"};
    const wifi_profile_t next = {.mode = WIFI_PROFILE_HOTSPOT,
        .ssid = "new network", .password = "new-password"};
    wifi_profile_t actual = next;
    reset();
    assert(wifi_store_load(&actual) == ESP_ERR_NVS_NOT_FOUND);
    assert(!memcmp(&actual, &next, sizeof(actual)));
    assert(wifi_store_load(NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_store_save(NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_store_save(&old) == ESP_OK);
    expect_profile(&old);
    assert(wifi_store_save(&next) == ESP_OK);
    expect_profile(&next);
    const int commits_before = commit_calls;
    assert(wifi_store_save(&next) == ESP_OK && commit_calls == commits_before);

    /* A failed first override must restore the absence of a saved profile. */
    reset();
    fail_commit = 1;
    failed_commit_persists = true;
    assert(wifi_store_save(&next) == ESP_FAIL);
    assert(wifi_store_load(&actual) == ESP_ERR_NVS_NOT_FOUND);

    for (int persists = 0; persists < 2; ++persists) {
        reset();
        assert(wifi_store_save(&old) == ESP_OK);
        fail_commit = commit_calls + 1;
        failed_commit_persists = persists;
        assert(wifi_store_save(&next) == ESP_FAIL);
        expect_profile(&old);
    }
    /* Even successful commits need independent readback before reporting saved. */
    reset();
    assert(wifi_store_save(&old) == ESP_OK);
    corrupt_read = opens + 3;
    assert(wifi_store_save(&next) == ESP_ERR_INVALID_RESPONSE);
    expect_profile(&old);
    for (int stage = 1; stage <= 3; ++stage) {
        reset();
        assert(wifi_store_save(&old) == ESP_OK);
        fail_open = opens + stage;
        assert(wifi_store_save(&next) == (stage == 3 ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL));
        expect_profile(&old);
    }
    reset();
    assert(wifi_store_save(&old) == ESP_OK);
    fail_commit = commit_calls + 1;
    failed_commit_persists = rollback_fails = true;
    assert(wifi_store_save(&next) == ESP_ERR_INVALID_STATE);
    expect_profile(&next); /* Caller must not claim rollback or reboot. */

    reset();
    assert(wifi_store_save(&old) == ESP_OK);
    committed[0] = 0;
    actual = next;
    assert(wifi_store_load(&actual) == ESP_ERR_INVALID_RESPONSE);
    assert(!memcmp(&actual, &next, sizeof(actual)));
    /* Invalid but correctly sized blobs can be replaced and restored verbatim. */
    assert(wifi_store_save(&next) == ESP_OK);
    expect_profile(&next);
    stored_length = 1;
    actual = old;
    assert(wifi_store_load(&actual) == ESP_ERR_INVALID_SIZE);
    assert(!memcmp(&actual, &old, sizeof(actual)));
    stored_length = sizeof(committed) + 1;
    assert(wifi_store_load(&actual) == ESP_ERR_NVS_INVALID_LENGTH);
    puts("PASS private Wi-Fi NVS save, reload, commit/readback failures and rollback");
    return 0;
}
