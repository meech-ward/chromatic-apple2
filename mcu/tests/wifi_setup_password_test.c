/* SPDX-License-Identifier: MIT */
#include "wifi_setup_password.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    uint8_t entropy[WIFI_SETUP_PASSWORD_ENTROPY_BYTES] = {0};
    struct { char before, text[WIFI_SETUP_PASSWORD_BYTES], after; } output;
    memset(&output, '!', sizeof(output));
    assert(wifi_setup_password_generate(output.text, entropy));
    assert(!strcmp(output.text, "ACRE-ACRE-0000"));
    assert(output.before == '!' && output.after == '!');
    assert(wifi_setup_password_valid(output.text));
    assert(!wifi_setup_password_generate(NULL, entropy));
    assert(!wifi_setup_password_generate(output.text, NULL));

    /* Each byte independently selects one of 256 distinct, four-letter words. */
    char words[256][5];
    for (unsigned i = 0; i < 256; ++i) {
        entropy[0] = entropy[1] = (uint8_t)i;
        assert(wifi_setup_password_generate(output.text, entropy));
        assert(strlen(output.text) == 14 && wifi_setup_password_valid(output.text));
        assert(!memcmp(output.text, output.text + 5, 4));
        memcpy(words[i], output.text, 4);
        words[i][4] = 0;
        for (unsigned j = 0; j < 4; ++j)
            assert(words[i][j] >= 'A' && words[i][j] <= 'Z');
        for (unsigned j = 0; j < i; ++j) assert(strcmp(words[i], words[j]));
    }
    entropy[0] = 0;
    entropy[1] = 255;
    unsigned counts[10000] = {0};
    unsigned rejected = 0;
    for (unsigned value = 0; value <= 65535; ++value) {
        entropy[2] = (uint8_t)value;
        entropy[3] = (uint8_t)(value >> 8);
        strcpy(output.text, "UNCHANGED-----");
        if (!wifi_setup_password_generate(output.text, entropy)) {
            ++rejected;
            assert(value >= 60000 && !strcmp(output.text, "UNCHANGED-----"));
        } else {
            assert(value < 60000 && wifi_setup_password_valid(output.text));
            assert(!memcmp(output.text, words[0], 4));
            assert(!memcmp(output.text + 5, words[255], 4));
            ++counts[atoi(output.text + 10)];
        }
        assert(output.before == '!' && output.after == '!');
    }
    assert(rejected == 5536);
    for (unsigned i = 0; i < 10000; ++i) assert(counts[i] == 6);
    assert(wifi_setup_password_valid("MINT-LAKE-4826"));
    assert(!wifi_setup_password_valid(NULL));
    assert(!wifi_setup_password_valid(""));
    assert(!wifi_setup_password_valid("A"));
    assert(!wifi_setup_password_valid("MINT-LAKE-482"));
    assert(!wifi_setup_password_valid("MINT-LAKE-48261"));
    assert(!wifi_setup_password_valid("mint-lake-4826"));
    assert(!wifi_setup_password_valid("MINT-LAKE-482A"));
    assert(!wifi_setup_password_valid("MINT_LAKE-4826"));
    assert(!wifi_setup_password_valid("AAAA-LAKE-4826"));
    puts("PASS readable password word coverage, exact suffix uniformity and boundaries");
    return 0;
}
