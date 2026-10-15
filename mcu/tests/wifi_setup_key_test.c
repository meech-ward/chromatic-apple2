/* SPDX-License-Identifier: MIT */
#include "wifi_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct entry { bool present; size_t length; uint8_t bytes[WIFI_PROFILE_BLOB_BYTES]; };
static struct entry profile, setup, staged_profile, staged_setup;
static bool namespace_exists, opened, commit_failure_persists;
static int opens, commits, fail_open, fail_commit, fail_set, sets, corrupt_read;

static struct entry *entry(const char *key, bool staged)
{
    if (!strcmp(key, "profile")) return staged ? &staged_profile : &profile;
    assert(!strcmp(key, "setup_key"));
    return staged ? &staged_setup : &setup;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "a2_wifi") && !opened);
    if (++opens == fail_open) return ESP_FAIL;
    if (!namespace_exists && mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) namespace_exists = true;
    opened = true;
    staged_profile = profile;
    staged_setup = setup;
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
    assert(opened);
    struct entry *source = entry(key, false);
    if (!source->present) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < source->length) return ESP_ERR_NVS_INVALID_LENGTH;
    *length = source->length;
    memcpy(value, source->bytes, source->length);
    if (opens == corrupt_read && source->length) ((char *)value)[0] = '!';
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(opened && handle == NVS_READWRITE);
    if (++sets == fail_set) return ESP_FAIL;
    struct entry *target = entry(key, true);
    assert(length <= sizeof(target->bytes));
    target->present = true;
    target->length = length;
    memcpy(target->bytes, value, length);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(opened && handle == NVS_READWRITE);
    struct entry *target = entry(key, true);
    const bool present = target->present;
    target->present = false;
    return present ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && handle == NVS_READWRITE);
    const bool fail = ++commits == fail_commit;
    if (!fail || commit_failure_persists) {
        profile = staged_profile;
        setup = staged_setup;
    }
    return fail ? ESP_FAIL : ESP_OK;
}

static void reset(void)
{
    assert(!opened);
    namespace_exists = commit_failure_persists = false;
    opens = commits = fail_open = fail_commit = fail_set = sets = corrupt_read = 0;
    memset(&profile, 0, sizeof(profile));
    memset(&setup, 0, sizeof(setup));
}

int main(void)
{
    const char candidate[13] = "1234ABCD5678";
    const char replacement[13] = "ABCD0123EF89";
    char output[13] = "UNCHANGED---";
    const wifi_profile_t old = {.mode = WIFI_PROFILE_STATION,
        .ssid = "saved network", .password = "saved-password"};
    reset();
    assert(wifi_store_save(&old) == ESP_OK);
    const struct entry before = profile;
    assert(wifi_store_setup_key(output, candidate) == ESP_OK);
    assert(!strcmp(output, candidate) && !memcmp(&profile, &before, sizeof(profile)));
    const int commits_before = commits;
    assert(wifi_store_setup_key(output, replacement) == ESP_OK);
    assert(!strcmp(output, candidate) && commits == commits_before);
    assert(wifi_store_setup_key(output, NULL) == ESP_OK);
    assert(wifi_store_setup_key(NULL, candidate) == ESP_ERR_INVALID_ARG);
    wifi_profile_t loaded;
    assert(wifi_store_load(&loaded) == ESP_OK && !strcmp(loaded.ssid, old.ssid));

    reset();
    assert(wifi_store_setup_key(output, "lowercase---") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, "12345678901") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, "1234567890123") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, NULL) == ESP_ERR_INVALID_ARG);
    assert(!setup.present && !namespace_exists);

    /* First creation must not claim success on failed writes or verification. */
    for (int stage = 1; stage <= 5; ++stage) {
        reset();
        strcpy(output, "UNCHANGED---");
        if (stage <= 3) fail_open = stage;
        else if (stage == 4) fail_set = 1;
        else corrupt_read = 3;
        const esp_err_t result = wifi_store_setup_key(output, candidate);
        assert(result == (stage == 5 ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL));
        assert(!strcmp(output, "UNCHANGED---"));
        assert(!profile.present);
    }
    for (int persists = 0; persists < 2; ++persists) {
        reset();
        strcpy(output, "UNCHANGED---");
        fail_commit = 1;
        commit_failure_persists = persists;
        assert(wifi_store_setup_key(output, candidate) == ESP_FAIL);
        assert(!strcmp(output, "UNCHANGED---"));
        fail_commit = 0;
        /* A later boot reuses a valid key even if the failed commit persisted. */
        assert(wifi_store_setup_key(output, persists ? replacement : candidate) == ESP_OK);
        assert(!strcmp(output, candidate));
    }
    reset();
    assert(wifi_store_setup_key(output, candidate) == ESP_OK);
    setup.bytes[0] = '!';
    strcpy(output, "UNCHANGED---");
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    assert(!strcmp(output, "UNCHANGED---"));
    setup.bytes[0] = '1';
    setup.length = 12;
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    setup.length = 14;
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_NVS_INVALID_LENGTH);
    puts("PASS persistent setup key reuse, private-key isolation and failure handling");
    return 0;
}
