// SPDX-License-Identifier: MIT
#ifndef A2_STORAGE_H
#define A2_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vendor/littlefs/lfs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define A2_STORAGE_FLASH_BYTES (4u * 1024u * 1024u)
#define A2_STORAGE_ERASE_BYTES (64u * 1024u)
#define A2_STORAGE_BLOCKS 64u
#define A2_STORAGE_CACHE_BYTES 256u
#define A2_DISK_IMAGE_BYTES (35u * 16u * 256u)

// Absolute flash offsets, relative to the beginning of this 4 MiB device.
// Return 0 or a negative littlefs error. read is 16-byte aligned; program is
// 256-byte aligned and issued in 256-byte pages. erase erases exactly 64 KiB.
// sync must wait for all earlier operations to be durable before returning.
typedef struct {
    void *context;
    int (*read)(void *context, uint32_t offset, void *data, size_t size);
    int (*program)(void *context, uint32_t offset, const void *data, size_t size);
    int (*erase)(void *context, uint32_t sector_offset);
    int (*sync)(void *context);
} a2_storage_backend;

// Keep these objects at stable addresses until unmounted/closed. Serialize API
// calls externally. No heap allocation is needed, including per-image caches.
typedef struct {
    lfs_t lfs;
    struct lfs_config config;
    a2_storage_backend backend;
    uint8_t read_cache[A2_STORAGE_CACHE_BYTES];
    uint8_t program_cache[A2_STORAGE_CACHE_BYTES];
    uint8_t lookahead[8];
    unsigned open_images;
    bool mounted;
} a2_storage;

typedef struct {
    a2_storage *storage;
    lfs_file_t file;
    struct lfs_file_config config;
    uint8_t cache[A2_STORAGE_CACHE_BYTES];
    bool opened;
    bool writable;
} a2_disk_image;

typedef struct {
    char name[LFS_NAME_MAX + 1];
    uint32_t size;
    bool directory;
} a2_storage_entry;
// Return 0 to continue, or a nonzero value to stop and propagate that value.
typedef int (*a2_storage_visitor)(void *context, const a2_storage_entry *entry);

// init operates on a fresh/unmounted object. format is explicitly destructive;
// mount NEVER formats or repairs arbitrary data after a failed mount.
int a2_storage_init(a2_storage *storage, const a2_storage_backend *backend);
int a2_storage_format(a2_storage *storage);
int a2_storage_mount(a2_storage *storage);
int a2_storage_unmount(a2_storage *storage);
int a2_storage_mkdir(a2_storage *storage, const char *path);
// Lists directories and valid 140 KiB .dsk files, excluding '.' and '..'.
int a2_storage_catalog(a2_storage *storage, const char *directory,
                       a2_storage_visitor visitor, void *context);

// Creates a new zero-filled raw disk; it does not install an Apple DOS format
// or overwrite an existing file. Paths must end in .dsk (case insensitive).
int a2_disk_image_create(a2_storage *storage, const char *path);
// image must initially be zero-initialized, or previously closed.
int a2_disk_image_open(a2_storage *storage, a2_disk_image *image,
                       const char *path, bool writable);
// Reads/writes are bounded to 143360 bytes and return 0 on complete success.
// A track is 4096 bytes at track_number*4096. Writes commit at sync/close.
int a2_disk_image_read_at(a2_disk_image *image, uint32_t offset,
                          void *data, size_t size);
int a2_disk_image_write_at(a2_disk_image *image, uint32_t offset,
                           const void *data, size_t size);
int a2_disk_image_sync(a2_disk_image *image);
int a2_disk_image_close(a2_disk_image *image);

#ifdef __cplusplus
}
#endif
#endif
