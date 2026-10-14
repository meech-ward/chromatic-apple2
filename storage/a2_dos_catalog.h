// SPDX-License-Identifier: MIT
#ifndef A2_DOS_CATALOG_H
#define A2_DOS_CATALOG_H
#include <stdbool.h>
#include <stdint.h>

enum { A2_DOS_MAX_PROGRAMS = 64, A2_DOS_NAME_BYTES = 31 };
enum { A2_DOS_ERR_INVAL = -22, A2_DOS_ERR_IO = -5, A2_DOS_ERR_CORRUPT = -84 };
typedef struct { char name[A2_DOS_NAME_BYTES]; uint8_t type; bool locked; } a2_dos_program;
typedef struct {
    a2_dos_program programs[A2_DOS_MAX_PROGRAMS];
    unsigned count;
    bool truncated;
} a2_dos_catalog;
typedef bool (*a2_dos_track_reader)(void *context, unsigned track, uint8_t output[4096]);

// Read-only DOS3.3,35-track,16-sector,256-byte-sector catalog reader.
// Track data uses DOS logical sector order, matching the .dsk/DiskII backend.
// Returns0 on success; on any error count/truncated are cleared. Live Integer
// BASIC(type1) and Applesoft(type2) entries only; lockbit is separate. Names
// lose their highbits and trailing spaces. Empty/leading-space names, controls,
// DEL, comma, colon and doublequote are omitted, so callers can issue RUN name.
// The caller supplies a distinct4096-byte scratch buffer; no heap/large stack.
int a2_dos_catalog_read(a2_dos_track_reader reader, void *context,
                        a2_dos_catalog *catalog, uint8_t scratch[4096]);
#endif
