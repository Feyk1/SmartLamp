#ifndef NET_TRANSPORT_H
#define NET_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/*
 * Transport HAL for the WebSocket client.
 * POSIX uses sockets; a port to an MCU only needs to reimplement this layer
 * (lwIP + mbedTLS for WSS).
 */
typedef struct {
    int fd;
} net_transport_t;

int net_transport_open(net_transport_t *transport, const char *host, uint16_t port,
                      uint32_t timeout_ms);
int net_transport_send_all(net_transport_t *transport, const uint8_t *buf,
                          size_t len);
int net_transport_recv(net_transport_t *transport, uint8_t *buf, size_t len,
                      uint32_t timeout_ms);
void net_transport_close(net_transport_t *transport);

#endif /* NET_TRANSPORT_H */
