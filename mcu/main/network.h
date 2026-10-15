/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
void network_start(void);
const char *network_ip(void);
/* True after station DHCP or hotspot AP_START; false while disconnected.
 * A true result also establishes that Wi-Fi is active for SSH entropy.
 */
bool network_ready(void);
