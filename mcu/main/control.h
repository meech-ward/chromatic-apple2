/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "network.h"
bool control_command(const char *command, char *output, size_t capacity);
void control_status(char *output, size_t capacity);
void control_text(char *output, size_t capacity);
/* Bounded-wait terminal input: false means retry after the emulator consumes
 * its current key. Ctrl-] opens MENU; menu arrows/Return/Escape navigate it. */
bool control_terminal_key(unsigned char key);
bool control_terminal_text(char *output, size_t capacity);
/* One streaming import at a time. Abort is safe after any failed operation;
 * the importer retains the machine/storage lock until finish or abort.
 */
bool control_import_begin(const char *name);
bool control_import_write(const void *data, size_t size);
bool control_import_finish(void);
void control_import_abort(void);
int control_storage_error(void);
