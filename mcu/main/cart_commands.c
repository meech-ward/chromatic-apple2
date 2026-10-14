/* SPDX-License-Identifier: MIT */
#include "cart_commands.h"
#include "shell_link.h"
#include "mbedtls/sha256.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char retained_pattern[] =
    "5f7f27e9619081bad76dc3b906923cedac9bc7ff8c66e38f6c0c0fbe787a531f";

static void append(char *output, size_t capacity, const char *format, ...)
{
    const size_t used = strlen(output);
    if (used >= capacity) return;
    va_list args;
    va_start(args, format);
    vsnprintf(output + used, capacity - used, format, args);
    va_end(args);
}

static bool number(const char *text, unsigned maximum, unsigned *value)
{
    if (!text || *text == '-' || *text == '+') return false;
    char *end;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno || end == text || *end || parsed > maximum) return false;
    *value = parsed;
    return true;
}

static bool link_failure(char *output, size_t capacity)
{
    append(output, capacity, "ERROR %s\n", shell_link_error());
    return false;
}

static bool cleanup(char *output, size_t capacity)
{
    bool ok = true;
    const uint16_t addresses[] = {0x0000, 0x4000, 0x3000, 0x2000};
    const uint8_t values[] = {0, 0, 0, 1};
    for (unsigned i = 0; i < sizeof(values); ++i) {
        if (!shell_link_mapper_write(addresses[i], values[i])) {
            append(output, capacity, "ERROR cleanup address=%04x: %s\n", addresses[i], shell_link_error());
            ok = false;
        }
    }
    if (!shell_link_release()) {
        append(output, capacity, "ERROR release: %s\n", shell_link_error());
        ok = false;
    }
    append(output, capacity, "CART_CLEANUP ok=%u ram_disabled=%s bank_defaults=%s\n",
           ok, ok ? "true" : "unconfirmed", ok ? "true" : "unconfirmed");
    return ok;
}

static bool read_bytes(uint16_t address, uint8_t *output, unsigned count)
{
    for (unsigned offset = 0; offset < count;) {
        const unsigned chunk = count - offset < 32 ? count - offset : 32;
        if (!shell_link_read_block(address + offset, output + offset, chunk)) return false;
        offset += chunk;
    }
    return true;
}

static bool header(char *output, size_t capacity)
{
    uint8_t bytes[80];
    if (!read_bytes(0x100, bytes, sizeof(bytes))) return link_failure(output, capacity);
    uint8_t checksum = 0;
    for (unsigned i = 0x34; i <= 0x4c; ++i) checksum -= bytes[i] + 1;
    char title[17];
    for (unsigned i = 0; i < 16; ++i) {
        const uint8_t value = bytes[0x34 + i];
        title[i] = value >= 32 && value <= 126 && value != '"' && value != '\\' ? value : '.';
    }
    title[16] = 0;
    const bool valid = checksum == bytes[0x4d];
    append(output, capacity,
           "CART_HEADER title=\"%s\" checksum_valid=%u mapper=%u rom_code=%u ram_code=%u data=",
           title, valid, bytes[0x47], bytes[0x48], bytes[0x49]);
    for (unsigned i = 0; i < sizeof(bytes); ++i) append(output, capacity, "%02x", bytes[i]);
    append(output, capacity, "\n");
    return valid;
}

static bool read_command(uint16_t address, unsigned count, char *output, size_t capacity)
{
    const bool ram = address >= 0xa000;
    bool ok = true;
    if (ram && (!shell_link_mapper_write(0x0000, 0x0a) || !shell_link_mapper_write(0x4000, 0)))
        ok = link_failure(output, capacity);
    uint8_t bytes[32];
    for (unsigned offset = 0; ok && offset < count;) {
        const unsigned chunk = count - offset < 32 ? count - offset : 32;
        if (!shell_link_read_block(address + offset, bytes, chunk)) {
            ok = link_failure(output, capacity); break;
        }
        append(output, capacity, "CART_DATA %04x ", address + offset);
        for (unsigned i = 0; i < chunk; ++i) append(output, capacity, "%02x", bytes[i]);
        append(output, capacity, "\n");
        offset += chunk;
    }
    if (ram) {
        const bool cleaned = cleanup(output, capacity);
        ok = cleaned && ok;
    }
    return ok;
}

static bool ram_digest(char *output, size_t capacity)
{
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts(&context, 0) == 0;
    if (!ok) append(output, capacity, "ERROR SHA256 initialization failed\n");
    if (ok && !shell_link_mapper_write(0x0000, 0x0a)) ok = link_failure(output, capacity);
    uint8_t bytes[32], digest[32];
    for (unsigned bank = 0; ok && bank < 4; ++bank) {
        if (!shell_link_mapper_write(0x4000, bank)) { ok = link_failure(output, capacity); break; }
        for (unsigned offset = 0; ok && offset < 8192; offset += sizeof(bytes)) {
            if (!shell_link_read_block(0xa000 + offset, bytes, sizeof(bytes))) {
                ok = link_failure(output, capacity); break;
            }
            if (mbedtls_sha256_update(&context, bytes, sizeof(bytes)) != 0) {
                append(output, capacity, "ERROR SHA256 update failed\n"); ok = false;
            }
        }
        if (ok) append(output, capacity, "CART_RAM_BANK_COMPLETE bank=%u bytes=8192\n", bank);
    }
    if (ok && mbedtls_sha256_finish(&context, digest) != 0) {
        append(output, capacity, "ERROR SHA256 finalization failed\n"); ok = false;
    }
    mbedtls_sha256_free(&context);
    if (ok) {
        char hex[65];
        for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
        append(output, capacity, "CART_RAM_DIGEST bytes=32768 sha256=%s retained_pattern_match=%u\n",
               hex, !strcmp(hex, retained_pattern));
    }
    const bool cleaned = cleanup(output, capacity);
    return ok && cleaned;
}

static bool ram_test(char *output, size_t capacity)
{
    uint8_t original = 0, observed = 0;
    bool ok = true, old_valid = false, attempted = false, changed_verified = false, restored = false;
    if (!shell_link_mapper_write(0x0000, 0x0a) || !shell_link_mapper_write(0x4000, 3) ||
        !shell_link_read(0xbfff, &original)) ok = link_failure(output, capacity);
    else old_valid = true;
    const uint8_t alternate = original ^ 0xa5;
    if (old_valid) {
        attempted = true;
        if (!shell_link_ram_write(0xbfff, alternate)) ok = link_failure(output, capacity);
        else if (!shell_link_read(0xbfff, &observed)) ok = link_failure(output, capacity);
        else {
            changed_verified = observed == alternate;
            if (!changed_verified) {
                append(output, capacity, "ERROR alternate readback expected=%02x actual=%02x\n", alternate, observed);
                ok = false;
            }
        }
    }
    if (attempted) {
        // A failed write ACK does not prove the byte stayed unchanged. Reassert
        // the original bank, attempt restoration, and verify the actual byte.
        if (!shell_link_mapper_write(0x0000, 0x0a) || !shell_link_mapper_write(0x4000, 3))
            ok = link_failure(output, capacity);
        else {
            if (!shell_link_ram_write(0xbfff, original)) ok = link_failure(output, capacity);
            if (!shell_link_read(0xbfff, &observed)) ok = link_failure(output, capacity);
            else restored = observed == original;
        }
        if (!restored) append(output, capacity, "ERROR original SRAM byte restoration is unconfirmed\n");
    }
    append(output, capacity,
           "CART_RAM_TEST bank=3 address=bfff old_valid=%u original=%02x alternate=%02x changed_verified=%u restored=%u\n",
           old_valid, original, alternate, changed_verified, restored);
    const bool cleaned = cleanup(output, capacity);
    return ok && old_valid && changed_verified && restored && cleaned;
}

bool cart_command(int argc, char **argv, char *output, size_t capacity)
{
    if (!output || !capacity) return false;
    if (capacity > 4096) capacity = 4096;
    output[0] = 0;
    if (argc < 2 || !argv || strcmp(argv[0], "cart")) goto usage;
    if (argc == 2 && !strcmp(argv[1], "diag")) {
        uint8_t values[5];
        for (unsigned i = 0; i < 5; ++i)
            if (!shell_link_diag(i, values + i)) return link_failure(output, capacity);
        append(output, capacity, "CART_DIAG build=%02x buttons=%02x menu=%02x status=%02x straps=%02x\n",
               values[0], values[1], values[2], values[3], values[4]);
        return true;
    }
    if (argc == 2 && !strcmp(argv[1], "header")) return header(output, capacity);
    if (argc == 2 && !strcmp(argv[1], "release")) return cleanup(output, capacity);
    if (argc == 2 && !strcmp(argv[1], "ram-digest")) return ram_digest(output, capacity);
    if (argc == 2 && !strcmp(argv[1], "ram-test")) return ram_test(output, capacity);
    unsigned address, count;
    if (argc == 3 && !strcmp(argv[1], "bank") && number(argv[2], 255, &address)) {
        if (!shell_link_mapper_write(0x3000, 0) || !shell_link_mapper_write(0x2000, address))
            return link_failure(output, capacity);
        append(output, capacity, "CART_BANK bank=%u selected=true\n", address);
        return true;
    }
    if (argc == 4 && !strcmp(argv[1], "read") && number(argv[2], 0xbfff, &address) &&
        number(argv[3], 256, &count) && count > 0 &&
        ((address <= 0x7fff && count <= 0x8000 - address) ||
         (address >= 0xa000 && count <= 0xc000 - address)))
        return read_command(address, count, output, capacity);
usage:
    append(output, capacity,
        "ERROR commands: cart diag/header/release; cart read <address> <1..256>; cart bank <0..255>; cart ram-digest/ram-test\n"
        "ROM bank selection persists until release. RAM reads use bank0; SRAM tests restore bytes and disable RAM.\n");
    return false;
}
