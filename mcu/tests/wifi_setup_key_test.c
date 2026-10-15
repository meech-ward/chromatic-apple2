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

static void legacy_key(void)
{
    namespace_exists = setup.present = true;
    setup.length = 13;
    memcpy(setup.bytes, "1234ABCD5678", setup.length);
}

int main(void)
{
    const char candidate[WIFI_SETUP_PASSWORD_BYTES] = "MINT-LAKE-4826";
    const char replacement[WIFI_SETUP_PASSWORD_BYTES] = "WAVE-FROG-1075";
    char output[WIFI_SETUP_PASSWORD_BYTES] = "UNCHANGED-----";
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
    assert(wifi_store_setup_key(output, "mint-lake-4826") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, "MINT-LAKE-482") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, "MINT-LAKE-48261") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, "AAAA-LAKE-4826") == ESP_ERR_INVALID_ARG);
    assert(wifi_store_setup_key(output, NULL) == ESP_ERR_INVALID_ARG);
    assert(!setup.present && !namespace_exists);

    /* Existing hexadecimal keys migrate once; all private profiles are intact. */
    reset();
    assert(wifi_store_save(&old) == ESP_OK);
    const struct entry original_profile = profile;
    legacy_key();
    assert(wifi_store_setup_key(output, NULL) == ESP_ERR_INVALID_ARG);
    assert(setup.length == 13);
    assert(wifi_store_setup_key(output, candidate) == ESP_OK);
    assert(setup.length == WIFI_SETUP_PASSWORD_BYTES && !strcmp(output, candidate));
    assert(!memcmp(&profile, &original_profile, sizeof(profile)));
    const int migrated_commits = commits;
    assert(wifi_store_setup_key(output, replacement) == ESP_OK);
    assert(commits == migrated_commits && !strcmp(output, candidate));

    /* Creation and migration cannot claim success on failed writes/readback. */
    for (int migrate = 0; migrate < 2; ++migrate) {
        for (int stage = 1; stage <= 5; ++stage) {
            reset();
            assert(wifi_store_save(&old) == ESP_OK);
            const struct entry preserved = profile;
            if (migrate) legacy_key();
            strcpy(output, "UNCHANGED-----");
            if (stage <= 3) fail_open = opens + stage;
            else if (stage == 4) fail_set = sets + 1;
            else corrupt_read = opens + 3;
            const esp_err_t result = wifi_store_setup_key(output, candidate);
            assert(result == (stage == 5 ? ESP_ERR_INVALID_RESPONSE : ESP_FAIL));
            assert(!strcmp(output, "UNCHANGED-----"));
            assert(!memcmp(&profile, &preserved, sizeof(profile)));
        }
        for (int persists = 0; persists < 2; ++persists) {
            reset();
            assert(wifi_store_save(&old) == ESP_OK);
            const struct entry preserved = profile;
            if (migrate) legacy_key();
            strcpy(output, "UNCHANGED-----");
            fail_commit = commits + 1;
            commit_failure_persists = persists;
            assert(wifi_store_setup_key(output, candidate) == ESP_FAIL);
            assert(!strcmp(output, "UNCHANGED-----"));
            assert(!memcmp(&profile, &preserved, sizeof(profile)));
            fail_commit = 0;
            /* A later boot reuses the new key if the failed commit persisted. */
            assert(wifi_store_setup_key(output, replacement) == ESP_OK);
            assert(!strcmp(output, persists ? candidate : replacement));
            assert(!memcmp(&profile, &preserved, sizeof(profile)));
        }
    }
    reset();
    assert(wifi_store_setup_key(output, candidate) == ESP_OK);
    setup.bytes[0] = '!';
    strcpy(output, "UNCHANGED-----");
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    assert(!strcmp(output, "UNCHANGED-----"));
    setup.bytes[0] = 'M';
    setup.length = 14;
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    setup.length = 16;
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_NVS_INVALID_LENGTH);
    legacy_key();
    setup.bytes[4] = 'a';
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    setup.bytes[4] = 'A';
    setup.bytes[12] = '0';
    assert(wifi_store_setup_key(output, replacement) == ESP_ERR_INVALID_RESPONSE);
    puts("PASS persistent readable setup password, legacy migration and failure isolation");
    return 0;
}
