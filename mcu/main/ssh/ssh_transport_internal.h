/* SPDX-License-Identifier: MIT. Byte-stream platform adapter; not application API. */
#ifndef APPLE2_SSH_TRANSPORT_INTERNAL_H
#define APPLE2_SSH_TRANSPORT_INTERNAL_H
#include "ssh_transport.h"
#define SSH_TRANSPORT_WIRE_CAPACITY 8192u
#ifdef __cplusplus
extern "C" {
#endif
bool ssh_transport_wire_accept(void);
bool ssh_transport_wire_receive(const uint8_t *data, size_t length);
size_t ssh_transport_wire_receive_space(void);
const uint8_t *ssh_transport_wire_output(size_t *length);
void ssh_transport_wire_sent(size_t length);
void ssh_transport_wire_closed(void);
bool ssh_transport_wire_should_close(void);
bool ssh_platform_listen(void);
void ssh_platform_stop(void);
void ssh_platform_poll(void);
void ssh_platform_random(uint8_t *out, size_t length);
uint32_t ssh_platform_milliseconds(void);
#ifdef __cplusplus
}
#endif
#endif
