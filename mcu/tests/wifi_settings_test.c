/* SPDX-License-Identifier: MIT */
#include "wifi_settings.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *csrf = "ab0123456789abcdef";

static bool form(const char *body, wifi_profile_t *profile)
{
    return wifi_form_parse(body, strlen(body), csrf, profile);
}

int main(void)
{
    wifi_profile_t profile = {.mode = WIFI_PROFILE_STATION,
        .ssid = "A network", .password = "new-password"};
    assert(wifi_profile_valid(&profile));
    assert(form("mode=station&ssid=A+network&password=new-password&csrf=ab0123456789abcdef", &profile));
    assert(profile.mode == WIFI_PROFILE_STATION && !strcmp(profile.ssid, "A network"));
    assert(!strcmp(profile.password, "new-password"));
    assert(form("csrf=ab0123456789abcdef&password=abc%26def%3Dghi%2Bjkl&ssid=Caf%C3%A9&mode=hotspot", &profile));
    assert(profile.mode == WIFI_PROFILE_HOTSPOT && !strcmp(profile.ssid, "Caf\xc3\xa9"));
    assert(!strcmp(profile.password, "abc&def=ghi+jkl"));

    const char *invalid[] = {
        "mode=station&ssid=x&password=new-password", /* Missing CSRF. */
        "mode=station&ssid=x&password=new-password&csrf=wrong",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcde",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef0",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef&ssid=y",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef&%73sid=y",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef&unknown=y",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef&",
        "&mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=x&&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef&bare",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef%",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef%0",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef%gg",
        "mode=station&ssid=x&password=new-password&csrf=ab0123456789abcdef%00",
        "mode=station&ssid=%00x&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=%0ax&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=%7Fx&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=%c0%a0&password=new-password&csrf=ab0123456789abcdef", /* Overlong UTF-8. */
        "mode=station&ssid=%ed%a0%80&password=new-password&csrf=ab0123456789abcdef", /* Surrogate. */
        "mode=station&ssid=%f4%90%80%80&password=new-password&csrf=ab0123456789abcdef", /* > U+10FFFF. */
        "mode=station&ssid=%e2%80%ae&password=new-password&csrf=ab0123456789abcdef", /* Bidi override. */
        "mode=station&ssid=%c2%85&password=new-password&csrf=ab0123456789abcdef", /* C1 control. */
        "mode=station&ssid=%e2%80&password=new-password&csrf=ab0123456789abcdef", /* Truncated UTF-8. */
        "mode=station&ssid=x&password=short&csrf=ab0123456789abcdef",
        "mode=station&ssid=x&password=&csrf=ab0123456789abcdef",
        "mode=station&ssid=&password=new-password&csrf=ab0123456789abcdef",
        "mode=stations&ssid=x&password=new-password&csrf=ab0123456789abcdef",
        "mode=other&ssid=x&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=123456789012345678901234567890123&password=new-password&csrf=ab0123456789abcdef",
        "mode=station&ssid=x&password=unicode-%C3%A9&csrf=ab0123456789abcdef",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const wifi_profile_t before = profile;
        assert(!form(invalid[i], &profile));
        assert(!memcmp(&profile, &before, sizeof(profile)));
    }
    assert(!wifi_form_parse(NULL, 1, csrf, &profile));
    assert(!wifi_form_parse("x", 1, NULL, &profile));
    assert(!wifi_form_parse("x", 1, "", &profile));
    assert(!wifi_form_parse("x", 0, csrf, &profile));
    char too_large[WIFI_FORM_MAX_BYTES + 1];
    memset(too_large, 'x', sizeof(too_large));
    assert(!wifi_form_parse(too_large, sizeof(too_large), csrf, &profile));
    const char nul_body[] = "mode=station&ssid=a\0b&password=new-password&csrf=ab0123456789abcdef";
    assert(!wifi_form_parse(nul_body, sizeof(nul_body) - 1, csrf, &profile));

    memset(&profile, 0, sizeof(profile));
    profile.mode = WIFI_PROFILE_STATION;
    memset(profile.ssid, 's', 32);
    memset(profile.password, 'a', 64);
    assert(wifi_profile_valid(&profile));
    profile.password[63] = 'g';
    assert(!wifi_profile_valid(&profile));
    profile.password[63] = 0;
    assert(wifi_profile_valid(&profile));
    profile.mode = WIFI_PROFILE_HOTSPOT;
    assert(wifi_profile_valid(&profile));
    profile.password[63] = 'a';
    assert(!wifi_profile_valid(&profile));
    profile.password[0] = 0;
    assert(!wifi_profile_valid(&profile));
    profile.mode = WIFI_PROFILE_STATION;
    assert(wifi_profile_valid(&profile)); /* Existing open-network default. */
    strcpy(profile.password, "new-password");
    profile.ssid[32] = 's';
    assert(!wifi_profile_valid(&profile));
    profile.ssid[32] = 0;
    memset(profile.password, 'a', sizeof(profile.password));
    assert(!wifi_profile_valid(&profile));
    strcpy(profile.password, "new-password");

    uint8_t blob[WIFI_PROFILE_BLOB_BYTES];
    assert(wifi_profile_encode(&profile, blob));
    wifi_profile_t decoded;
    assert(wifi_profile_decode(blob, sizeof(blob), &decoded));
    assert(!strcmp(profile.ssid, decoded.ssid) && !strcmp(profile.password, decoded.password));
    assert(profile.mode == decoded.mode);
    assert(!wifi_profile_decode(blob, sizeof(blob) - 1, &decoded));
    for (size_t at = 0; at < 8; ++at) {
        uint8_t broken[sizeof(blob)];
        memcpy(broken, blob, sizeof(blob));
        broken[at] = 255;
        assert(!wifi_profile_decode(broken, sizeof(broken), &decoded));
    }
    blob[103] = 1; /* Noncanonical padding cannot hide extra data. */
    assert(!wifi_profile_decode(blob, sizeof(blob), &decoded));

    /* Exercise malformed binary/percent data at every length under sanitizers. */
    unsigned random = 1234567;
    for (size_t n = 0; n < 10000; ++n) {
        char bytes[WIFI_FORM_MAX_BYTES];
        const size_t length = n % sizeof(bytes);
        for (size_t i = 0; i < length; ++i) {
            random = random * 1664525u + 1013904223u;
            bytes[i] = (char)(random >> 24);
        }
        (void)wifi_form_parse(bytes, length, csrf, &decoded);
    }
    puts("PASS Wi-Fi profiles, strict setup form and versioned blob");
    return 0;
}
