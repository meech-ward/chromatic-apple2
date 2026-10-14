/* SPDX-License-Identifier: MIT
 * Original ESP-IDF BSD socket adapter. One task owns all transport/socket IO.
 */
#include "ssh_transport_internal.h"
#include "lwip/sockets.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#ifndef SSH_LISTEN_PORT
#define SSH_LISTEN_PORT 22
#endif
#ifndef SSH_LISTEN_ADDRESS
#define SSH_LISTEN_ADDRESS INADDR_ANY
#endif
static int listener = -1, client = -1;

static bool transient(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
static void close_client(void) {
    if (client >= 0) { close(client); client = -1; }
    ssh_transport_wire_closed();
}
bool ssh_platform_listen(void) {
    if (listener >= 0) return true;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(SSH_LISTEN_PORT),
                                  .sin_addr.s_addr = htonl(SSH_LISTEN_ADDRESS)};
    if (!nonblocking(fd) || bind(fd, (struct sockaddr *)&address, sizeof(address)) || listen(fd, 1)) {
        close(fd); return false;
    }
    listener = fd;
    return true;
}
void ssh_platform_stop(void) {
    if (listener >= 0) { close(listener); listener = -1; }
    close_client();
}
void ssh_platform_poll(void) {
    if (listener < 0) return;
    int candidate = accept(listener, NULL, NULL);
    if (candidate >= 0) {
        if (client >= 0 || !nonblocking(candidate)) close(candidate);
        else {
            int yes = 1;
            (void)setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
            (void)setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
            client = candidate;
            if (!ssh_transport_wire_accept()) { close_client(); return; }
        }
    }
    if (client < 0) return;
    uint8_t incoming[512];
    size_t budget = 2048;
    while (budget) {
        size_t size = ssh_transport_wire_receive_space();
        if (!size) break;
        if (size > sizeof(incoming)) size = sizeof(incoming);
        if (size > budget) size = budget;
        int received = recv(client, incoming, size, 0);
        if (received > 0) {
            if (!ssh_transport_wire_receive(incoming, (size_t)received)) { close_client(); return; }
            budget -= (size_t)received;
        } else if (!received || !transient()) { close_client(); return; }
        else break;
    }
    size_t size;
    const uint8_t *outgoing = ssh_transport_wire_output(&size);
    if (size > 1460) size = 1460;
    if (size) {
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        int sent = send(client, outgoing, size, flags);
        if (sent > 0) ssh_transport_wire_sent((size_t)sent);
        else if (!sent || !transient()) { close_client(); return; }
    }
    if (ssh_transport_wire_should_close()) close_client();
}
void ssh_platform_random(uint8_t *output, size_t size) { esp_fill_random(output, size); }
uint32_t ssh_platform_milliseconds(void) { return (uint32_t)(esp_timer_get_time()/1000); }
