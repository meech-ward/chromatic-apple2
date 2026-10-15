/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include "wifi_setup_password.h"
void network_start(void);
const char *network_ip(void);
/* True after station DHCP or hotspot AP_START; false while disconnected.
 * A true result also establishes that Wi-Fi is active for SSH entropy.
 */
bool network_ready(void);

/* Temporary WPA2 setup AP; settings are saved separately from firmware. */
typedef struct {
    char ssid[33], password[WIFI_SETUP_PASSWORD_BYTES], url[32], status[41];
} network_setup_info_t;
bool network_setup_start(void);
void network_setup_stop(void);
void network_setup_info(network_setup_info_t *out);
