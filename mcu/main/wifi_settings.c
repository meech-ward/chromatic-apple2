/* SPDX-License-Identifier: MIT */
#include "wifi_settings.h"
#include <string.h>

static size_t bounded_length(const char *value, size_t capacity)
{
    const char *end = memchr(value, 0, capacity);
    return end ? (size_t)(end - value) : capacity;
}

static bool ssid_valid(const char *ssid, size_t length)
{
    if (!length || length > 32) return false;
    const uint8_t *bytes = (const uint8_t *)ssid;
    size_t at = 0;
    while (at < length) {
        uint32_t codepoint = bytes[at++];
        unsigned continuations = 0;
        uint32_t minimum = 0;
        if (codepoint >= 0xc2 && codepoint <= 0xdf) {
            continuations = 1; minimum = 0x80; codepoint &= 0x1f;
        } else if (codepoint >= 0xe0 && codepoint <= 0xef) {
            continuations = 2; minimum = 0x800; codepoint &= 0x0f;
        } else if (codepoint >= 0xf0 && codepoint <= 0xf4) {
            continuations = 3; minimum = 0x10000; codepoint &= 7;
        } else if (codepoint >= 0x80) return false;
        if (length - at < continuations) return false;
        for (unsigned i = 0; i < continuations; ++i) {
            const uint8_t next = bytes[at++];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f) ||
            codepoint == 0x200e || codepoint == 0x200f ||
            (codepoint >= 0x2028 && codepoint <= 0x202e) ||
            (codepoint >= 0x2066 && codepoint <= 0x2069)) return false;
    }
    return true;
}

static int hex_digit(unsigned char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool wifi_profile_valid(const wifi_profile_t *profile)
{
    if (!profile || (profile->mode != WIFI_PROFILE_STATION &&
                     profile->mode != WIFI_PROFILE_HOTSPOT)) return false;
    const size_t ssid_length = bounded_length(profile->ssid, sizeof(profile->ssid));
    const size_t password_length = bounded_length(profile->password, sizeof(profile->password));
    if (!ssid_valid(profile->ssid, ssid_length)) return false;
    if (!password_length) return profile->mode == WIFI_PROFILE_STATION;
    if (password_length == 64 && profile->mode == WIFI_PROFILE_STATION) {
        for (size_t i = 0; i < password_length; ++i)
            if (hex_digit((unsigned char)profile->password[i]) < 0) return false;
        return true;
    }
    if (password_length < 8 || password_length > 63) return false;
    for (size_t i = 0; i < password_length; ++i)
        if ((unsigned char)profile->password[i] < 0x20 ||
            (unsigned char)profile->password[i] > 0x7e) return false;
    return true;
}

static bool decode_field(const char *encoded, size_t length, char *output, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)encoded[i];
        if (value == '%') {
            if (length - i < 3) return false;
            const int high = hex_digit((unsigned char)encoded[i + 1]);
            const int low = hex_digit((unsigned char)encoded[i + 2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (value == '+') value = ' ';
        if (used + 1 >= capacity || value < 0x20 || value == 0x7f) return false;
        output[used++] = (char)value;
    }
    output[used] = 0;
    return true;
}

bool wifi_form_parse(const char *body, size_t length, const char *expected_csrf,
                     wifi_profile_t *profile)
{
    if (!body || !profile || !expected_csrf || !length || length > WIFI_FORM_MAX_BYTES)
        return false;
    const size_t csrf_length = bounded_length(expected_csrf, 65);
    if (!csrf_length || csrf_length > 64) return false;
    wifi_profile_t parsed = {0};
    char csrf[65] = {0}, mode[8] = {0};
    unsigned seen = 0;
    size_t at = 0;
    while (at < length) {
        const size_t start = at;
        while (at < length && body[at] != '&') ++at;
        const size_t end = at;
        if (at < length && ++at == length) return false;
        const char *separator = memchr(body + start, '=', end - start);
        if (!separator) return false;
        const size_t key_length = (size_t)(separator - (body + start));
        char key[9];
        if (!decode_field(body + start, key_length, key, sizeof(key))) return false;
        unsigned field;
        char *value;
        size_t capacity;
        if (!strcmp(key, "ssid")) {
            field = 1; value = parsed.ssid; capacity = sizeof(parsed.ssid);
        } else if (!strcmp(key, "password")) {
            field = 2; value = parsed.password; capacity = sizeof(parsed.password);
        } else if (!strcmp(key, "mode")) {
            field = 4; value = mode; capacity = sizeof(mode);
        } else if (!strcmp(key, "csrf")) {
            field = 8; value = csrf; capacity = sizeof(csrf);
        } else return false;
        if (seen & field) return false;
        seen |= field;
        const char *encoded = separator + 1;
        if (!decode_field(encoded, (size_t)((body + end) - encoded), value, capacity))
            return false;
    }
    if (seen != 15) return false;
    if (!strcmp(mode, "station")) parsed.mode = WIFI_PROFILE_STATION;
    else if (!strcmp(mode, "hotspot")) parsed.mode = WIFI_PROFILE_HOTSPOT;
    else return false;
    unsigned difference = (unsigned)(strlen(csrf) ^ csrf_length);
    for (size_t i = 0; i < csrf_length; ++i)
        difference |= (unsigned char)csrf[i] ^ (unsigned char)expected_csrf[i];
    if (difference || !parsed.password[0] || !wifi_profile_valid(&parsed)) return false;
    *profile = parsed;
    return true;
}

bool wifi_profile_encode(const wifi_profile_t *profile,
                         uint8_t blob[WIFI_PROFILE_BLOB_BYTES])
{
    if (!blob || !wifi_profile_valid(profile)) return false;
    memset(blob, 0, WIFI_PROFILE_BLOB_BYTES);
    memcpy(blob, "A2WF", 4);
    blob[4] = 1;
    blob[5] = (uint8_t)profile->mode;
    blob[6] = (uint8_t)strlen(profile->ssid);
    blob[7] = (uint8_t)strlen(profile->password);
    memcpy(blob + 8, profile->ssid, blob[6]);
    memcpy(blob + 40, profile->password, blob[7]);
    return true;
}

bool wifi_profile_decode(const uint8_t *blob, size_t length, wifi_profile_t *profile)
{
    if (!blob || !profile || length != WIFI_PROFILE_BLOB_BYTES ||
        memcmp(blob, "A2WF", 4) || blob[4] != 1 || blob[5] > 1 ||
        !blob[6] || blob[6] > 32 || blob[7] > 64) return false;
    wifi_profile_t decoded = {.mode = (wifi_profile_mode_t)blob[5]};
    memcpy(decoded.ssid, blob + 8, blob[6]);
    memcpy(decoded.password, blob + 40, blob[7]);
    if (!wifi_profile_valid(&decoded)) return false;
    uint8_t canonical[WIFI_PROFILE_BLOB_BYTES];
    if (!wifi_profile_encode(&decoded, canonical) || memcmp(blob, canonical, sizeof(canonical)))
        return false;
    *profile = decoded;
    return true;
}
