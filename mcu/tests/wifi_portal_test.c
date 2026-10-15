/* SPDX-License-Identifier: MIT */
#include "wifi_portal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char token[] = "0123456789abcdef0123456789abcdef";

int main(int argc, char **argv)
{
    char html[6144];
    if (argc == 2) {
        if (!wifi_portal_render(html, sizeof(html), argv[1], token, false)) return 1;
        fputs(html, stdout);
        return 0;
    }
    size_t length = wifi_portal_render(html, sizeof(html), "Home Wi-Fi", token, false);
    assert(length > 4000 && length < 5000 && strlen(html) == length);
    assert(strstr(html, "value=station selected"));
    assert(strstr(html, "value=hotspot>"));
    assert(strstr(html, "name=csrf value='0123456789abcdef0123456789abcdef'"));
    assert(strstr(html, "name=password type=password required"));
    assert(strstr(html, "8–63 printable ASCII characters, or a 64-digit hexadecimal key"));
    assert(!strstr(html, "/^.{8,63}$/"));
    assert(!strstr(html, "type=password value="));
    assert(strstr(html, "action=/wifi/save"));
    assert(strstr(html, "fetch('/wifi/status'"));
    assert(wifi_portal_render(html, sizeof(html), "\"'<>&", token, true));
    assert(strstr(html, "value='&quot;&#39;&lt;&gt;&amp;'"));
    assert(strstr(html, "value=hotspot selected"));
    assert(strstr(html, "required minlength=8 maxlength=63"));
    assert(!wifi_portal_render(html, sizeof(html), "abc\nxyz", token, true));
    assert(html[0] == 0);
    assert(!wifi_portal_render(html, sizeof(html), "SSID", "not-a-token", false));
    assert(!wifi_portal_render(html, sizeof(html), "SSID", "0123456789abcdef0123456789abcde'<>", false));
    assert(!wifi_portal_render(html, sizeof(html), "abcdefghijklmnopqrstuvwxyz1234567", token, false));
    assert(!wifi_portal_render(html, sizeof(html), NULL, token, false));
    assert(!wifi_portal_render(html, sizeof(html), "SSID", NULL, false));
    assert(!wifi_portal_render(NULL, sizeof(html), "SSID", token, false));
    assert(!wifi_portal_render(html, 0, "SSID", token, false));
    char worst[33];
    memset(worst, '"', 32); worst[32] = 0;
    length = wifi_portal_render(html, sizeof(html), worst, token, false);
    assert(length && length < sizeof(html));
    /* Every shorter capacity fails cleanly and preserves the first unavailable
     * byte; the exact successful capacity includes the final terminator. */
    for (size_t capacity = 1; capacity <= length; ++capacity) {
        memset(html, '#', sizeof(html));
        assert(!wifi_portal_render(html, capacity, worst, token, false));
        assert(html[0] == 0 && html[capacity] == '#');
    }
    assert(wifi_portal_render(html, length + 1, worst, token, false) == length);
    assert(strstr(wifi_portal_saved(), "Wi-Fi settings saved"));
    assert(strstr(wifi_portal_saved(), "href=/wifi"));
    puts("PASS Wi-Fi portal escaping, modes, bounded output and native confirmation");
    return 0;
}
