/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* Call once after network_start has initialized NVS. The
 * task waits for a ready station/hotspot before persisting a fresh host seed.
 * One authenticated user "apple" gets the machine's terminal, not an OS shell.
 */
void ssh_terminal_start(void);
#ifdef __cplusplus
}
#endif
