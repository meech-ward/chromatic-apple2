// SPDX-License-Identifier: MIT
#include "a2_is29gl032.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct {
    uint8_t *array, manufacturer, regions;
    unsigned bank, mode, phase, programs, erases, raw_writes;
    unsigned busy_reads, pending_kind, fault;
    unsigned buffer_programs, byte_programs, buffer_count, buffer_loaded;
    unsigned fail_write_at;
    unsigned block_calls;
    uint32_t pending_offset;
    uint32_t buffer_start;
    uint16_t buffer_command;
    uint8_t buffer[256];
    uint8_t pending_data;
    uint64_t time_ms;
    bool omit_erase_last;
} chip_model;

static uint32_t physical(chip_model *model, uint16_t address) {
    CHECK(address < 0x8000);
    return address < 0x4000 ? address : model->bank * 0x4000u + address - 0x4000u;
}

static int select_bank(void *context, uint16_t bank) {
    chip_model *model = context;
    // Changing the mapper while a flash command is armed is not supported.
    CHECK(model->mode == 0 && model->phase == 0 && !model->busy_reads && bank < 256);
    model->bank = bank;
    return 0;
}

static uint8_t cfi(chip_model *model, uint32_t address) {
    if (address & 1) return 0;
    switch (address / 2) {
        case 0x10: return 'Q'; case 0x11: return 'R'; case 0x12: return 'Y';
        case 0x13: return 2; case 0x27: return 22; case 0x28: return 2;
        case 0x2c: return model->regions; case 0x2d: return 63; case 0x30: return 1;
        default: return 0;
    }
}

static void commit_pending(chip_model *model) {
    if (model->pending_kind == 1) {
        CHECK((model->array[model->pending_offset] & model->pending_data) == model->pending_data);
        model->array[model->pending_offset] &= model->pending_data;
        model->programs++;
        model->byte_programs++;
    } else if (model->pending_kind == 3) {
        for (unsigned i = 0; i < model->buffer_count; ++i) {
            uint32_t address = model->buffer_start + i;
            CHECK((model->array[address] & model->buffer[i]) == model->buffer[i]);
            if (!(model->fault == 6 && i == 0)) model->array[address] &= model->buffer[i];
            model->programs++;
        }
        model->buffer_programs++;
    } else {
        uint32_t base = model->pending_offset & ~(A2_STORAGE_ERASE_BYTES - 1u);
        uint8_t previous_last = model->array[base + A2_STORAGE_ERASE_BYTES - 1];
        memset(model->array + base, 0xff, A2_STORAGE_ERASE_BYTES);
        if (model->omit_erase_last) model->array[base + A2_STORAGE_ERASE_BYTES - 1] = previous_last;
        model->erases++;
    }
}

static int read_bus(void *context, uint16_t address, void *data, size_t size) {
    chip_model *model = context;
    uint8_t *output = data;
    CHECK(size <= 256 && (size_t)address + size <= 0x8000);
    for (size_t i = 0; i < size; ++i) {
        uint32_t offset = physical(model, (uint16_t)(address + i));
        if (model->busy_reads) {
            CHECK(size == 1 && offset == model->pending_offset);
            uint8_t busy = (model->pending_data ^ 0x80) & 0x80;
            if (model->fault == 1) { output[i] = busy | 0x20; continue; }
            if (model->fault == 2) { output[i] = busy; continue; }
            if (model->fault == 4) { output[i] = busy | 0x02; continue; }
            if (model->fault == 3 && model->busy_reads == 2) {
                model->busy_reads--;
                output[i] = busy | 0x20;
                continue;
            }
            if (model->fault == 5 && model->busy_reads == 2) {
                model->busy_reads--;
                output[i] = busy | 0x02;
                continue;
            }
            if (--model->busy_reads) { output[i] = busy; continue; }
            commit_pending(model);
        }
        if (model->mode == 1) {
            switch (offset & 0x1fff) {
                case 0: case 1: output[i] = model->manufacturer; break;
                case 2: case 3: output[i] = 0x7e; break;
                case 0x1c: case 0x1d: output[i] = 0x1d; break;
                case 0x1e: case 0x1f: output[i] = 1; break;
                default: output[i] = 0xff;
            }
        } else if (model->mode == 2) output[i] = cfi(model, offset);
        else output[i] = model->array[offset];
    }
    return 0;
}

static int write_cycle(chip_model *model, uint16_t address, uint8_t data) {
    CHECK((address >= 0x4000 && address < 0x8000)
        || (address == 0 && data == 0xf0)
        || (address == 0xaa && data == 0x98)
        || (address == 0x555 && data == 0x55)
        || (address == 0xaaa && (data == 0xaa || data == 0x90 || data == 0xa0 || data == 0x80)));
    // A programmed data byte may itself be F0; command decoding comes later.
    if (model->phase == 3) {
        CHECK(!model->busy_reads);
        model->pending_offset = physical(model, address);
        model->pending_data = data;
        model->pending_kind = 1;
        model->busy_reads = 2;
        model->phase = 0;
        return 0;
    }
    if (model->phase == 7) {
        CHECK(address == model->buffer_command);
        model->buffer_count = (unsigned)data + 1u;
        model->buffer_loaded = 0;
        model->phase = 8;
        return 0;
    }
    if (model->phase == 8) {
        uint32_t offset = physical(model, address);
        CHECK(model->buffer_loaded < model->buffer_count);
        if (!model->buffer_loaded) model->buffer_start = offset;
        CHECK(offset == model->buffer_start + model->buffer_loaded);
        CHECK((offset & ~255u) == (model->buffer_start & ~255u));
        CHECK((offset & ~(A2_STORAGE_ERASE_BYTES - 1u)) ==
              (physical(model, model->buffer_command) & ~(A2_STORAGE_ERASE_BYTES - 1u)));
        model->buffer[model->buffer_loaded++] = data;
        if (model->buffer_loaded == model->buffer_count) model->phase = 9;
        return 0;
    }
    if (model->phase == 9) {
        CHECK(address == model->buffer_command && data == 0x29);
        model->pending_offset = model->buffer_start + model->buffer_count - 1;
        model->pending_data = model->buffer[model->buffer_count - 1];
        model->pending_kind = 3;
        model->busy_reads = 2;
        model->phase = 0;
        return 0;
    }
    if (data == 0xf0) {
        model->mode = model->phase = model->busy_reads = 0;
        return 0;
    }
    CHECK(model->mode == 0 && !model->busy_reads);
    switch (model->phase) {
        case 0:
            if (address == 0xaaa && data == 0xaa) model->phase = 1;
            else if (address == 0xaa && data == 0x98) model->mode = 2;
            else CHECK(false);
            break;
        case 1:
            CHECK(address == 0x555 && data == 0x55); model->phase = 2; break;
        case 2:
            model->phase = 0;
            if (address >= 0x4000 && data == 0x25) {
                model->buffer_command = address;
                model->phase = 7;
            } else {
                CHECK(address == 0xaaa);
                if (data == 0x90) model->mode = 1;
                else if (data == 0xa0) model->phase = 3;
                else if (data == 0x80) model->phase = 4;
                else CHECK(false);
            }
            break;
        case 4:
            CHECK(address == 0xaaa && data == 0xaa); model->phase = 5; break;
        case 5:
            CHECK(address == 0x555 && data == 0x55); model->phase = 6; break;
        case 6:
            CHECK(data == 0x30);
            model->pending_offset = physical(model, address);
            model->pending_data = 0xff;
            model->pending_kind = 2;
            model->busy_reads = 2;
            model->phase = 0;
            break;
        default: CHECK(false);
    }
    return 0;
}

static int write_bus(void *context, uint16_t address, uint8_t data) {
    chip_model *model = context;
    model->raw_writes++;
    int error = write_cycle(model, address, data);
    // The cycle may have reached the chip even though transport reported IO.
    if (model->raw_writes == model->fail_write_at) return LFS_ERR_IO;
    return error;
}

static int write_block(void *context, uint16_t address, const void *data, size_t size) {
    chip_model *model = context;
    CHECK(data && size >= 1 && size <= 32);
    CHECK(address >= 0x4000 && (size_t)address + size <= 0x8000);
    CHECK(model->phase == 8); // Batches may contain loaded data, never commands.
    model->block_calls++;
    const uint8_t *input = data;
    for (size_t i = 0; i < size; ++i) {
        int error = write_bus(model, (uint16_t)(address + i), input[i]);
        if (error) return error;
    }
    return 0;
}

static int sync_bus(void *context) { (void)context; return 0; }
static uint64_t now_ms(void *context) { return ((chip_model *)context)->time_ms; }
static void wait_ms(void *context, uint32_t size) { ((chip_model *)context)->time_ms += size; }

static void ready(a2_is29gl032 *flash, a2_storage_backend *backend) {
    if (flash->requires_reset) {
        chip_model *model = flash->bus.context;
        model->mode = model->phase = model->busy_reads = 0; // Simulated power reset.
        a2_is29gl032_bus bus = flash->bus;
        CHECK(a2_is29gl032_init(flash, &bus, 20, 100) == 0);
    }
    CHECK(a2_is29gl032_probe(flash, NULL) == 0);
    CHECK(a2_is29gl032_allow_writes(flash) == 0);
    CHECK(a2_is29gl032_backend(flash, backend) == 0);
}

int main(void) {
    puts("Testing exact IS29GL032 command backend against a mapped NOR model");
    chip_model model = {.array = malloc(A2_STORAGE_FLASH_BYTES), .manufacturer = 0x9d, .regions = 1};
    CHECK(model.array);
    memset(model.array, 0xff, A2_STORAGE_FLASH_BYTES);
    a2_is29gl032_bus bus = {.context = &model, .select_bank = select_bank,
        .read = read_bus, .raw_write = write_bus, .sync = sync_bus,
        .now_ms = now_ms, .wait_ms = wait_ms, .raw_write_block = write_block};
    a2_is29gl032 flash;
    a2_storage_backend backend;
    CHECK(a2_is29gl032_init(&flash, &bus, 20, 100) == 0);
    CHECK(model.raw_writes == 0);
    CHECK(a2_is29gl032_backend(&flash, &backend) == LFS_ERR_CORRUPT);
    CHECK(a2_is29gl032_allow_writes(&flash) == LFS_ERR_CORRUPT);
    model.manufacturer = 1;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_CORRUPT);
    model.manufacturer = 0x9d;
    model.regions = 2;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_CORRUPT);
    model.regions = 1;
    a2_is29gl032_info info;
    CHECK(a2_is29gl032_probe(&flash, &info) == 0);
    CHECK(info.bytes == A2_STORAGE_FLASH_BYTES && info.sector_bytes == 65536 && info.sectors == 64);
    CHECK(model.programs == 0 && model.erases == 0);
    CHECK(a2_is29gl032_backend(&flash, &backend) == 0);
    uint8_t page[256], output[512];
    for (unsigned i = 0; i < sizeof(page); ++i) page[i] = (uint8_t)i;
    CHECK(backend.program(backend.context, 0, page, sizeof(page)) == A2_FLASH_ERR_READONLY);
    CHECK(backend.erase(backend.context, 0) == A2_FLASH_ERR_READONLY);
    CHECK(a2_is29gl032_allow_writes(&flash) == 0);
    CHECK(backend.program(backend.context, 0x3f00, page, sizeof(page)) == 0);
    CHECK(backend.program(backend.context, 0x4000, page, sizeof(page)) == 0);
    CHECK(backend.read(backend.context, 0x3f00, output, sizeof(output)) == 0);
    CHECK(!memcmp(output, page, 256) && !memcmp(output + 256, page, 256));
    memset(page, 0xff, sizeof(page));
    unsigned before = model.programs;
    CHECK(backend.program(backend.context, 0x4000, page, sizeof(page)) == LFS_ERR_INVAL);
    CHECK(model.programs == before);
    model.array[65536] = 0x5a;
    CHECK(backend.erase(backend.context, 0) == 0);
    CHECK(model.array[65536] == 0x5a);
    for (unsigned i = 0; i < 65536; ++i) CHECK(model.array[i] == 0xff);

    // A complete x8 page uses count FF and 261 writes instead of 1024 byte cycles.
    memset(page, 0, sizeof(page));
    unsigned writes = model.raw_writes, buffers = model.buffer_programs;
    unsigned batches = model.block_calls;
    CHECK(backend.program(backend.context, 0x20000, page, sizeof(page)) == 0);
    CHECK(model.raw_writes == writes + 261 && model.buffer_programs == buffers + 1);
    CHECK(model.block_calls == batches + 8); // Eight 32-byte transport requests.
    CHECK(model.buffer_count == 256);
    writes = model.raw_writes;
    CHECK(backend.program(backend.context, 0x20000, page, sizeof(page)) == 0);
    CHECK(model.raw_writes == writes); // Identical pages cause no program cycles.

    // Sparse unchanged gaps are never reprogrammed. Command-looking payloads
    // must be loaded as data; a lone byte retains the cheaper byte algorithm.
    memset(page, 0xff, sizeof(page));
    page[0] = 0xaa; page[1] = 0x25; page[2] = 0x29; page[255] = 0xf0;
    writes = model.raw_writes; buffers = model.buffer_programs;
    unsigned bytes = model.byte_programs;
    CHECK(backend.program(backend.context, 0x20300, page, sizeof(page)) == 0);
    CHECK(model.raw_writes == writes + 12);
    CHECK(model.buffer_programs == buffers + 1 && model.byte_programs == bytes + 1);
    CHECK(!memcmp(model.array + 0x20300, page, sizeof(page)));
    memset(page, 0xa5, sizeof(page));
    CHECK(backend.program(backend.context, A2_STORAGE_FLASH_BYTES - 256, page, sizeof(page)) == 0);
    CHECK(!memcmp(model.array + A2_STORAGE_FLASH_BYTES - 256, page, sizeof(page)));

    // Backends lacking the optional transport batch still use buffer commands,
    // with exactly the same data. This fallback is chosen before any operation.
    flash.bus.raw_write_block = NULL;
    batches = model.block_calls;
    CHECK(backend.program(backend.context, 0x20400, page, sizeof(page)) == 0);
    CHECK(model.block_calls == batches && !memcmp(model.array + 0x20400, page, sizeof(page)));
    flash.bus.raw_write_block = write_block;

    // DQ1 abort and completion races are observed only at the final loaded byte.
    memset(page, 0, sizeof(page));
    model.fault = 4;
    writes = model.raw_writes;
    CHECK(backend.program(backend.context, 0x30000, page, sizeof(page)) == LFS_ERR_IO);
    CHECK(model.raw_writes == writes + 261 && flash.requires_reset);
    CHECK(!flash.identified && !flash.writes_allowed);
    writes = model.raw_writes;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_IO && model.raw_writes == writes);
    model.fault = 0; ready(&flash, &backend);
    model.fault = 5;
    CHECK(backend.program(backend.context, 0x30100, page, sizeof(page)) == 0);
    model.fault = 6; // Successful DQ7 but one byte failed to program.
    CHECK(backend.program(backend.context, 0x30200, page, sizeof(page)) == LFS_ERR_CORRUPT);
    CHECK(flash.requires_reset);
    model.fault = 0; ready(&flash, &backend);

    // A failed reply during data load or after confirm must never send a reset
    // or retry. The chip may still be expecting a data byte or already busy.
    writes = model.raw_writes;
    model.fail_write_at = writes + 5;
    CHECK(backend.program(backend.context, 0x30300, page, sizeof(page)) == LFS_ERR_IO);
    CHECK(model.raw_writes == writes + 5 && model.phase == 8 && flash.requires_reset);
    CHECK(model.array[0x30300] == 0xff);
    writes = model.raw_writes;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_IO && model.raw_writes == writes);
    model.fail_write_at = 0; ready(&flash, &backend);
    writes = model.raw_writes;
    model.fail_write_at = writes + 261;
    CHECK(backend.program(backend.context, 0x30400, page, sizeof(page)) == LFS_ERR_IO);
    CHECK(model.raw_writes == writes + 261 && model.busy_reads && flash.requires_reset);
    writes = model.raw_writes;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_IO && model.raw_writes == writes);
    model.fail_write_at = 0; ready(&flash, &backend);

    memset(page, 0, sizeof(page));
    model.fault = 1;
    writes = model.raw_writes;
    CHECK(backend.program(backend.context, 0, page, sizeof(page)) == LFS_ERR_IO);
    CHECK(!flash.identified && !flash.writes_allowed);
    CHECK(model.raw_writes == writes + 261); // One full buffer; no recovery write.
    writes = model.raw_writes;
    CHECK(a2_is29gl032_probe(&flash, NULL) == LFS_ERR_IO);
    CHECK(model.raw_writes == writes);
    model.fault = 0; ready(&flash, &backend);
    model.fault = 2;
    uint64_t start = model.time_ms;
    CHECK(backend.program(backend.context, 0, page, sizeof(page)) == A2_FLASH_ERR_TIMEOUT);
    CHECK(model.time_ms - start == 20 && !flash.writes_allowed);
    model.fault = 0; ready(&flash, &backend);
    model.fault = 3; // DQ5 race: second DQ7 read sees successful completion.
    CHECK(backend.program(backend.context, 0, page, sizeof(page)) == 0);
    model.fault = 0;
    model.omit_erase_last = true;
    model.array[65535] = 0;
    CHECK(backend.erase(backend.context, 0) == LFS_ERR_CORRUPT);
    CHECK(!flash.identified);
    model.omit_erase_last = false;

    // Connect the verified model through the real LittleFS adapter, then boot
    // a fresh filesystem instance and read the saved image with writes disabled.
    memset(model.array, 0xff, A2_STORAGE_FLASH_BYTES);
    ready(&flash, &backend);
    a2_storage storage;
    CHECK(a2_storage_init(&storage, &backend) == 0);
    CHECK(a2_storage_format(&storage) == 0);
    CHECK(a2_storage_mount(&storage) == 0);
    CHECK(a2_disk_image_create(&storage, "/TEST.dsk") == 0);
    a2_disk_image image = {0};
    CHECK(a2_disk_image_open(&storage, &image, "/TEST.dsk", true) == 0);
    for (unsigned i = 0; i < sizeof(page); ++i) page[i] = (uint8_t)(i ^ 0xa5);
    CHECK(a2_disk_image_write_at(&image, 65536 - 128, page, sizeof(page)) == 0);
    CHECK(a2_disk_image_sync(&image) == 0);
    CHECK(a2_disk_image_close(&image) == 0);
    CHECK(a2_storage_unmount(&storage) == 0);
    CHECK(a2_is29gl032_probe(&flash, NULL) == 0);
    CHECK(!flash.writes_allowed);
    CHECK(a2_storage_init(&storage, &backend) == 0);
    CHECK(a2_storage_mount(&storage) == 0);
    CHECK(a2_disk_image_open(&storage, &image, "/TEST.dsk", false) == 0);
    CHECK(a2_disk_image_read_at(&image, 65536 - 128, output, sizeof(page)) == 0);
    CHECK(!memcmp(output, page, sizeof(page)));
    CHECK(a2_disk_image_close(&image) == 0);
    CHECK(a2_storage_unmount(&storage) == 0);
    printf("PASS: exact ID/CFI gates, mapper boundaries, byte/buffer programming, sector erase/readback, DQ5 retry, deadlines, LittleFS persistence (%u programmed bytes, %u buffer operations)\n", model.programs, model.buffer_programs);
    free(model.array);
    return 0;
}
