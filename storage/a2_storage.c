// SPDX-License-Identifier: MIT
#include "a2_storage.h"
#include <string.h>

static bool valid_region(lfs_block_t block, lfs_off_t offset, lfs_size_t size) {
    return block < A2_STORAGE_BLOCKS && offset <= A2_STORAGE_ERASE_BYTES
        && size <= A2_STORAGE_ERASE_BYTES - offset;
}

static int backend_read(const struct lfs_config *config, lfs_block_t block,
                        lfs_off_t offset, void *data, lfs_size_t size) {
    a2_storage *storage = config->context;
    if (!valid_region(block, offset, size)) return LFS_ERR_INVAL;
    return storage->backend.read(storage->backend.context,
        block * A2_STORAGE_ERASE_BYTES + offset, data, size);
}

static int backend_program(const struct lfs_config *config, lfs_block_t block,
                           lfs_off_t offset, const void *data, lfs_size_t size) {
    a2_storage *storage = config->context;
    if (!valid_region(block, offset, size)) return LFS_ERR_INVAL;
    return storage->backend.program(storage->backend.context,
        block * A2_STORAGE_ERASE_BYTES + offset, data, size);
}

static int backend_erase(const struct lfs_config *config, lfs_block_t block) {
    a2_storage *storage = config->context;
    if (block >= A2_STORAGE_BLOCKS) return LFS_ERR_INVAL;
    return storage->backend.erase(storage->backend.context,
                                  block * A2_STORAGE_ERASE_BYTES);
}

static int backend_sync(const struct lfs_config *config) {
    a2_storage *storage = config->context;
    return storage->backend.sync(storage->backend.context);
}

static bool initialized(const a2_storage *storage) {
    return storage && storage->backend.read && storage->backend.program
        && storage->backend.erase && storage->backend.sync;
}

int a2_storage_init(a2_storage *storage, const a2_storage_backend *backend) {
    if (!storage || !backend || !backend->read || !backend->program
            || !backend->erase || !backend->sync) return LFS_ERR_INVAL;
    memset(storage, 0, sizeof(*storage));
    storage->backend = *backend;
    storage->config = (struct lfs_config){
        .context = storage, .read = backend_read, .prog = backend_program,
        .erase = backend_erase, .sync = backend_sync,
        .read_size = 16, .prog_size = A2_STORAGE_CACHE_BYTES,
        .block_size = A2_STORAGE_ERASE_BYTES, .block_count = A2_STORAGE_BLOCKS,
        .block_cycles = 500, .cache_size = A2_STORAGE_CACHE_BYTES,
        .lookahead_size = sizeof(storage->lookahead), .metadata_max = 4096,
        .read_buffer = storage->read_cache,
        .prog_buffer = storage->program_cache,
        .lookahead_buffer = storage->lookahead,
    };
    return 0;
}

int a2_storage_format(a2_storage *storage) {
    if (!initialized(storage) || storage->mounted) return LFS_ERR_INVAL;
    return lfs_format(&storage->lfs, &storage->config);
}

int a2_storage_mount(a2_storage *storage) {
    if (!initialized(storage) || storage->mounted) return LFS_ERR_INVAL;
    int error = lfs_mount(&storage->lfs, &storage->config);
    if (!error) storage->mounted = true;
    return error;
}

int a2_storage_unmount(a2_storage *storage) {
    if (!storage || !storage->mounted || storage->open_images) return LFS_ERR_INVAL;
    int error = lfs_unmount(&storage->lfs);
    if (!error) storage->mounted = false;
    return error;
}

int a2_storage_mkdir(a2_storage *storage, const char *path) {
    if (!storage || !storage->mounted || !path) return LFS_ERR_INVAL;
    return lfs_mkdir(&storage->lfs, path);
}

static bool disk_name(const char *path) {
    if (!path) return false;
    size_t length = strlen(path);
    if (length < 5) return false;
    const char *suffix = path + length - 4;
    return suffix[0] == '.' && (suffix[1] == 'd' || suffix[1] == 'D')
        && (suffix[2] == 's' || suffix[2] == 'S')
        && (suffix[3] == 'k' || suffix[3] == 'K');
}

int a2_storage_catalog(a2_storage *storage, const char *directory,
                       a2_storage_visitor visitor, void *context) {
    if (!storage || !storage->mounted || !directory || !visitor) return LFS_ERR_INVAL;
    lfs_dir_t dir;
    int error = lfs_dir_open(&storage->lfs, &dir, directory);
    if (error) return error;
    struct lfs_info info;
    while ((error = lfs_dir_read(&storage->lfs, &dir, &info)) > 0) {
        if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) continue;
        if (info.type != LFS_TYPE_DIR
                && !(info.type == LFS_TYPE_REG && disk_name(info.name)
                     && info.size == A2_DISK_IMAGE_BYTES)) continue;
        a2_storage_entry entry = {.size = info.size,
                                  .directory = info.type == LFS_TYPE_DIR};
        memcpy(entry.name, info.name, strlen(info.name) + 1);
        error = visitor(context, &entry);
        if (error) break;
    }
    int close_error = lfs_dir_close(&storage->lfs, &dir);
    return error ? error : close_error;
}

static int open_image(a2_storage *storage, a2_disk_image *image,
                      const char *path, int flags) {
    if (!storage || !storage->mounted || !image || image->opened || !disk_name(path))
        return LFS_ERR_INVAL;
    memset(image, 0, sizeof(*image));
    image->config.buffer = image->cache;
    int error = lfs_file_opencfg(&storage->lfs, &image->file, path, flags, &image->config);
    if (error) return error;
    image->storage = storage;
    image->opened = true;
    image->writable = (flags & LFS_O_RDWR) == LFS_O_RDWR || (flags & LFS_O_WRONLY);
    storage->open_images++;
    return 0;
}

int a2_disk_image_create(a2_storage *storage, const char *path) {
    a2_disk_image image = {0};
    int error = open_image(storage, &image, path, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL);
    if (error) return error;
    const uint8_t zeros[A2_STORAGE_CACHE_BYTES] = {0};
    for (uint32_t offset = 0; offset < A2_DISK_IMAGE_BYTES; offset += sizeof(zeros)) {
        lfs_ssize_t count = lfs_file_write(&storage->lfs, &image.file, zeros, sizeof(zeros));
        if (count != (lfs_ssize_t)sizeof(zeros)) {
            error = count < 0 ? (int)count : LFS_ERR_IO;
            break;
        }
    }
    int close_error = a2_disk_image_close(&image);
    if (!error) error = close_error;
    if (error) (void)lfs_remove(&storage->lfs, path);
    return error;
}

int a2_disk_image_open(a2_storage *storage, a2_disk_image *image,
                       const char *path, bool writable) {
    int error = open_image(storage, image, path, writable ? LFS_O_RDWR : LFS_O_RDONLY);
    if (error) return error;
    lfs_soff_t size = lfs_file_size(&storage->lfs, &image->file);
    if (size != A2_DISK_IMAGE_BYTES) {
        (void)a2_disk_image_close(image);
        return size < 0 ? (int)size : LFS_ERR_CORRUPT;
    }
    return 0;
}

static int check_range(a2_disk_image *image, uint32_t offset, const void *data, size_t size) {
    if (!image || !image->opened) return LFS_ERR_BADF;
    if ((!data && size) || offset > A2_DISK_IMAGE_BYTES
            || size > A2_DISK_IMAGE_BYTES - offset) return LFS_ERR_INVAL;
    return 0;
}

int a2_disk_image_read_at(a2_disk_image *image, uint32_t offset, void *data, size_t size) {
    int error = check_range(image, offset, data, size);
    if (error || !size) return error;
    lfs_soff_t position = lfs_file_seek(&image->storage->lfs, &image->file,
                                      (lfs_soff_t)offset, LFS_SEEK_SET);
    if (position < 0) return (int)position;
    lfs_ssize_t count = lfs_file_read(&image->storage->lfs, &image->file, data, (lfs_size_t)size);
    return count == (lfs_ssize_t)size ? 0 : count < 0 ? (int)count : LFS_ERR_IO;
}

int a2_disk_image_write_at(a2_disk_image *image, uint32_t offset, const void *data, size_t size) {
    int error = check_range(image, offset, data, size);
    if (error) return error;
    if (!image->writable) return LFS_ERR_BADF;
    if (!size) return 0;
    lfs_soff_t position = lfs_file_seek(&image->storage->lfs, &image->file,
                                      (lfs_soff_t)offset, LFS_SEEK_SET);
    if (position < 0) return (int)position;
    lfs_ssize_t count = lfs_file_write(&image->storage->lfs, &image->file, data, (lfs_size_t)size);
    return count == (lfs_ssize_t)size ? 0 : count < 0 ? (int)count : LFS_ERR_IO;
}

int a2_disk_image_sync(a2_disk_image *image) {
    if (!image || !image->opened) return LFS_ERR_BADF;
    return lfs_file_sync(&image->storage->lfs, &image->file);
}

int a2_disk_image_close(a2_disk_image *image) {
    if (!image || !image->opened) return LFS_ERR_BADF;
    int error = lfs_file_close(&image->storage->lfs, &image->file);
    image->storage->open_images--;
    image->opened = false;
    image->storage = NULL;
    return error;
}
