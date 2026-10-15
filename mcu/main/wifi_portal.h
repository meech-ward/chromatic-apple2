/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* All output is self-contained HTML. SSID is HTML escaped; passwords are never
 * accepted by this renderer. csrf must contain exactly 32 hexadecimal digits.
 * Returns bytes written excluding NUL, or zero for invalid input/short output.
 */
size_t wifi_portal_render(char *output, size_t capacity, const char *ssid,
                          const char *csrf, bool hotspot);
/* Plain form submissions receive this confirmation when JavaScript is off. */
const char *wifi_portal_saved(void);
