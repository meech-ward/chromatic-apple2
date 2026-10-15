/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WIFI_SETUP_PASSWORD_BYTES 15
#define WIFI_SETUP_PASSWORD_ENTROPY_BYTES 4

/* Two independently indexed words and a four-digit suffix. The final two bytes
 * are a little-endian integer: values >=60000 are rejected, so each suffix has
 * exactly six representations and no modulo bias. Retry with fresh hardware
 * entropy on rejection. Output is untouched on failure.
 */
bool wifi_setup_password_generate(char output[WIFI_SETUP_PASSWORD_BYTES],
                                 const uint8_t entropy[WIFI_SETUP_PASSWORD_ENTROPY_BYTES]);
bool wifi_setup_password_valid(const char *password);
