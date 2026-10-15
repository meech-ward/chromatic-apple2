/* SPDX-License-Identifier: MIT */
#include "wifi_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct entry { bool present; size_t length; uint8_t bytes[WIFI_PROFILE_BLOB_BYTES]; };
enum { PROFILE, SETUP, POWER, OTHER, ENTRY_COUNT };
static struct entry saved[ENTRY_COUNT], staged[ENTRY_COUNT], unrelated[3];
static struct entry kept_profile, kept_setup, kept_other, kept_unrelated[3];
static bool opened, namespace_exists, persist_failed_commit[4];
static int opens, reads, writes, erases, commits;
static int fail_open, fail_read, fail_write, fail_erase, corrupt_read;
static bool fail_commit[4];

static int index_of(const char *key)
{
    if (!strcmp(key, "profile")) return PROFILE;
    if (!strcmp(key, "setup_key")) return SETUP;
    if (!strcmp(key, "enabled")) return POWER;
    assert(!strcmp(key, "other"));
    return OTHER;
}

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(name, "a2_wifi") && !opened);
    if (++opens == fail_open) return ESP_FAIL;
    if (mode == NVS_READONLY && !namespace_exists) return ESP_ERR_NVS_NOT_FOUND;
    if (mode == NVS_READWRITE) namespace_exists = true;
    opened = true;
    memcpy(staged, saved, sizeof(staged));
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
    if (++reads == fail_read) return ESP_FAIL;
    const struct entry *source = &saved[index_of(key)];
    if (!source->present) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < source->length) return ESP_ERR_NVS_INVALID_LENGTH;
    *length = source->length;
    memcpy(value, source->bytes, source->length);
    if (reads == corrupt_read && source->length) ((uint8_t *)value)[0] ^= 0x80;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    assert(opened && handle == NVS_READWRITE && index_of(key) == POWER);
    if (++writes == fail_write) return ESP_FAIL;
    struct entry *target = &staged[POWER];
    assert(length <= sizeof(target->bytes));
    target->present = true;
    target->length = length;
    memcpy(target->bytes, value, length);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    assert(opened && handle == NVS_READWRITE && index_of(key) == POWER);
    if (++erases == fail_erase) return ESP_FAIL;
    const bool present = staged[POWER].present;
    staged[POWER].present = false;
    return present ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(opened && handle == NVS_READWRITE && commits < 3);
    ++commits;
    if (!fail_commit[commits] || persist_failed_commit[commits])
        memcpy(saved, staged, sizeof(saved));
    return fail_commit[commits] ? ESP_FAIL : ESP_OK;
}

static void assert_isolated(void)
{
    assert(!opened);
    assert(!memcmp(&saved[PROFILE], &kept_profile, sizeof(kept_profile)));
    assert(!memcmp(&saved[SETUP], &kept_setup, sizeof(kept_setup)));
    assert(!memcmp(&saved[OTHER], &kept_other, sizeof(kept_other)));
    assert(!memcmp(unrelated, kept_unrelated, sizeof(unrelated)));
}

static void reset(bool present, bool enabled)
{
    assert(!opened);
    memset(saved, 0, sizeof(saved));
    memset(unrelated, 0x5a, sizeof(unrelated));
    saved[PROFILE].present = saved[SETUP].present = saved[OTHER].present = true;
    saved[PROFILE].length = WIFI_PROFILE_BLOB_BYTES;
    memset(saved[PROFILE].bytes, 0x37, sizeof(saved[PROFILE].bytes));
    saved[SETUP].length = WIFI_SETUP_PASSWORD_BYTES;
    memcpy(saved[SETUP].bytes, "MINT-LAKE-4826", WIFI_SETUP_PASSWORD_BYTES);
    saved[OTHER].length = 7;
    memcpy(saved[OTHER].bytes, "private", 7);
    saved[POWER].present = present;
    saved[POWER].length = 2;
    saved[POWER].bytes[0] = 1;
    saved[POWER].bytes[1] = enabled ? 1 : 0;
    kept_profile = saved[PROFILE];
    kept_setup = saved[SETUP];
    kept_other = saved[OTHER];
    memcpy(kept_unrelated, unrelated, sizeof(unrelated));
    namespace_exists = true;
    opens = reads = writes = erases = commits = 0;
    fail_open = fail_read = fail_write = fail_erase = corrupt_read = 0;
    memset(fail_commit, 0, sizeof(fail_commit));
    memset(persist_failed_commit, 0, sizeof(persist_failed_commit));
}

static void expect_enabled(bool expected)
{
    bool actual = !expected;
    assert(wifi_store_load_enabled(&actual) == ESP_OK && actual == expected);
    assert_isolated();
}

int main(void)
{
    reset(false, false);
    bool enabled = true;
    assert(wifi_store_load_enabled(NULL) == ESP_ERR_INVALID_ARG);
    assert(wifi_store_load_enabled(&enabled) == ESP_ERR_NVS_NOT_FOUND && enabled);
    assert(wifi_store_save_enabled(false) == ESP_OK);
    expect_enabled(false); /* Fresh handles model a boot with no cached setting. */
    assert(wifi_store_save_enabled(true) == ESP_OK);
    expect_enabled(true);
    const int before_writes = writes, before_commits = commits;
    assert(wifi_store_save_enabled(true) == ESP_OK);
    assert(writes == before_writes && commits == before_commits);
    assert_isolated();

    /* Absent, malformed and failed loads never overwrite the caller's default. */
    for (int malformed = 0; malformed < 5; ++malformed) {
        reset(true, false);
        esp_err_t expected = ESP_ERR_INVALID_RESPONSE;
        if (malformed == 0) saved[POWER].bytes[0] = 2;
        else if (malformed == 1) saved[POWER].bytes[1] = 2;
        else {
            saved[POWER].length = malformed == 2 ? 0 : malformed == 3 ? 1 : 3;
            expected = malformed == 4 ? ESP_ERR_NVS_INVALID_LENGTH : ESP_ERR_INVALID_SIZE;
        }
        enabled = true;
        assert(wifi_store_load_enabled(&enabled) == expected && enabled);
        if (malformed >= 2) {
            assert(wifi_store_save_enabled(true) == expected);
            assert(!writes && !commits);
        }
        assert_isolated();
    }
    reset(false, false);
    namespace_exists = false;
    enabled = false;
    assert(wifi_store_load_enabled(&enabled) == ESP_ERR_NVS_NOT_FOUND && !enabled);
    namespace_exists = true;
    assert_isolated();

    /* Every save stage, both an initial value and a replacement, preserves
     * unrelated keys. A failed commit is tested both persisted and discarded.
     */
    for (int present = 0; present < 2; ++present) {
        for (int stage = 0; stage < 9; ++stage) {
            reset(present, false);
            esp_err_t expected = ESP_FAIL;
            if (stage == 0) fail_open = 1;
            else if (stage == 1) fail_read = 1;
            else if (stage == 2) fail_open = 2;
            else if (stage == 3) fail_write = 1;
            else if (stage <= 5) {
                fail_commit[1] = true;
                persist_failed_commit[1] = stage == 5;
            } else {
                expected = ESP_ERR_INVALID_RESPONSE;
                if (stage == 6) fail_open = 3;
                if (stage == 7) fail_read = 2;
                if (stage == 8) corrupt_read = 2;
            }
            assert(wifi_store_save_enabled(true) == expected);
            assert(saved[POWER].present == (bool)present);
            if (present) expect_enabled(false);
            else {
                enabled = false;
                assert(wifi_store_load_enabled(&enabled) == ESP_ERR_NVS_NOT_FOUND && !enabled);
            }
            assert_isolated();
        }
    }

    /* Valid-length unknown versions are restored byte-for-byte on failure. */
    reset(true, false);
    saved[POWER].bytes[0] = 7;
    saved[POWER].bytes[1] = 0x91;
    fail_commit[1] = persist_failed_commit[1] = true;
    assert(wifi_store_save_enabled(true) == ESP_FAIL);
    assert(saved[POWER].length == 2 && saved[POWER].bytes[0] == 7 &&
           saved[POWER].bytes[1] == 0x91);
    assert_isolated();

    /* An uncertain rollback must never be reported as a saved preference. */
    for (int stage = 0; stage < 5; ++stage) {
        reset(true, false);
        fail_commit[1] = persist_failed_commit[1] = true;
        if (stage == 0) fail_open = 3;
        if (stage == 1) fail_write = 2;
        if (stage == 2) fail_commit[2] = true;
        if (stage == 3) fail_open = 4;
        if (stage == 4) fail_read = 2;
        assert(wifi_store_save_enabled(true) == ESP_ERR_INVALID_STATE);
        assert_isolated();
    }
    reset(false, false);
    fail_commit[1] = persist_failed_commit[1] = true;
    fail_erase = 1;
    assert(wifi_store_save_enabled(true) == ESP_ERR_INVALID_STATE);
    assert(saved[POWER].present);
    assert_isolated();
    reset(true, false);
    fail_commit[1] = persist_failed_commit[1] = true;
    fail_commit[2] = persist_failed_commit[2] = true;
    assert(wifi_store_save_enabled(true) == ESP_FAIL);
    expect_enabled(false); /* A failed rollback commit may still have persisted. */

    puts("PASS persistent Wi-Fi power, isolated keys and verified failure rollback");
    return 0;
}
