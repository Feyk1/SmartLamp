/**
 * @file rest_client.c
 * @brief Minimal HTTP/1.1 client for the firmware REST telemetry.
 *
 * It reuses the socket HAL (`net_transport`) and does not support TLS: the
 * local demo speaks `http://` to the backend on the Docker network. A port to
 * an MCU replaces the transport with lwIP + mbedTLS.
 */

#include "rest_client.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "net_transport.h"

#define REST_HOST_MAX       128U
#define REST_TOKEN_MAX      96U
#define REST_PATH_MAX       256U
#define REST_HEADER_MAX     512U
#define REST_RESPONSE_MAX   2048U
#define REST_TIMEOUT_MS     2000U

static char s_host[REST_HOST_MAX];
static uint16_t s_port = 80U;
static char s_token[REST_TOKEN_MAX];
static bool s_ready = false;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Parses "http://host[:port]" into host and port.
 *
 * @return 0 if the URL is valid; -1 otherwise.
 */
static int rest_parse_base(const char *url, char *host, size_t host_size,
                           uint16_t *port)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *colon = NULL;
    long port_value = 80L;
    size_t host_len;

    if ((url == NULL) || (strncmp(url, "http://", 7U) != 0)) {
        return -1;
    }

    p = url + 7U;
    host_start = p;
    host_end = strchr(p, '/');
    if (host_end == NULL) {
        host_end = p + strlen(p);
    }

    for (const char *q = host_start; q < host_end; q++) {
        if (*q == ':') {
            colon = q;
            break;
        }
    }

    if (colon != NULL) {
        host_len = (size_t)(colon - host_start);
        port_value = strtol(colon + 1, NULL, 10);
    } else {
        host_len = (size_t)(host_end - host_start);
    }

    if ((host_len == 0U) || (host_len >= host_size) || (port_value <= 0L) ||
        (port_value > 65535L)) {
        return -1;
    }

    (void)memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)port_value;
    return 0;
}

/**
 * @brief Fills `path` with the device token.
 *
 * @return 0 if it fits in the buffer; -1 otherwise.
 */
static int rest_with_token(const char *path, char *full, size_t full_size)
{
    int n;

    if (strchr(path, '?') != NULL) {
        n = snprintf(full, full_size, "%s&token=%s", path, s_token);
    } else {
        n = snprintf(full, full_size, "%s?token=%s", path, s_token);
    }
    if ((n <= 0) || ((size_t)n >= full_size)) {
        return -1;
    }
    return 0;
}

/**
 * @brief Extracts the status code from the first line of the response.
 *
 * @return HTTP status (100-599) or -1 if it cannot be parsed.
 */
static int rest_parse_status(const char *response)
{
    const char *p;
    int status = 0;
    int digits = 0;

    if (strncmp(response, "HTTP/", 5U) != 0) {
        return -1;
    }
    p = strchr(response, ' ');
    if (p == NULL) {
        return -1;
    }
    p++;
    while ((digits < 3) && (isdigit((unsigned char)*p) != 0)) {
        status = (status * 10) + (*p - '0');
        p++;
        digits++;
    }
    if (digits == 0) {
        return -1;
    }
    return status;
}

/**
 * @brief Opens the connection and sends the request (with the lock held).
 *
 * @return 0 if sent; 1 if it could not connect; -1 on a send error.
 */
static int rest_send_request(net_transport_t *transport, const char *method,
                             const char *path, const char *body)
{
    char full_path[REST_PATH_MAX];
    char header[REST_HEADER_MAX];
    size_t body_len = (body != NULL) ? strlen(body) : 0U;
    int n;

    if (rest_with_token(path, full_path, sizeof(full_path)) != 0) {
        return -1;
    }
    if (net_transport_open(transport, s_host, s_port, REST_TIMEOUT_MS) != 0) {
        return 1;
    }

    if (body_len > 0U) {
        n = snprintf(header, sizeof(header),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: smartlamp-fw\r\n"
                     "Accept: application/json\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     method, full_path, s_host, (unsigned)s_port,
                     (unsigned)body_len);
    } else {
        n = snprintf(header, sizeof(header),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: smartlamp-fw\r\n"
                     "Accept: application/json\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     method, full_path, s_host, (unsigned)s_port);
    }
    if ((n <= 0) || ((size_t)n >= sizeof(header))) {
        net_transport_close(transport);
        return -1;
    }

    if ((net_transport_send_all(transport, (const uint8_t *)header,
                                (size_t)n) != 0) ||
        ((body_len > 0U) &&
         (net_transport_send_all(transport, (const uint8_t *)body, body_len) !=
          0))) {
        net_transport_close(transport);
        return -1;
    }
    return 0;
}

/**
 * @brief Performs a request and returns the status code.
 */
static int rest_request(const char *method, const char *path, const char *body,
                        char *out, size_t out_size)
{
    net_transport_t transport;
    char response[REST_RESPONSE_MAX];
    const char *body_start;
    size_t used = 0U;
    int status;
    int rc;

    if ((!s_ready) || (path == NULL)) {
        return -1;
    }
    if (pthread_mutex_lock(&s_lock) != 0) {
        return -1;
    }

    rc = rest_send_request(&transport, method, path, body);
    if (rc != 0) {
        (void)pthread_mutex_unlock(&s_lock);
        return (rc > 0) ? 0 : -1;
    }

    while (used < (sizeof(response) - 1U)) {
        int received = net_transport_recv(&transport,
                                          (uint8_t *)response + used,
                                          sizeof(response) - 1U - used,
                                          REST_TIMEOUT_MS);
        if (received <= 0) {
            break; /* the server closes after sending (Connection: close) */
        }
        used += (size_t)received;
    }
    response[used] = '\0';
    net_transport_close(&transport);
    (void)pthread_mutex_unlock(&s_lock);

    if (used == 0U) {
        return 0;
    }

    status = rest_parse_status(response);
    if (status < 0) {
        LOG_DEBUG("rest: invalid response: %.32s", response);
        return -1;
    }

    if ((out != NULL) && (out_size > 0U)) {
        body_start = strstr(response, "\r\n\r\n");
        out[0] = '\0';
        if (body_start != NULL) {
            body_start += 4;
            (void)snprintf(out, out_size, "%s", body_start);
        }
    }

    return status;
}

int rest_client_get_stream(const char *path, rest_sink_fn sink, void *ctx,
                           size_t *total)
{
    net_transport_t transport;
    char buffer[REST_RESPONSE_MAX];
    char *body_start = NULL;
    size_t used = 0U;
    size_t transferred = 0U;
    int status;
    int rc;

    if ((!s_ready) || (path == NULL) || (sink == NULL)) {
        return -1;
    }
    if (pthread_mutex_lock(&s_lock) != 0) {
        return -1;
    }

    rc = rest_send_request(&transport, "GET", path, NULL);
    if (rc != 0) {
        (void)pthread_mutex_unlock(&s_lock);
        return (rc > 0) ? 0 : -1;
    }

    /* Read up to the end of the headers (the body may come after). */
    while (used < (sizeof(buffer) - 1U)) {
        int received = net_transport_recv(&transport, (uint8_t *)buffer + used,
                                          sizeof(buffer) - 1U - used,
                                          REST_TIMEOUT_MS);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
        buffer[used] = '\0';
        body_start = strstr(buffer, "\r\n\r\n");
        if (body_start != NULL) {
            break;
        }
    }

    if ((used == 0U) || (body_start == NULL)) {
        net_transport_close(&transport);
        (void)pthread_mutex_unlock(&s_lock);
        return -1;
    }

    status = rest_parse_status(buffer);
    body_start += 4;

    if ((status >= 200) && (status < 300)) {
        size_t body_len = used - (size_t)(body_start - buffer);

        if ((body_len > 0U) &&
            (sink(ctx, (const uint8_t *)body_start, body_len) != 0)) {
            net_transport_close(&transport);
            (void)pthread_mutex_unlock(&s_lock);
            return -1;
        }
        transferred += body_len;

        for (;;) {
            int received = net_transport_recv(&transport, (uint8_t *)buffer,
                                              sizeof(buffer), REST_TIMEOUT_MS);

            if (received <= 0) {
                break;
            }
            if (sink(ctx, (const uint8_t *)buffer, (size_t)received) != 0) {
                net_transport_close(&transport);
                (void)pthread_mutex_unlock(&s_lock);
                return -1;
            }
            transferred += (size_t)received;
        }
    }

    net_transport_close(&transport);
    (void)pthread_mutex_unlock(&s_lock);

    if (total != NULL) {
        *total = transferred;
    }
    return status;
}

int rest_client_init(const char *base_url, const char *token)
{
    if ((base_url == NULL) || (token == NULL)) {
        return -1;
    }
    if (rest_parse_base(base_url, s_host, sizeof(s_host), &s_port) != 0) {
        return -1;
    }
    if (strlen(token) >= sizeof(s_token)) {
        return -1;
    }
    (void)strncpy(s_token, token, sizeof(s_token) - 1U);
    s_token[sizeof(s_token) - 1U] = '\0';
    s_ready = true;
    return 0;
}

void rest_client_deinit(void)
{
    s_ready = false;
}

bool rest_client_is_ready(void)
{
    return s_ready;
}

int rest_client_post(const char *path, const char *json)
{
    return rest_request("POST", path, json, NULL, 0U);
}

int rest_client_get(const char *path, char *out, size_t out_size)
{
    return rest_request("GET", path, NULL, out, out_size);
}
