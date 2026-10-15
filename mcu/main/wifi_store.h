/* SPDX-License-Identifier: MIT */
#pragma once
#include "esp_err.h"
#include "wifi_settings.h"
#include "wifi_setup_password.h"

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

/* Reuse the private setup AP password across restarts so phones remember it.
 * A valid legacy 12-digit hexadecimal key is migrated once to candidate's word
 * password. Missing keys also use candidate; existing word passwords are kept.
 * No profile/SSH keys are changed; output remains untouched on any failure.
 */
esp_err_t wifi_store_setup_key(char output[WIFI_SETUP_PASSWORD_BYTES],
                               const char candidate[WIFI_SETUP_PASSWORD_BYTES]);

/* Independent power preference: NOT_FOUND leaves enabled untouched, and callers
 * use true for the initial default. Saving does not change the network profile
 * or setup password. A failed save verifies rollback before returning; an
 * INVALID_STATE result means the previous preference could not be confirmed.
 */
esp_err_t wifi_store_load_enabled(bool *enabled);
esp_err_t wifi_store_save_enabled(bool enabled);
