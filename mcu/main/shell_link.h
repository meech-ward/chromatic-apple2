/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SHELL_MAX_READ = 32, SHELL_MAX_WRITE = 32,
       SHELL_MAX_PAYLOAD = 4 + SHELL_MAX_READ,
       SHELL_MAX_FRAME = SHELL_MAX_PAYLOAD + 4,
       SHELL_MAX_REQUEST_FRAME = 9 + SHELL_MAX_WRITE };
typedef struct {
    uint8_t channel, length;
    uint8_t payload[SHELL_MAX_PAYLOAD];
} shell_frame_t;
typedef struct {
    uint8_t bytes[SHELL_MAX_FRAME];
    uint8_t used, expected;
} shell_parser_t;

/* Portable framing helpers also used by the host rejection tests. */
uint8_t shell_protocol_crc(const uint8_t *bytes, size_t length);
bool shell_protocol_request_valid(uint8_t operation, uint16_t address, uint8_t value);
bool shell_protocol_encode(uint8_t token, uint8_t operation, uint16_t address,
                           uint8_t value, uint8_t output[9]);
/* Returns frame size, or zero without writing output. Op7 needs value bytes of
 * data; fixed requests take no data. The fixed nine-byte helper rejects op7. */
size_t shell_protocol_encode_packet(uint8_t token, uint8_t operation,
                                    uint16_t address, uint8_t value,
                                    const void *data, size_t data_size,
                                    uint8_t *output, size_t capacity);
void shell_protocol_parser_reset(shell_parser_t *parser);
bool shell_protocol_feed(shell_parser_t *parser, uint8_t byte, shell_frame_t *frame);
bool shell_protocol_matches(const shell_frame_t *frame, uint8_t token,
                            uint16_t address, size_t expected_data);
bool shell_protocol_write_matches(const shell_frame_t *frame, uint8_t token,
                                  uint16_t address, size_t count);

void shell_link_init(void);
bool shell_link_read(uint16_t address, uint8_t *value);
bool shell_link_read_block(uint16_t address, uint8_t *output, size_t count);
bool shell_link_mapper_write(uint16_t address, uint8_t value);
bool shell_link_ram_write(uint16_t address, uint8_t value);
bool shell_link_diag(uint16_t address, uint8_t *value);
bool shell_link_release(void);
bool shell_link_flash_command(uint16_t address, uint8_t value);
/* Raw data phase only, not a standalone flash program operation. The complete
 * 1..32 byte range must lie within $4000..$7FFF. Never retries a failed write. */
bool shell_link_flash_write_block(uint16_t address, const void *data, size_t count);
bool shell_link_audio_arm(bool enable);
/* Error storage is per task. A complete wire request is serialized internally;
 * callers must serialize multi-request mapper/bank transactions themselves. */
const char *shell_link_error(void);
