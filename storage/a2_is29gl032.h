// SPDX-License-Identifier: MIT
#ifndef A2_IS29GL032_H
#define A2_IS29GL032_H
#include "a2_storage.h"

// The callbacks implement cartridge bus cycles, not flash command algorithms.
// select_bank maps bank 0..255 at CPU 4000..7fff; 0000..3fff remains bank zero.
// raw_write uses the cartridge WR pin and passes CPU addresses unchanged:
// commands use 0000:F0,00AA:98,0555:55,0AAA:AA/90/A0/80; data/erase addresses
// use 4000..7fff, as do buffer 25/count/data/29 cycles. Do not translate low
// command addresses into ROM_WINDOW. The optional raw_write_block callback
// executes 1..32 sequential raw WR cycles entirely within 4000..7fff. It is used
// only for loaded buffer data; a failed batch is never retried or replayed.
// All callbacks must complete their own
// bounded transport I/O before returning. Serialize all access to this mapper.
typedef struct {
    void *context;
    int (*select_bank)(void *context, uint16_t bank);
    int (*read)(void *context, uint16_t cpu_address, void *data, size_t size);
    int (*raw_write)(void *context, uint16_t cpu_address, uint8_t data);
    int (*sync)(void *context);
    uint64_t (*now_ms)(void *context);
    void (*wait_ms)(void *context, uint32_t milliseconds);
    int (*raw_write_block)(void *context, uint16_t cpu_address,
                            const void *data, size_t size);
} a2_is29gl032_bus;

typedef struct {
    uint8_t manufacturer, device1, device2, device3;
    uint32_t bytes, sector_bytes, sectors;
} a2_is29gl032_info;

typedef struct {
    a2_is29gl032_bus bus;
    a2_is29gl032_info info;
    uint32_t program_timeout_ms, erase_timeout_ms;
    bool identified, writes_allowed, requires_reset;
} a2_is29gl032;

#define A2_FLASH_ERR_TIMEOUT (-110)
#define A2_FLASH_ERR_READONLY (-30)

// No bus operations occur in init. Timeouts bound each byte/buffer program and sector
// erase; the caller must also bound the callbacks' transport operations.
int a2_is29gl032_init(a2_is29gl032 *flash, const a2_is29gl032_bus *bus,
                      uint32_t program_timeout_ms, uint32_t erase_timeout_ms);
// Establish a hardware-reset/known-idle cartridge before first probe. Probe
// issues only reset, identification and CFI query commands; no array data
// is programmed/erased. It checks exact ISSI ID and 4 MiB uniform CFI geometry.
// It always revokes write permission first. WP#/ASP protection remains unchanged.
// Following an operation failure, hardware-reset the cartridge and init again;
// probe refuses while requires_reset is set, without issuing further commands.
int a2_is29gl032_probe(a2_is29gl032 *flash, a2_is29gl032_info *info);
// A separate deliberate step, permitted only after a successful exact probe.
// This does not inspect or change WP#/ASP protection; refused operations fail.
int a2_is29gl032_allow_writes(a2_is29gl032 *flash);
void a2_is29gl032_disallow_writes(a2_is29gl032 *flash);
// Exposes the LittleFS backend only after identification. Its program/erase
// callbacks additionally require allow_writes; reads work while write-disabled.
int a2_is29gl032_backend(a2_is29gl032 *flash, a2_storage_backend *backend);

#endif
