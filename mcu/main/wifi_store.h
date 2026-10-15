/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "wifi_settings.h"

/* Call after nvs_flash_init(). No NVS partition is erased or reinitialized.
 * NOT_FOUND means compiled defaults remain active. Other load errors leave the
 * output untouched. Passwords are stored only in the private a2_wifi namespace.
 */
esp_err_t wifi_store_load(wifi_profile_t *profile);
/* Writes one atomic, versioned blob and verifies it through a fresh handle.
 * Failures attempt to restore the previous committed profile. INVALID_STATE
 * means restoration could not be verified: retain the active network and tell
 * the user to retry; never report success or reboot on a failed save.
 */
esp_err_t wifi_store_save(const wifi_profile_t *profile);

/* Reuse the private setup AP key across restarts so phones remember its network.
 * Existing values must be exactly 12 uppercase hexadecimal digits. Only when
 * absent is candidate validated, committed, then checked through a fresh handle.
 * No profile/SSH keys are changed; output remains untouched on any failure.
 */
esp_err_t wifi_store_setup_key(char output[13], const char candidate[13]);
