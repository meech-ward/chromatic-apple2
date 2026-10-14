// SPDX-License-Identifier: MIT
#ifndef A2_IMPORT_H
#define A2_IMPORT_H
#include "a2_storage.h"

#define A2_IMPORT_NAME_MAX 63u

// Zero-initialize and keep at a stable address. Allow only one active import
// per filesystem, and serialize filesystem calls externally. The .dsk.tmp
// namespace is reserved for uploads; beginning again replaces a stale temp.
typedef struct {
    a2_storage *storage;
    lfs_file_t file;
    struct lfs_file_config config;
    uint8_t cache[A2_STORAGE_CACHE_BYTES];
    char target[A2_IMPORT_NAME_MAX + 2];
    char temporary[A2_IMPORT_NAME_MAX + 6];
    uint32_t received;
    int error;
    bool active, file_open;
} a2_import;

// Safe ASCII basename, 5..63 characters, beginning with a letter/digit.
// Remaining characters may be letters/digits, space, underscore, hyphen or
// single dots; suffix must be .dsk, case insensitive. No paths or '..'.
int a2_import_begin(a2_import *upload, a2_storage *storage, const char *name);
int a2_import_write(a2_import *upload, const void *data, size_t size);
// Requires exactly 143360 bytes. Sync/close precede atomic replacement. A failed
// precommit finish cleans up the temp and preserves an existing target. During
// the final atomic rename, power loss/ambiguous I/O may leave old or complete new.
int a2_import_finish(a2_import *upload);
// Idempotent. Closes/removes only the reserved temporary file, never the target.
int a2_import_abort(a2_import *upload);

#endif
