// SPDX-License-Identifier: MIT
// Original parser from Apple's The DOS Manual, AppendixC,pp129-133:
// https://device.report/m/c08d8e894bc01bf74e7348df79c1e3b2360f43ada6e2b08e39f9686651575161
#include "a2_dos_catalog.h"
#include <string.h>

static int failed(a2_dos_catalog *catalog, int error) {
    catalog->count = 0; catalog->truncated = false;
    return error;
}
static bool pointer(unsigned track, unsigned sector) {
    return track > 0 && track < 35 && sector < 16;
}
static bool name_decode(const uint8_t source[30], char name[A2_DOS_NAME_BYTES]) {
    unsigned size = 30;
    while (size && (source[size-1] & 0x7f) == ' ') --size;
    if (!size || (source[0] & 0x7f) == ' ') return false;
    for (unsigned i = 0; i < size; ++i) {
        uint8_t c = source[i] & 0x7f;
        if (c < 32 || c >= 127 || c == ',' || c == ':' || c == '"') return false;
        name[i] = (char)c;
    }
    name[size] = 0;
    return true;
}
int a2_dos_catalog_read(a2_dos_track_reader reader, void *context,
                        a2_dos_catalog *catalog, uint8_t scratch[4096]) {
    if (!catalog) return A2_DOS_ERR_INVAL;
    memset(catalog, 0, sizeof(*catalog));
    if (!reader || !scratch) return A2_DOS_ERR_INVAL;
    if (!reader(context, 17, scratch)) return A2_DOS_ERR_IO;
    // Standard DOS3.3 geometry. No allocation bitmap or file contents are read.
    if (scratch[3] != 3 || scratch[0x27] != 122 || scratch[0x34] != 35
            || scratch[0x35] != 16 || scratch[0x36] != 0 || scratch[0x37] != 1)
        return A2_DOS_ERR_CORRUPT;
    unsigned track = scratch[1], sector = scratch[2], loaded_track = 17;
    uint8_t visited[70] = {0}; // One bit for each possible physical sector.
    for (unsigned links = 0; track || sector; ++links) {
        if (!pointer(track, sector) || (track == 17 && sector == 0) || links >= 560)
            return failed(catalog, A2_DOS_ERR_CORRUPT);
        unsigned location = track*16 + sector;
        if (visited[location/8] & (1u << (location%8)))
            return failed(catalog, A2_DOS_ERR_CORRUPT);
        visited[location/8] |= (uint8_t)(1u << (location%8));
        if (loaded_track != track) {
            if (!reader(context, track, scratch)) return failed(catalog, A2_DOS_ERR_IO);
            loaded_track = track;
        }
        const uint8_t *page = scratch + sector*256;
        unsigned next_track = page[1], next_sector = page[2];
        for (unsigned i = 0; i < 7; ++i) {
            const uint8_t *entry = page + 11 + i*35;
            if (!entry[0] || entry[0] == 0xff) continue; // Unused/deleted.
            if (!pointer(entry[0], entry[1])) return failed(catalog, A2_DOS_ERR_CORRUPT);
            uint8_t type = entry[2] & 0x7f;
            if (type != 1 && type != 2) continue;
            a2_dos_program program = {.type = type, .locked = (entry[2] & 0x80) != 0};
            if (!name_decode(entry+3, program.name)) continue;
            if (catalog->count == A2_DOS_MAX_PROGRAMS) catalog->truncated = true;
            else catalog->programs[catalog->count++] = program;
        }
        track = next_track; sector = next_sector;
    }
    return 0;
}
