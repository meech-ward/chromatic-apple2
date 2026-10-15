/* SPDX-License-Identifier: MIT */
#include "wifi_setup_password.h"
#include <stddef.h>
#include <string.h>

/* Four-letter words keep the password short and fit the handheld display.
 * Uppercase matches the Apple II font used for the on-device setup details.
 */
static const char words[][5] = {
#include "password_words.inc"
};
_Static_assert(sizeof(words) / sizeof(words[0]) == 256, "word index must cover one byte");

bool wifi_setup_password_generate(char output[WIFI_SETUP_PASSWORD_BYTES],
                                 const uint8_t entropy[WIFI_SETUP_PASSWORD_ENTROPY_BYTES])
{
    if (!output || !entropy) return false;
    unsigned suffix = (unsigned)entropy[2] | ((unsigned)entropy[3] << 8);
    if (suffix >= 60000) return false;
    suffix %= 10000;
    char next[WIFI_SETUP_PASSWORD_BYTES];
    memcpy(next, words[entropy[0]], 4);
    next[4] = '-';
    memcpy(next + 5, words[entropy[1]], 4);
    next[9] = '-';
    for (int i = 13; i >= 10; --i) {
        next[i] = (char)('0' + suffix % 10);
        suffix /= 10;
    }
    next[14] = 0;
    memcpy(output, next, sizeof(next));
    return true;
}

static bool known_word(const char *word)
{
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (!memcmp(word, words[i], 4)) return true;
    return false;
}

bool wifi_setup_password_valid(const char *password)
{
    if (!password) return false;
    size_t length = 0;
    while (length < WIFI_SETUP_PASSWORD_BYTES && password[length]) ++length;
    if (length != WIFI_SETUP_PASSWORD_BYTES - 1 || password[4] != '-' ||
        password[9] != '-') return false;
    for (size_t i = 10; i < 14; ++i)
        if (password[i] < '0' || password[i] > '9') return false;
    return known_word(password) && known_word(password + 5);
}
