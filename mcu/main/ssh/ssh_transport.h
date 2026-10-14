/* SPDX-License-Identifier: MIT */
#ifndef APPLE2_SSH_TRANSPORT_H
#define APPLE2_SSH_TRANSPORT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* One SSH-2 session, user "apple", password authentication. The caller owns
 * durable storage of a cryptographically random 32-byte Ed25519 host seed.
 * Call all functions on core 0, outside lwIP callbacks. init does not listen. */
bool ssh_transport_init(const uint8_t host_seed[32], const char *password);
bool ssh_transport_set_enabled(bool enabled);
bool ssh_transport_enabled(void);
bool ssh_transport_listening(void);
void ssh_transport_poll(void);
bool ssh_transport_connected(void); /* authenticated, shell requested */
uint32_t ssh_transport_session_id(void); /* increments for every accepted shell */
bool ssh_transport_take_interrupt(void); /* priority Ctrl-C; cancels earlier queued paste */
/* Client sent EOF and its transport input has drained. The caller should drain
 * its own keyboard queue and output, then call disconnect(). */
bool ssh_transport_input_ended(void);
size_t ssh_transport_read(uint8_t *data, size_t capacity);
size_t ssh_transport_write(const uint8_t *data, size_t length);
void ssh_transport_disconnect(void);
void ssh_transport_terminal_size(unsigned *columns, unsigned *rows);
const char *ssh_transport_status(void);
const char *ssh_transport_fingerprint(void); /* SHA256:<base64>, public */

#ifdef __cplusplus
}
#endif
#endif
