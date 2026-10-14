// SPDX-License-Identifier: MIT
// Original implementation from ISSI IS29GL064/032 datasheet tables 6.1, 6.2,
// 7.2, section 6.10 and tables 10.1-10.5. x8 command addresses include A-1.
#include "a2_is29gl032.h"
#include <string.h>

#define BANK_BYTES 0x4000u
#define ROM_WINDOW 0x4000u
#define UNLOCK_A 0x0aaau
#define UNLOCK_B 0x0555u

static int raw(a2_is29gl032 *flash, uint16_t address, uint8_t data) {
    return flash->bus.raw_write(flash->bus.context, address, data);
}

static int read_byte(a2_is29gl032 *flash, uint16_t address, uint8_t *data) {
    return flash->bus.read(flash->bus.context, address, data, 1);
}

static int unlock(a2_is29gl032 *flash, uint8_t command) {
    int error = raw(flash, UNLOCK_A, 0xaa);
    if (!error) error = raw(flash, UNLOCK_B, 0x55);
    if (!error) error = raw(flash, UNLOCK_A, command);
    return error;
}

static int reset_array(a2_is29gl032 *flash) {
    return raw(flash, 0, 0xf0);
}

static int query_byte(a2_is29gl032 *flash, uint16_t word_address, uint8_t *data) {
    return read_byte(flash, (uint16_t)(word_address * 2u), data);
}

int a2_is29gl032_init(a2_is29gl032 *flash, const a2_is29gl032_bus *bus,
                      uint32_t program_timeout_ms, uint32_t erase_timeout_ms) {
    if (!flash || !bus || !bus->select_bank || !bus->read || !bus->raw_write
            || !bus->sync || !bus->now_ms || !bus->wait_ms
            || !program_timeout_ms || !erase_timeout_ms) return LFS_ERR_INVAL;
    memset(flash, 0, sizeof(*flash));
    flash->bus = *bus;
    flash->program_timeout_ms = program_timeout_ms;
    flash->erase_timeout_ms = erase_timeout_ms;
    return 0;
}

int a2_is29gl032_probe(a2_is29gl032 *flash, a2_is29gl032_info *info) {
    if (!flash || !flash->bus.read) return LFS_ERR_INVAL;
    if (flash->requires_reset) return LFS_ERR_IO;
    flash->identified = false;
    flash->writes_allowed = false;
    memset(&flash->info, 0, sizeof(flash->info));
    int error = reset_array(flash);
    if (!error) error = flash->bus.select_bank(flash->bus.context, 0);
    if (!error) error = unlock(flash, 0x90);
    const uint16_t id_addresses[] = {0, 2, 0x1c, 0x1e};
    uint8_t ids[4] = {0};
    for (unsigned i = 0; i < 4 && !error; ++i)
        error = read_byte(flash, id_addresses[i], &ids[i]);
    flash->info.manufacturer = ids[0];
    flash->info.device1 = ids[1];
    flash->info.device2 = ids[2];
    flash->info.device3 = ids[3];
    int reset_error = reset_array(flash);
    if (!error) error = reset_error;
    if (!error && (ids[0] != 0x9d || ids[1] != 0x7e
            || ids[2] != 0x1d || ids[3] != 0x01)) error = LFS_ERR_CORRUPT;

    // Exact supported CFI table: AMD command set, x8/x16, exponent22, one
    // region whose count-minus-one is63 and sector size is256*256 bytes.
    static const struct { uint16_t word; uint8_t value; } geometry[] = {
        {0x10, 'Q'}, {0x11, 'R'}, {0x12, 'Y'}, {0x13, 2}, {0x14, 0},
        {0x27, 22}, {0x28, 2}, {0x29, 0}, {0x2c, 1},
        {0x2d, 63}, {0x2e, 0}, {0x2f, 0}, {0x30, 1},
    };
    if (!error) error = raw(flash, 0xaa, 0x98);
    for (size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]) && !error; ++i) {
        uint8_t value;
        error = query_byte(flash, geometry[i].word, &value);
        if (!error && value != geometry[i].value) error = LFS_ERR_CORRUPT;
    }
    reset_error = reset_array(flash);
    if (!error) error = reset_error;
    if (!error) {
        flash->info.bytes = A2_STORAGE_FLASH_BYTES;
        flash->info.sector_bytes = A2_STORAGE_ERASE_BYTES;
        flash->info.sectors = A2_STORAGE_BLOCKS;
    }
    if (!error) error = flash->bus.select_bank(flash->bus.context, 0);
    flash->identified = !error;
    if (info) *info = flash->info;
    return error;
}

int a2_is29gl032_allow_writes(a2_is29gl032 *flash) {
    if (!flash || !flash->identified) return LFS_ERR_CORRUPT;
    flash->writes_allowed = true;
    return 0;
}

void a2_is29gl032_disallow_writes(a2_is29gl032 *flash) {
    if (flash) flash->writes_allowed = false;
}

static int fail_operation(a2_is29gl032 *flash, int error) {
    // Do not send even F0 here: after an ambiguous transport failure following
    // A0 or a buffer load, that byte could be consumed as program data. Require
    // hardware reset instead of attempting any software abort/reset sequence.
    flash->writes_allowed = false;
    flash->identified = false;
    flash->requires_reset = true;
    return error;
}

static int poll(a2_is29gl032 *flash, uint16_t address, uint8_t expected,
                 uint32_t timeout_ms, bool buffered) {
    uint64_t start = flash->bus.now_ms(flash->bus.context);
    for (;;) {
        if (flash->bus.now_ms(flash->bus.context) - start >= timeout_ms)
            return A2_FLASH_ERR_TIMEOUT;
        uint8_t status;
        int error = read_byte(flash, address, &status);
        if (error) return error;
        if ((status & 0x80) == (expected & 0x80)) return 0;
        if ((status & 0x20) || (buffered && (status & 0x02))) {
            // DQ5 signals an embedded failure; DQ1 additionally signals a
            // buffer abort. Recheck DQ7 before rejecting a completion race.
            error = read_byte(flash, address, &status);
            if (error) return error;
            return (status & 0x80) == (expected & 0x80) ? 0 : LFS_ERR_IO;
        }
        flash->bus.wait_ms(flash->bus.context, 1);
    }
}

static int program_run(a2_is29gl032 *flash, uint16_t address,
                        const uint8_t *input, size_t size) {
    int error;
    if (size == 1) {
        // A single changed byte takes four writes; the buffer needs six.
        error = unlock(flash, 0xa0);
        if (!error) error = raw(flash, address, input[0]);
    } else {
        // Section 6.10: x8 has a 256-byte buffer page. The caller supplies one
        // sequential run within it, so no mapper, page or sector can change.
        error = raw(flash, UNLOCK_A, 0xaa);
        if (!error) error = raw(flash, UNLOCK_B, 0x55);
        if (!error) error = raw(flash, address, 0x25);
        if (!error) error = raw(flash, address, (uint8_t)(size - 1));
        for (size_t i = 0; i < size && !error;) {
            size_t chunk = size - i;
            if (flash->bus.raw_write_block) {
                if (chunk > 32) chunk = 32;
                error = flash->bus.raw_write_block(flash->bus.context,
                            (uint16_t)(address + i), input + i, chunk);
            } else {
                chunk = 1;
                error = raw(flash, (uint16_t)(address + i), input[i]);
            }
            i += chunk;
        }
        if (!error) error = raw(flash, address, 0x29);
    }
    // Status belongs to the LAST loaded address, including its DQ7 value.
    if (!error) error = poll(flash, (uint16_t)(address + size - 1),
                input[size - 1], flash->program_timeout_ms, size > 1);
    return error;
}

static int flash_read(void *context, uint32_t offset, void *data, size_t size) {
    a2_is29gl032 *flash = context;
    if (!flash->identified) return LFS_ERR_CORRUPT;
    if ((!data && size) || offset > A2_STORAGE_FLASH_BYTES
            || size > A2_STORAGE_FLASH_BYTES - offset) return LFS_ERR_INVAL;
    uint8_t *output = data;
    while (size) {
        uint32_t within = offset % BANK_BYTES;
        size_t chunk = BANK_BYTES - within;
        if (chunk > 256) chunk = 256;
        if (chunk > size) chunk = size;
        int error = flash->bus.select_bank(flash->bus.context, (uint16_t)(offset / BANK_BYTES));
        if (!error) error = flash->bus.read(flash->bus.context,
                            (uint16_t)(ROM_WINDOW + within), output, chunk);
        if (error) return fail_operation(flash, error);
        offset += (uint32_t)chunk;
        output += chunk;
        size -= chunk;
    }
    return 0;
}

static int flash_program(void *context, uint32_t offset, const void *data, size_t size) {
    a2_is29gl032 *flash = context;
    if (!flash->identified || !flash->writes_allowed) return A2_FLASH_ERR_READONLY;
    if (!data || offset % 256 || size != 256
            || offset > A2_STORAGE_FLASH_BYTES - size) return LFS_ERR_INVAL;
    const uint8_t *input = data;
    uint8_t before[256];
    int error = flash_read(flash, offset, before, sizeof(before));
    if (error) return error;
    // Validate the entire page first: reject 0->1 requests before any mutation.
    for (size_t i = 0; i < size; ++i)
        if ((before[i] & input[i]) != input[i]) return LFS_ERR_INVAL;
    bool changed = false;
    for (size_t i = 0; i < size;) {
        if (before[i] == input[i]) { ++i; continue; }
        size_t first = i++;
        while (i < size && before[i] != input[i]) ++i;
        changed = true;
        uint16_t address = (uint16_t)(ROM_WINDOW + (offset + first) % BANK_BYTES);
        error = program_run(flash, address, input + first, i - first);
        if (error) return fail_operation(flash, error);
    }
    if (changed) {
        // Verify the whole page, including gaps deliberately left unchanged.
        error = flash_read(flash, offset, before, sizeof(before));
        if (!error && memcmp(before, input, size)) error = LFS_ERR_CORRUPT;
        if (error) return fail_operation(flash, error);
    }
    return 0;
}

static int flash_erase(void *context, uint32_t offset) {
    a2_is29gl032 *flash = context;
    if (!flash->identified || !flash->writes_allowed) return A2_FLASH_ERR_READONLY;
    if (offset % A2_STORAGE_ERASE_BYTES
            || offset > A2_STORAGE_FLASH_BYTES - A2_STORAGE_ERASE_BYTES) return LFS_ERR_INVAL;
    int error = flash->bus.select_bank(flash->bus.context, (uint16_t)(offset / BANK_BYTES));
    if (!error) error = unlock(flash, 0x80);
    if (!error) error = raw(flash, UNLOCK_A, 0xaa);
    if (!error) error = raw(flash, UNLOCK_B, 0x55);
    if (!error) error = raw(flash, ROM_WINDOW, 0x30);
    if (!error) error = poll(flash, ROM_WINDOW, 0xff, flash->erase_timeout_ms, false);
    if (error) return fail_operation(flash, error);
    uint8_t buffer[256];
    for (uint32_t position = offset; position < offset + A2_STORAGE_ERASE_BYTES; position += sizeof(buffer)) {
        error = flash_read(flash, position, buffer, sizeof(buffer));
        if (error) return error;
        for (size_t i = 0; i < sizeof(buffer); ++i)
            if (buffer[i] != 0xff) return fail_operation(flash, LFS_ERR_CORRUPT);
    }
    return 0;
}

static int flash_sync(void *context) {
    a2_is29gl032 *flash = context;
    if (!flash->identified) return LFS_ERR_CORRUPT;
    return flash->bus.sync(flash->bus.context);
}

int a2_is29gl032_backend(a2_is29gl032 *flash, a2_storage_backend *backend) {
    if (!flash || !backend || !flash->identified) return LFS_ERR_CORRUPT;
    *backend = (a2_storage_backend){.context = flash, .read = flash_read,
        .program = flash_program, .erase = flash_erase, .sync = flash_sync};
    return 0;
}
