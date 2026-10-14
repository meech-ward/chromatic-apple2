// SPDX-License-Identifier: MIT
#include "a2_import.h"
#include <stdio.h>
#include <string.h>

static bool alphanumeric(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9');
}

static bool safe_name(const char *name) {
    if (!name) return false;
    size_t length = 0;
    while (length <= A2_IMPORT_NAME_MAX && name[length]) length++;
    if (length < 5 || length > A2_IMPORT_NAME_MAX || !alphanumeric(name[0])) return false;
    for (size_t i = 1; i < length; ++i) {
        if (!(alphanumeric(name[i]) || name[i] == ' ' || name[i] == '_'
                || name[i] == '-' || name[i] == '.')) return false;
        if (name[i] == '.' && name[i - 1] == '.') return false;
    }
    const char *suffix = name + length - 4;
    return suffix[0] == '.' && (suffix[1] == 'd' || suffix[1] == 'D')
        && (suffix[2] == 's' || suffix[2] == 'S')
        && (suffix[3] == 'k' || suffix[3] == 'K');
}

static int close_file(a2_import *upload) {
    if (!upload->file_open) return 0;
    int error = lfs_file_close(&upload->storage->lfs, &upload->file);
    upload->file_open = false;
    upload->storage->open_images--;
    return error;
}

int a2_import_begin(a2_import *upload, a2_storage *storage, const char *name) {
    if (!upload || upload->active || !storage || !storage->mounted || !safe_name(name))
        return LFS_ERR_INVAL;
    memset(upload, 0, sizeof(*upload));
    upload->storage = storage;
    (void)snprintf(upload->target, sizeof(upload->target), "/%s", name);
    (void)snprintf(upload->temporary, sizeof(upload->temporary), "/%s.tmp", name);
    upload->config.buffer = upload->cache;
    int error = lfs_file_opencfg(&storage->lfs, &upload->file, upload->temporary,
        LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &upload->config);
    if (error) {
        upload->storage = NULL;
        return error;
    }
    upload->active = upload->file_open = true;
    storage->open_images++;
    return 0;
}

int a2_import_write(a2_import *upload, const void *data, size_t size) {
    if (!upload || !upload->active || !upload->file_open) return LFS_ERR_BADF;
    if (upload->error) return upload->error;
    if ((!data && size) || size > A2_DISK_IMAGE_BYTES - upload->received)
        return upload->error = LFS_ERR_INVAL;
    if (!size) return 0;
    lfs_ssize_t written = lfs_file_write(&upload->storage->lfs, &upload->file,
                                        data, (lfs_size_t)size);
    if (written > 0) upload->received += (uint32_t)written;
    if (written != (lfs_ssize_t)size)
        upload->error = written < 0 ? (int)written : LFS_ERR_IO;
    return upload->error;
}

int a2_import_abort(a2_import *upload) {
    if (!upload) return LFS_ERR_INVAL;
    if (!upload->active) return 0;
    int error = close_file(upload);
    int remove_error = lfs_remove(&upload->storage->lfs, upload->temporary);
    if (remove_error == LFS_ERR_NOENT) remove_error = 0;
    upload->active = false;
    upload->storage = NULL;
    return error ? error : remove_error;
}

int a2_import_finish(a2_import *upload) {
    if (!upload || !upload->active) return LFS_ERR_BADF;
    int error = upload->error;
    if (!error && upload->received != A2_DISK_IMAGE_BYTES) error = LFS_ERR_INVAL;
    if (!error) error = lfs_file_sync(&upload->storage->lfs, &upload->file);
    if (!error) error = close_file(upload);
    if (!error) error = lfs_rename(&upload->storage->lfs, upload->temporary, upload->target);
    if (error) {
        (void)a2_import_abort(upload);
        return error;
    }
    upload->active = false;
    upload->storage = NULL;
    return 0;
}
