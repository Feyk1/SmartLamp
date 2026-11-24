/**
 * @file net_transport_posix.c
 * @brief WebSocket transport over POSIX sockets.
 */

#include "net_transport.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/** @brief Monotonic clock in ms (for deadlines that survive EINTR). */
static int64_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000) + ((int64_t)ts.tv_nsec / 1000000);
}

/** @brief Milliseconds left until `deadline` (>= 0). */
static int remaining_ms(int64_t deadline)
{
    int64_t left = deadline - now_ms();

    if (left < 0) {
        return 0;
    }
    return (int)left;
}

int net_transport_open(net_transport_t *transport, const char *host, uint16_t port,
                      uint32_t timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    char port_str[8];
    int fd = -1;

    (void)timeout_ms;

    if ((transport == NULL) || (host == NULL)) {
        return -1;
    }
    transport->fd = -1;

    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        int connected = 0;

        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        /* The FreeRTOS tick (SIGALRM every 1 ms) interrupts connect();
         * after EINTR we wait for the socket to become writable and
         * check SO_ERROR instead of retrying connect blindly. */
        for (;;) {
            struct pollfd pfd;
            int so_error = 0;
            socklen_t so_len = sizeof(so_error);
            int prc;

            if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
                connected = 1;
                break;
            }
            if (errno != EINTR) {
                break;
            }
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            do {
                prc = poll(&pfd, 1, (int)timeout_ms);
            } while ((prc < 0) && (errno == EINTR));
            if ((prc <= 0) ||
                (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0) ||
                (so_error != 0)) {
                break;
            }
            connected = 1;
            break;
        }

        if (connected != 0) {
            break;
        }
        (void)close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0) {
        return -1;
    }
    transport->fd = fd;
    return 0;
}

int net_transport_send_all(net_transport_t *transport, const uint8_t *buf,
                          size_t len)
{
    size_t offset = 0U;

    if ((transport == NULL) || (transport->fd < 0) || (buf == NULL)) {
        return -1;
    }

    while (offset < len) {
        ssize_t written = send(transport->fd, buf + offset, len - offset,
                               MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

int net_transport_recv(net_transport_t *transport, uint8_t *buf, size_t len,
                      uint32_t timeout_ms)
{
    struct pollfd pfd;
    int64_t deadline;
    int rc;
    ssize_t received;

    if ((transport == NULL) || (transport->fd < 0) || (buf == NULL) ||
        (len == 0U)) {
        return -1;
    }

    pfd.fd = transport->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    deadline = now_ms() + (int64_t)timeout_ms;

    /* The FreeRTOS POSIX port delivers the tick through SIGALRM every 1 ms
     * with sa_flags=0 (no SA_RESTART): poll()/recv() may return EINTR
     * repeatedly. We retry while honoring the overall deadline. */
    for (;;) {
        pfd.revents = 0;
        rc = poll(&pfd, 1, remaining_ms(deadline));
        if (rc == 0) {
            return 0; /* timeout */
        }
        if (rc < 0) {
            if (errno == EINTR) {
                if (remaining_ms(deadline) == 0) {
                    return 0;
                }
                continue;
            }
            return -1;
        }
        if ((pfd.revents & POLLIN) != 0) {
            break;
        }
        if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            return -1;
        }
        return -1;
    }

    for (;;) {
        received = recv(transport->fd, buf, len, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1; /* peer closed */
        }
        return (int)received;
    }
}

void net_transport_close(net_transport_t *transport)
{
    if ((transport != NULL) && (transport->fd >= 0)) {
        (void)close(transport->fd);
        transport->fd = -1;
    }
}
