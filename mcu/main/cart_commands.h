/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Caller serializes the full command with its machine/storage mutex. */
bool cart_command(int argc, char **argv, char *output, size_t capacity);
