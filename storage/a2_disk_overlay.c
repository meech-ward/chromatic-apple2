// SPDX-License-Identifier: MIT
// Original sector journal over the pinned, unmodified BSD3 LittleFS library.
#include "a2_disk_overlay.h"
#include <stdio.h>
#include <string.h>

#define HEADER_BYTES 64u
#define NONE UINT32_MAX
#define COMMIT_SECTOR UINT16_MAX

static uint32_t crc_update(uint32_t crc, const void *data, size_t size) {
    const uint8_t *p = data;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}
static uint32_t crc(const void *data, size_t size) {
    return ~crc_update(UINT32_MAX, data, size);
}
static void put16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8 * i));
}
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static bool alnum_ascii(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
static bool safe_name(const char *p) {
    if (!p) return false;
    size_t n = 0;
    while (n <= 63 && p[n]) ++n;
    if (n < 5 || n > 63 || !alnum_ascii(p[0])) return false;
    for (size_t i = 1; i < n; ++i)
        if (!(alnum_ascii(p[i]) || p[i] == ' ' || p[i] == '_' || p[i] == '-' || p[i] == '.')
                || (p[i] == '.' && p[i-1] == '.')) return false;
    return p[n-4] == '.' && (p[n-3] == 'd' || p[n-3] == 'D')
        && (p[n-2] == 's' || p[n-2] == 'S') && (p[n-1] == 'k' || p[n-1] == 'K');
}
static int fail(a2_disk_overlay *o, int error) {
    o->error = error; o->state = A2_OVERLAY_ERROR;
    return error;
}
static int usable(a2_disk_overlay *o) {
    if (!o || !o->opened || !o->base || !o->base->opened) return LFS_ERR_BADF;
    return o->error;
}
static int read_exact(a2_disk_overlay *o, lfs_file_t *file, void *data, size_t size) {
    lfs_ssize_t n = lfs_file_read(&o->base->storage->lfs, file, data, (lfs_size_t)size);
    return n == (lfs_ssize_t)size ? 0 : n < 0 ? (int)n : LFS_ERR_CORRUPT;
}
static int write_exact(a2_disk_overlay *o, lfs_file_t *file, const void *data, size_t size) {
    lfs_ssize_t n = lfs_file_write(&o->base->storage->lfs, file, data, (lfs_size_t)size);
    return n == (lfs_ssize_t)size ? 0 : n < 0 ? (int)n : LFS_ERR_IO;
}
static int seek_file(a2_disk_overlay *o, lfs_file_t *file, uint32_t offset) {
    lfs_soff_t n = lfs_file_seek(&o->base->storage->lfs, file, (lfs_soff_t)offset, LFS_SEEK_SET);
    return n == (lfs_soff_t)offset ? 0 : n < 0 ? (int)n : LFS_ERR_IO;
}
static int open_file(a2_disk_overlay *o, lfs_file_t *file, const char *path,
                      int flags, struct lfs_file_config *config) {
    int error = lfs_file_opencfg(&o->base->storage->lfs, file, path, flags, config);
    if (!error) o->base->storage->open_images++;
    return error;
}
static int close_file(a2_disk_overlay *o, lfs_file_t *file, bool discard) {
    // Pinned LittleFS's public LFS_F_ERRED flag makes close release its handle
    // without retrying sync after ambiguous IO. Never write an abort record.
    if (discard) file->flags |= LFS_F_ERRED;
    int error = lfs_file_close(&o->base->storage->lfs, file);
    o->base->storage->open_images--;
    return error;
}
static void make_header(a2_disk_overlay *o, uint8_t data[HEADER_BYTES]) {
    memset(data, 0, HEADER_BYTES);
    memcpy(data, "A2OVL001", 8); memcpy(data + 8, o->hash, 32);
    put32(data + 40, A2_DISK_IMAGE_BYTES); put32(data + 44, A2_OVERLAY_RECORD_BYTES);
    put32(data + 60, crc(data, 60));
}
static void make_record(a2_disk_overlay *o, unsigned sector, const uint8_t *data) {
    memcpy(o->record, "A2S1", 4); put16(o->record + 4, sector); put16(o->record + 6, 0);
    memcpy(o->record + 8, data, 256);
    put32(o->record + 264, crc(o->record, 264));
}
static bool valid_record(a2_disk_overlay *o) {
    return !memcmp(o->record, "A2S1", 4) && !get16(o->record + 6)
        && get32(o->record + 264) == crc(o->record, 264);
}
static void make_commit(a2_disk_overlay *o, unsigned track, unsigned count, uint32_t group_crc) {
    uint8_t payload[256] = {0};
    put16(payload, count); put16(payload + 2, track); put32(payload + 4, ~group_crc);
    make_record(o, COMMIT_SECTOR, payload);
}
static int scan(a2_disk_overlay *o) {
    o->state = A2_OVERLAY_SCAN; o->progress = 0;
    memset(o->latest, 0xff, sizeof(o->latest));
    lfs_soff_t size = lfs_file_size(&o->base->storage->lfs, &o->file);
    if (size < 0) return (int)size;
    if ((uint32_t)size < HEADER_BYTES || (uint32_t)size > A2_OVERLAY_LIMIT) return LFS_ERR_CORRUPT;
    uint8_t expected[HEADER_BYTES], actual[HEADER_BYTES];
    make_header(o, expected);
    int error = seek_file(o, &o->file, 0);
    if (!error) error = read_exact(o, &o->file, actual, sizeof(actual));
    if (!error && memcmp(expected, actual, sizeof(actual))) error = LFS_ERR_CORRUPT;
    if (error) return error;
    uint32_t pending[16], group_crc = UINT32_MAX;
    unsigned pending_count = 0, track = 0, mask = 0;
    o->committed_bytes = HEADER_BYTES;
    o->total = ((uint32_t)size - HEADER_BYTES) / A2_OVERLAY_RECORD_BYTES;
    for (uint32_t offset = HEADER_BYTES;
            offset + A2_OVERLAY_RECORD_BYTES <= (uint32_t)size; offset += A2_OVERLAY_RECORD_BYTES) {
        error = read_exact(o, &o->file, o->record, sizeof(o->record));
        if (error) return error;
        if (!valid_record(o)) return LFS_ERR_CORRUPT;
        unsigned sector = get16(o->record + 4);
        if (sector == COMMIT_SECTOR) {
            if (!pending_count || get16(o->record + 8) != pending_count
                    || get16(o->record + 10) != track || get32(o->record + 12) != ~group_crc)
                return LFS_ERR_CORRUPT;
            for (unsigned i = 0; i < pending_count; ++i) {
                // The sector number is held in the low4 bits: offsets are
                // recovered separately from the sequential group's record list.
                unsigned s = pending[i] & 15u;
                o->latest[track * 16 + s] = pending[i] >> 4;
            }
            pending_count = 0; mask = 0; group_crc = UINT32_MAX;
            o->committed_bytes = offset + A2_OVERLAY_RECORD_BYTES;
        } else {
            if (sector >= A2_OVERLAY_SECTORS || pending_count == 16
                    || (pending_count && sector / 16 != track) || (mask & (1u << (sector % 16))))
                return LFS_ERR_CORRUPT;
            track = sector / 16; mask |= 1u << (sector % 16);
            pending[pending_count++] = (offset << 4) | (sector % 16);
            group_crc = crc_update(group_crc, o->record, sizeof(o->record));
        }
        o->progress++;
    }
    o->journal_bytes = (uint32_t)size;
    o->recovered_tail = o->committed_bytes != (uint32_t)size;
    return 0;
}

int a2_disk_overlay_open(a2_disk_overlay *o, a2_disk_image *base,
                         const char *name, const uint8_t hash[32]) {
    if (!o || o->opened || !base || !base->opened || base->writable || !hash || !safe_name(name))
        return LFS_ERR_INVAL;
    memset(o, 0, sizeof(*o)); o->base = base; memcpy(o->hash, hash, 32);
    char hex[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(hex + 2*i, 3, "%02x", hash[i]);
    int length = snprintf(o->path, sizeof(o->path), "/%s.%s.a2j", name, hex);
    if (length < 0 || (size_t)length + 5 > sizeof(o->temporary)) return fail(o, LFS_ERR_INVAL);
    memcpy(o->temporary, o->path, (size_t)length);
    memcpy(o->temporary + length, ".tmp", 5);
    o->config.buffer = o->cache;
    bool created = true;
    int error = open_file(o, &o->file, o->path, LFS_O_RDWR | LFS_O_CREAT | LFS_O_EXCL, &o->config);
    if (error == LFS_ERR_EXIST) {
        created = false;
        error = open_file(o, &o->file, o->path, LFS_O_RDWR, &o->config);
    }
    if (error) return fail(o, error);
    o->opened = true;
    lfs_soff_t size = lfs_file_size(&base->storage->lfs, &o->file);
    if (created && size == 0) {
        uint8_t header[HEADER_BYTES]; make_header(o, header);
        error = write_exact(o, &o->file, header, sizeof(header));
        if (!error) error = lfs_file_sync(&base->storage->lfs, &o->file);
    } else if (size < 0) error = (int)size;
    if (!error) error = scan(o);
    if (error) {
        (void)close_file(o, &o->file, true); o->opened = false;
        return fail(o, error);
    }
    o->state = A2_OVERLAY_READY;
    return 0;
}
static int read_sector(a2_disk_overlay *o, unsigned sector, uint8_t data[256]) {
    if (o->latest[sector] == NONE)
        return a2_disk_image_read_at(o->base, sector * 256u, data, 256);
    int error = seek_file(o, &o->file, o->latest[sector]);
    if (!error) error = read_exact(o, &o->file, o->record, sizeof(o->record));
    if (!error && (!valid_record(o) || get16(o->record + 4) != sector)) error = LFS_ERR_CORRUPT;
    if (!error) memcpy(data, o->record + 8, 256);
    return error;
}
int a2_disk_overlay_read_track(a2_disk_overlay *o, unsigned track, uint8_t data[4096]) {
    int error = usable(o);
    if (error) return error;
    if (track >= 35 || !data) return LFS_ERR_INVAL;
    for (unsigned i = 0; i < 16 && !error; ++i) error = read_sector(o, track*16+i, data+i*256);
    return error ? fail(o, error) : 0;
}

static int replace_journal(a2_disk_overlay *o, bool empty) {
    int error = usable(o);
    if (error) return error;
    o->state = empty ? A2_OVERLAY_RESET : A2_OVERLAY_COMPACT;
    o->progress = 0; o->total = empty ? 0 : A2_OVERLAY_SECTORS;
    lfs_file_t temporary;
    uint8_t cache[A2_STORAGE_CACHE_BYTES];
    struct lfs_file_config config = {.buffer = cache};
    error = open_file(o, &temporary, o->temporary, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC, &config);
    if (error) return fail(o, error);
    uint8_t header[HEADER_BYTES]; make_header(o, header);
    error = write_exact(o, &temporary, header, sizeof(header));
    for (unsigned track = 0; !empty && track < 35 && !error; ++track) {
        unsigned count = 0; uint32_t group_crc = UINT32_MAX;
        for (unsigned s = 0; s < 16 && !error; ++s) {
            unsigned sector = track*16+s;
            if (o->latest[sector] != NONE) {
                error = read_sector(o, sector, o->compare);
                if (!error) {
                    make_record(o, sector, o->compare);
                    group_crc = crc_update(group_crc, o->record, sizeof(o->record));
                    error = write_exact(o, &temporary, o->record, sizeof(o->record));
                    ++count;
                }
            }
            o->progress++;
        }
        if (!error && count) {
            make_commit(o, track, count, group_crc);
            error = write_exact(o, &temporary, o->record, sizeof(o->record));
        }
    }
    if (!error) error = lfs_file_sync(&o->base->storage->lfs, &temporary);
    int close_error = close_file(o, &temporary, error != 0);
    if (!error) error = close_error;
    if (error) return fail(o, error); // Leave old journal and scratch untouched.
    error = close_file(o, &o->file, false); o->opened = false;
    if (!error) error = lfs_rename(&o->base->storage->lfs, o->temporary, o->path);
    if (error) return fail(o, error);
    error = open_file(o, &o->file, o->path, LFS_O_RDWR, &o->config);
    if (error) return fail(o, error);
    o->opened = true;
    error = scan(o);
    if (error) return fail(o, error);
    o->state = A2_OVERLAY_READY;
    return 0;
}
int a2_disk_overlay_compact(a2_disk_overlay *o) { return replace_journal(o, false); }
int a2_disk_overlay_reset_journal(a2_disk_overlay *o) { return replace_journal(o, true); }

int a2_disk_overlay_write_track(a2_disk_overlay *o, unsigned track, const uint8_t data[4096]) {
    int error = usable(o);
    if (error) return error;
    if (track >= 35 || !data) return LFS_ERR_INVAL;
    unsigned changed[16], count = 0;
    for (unsigned s = 0; s < 16; ++s) {
        error = read_sector(o, track*16+s, o->compare);
        if (error) return fail(o, error);
        if (memcmp(o->compare, data+s*256, 256)) changed[count++] = s;
    }
    o->last_changed_sectors = count;
    if (!count) return 0;
    uint32_t additional = (count+1)*A2_OVERLAY_RECORD_BYTES;
    if (o->recovered_tail || o->journal_bytes + additional > A2_OVERLAY_LIMIT) {
        error = a2_disk_overlay_compact(o);
        if (error) return error;
    }
    if (o->journal_bytes + additional > A2_OVERLAY_LIMIT) return fail(o, LFS_ERR_NOSPC);
    error = seek_file(o, &o->file, o->journal_bytes);
    o->state = A2_OVERLAY_WRITE; o->progress = 0; o->total = count;
    uint32_t group_crc = UINT32_MAX, start = o->journal_bytes;
    for (unsigned i = 0; i < count && !error; ++i) {
        make_record(o, track*16+changed[i], data+changed[i]*256);
        group_crc = crc_update(group_crc, o->record, sizeof(o->record));
        error = write_exact(o, &o->file, o->record, sizeof(o->record));
        o->progress++;
    }
    if (!error) {
        make_commit(o, track, count, group_crc);
        error = write_exact(o, &o->file, o->record, sizeof(o->record));
    }
    if (!error) error = lfs_file_sync(&o->base->storage->lfs, &o->file);
    if (error) return fail(o, error);
    for (unsigned i = 0; i < count; ++i) o->latest[track*16+changed[i]] = start+i*A2_OVERLAY_RECORD_BYTES;
    o->journal_bytes += additional; o->committed_bytes = o->journal_bytes;
    o->state = A2_OVERLAY_READY;
    return 0;
}
int a2_disk_overlay_sync(a2_disk_overlay *o) {
    int error = usable(o);
    if (!error) error = lfs_file_sync(&o->base->storage->lfs, &o->file);
    return error && o && o->opened ? fail(o, error) : error;
}
int a2_disk_overlay_close(a2_disk_overlay *o) {
    if (!o || !o->opened) return LFS_ERR_BADF;
    int error = o->error;
    int close_error = close_file(o, &o->file, error != 0);
    o->opened = false; o->state = A2_OVERLAY_CLOSED;
    return error ? error : close_error;
}
