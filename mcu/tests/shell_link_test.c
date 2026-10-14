/* SPDX-License-Identifier: MIT */
#include "shell_link.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t response(uint8_t *bytes, uint8_t token, uint8_t status,
                       uint16_t address, size_t count)
{
    const size_t length = status ? 5 : 4 + count;
    bytes[0] = 0x8f; bytes[1] = 0x0a; bytes[2] = length;
    bytes[3] = token; bytes[4] = status;
    bytes[5] = address >> 8; bytes[6] = address & 255;
    for (size_t i = 4; i < length; ++i) bytes[3 + i] = (uint8_t)(i * 13);
    bytes[3 + length] = shell_protocol_crc(bytes, 3 + length);
    return length + 4;
}

static bool feed(shell_parser_t *parser, const uint8_t *bytes, size_t count,
                 shell_frame_t *frame)
{
    bool accepted = false;
    for (size_t i = 0; i < count; ++i) {
        if (shell_protocol_feed(parser, bytes[i], frame)) accepted = true;
        assert(parser->used < SHELL_MAX_FRAME);
    }
    return accepted;
}

static void write_bursts(void)
{
    assert(shell_protocol_request_valid(7, 0x4000, 32));
    assert(shell_protocol_request_valid(7, 0x7fe0, 32));
    assert(shell_protocol_request_valid(7, 0x7fff, 1));
    assert(!shell_protocol_request_valid(7, 0x3fff, 1));
    assert(!shell_protocol_request_valid(7, 0x8000, 1));
    assert(!shell_protocol_request_valid(7, 0x7fe1, 32));
    assert(!shell_protocol_request_valid(7, 0xa000, 1));
    assert(!shell_protocol_request_valid(7, 0x4000, 0));
    assert(!shell_protocol_request_valid(7, 0x4000, 33));

    uint8_t data[32], packet[SHELL_MAX_REQUEST_FRAME + 2], ack[SHELL_MAX_FRAME];
    uint8_t fixed[9];
    memset(fixed, 0x5b, sizeof(fixed));
    assert(!shell_protocol_encode(7, 7, 0x4000, 1, fixed));
    for (unsigned i = 0; i < sizeof(fixed); ++i) assert(fixed[i] == 0x5b);
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 0x8f);
    for (size_t count = 1; count <= 32; ++count) {
        memset(packet, 0x5b, sizeof(packet));
        const uint16_t address = (uint16_t)(0x8000 - count);
        const size_t length = shell_protocol_encode_packet(0x57, 7, address,
                                  count, data, count, packet + 1, 9 + count);
        assert(length == 9 + count && packet[0] == 0x5b && packet[1 + length] == 0x5b);
        assert(packet[1] == 0x8f && packet[2] == 0x20 && packet[3] == 5 + count);
        assert(packet[4] == 0x57 && packet[5] == 7 && packet[6] == 0x7f);
        assert(packet[7] == (uint8_t)address && packet[8] == count);
        assert(!memcmp(packet + 9, data, count));
        assert(shell_protocol_crc(packet + 1, length) == 0);
        memset(packet, 0x5b, sizeof(packet));
        assert(!shell_protocol_encode_packet(1, 7, address, count, data, count,
                                              packet + 1, 8 + count));
        for (unsigned i = 0; i < sizeof(packet); ++i) assert(packet[i] == 0x5b);
        assert(!shell_protocol_encode_packet(1, 7, address, count, NULL, count,
                                              packet, sizeof(packet)));
        assert(!shell_protocol_encode_packet(1, 7, address, count, data, count - 1,
                                              packet, sizeof(packet)));
        assert(!shell_protocol_encode_packet(1, 7, address, count, data, SIZE_MAX,
                                              packet, sizeof(packet)));

        for (uint8_t status = 0; status <= 5; ++status) {
            const size_t ack_size = response(ack, 0x57, status, address, 1);
            ack[7] = count;
            ack[8] = shell_protocol_crc(ack, 8);
            assert(ack_size == 9);
            for (size_t split = 1; split < ack_size; ++split) {
                shell_parser_t parser = {0};
                shell_frame_t frame;
                assert(!feed(&parser, ack, split, &frame));
                assert(feed(&parser, ack + split, ack_size - split, &frame));
                assert(shell_protocol_write_matches(&frame, 0x57, address, count));
                assert(!shell_protocol_write_matches(&frame, 0x58, address, count));
                assert(!shell_protocol_write_matches(&frame, 0x57, address ^ 1, count));
                assert(!shell_protocol_write_matches(&frame, 0x57, address, count == 32 ? 31 : count + 1));
                assert(!shell_protocol_write_matches(&frame, 0x57, address, 0));
                assert(!shell_protocol_write_matches(&frame, 0x57, address, 33));
                frame.length = 6;
                assert(!shell_protocol_write_matches(&frame, 0x57, address, count));
            }
            shell_parser_t parser = {0};
            shell_frame_t frame;
            ack[8] ^= 1;
            assert(!feed(&parser, ack, ack_size, &frame));
            ack[8] ^= 1;
            shell_protocol_parser_reset(&parser);
            assert(!feed(&parser, ack, ack_size - 1, &frame));
        }
    }
    assert(!shell_protocol_encode_packet(1, 7, 0x7fff, 2, data, 2, packet, sizeof(packet)));
    assert(!shell_protocol_encode_packet(1, 6, 0x4000, 2, data, 2, packet, sizeof(packet)));
    assert(shell_protocol_encode_packet(1, 6, 0x4000, 0xff, NULL, 0, packet, sizeof(packet)) == 9);
}

int main(void)
{
    write_bursts();
    assert(shell_protocol_crc((const uint8_t *)"123456789", 9) == 0xb4);
    assert(shell_protocol_request_valid(0, 0x7fff, 0));
    assert(shell_protocol_request_valid(0, 0xa000, 0));
    assert(!shell_protocol_request_valid(0, 0x8000, 0));
    assert(!shell_protocol_request_valid(0, 0x9fff, 0));
    assert(!shell_protocol_request_valid(0, 0xc000, 0));
    assert(!shell_protocol_request_valid(0, 0, 1));
    assert(shell_protocol_request_valid(1, 0, 0));
    assert(shell_protocol_request_valid(1, 0, 0x0a));
    assert(!shell_protocol_request_valid(1, 0, 1));
    assert(shell_protocol_request_valid(1, 0x2000, 255));
    assert(shell_protocol_request_valid(1, 0x3000, 0));
    assert(!shell_protocol_request_valid(1, 0x3000, 1));
    assert(shell_protocol_request_valid(1, 0x4000, 3));
    assert(!shell_protocol_request_valid(1, 0x4000, 4));
    assert(!shell_protocol_request_valid(1, 0xa000, 0));
    assert(shell_protocol_request_valid(4, 0xa000, 0xff));
    assert(shell_protocol_request_valid(4, 0xbfff, 0));
    assert(!shell_protocol_request_valid(4, 0x7fff, 0));
    assert(!shell_protocol_request_valid(4, 0x9fff, 0));
    assert(!shell_protocol_request_valid(4, 0xc000, 0));
    assert(shell_protocol_request_valid(5, 0x7fe0, 32));
    assert(shell_protocol_request_valid(5, 0x7fff, 1));
    assert(!shell_protocol_request_valid(5, 0x7fe1, 32));
    assert(shell_protocol_request_valid(5, 0xbfe0, 32));
    assert(shell_protocol_request_valid(5, 0xbfff, 1));
    assert(!shell_protocol_request_valid(5, 0xbfe1, 32));
    assert(!shell_protocol_request_valid(5, 0xa000, 0));
    assert(!shell_protocol_request_valid(5, 0xa000, 33));
    assert(!shell_protocol_request_valid(5, 0xffff, 32));
    assert(!shell_protocol_request_valid(6, 0xa000, 1));
    assert(!shell_protocol_request_valid(2, 1, 0));
    assert(!shell_protocol_request_valid(2, 0, 1));
    assert(shell_protocol_request_valid(3, 4, 0));
    assert(shell_protocol_request_valid(3, 14, 0));
    assert(shell_protocol_request_valid(3, 15, 0));
    assert(shell_protocol_request_valid(3, 32, 0));
    assert(shell_protocol_request_valid(3, 33, 0));
    assert(!shell_protocol_request_valid(3, 34, 0));
    assert(!shell_protocol_request_valid(3, 33, 1));
    assert(shell_protocol_request_valid(8, 0, 0));
    assert(shell_protocol_request_valid(8, 0, 1));
    assert(!shell_protocol_request_valid(8, 1, 1));
    assert(!shell_protocol_request_valid(8, 0, 2));
    assert(shell_protocol_request_valid(6, 0, 0xf0));
    assert(shell_protocol_request_valid(6, 0xaa, 0x98));
    assert(shell_protocol_request_valid(6, 0x555, 0x55));
    assert(shell_protocol_request_valid(6, 0xaaa, 0xa0));
    assert(shell_protocol_request_valid(6, 0x4000, 0x30));
    assert(shell_protocol_request_valid(6, 0x7fff, 0xff));
    assert(!shell_protocol_request_valid(6, 0, 0));
    assert(!shell_protocol_request_valid(6, 0x555, 0xaa));
    assert(!shell_protocol_request_valid(6, 0xaaa, 0xff));
    assert(!shell_protocol_request_valid(6, 0x3fff, 0x30));

    uint8_t tx[9];
    assert(shell_protocol_encode(0x57, 5, 0x7fe0, 32, tx));
    assert(tx[0] == 0x8f && tx[1] == 0x20 && tx[2] == 5 && tx[3] == 0x57);
    assert(tx[4] == 5 && tx[5] == 0x7f && tx[6] == 0xe0 && tx[7] == 32);
    assert(shell_protocol_crc(tx, sizeof(tx)) == 0);
    assert(!shell_protocol_encode(1, 4, 0, 0, tx));

    shell_parser_t parser = {0};
    shell_frame_t frame;
    uint8_t bytes[SHELL_MAX_FRAME];
    for (size_t count = 1; count <= 32; ++count) {
        const size_t size = response(bytes, 0x57, 0, 0x7fe0, count);
        for (size_t split = 1; split < size; ++split) {
            shell_protocol_parser_reset(&parser);
            assert(!feed(&parser, bytes, split, &frame));
            assert(feed(&parser, bytes + split, size - split, &frame));
            assert(shell_protocol_matches(&frame, 0x57, 0x7fe0, count));
            assert(!shell_protocol_matches(&frame, 0x58, 0x7fe0, count));
            assert(!shell_protocol_matches(&frame, 0x57, 0x7fe1, count));
            assert(!shell_protocol_matches(&frame, 0x57, 0x7fe0, count == 32 ? 31 : count + 1));
        }
        shell_protocol_parser_reset(&parser);
        bytes[size - 1] ^= 1;
        assert(!feed(&parser, bytes, size, &frame));
        bytes[size - 1] ^= 1;
        shell_protocol_parser_reset(&parser);
        assert(!feed(&parser, bytes, size - 1, &frame));
        // The driver resets partial input after a bounded idle gap or new request.
        shell_protocol_parser_reset(&parser);
        assert(feed(&parser, bytes, size, &frame));
    }
    const uint8_t oversized[] = {0x8f, 0x0a, 37};
    shell_protocol_parser_reset(&parser);
    assert(!feed(&parser, oversized, sizeof(oversized), &frame));
    size_t size = response(bytes, 0x57, 0, 0x7fe0, 32);
    bytes[7] = 0x8f; // A header byte inside payload is ordinary data.
    bytes[size - 1] = shell_protocol_crc(bytes, size - 1);
    assert(feed(&parser, bytes, size, &frame));
    assert(frame.payload[4] == 0x8f);
    assert(shell_protocol_matches(&frame, 0x57, 0x7fe0, 32));
    frame.channel = 0x0b;
    assert(!shell_protocol_matches(&frame, 0x57, 0x7fe0, 32));
    size = response(bytes, 0x57, 3, 0x7fe0, 32);
    assert(size == 9);
    assert(feed(&parser, bytes, size, &frame));
    assert(shell_protocol_matches(&frame, 0x57, 0x7fe0, 32));
    frame.length = 36;
    assert(!shell_protocol_matches(&frame, 0x57, 0x7fe0, 32));
    frame.length = 5; frame.payload[1] = 6;
    assert(!shell_protocol_matches(&frame, 0x57, 0x7fe0, 32));

    uint32_t random = 1;
    for (unsigned i = 0; i < 200000; ++i) {
        random ^= random << 13; random ^= random >> 17; random ^= random << 5;
        if (shell_protocol_feed(&parser, random, &frame)) assert(frame.length <= SHELL_MAX_PAYLOAD);
    }
    puts("PASS shell protocol: mapper/SRAM/window limits; all bulk read/write sizes/splits; CRC, capacity, oversized, truncated, identity/status/count rejection; bounded parser fuzz");
    return 0;
}
