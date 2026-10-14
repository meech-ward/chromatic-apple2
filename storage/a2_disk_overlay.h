// SPDX-License-Identifier: MIT
#ifndef A2_DISK_OVERLAY_H
#define A2_DISK_OVERLAY_H
#include "a2_storage.h"

#define A2_OVERLAY_SECTORS 560u
#define A2_OVERLAY_TRACK_BYTES 4096u
#define A2_OVERLAY_LIMIT (256u * 1024u)
#define A2_OVERLAY_PATH_BYTES 144u
#define A2_OVERLAY_RECORD_BYTES 268u

typedef enum {
    A2_OVERLAY_CLOSED, A2_OVERLAY_SCAN, A2_OVERLAY_READY,
    A2_OVERLAY_WRITE, A2_OVERLAY_COMPACT, A2_OVERLAY_RESET, A2_OVERLAY_ERROR
} a2_overlay_state;

// Zero-initialize and keep at a stable address. Serialize all overlay/base/LFS
// access. The caller retains a READONLY base handle and computes its actual
// full SHA256 before open; replacing an imported base requires a fresh handle,
// fresh hash and overlay open. No ROM/disk contents or crypto code are included.
typedef struct {
    a2_disk_image *base;
    lfs_file_t file;
    struct lfs_file_config config;
    uint8_t cache[A2_STORAGE_CACHE_BYTES];
    uint32_t latest[A2_OVERLAY_SECTORS];
    uint8_t hash[32], record[A2_OVERLAY_RECORD_BYTES], compare[256];
    char path[A2_OVERLAY_PATH_BYTES], temporary[A2_OVERLAY_PATH_BYTES];
    uint32_t journal_bytes, committed_bytes;
    unsigned progress, total, last_changed_sectors;
    int error;
    a2_overlay_state state;
    bool opened, recovered_tail;
} a2_disk_overlay;

// Safe ASCII basename ending .dsk, at most63 characters; no slashes or '..'.
// Creates a journal if absent; never changes/formats the base or filesystem.
// Complete corrupt records/header fail with CORRUPT. An incomplete trailing
// group is ignored, reported in recovered_tail and removed atomically before
// the next write. Reads still expose the last fully committed track groups.
int a2_disk_overlay_open(a2_disk_overlay *overlay, a2_disk_image *readonly_base,
                         const char *basename, const uint8_t base_sha256[32]);
int a2_disk_overlay_read_track(a2_disk_overlay *overlay, unsigned track,
                               uint8_t data[A2_OVERLAY_TRACK_BYTES]);
// Compare first, append only changed sectors, then sync before publishing the
// new index. A failed/ambiguous IO poisons this handle: no automatic retry or
// recovery writes. Reboot/reset the physical backend as required, then reopen.
// After an ambiguous commit, a reopened journal may contain old or new data.
int a2_disk_overlay_write_track(a2_disk_overlay *overlay, unsigned track,
                                const uint8_t data[A2_OVERLAY_TRACK_BYTES]);
int a2_disk_overlay_sync(a2_disk_overlay *overlay);
int a2_disk_overlay_close(a2_disk_overlay *overlay);
// These replace only the matching journal through temp+sync+atomic rename.
// reset_journal deliberately discards saved changes, retaining the base.
int a2_disk_overlay_compact(a2_disk_overlay *overlay);
int a2_disk_overlay_reset_journal(a2_disk_overlay *overlay);
#endif
