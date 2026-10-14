/* SPDX-License-Identifier: MIT */
#pragma once
#include "apple2.h"
#include "a2_dos_catalog.h"
#include <stdbool.h>
#include <stddef.h>
void cart_storage_init(apple2_t *machine, const uint8_t *slot_rom, size_t size);
bool cart_storage_command(int argc, char **argv, char *output, size_t capacity);
void cart_storage_autoboot(void);
bool cart_storage_reset(void);
void cart_storage_tick(void);
bool cart_storage_saving(void);
bool cart_storage_sync(void);
bool cart_storage_programs(a2_dos_catalog *catalog);
const char *cart_storage_name(void);
bool cart_storage_import_begin(const char *name);
bool cart_storage_import_write(const void *data, size_t size);
bool cart_storage_import_finish(void);
void cart_storage_import_abort(void);
int cart_storage_error(void);
