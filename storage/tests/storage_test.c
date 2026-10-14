// SPDX-License-Identifier: MIT
#include "a2_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

typedef struct {
    uint8_t *data;
    unsigned programs, erases, syncs, violations;
    uint64_t programmed_blocks;
    int program_budget;
} nor_flash;

static int nor_read(void *context, uint32_t offset, void *data, size_t size) {
    nor_flash *flash = context;
    if (offset % 16 || size % 16 || offset > A2_STORAGE_FLASH_BYTES
            || size > A2_STORAGE_FLASH_BYTES - offset) return LFS_ERR_INVAL;
    memcpy(data, flash->data + offset, size);
    return 0;
}

static int nor_program(void *context, uint32_t offset, const void *data, size_t size) {
    nor_flash *flash = context;
    const uint8_t *source = data;
    if (offset % 256 || size != 256 || offset > A2_STORAGE_FLASH_BYTES - size) {
        flash->violations++;
        return LFS_ERR_INVAL;
    }
    if (flash->program_budget == 0) return LFS_ERR_IO;
    for (size_t i = 0; i < size; ++i) {
        if ((flash->data[offset + i] & source[i]) != source[i]) {
            flash->violations++;
            return LFS_ERR_IO;
        }
    }
    memcpy(flash->data + offset, source, size);
    flash->programs++;
    flash->programmed_blocks |= UINT64_C(1) << (offset / A2_STORAGE_ERASE_BYTES);
    if (flash->program_budget > 0) flash->program_budget--;
    return 0;
}

static int nor_erase(void *context, uint32_t offset) {
    nor_flash *flash = context;
    if (offset % A2_STORAGE_ERASE_BYTES
            || offset > A2_STORAGE_FLASH_BYTES - A2_STORAGE_ERASE_BYTES) {
        flash->violations++;
        return LFS_ERR_INVAL;
    }
    memset(flash->data + offset, 0xff, A2_STORAGE_ERASE_BYTES);
    flash->erases++;
    return 0;
}

static int nor_sync(void *context) {
    ((nor_flash *)context)->syncs++;
    return 0;
}

typedef struct { unsigned directories, images; } catalog_count;
static int count_entries(void *context, const a2_storage_entry *entry) {
    catalog_count *count = context;
    if (entry->directory) {
        CHECK(!strcmp(entry->name, "disks"));
        count->directories++;
    } else {
        CHECK(!strcmp(entry->name, "BASIC.dsk"));
        CHECK(entry->size == A2_DISK_IMAGE_BYTES);
        count->images++;
    }
    return 0;
}

static void check_disk(a2_disk_image *image, const uint8_t *expected) {
    uint8_t data[1024];
    for (uint32_t offset = 0; offset < A2_DISK_IMAGE_BYTES; offset += sizeof(data)) {
        CHECK(a2_disk_image_read_at(image, offset, data, sizeof(data)) == 0);
        CHECK(!memcmp(data, expected + offset, sizeof(data)));
    }
}

int main(void) {
    puts("Testing 4 MiB NOR cartridge storage");
    fflush(stdout);
    nor_flash flash = {.data = malloc(A2_STORAGE_FLASH_BYTES), .program_budget = -1};
    uint8_t *snapshot = malloc(A2_STORAGE_FLASH_BYTES);
    uint8_t *expected = calloc(1, A2_DISK_IMAGE_BYTES);
    CHECK(flash.data && snapshot && expected);
    memset(flash.data, 0xff, A2_STORAGE_FLASH_BYTES);
    a2_storage_backend backend = {
        .context = &flash, .read = nor_read, .program = nor_program,
        .erase = nor_erase, .sync = nor_sync,
    };

    // Check that the test device itself rejects 0->1 programming.
    uint8_t page[256] = {0};
    CHECK(nor_program(&flash, 0, page, sizeof(page)) == 0);
    memset(page, 0xff, sizeof(page));
    CHECK(nor_program(&flash, 0, page, sizeof(page)) == LFS_ERR_IO);
    CHECK(nor_erase(&flash, 0) == 0);
    flash.programs = flash.erases = flash.violations = 0;
    flash.programmed_blocks = 0;

    a2_storage storage;
    CHECK(a2_storage_init(&storage, &backend) == 0);
    memcpy(snapshot, flash.data, A2_STORAGE_FLASH_BYTES);
    CHECK(a2_storage_mount(&storage) == LFS_ERR_CORRUPT);
    CHECK(!memcmp(snapshot, flash.data, A2_STORAGE_FLASH_BYTES));
    CHECK(!flash.programs && !flash.erases); // Missing filesystem was not formatted.
    CHECK(a2_storage_format(&storage) == 0);
    CHECK(a2_storage_mount(&storage) == 0);
    CHECK(a2_storage_format(&storage) == LFS_ERR_INVAL);
    CHECK(a2_storage_mkdir(&storage, "/disks") == 0);
    CHECK(a2_disk_image_create(&storage, "/disks/BASIC.dsk") == 0);
    CHECK(a2_disk_image_create(&storage, "/disks/BASIC.dsk") == LFS_ERR_EXIST);
    CHECK(a2_disk_image_create(&storage, "/disks/not-an-image.bin") == LFS_ERR_INVAL);
    catalog_count catalog = {0};
    CHECK(a2_storage_catalog(&storage, "/", count_entries, &catalog) == 0);
    CHECK(a2_storage_catalog(&storage, "/disks", count_entries, &catalog) == 0);
    CHECK(catalog.directories == 1 && catalog.images == 1);

    a2_disk_image image = {0};
    CHECK(a2_disk_image_open(&storage, &image, "/disks/BASIC.dsk", true) == 0);
    CHECK(a2_storage_unmount(&storage) == LFS_ERR_INVAL);
    check_disk(&image, expected);
    uint8_t pattern[8192];
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = (uint8_t)(i * 73u + 19u);
    const uint32_t boundary = A2_STORAGE_ERASE_BYTES - 512;
    CHECK(a2_disk_image_write_at(&image, boundary, pattern, sizeof(pattern)) == 0);
    memcpy(expected + boundary, pattern, sizeof(pattern));
    CHECK(a2_disk_image_write_at(&image, 2 * A2_STORAGE_ERASE_BYTES - 127, pattern, 512) == 0);
    memcpy(expected + 2 * A2_STORAGE_ERASE_BYTES - 127, pattern, 512);
    uint8_t marker = 0x5a;
    CHECK(a2_disk_image_write_at(&image, A2_DISK_IMAGE_BYTES - 1, &marker, 1) == 0);
    expected[A2_DISK_IMAGE_BYTES - 1] = marker;
    CHECK(a2_disk_image_write_at(&image, A2_DISK_IMAGE_BYTES - 1, pattern, 2) == LFS_ERR_INVAL);
    CHECK(a2_disk_image_read_at(&image, UINT32_MAX, page, 1) == LFS_ERR_INVAL);
    CHECK(a2_disk_image_read_at(&image, A2_DISK_IMAGE_BYTES, NULL, 0) == 0);
    unsigned before_sync = flash.syncs;
    CHECK(a2_disk_image_sync(&image) == 0);
    CHECK(flash.syncs > before_sync);
    CHECK(a2_disk_image_close(&image) == 0);
    CHECK(a2_storage_unmount(&storage) == 0);

    // Reconstruct the complete driver state, retaining only NOR contents.
    CHECK(a2_storage_init(&storage, &backend) == 0);
    CHECK(a2_storage_mount(&storage) == 0);
    CHECK(a2_disk_image_open(&storage, &image, "/disks/BASIC.dsk", false) == 0);
    check_disk(&image, expected);
    CHECK(a2_disk_image_write_at(&image, 0, page, sizeof(page)) == LFS_ERR_BADF);
    CHECK(a2_disk_image_close(&image) == 0);

    // Stop an overwrite after one successful page program. Do not close or
    // unmount: reboot from partially updated NOR, as after losing power.
    CHECK(a2_disk_image_open(&storage, &image, "/disks/BASIC.dsk", true) == 0);
    flash.program_budget = 1;
    CHECK(a2_disk_image_write_at(&image, 0, pattern, sizeof(pattern)) == LFS_ERR_IO);
    CHECK(flash.program_budget == 0);
    flash.program_budget = -1;
    memset(&image, 0, sizeof(image));
    CHECK(a2_storage_init(&storage, &backend) == 0);
    CHECK(a2_storage_mount(&storage) == 0);
    CHECK(a2_disk_image_open(&storage, &image, "/disks/BASIC.dsk", false) == 0);
    check_disk(&image, expected);
    CHECK(a2_disk_image_close(&image) == 0);
    CHECK(a2_storage_unmount(&storage) == 0);

    // Corrupt both root metadata blocks. A failed mount must preserve every
    // byte and issue no erase/program, leaving recovery decisions to the caller.
    memset(flash.data, 0, 2 * A2_STORAGE_ERASE_BYTES);
    memcpy(snapshot, flash.data, A2_STORAGE_FLASH_BYTES);
    unsigned before_programs = flash.programs, before_erases = flash.erases;
    CHECK(a2_storage_init(&storage, &backend) == 0);
    CHECK(a2_storage_mount(&storage) == LFS_ERR_CORRUPT);
    CHECK(!memcmp(snapshot, flash.data, A2_STORAGE_FLASH_BYTES));
    CHECK(flash.programs == before_programs && flash.erases == before_erases);
    CHECK(flash.violations == 0);
    CHECK(__builtin_popcountll(flash.programmed_blocks) >= 3);
    printf("PASS: catalog, full disk persistence, sector boundaries, readonly/bounds, interrupted-write recovery, non-destructive failed mounts; caches=%zu bytes + %zu per-image object\n",
           sizeof(storage), sizeof(image));
    free(expected);
    free(snapshot);
    free(flash.data);
    return 0;
}
