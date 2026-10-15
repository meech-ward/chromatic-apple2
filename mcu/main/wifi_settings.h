/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_FORM_MAX_BYTES 512
#define WIFI_PROFILE_BLOB_BYTES 104

typedef enum {
    WIFI_PROFILE_STATION = 0,
    WIFI_PROFILE_HOTSPOT = 1,
} wifi_profile_mode_t;

typedef struct {
    wifi_profile_mode_t mode;
    char ssid[33];
    char password[65];
} wifi_profile_t;

/* Existing station defaults may use an open network. New setup forms require
 * a WPA2 password; a hotspot always requires 8..63 printable ASCII bytes.
 * SSIDs contain 1..32 valid UTF-8 bytes without control/formatting characters.
 */
bool wifi_profile_valid(const wifi_profile_t *profile);
bool wifi_form_parse(const char *body, size_t length, const char *expected_csrf,
                     wifi_profile_t *profile);
/* Stable, canonical format; never serialize the C structure or its padding. */
bool wifi_profile_encode(const wifi_profile_t *profile,
                         uint8_t blob[WIFI_PROFILE_BLOB_BYTES]);
bool wifi_profile_decode(const uint8_t *blob, size_t length, wifi_profile_t *profile);
