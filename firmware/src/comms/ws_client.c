/**
 * @file ws_client.c
 * @brief Minimal WebSocket (RFC6455) client for the firmware.
 *
 * Subset: HTTP Upgrade handshake, text frames, ping/pong and close.
 * It does not support fragmentation or WSS (those will be added in the
 * transport/TLS layer). The protocol is portable; only net_transport_* is
 * platform-specific.
 */

#include "ws_client.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "cJSON.h"
#include "cmd_handler.h"
#include "ota_client.h"
#include "log.h"
#include "modbus_client.h"
#include "tasks.h"
#include "net_transport.h"

#define WS_MAX_URL        256U
#define WS_MAX_TOKEN      96U
#define WS_MAX_FRAME      1200U
#define WS_HEADER_MAX     512U
#define WS_BACKOFF_MIN_MS 1000U
#define WS_BACKOFF_MAX_MS 30000U

static char s_url[WS_MAX_URL];
static char s_token[WS_MAX_TOKEN];
static SemaphoreHandle_t s_lock = NULL;
static net_transport_t s_transport;
static bool s_connected = false;
static bool s_initialized = false;
static modbus_light_state_t s_last_state;
static bool s_last_state_valid = false;
static uint32_t s_rng = 0x12345678U;

/**
 * @brief Simple PRNG for masking keys/WebSocket-Key (demo).
 */
static uint8_t ws_rand_byte(void)
{
    s_rng = (s_rng * 1664525U) + 1013904223U;
    return (uint8_t)(s_rng >> 24U);
}

/**
 * @brief Standard Base64 (for Sec-WebSocket-Key).
 */
static size_t ws_base64_encode(const uint8_t *in, size_t len, char *out,
                               size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    size_t o = 0U;

    if ((in == NULL) || (out == NULL) || (out_size < 4U)) {
        return 0U;
    }

    for (i = 0U; (i + 2U) < len; i += 3U) {
        uint32_t v = ((uint32_t)in[i] << 16U) |
                     ((uint32_t)in[i + 1U] << 8U) |
                     (uint32_t)in[i + 2U];
        if ((o + 4U) >= out_size) {
            return 0U;
        }
        out[o++] = alphabet[(v >> 18U) & 0x3FU];
        out[o++] = alphabet[(v >> 12U) & 0x3FU];
        out[o++] = alphabet[(v >> 6U) & 0x3FU];
        out[o++] = alphabet[v & 0x3FU];
    }

    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16U;
        if ((i + 1U) < len) {
            v |= (uint32_t)in[i + 1U] << 8U;
        }
        if ((o + 4U) >= out_size) {
            return 0U;
        }
        out[o++] = alphabet[(v >> 18U) & 0x3FU];
        out[o++] = alphabet[(v >> 12U) & 0x3FU];
        out[o++] = ((i + 1U) < len) ? alphabet[(v >> 6U) & 0x3FU] : '=';
        out[o++] = '=';
    }

    out[o] = '\0';
    return o;
}

/**
 * @brief Parses ws://host[:port]/path.
 */
static int ws_parse_url(const char *url, char *host, size_t host_size,
                        uint16_t *port, char *path, size_t path_size)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *port_start = NULL;
    const char *path_start;
    size_t host_len;
    long port_value = 80L;

    if ((url == NULL) || (strncmp(url, "ws://", 5U) != 0)) {
        return -1;
    }

    p = url + 5U;
    host_start = p;
    path_start = strchr(p, '/');
    host_end = (path_start != NULL) ? path_start : (p + strlen(p));

    for (const char *q = host_start; q < host_end; q++) {
        if (*q == ':') {
            port_start = q;
            break;
        }
    }

    if (port_start != NULL) {
        host_len = (size_t)(port_start - host_start);
        port_value = strtol(port_start + 1, NULL, 10);
    } else {
        host_len = (size_t)(host_end - host_start);
    }

    if ((host_len == 0U) || (host_len >= host_size) ||
        (port_value <= 0L) || (port_value > 65535L)) {
        return -1;
    }

    (void)memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    *port = (uint16_t)port_value;

    if (path_start == NULL) {
        if (path_size < 2U) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
    } else {
        size_t path_len = strlen(path_start);
        if (path_len >= path_size) {
            return -1;
        }
        (void)memcpy(path, path_start, path_len + 1U);
    }
    return 0;
}

static int ws_send_handshake(const char *host, uint16_t port, const char *path,
                             const char *token)
{
    uint8_t key[16];
    char key_b64[32];
    char full_path[WS_MAX_URL];
    char request[WS_HEADER_MAX];
    int n;
    size_t i;

    if (strchr(path, '?') != NULL) {
        n = snprintf(full_path, sizeof(full_path), "%s&token=%s", path, token);
    } else {
        n = snprintf(full_path, sizeof(full_path), "%s?token=%s", path, token);
    }
    if ((n <= 0) || ((size_t)n >= sizeof(full_path))) {
        return -1;
    }

    for (i = 0U; i < sizeof(key); i++) {
        key[i] = ws_rand_byte();
    }
    if (ws_base64_encode(key, sizeof(key), key_b64, sizeof(key_b64)) == 0U) {
        return -1;
    }

    n = snprintf(request, sizeof(request),
                 "GET %s HTTP/1.1\r\n"
                 "Host: %s:%u\r\n"
                 "Upgrade: websocket\r\n"
                 "Connection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\n"
                 "Sec-WebSocket-Version: 13\r\n"
                 "\r\n",
                 full_path, host, (unsigned)port, key_b64);
    if ((n <= 0) || ((size_t)n >= sizeof(request))) {
        return -1;
    }

    return net_transport_send_all(&s_transport, (const uint8_t *)request,
                                 (size_t)n);
}

static int ws_read_handshake_response(void)
{
    char response[WS_HEADER_MAX];
    size_t used = 0U;

    while (used < (sizeof(response) - 1U)) {
        int n = net_transport_recv(&s_transport, (uint8_t *)response + used,
                                  sizeof(response) - 1U - used, 2000U);
        if (n < 0) {
            LOG_WARN("ws: connection closed during handshake");
            return -1;
        }
        if (n == 0) {
            LOG_WARN("ws: timeout waiting for handshake");
            return -1;
        }
        used += (size_t)n;
        response[used] = '\0';
        if (strstr(response, "\r\n\r\n") != NULL) {
            break;
        }
    }

    if (strncmp(response, "HTTP/1.1 101", 12U) != 0) {
        LOG_WARN("ws: handshake rejected: %.40s", response);
        return -1;
    }
    return 0;
}

static int ws_read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t offset = 0U;

    while (offset < len) {
        int n = net_transport_recv(&s_transport, buf + offset, len - offset,
                                  timeout_ms);
        if (n <= 0) {
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

/**
 * @brief Sends a masked frame (the lock must be held).
 */
static int ws_send_frame_locked(uint8_t opcode, const uint8_t *payload,
                                size_t len)
{
    uint8_t frame[WS_MAX_FRAME];
    uint8_t mask[4];
    size_t header_len;
    size_t i;

    if (len > (sizeof(frame) - 14U)) {
        return -1;
    }

    frame[0] = (uint8_t)(0x80U | opcode);
    if (len < 126U) {
        frame[1] = (uint8_t)(0x80U | len);
        header_len = 2U;
    } else if (len <= 0xFFFFU) {
        frame[1] = (uint8_t)(0x80U | 126U);
        frame[2] = (uint8_t)(len >> 8U);
        frame[3] = (uint8_t)(len & 0xFFU);
        header_len = 4U;
    } else {
        return -1;
    }

    for (i = 0U; i < 4U; i++) {
        mask[i] = ws_rand_byte();
        frame[header_len + i] = mask[i];
    }
    for (i = 0U; i < len; i++) {
        frame[header_len + 4U + i] = (uint8_t)(payload[i] ^ mask[i % 4U]);
    }

    return net_transport_send_all(&s_transport, frame, header_len + 4U + len);
}

static int ws_send_text_locked(const char *text)
{
    if ((!s_connected) || (text == NULL)) {
        return -1;
    }
    return ws_send_frame_locked(0x1U, (const uint8_t *)text, strlen(text));
}

static int ws_send_text(const char *text)
{
    int rc;

    if ((s_lock == NULL) ||
        (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)) {
        return -1;
    }
    rc = ws_send_text_locked(text);
    (void)xSemaphoreGive(s_lock);
    return rc;
}

static int ws_send_control(uint8_t opcode, const uint8_t *payload, size_t len)
{
    int rc;

    if ((s_lock == NULL) ||
        (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE)) {
        return -1;
    }
    rc = ws_send_frame_locked(opcode, payload, len);
    (void)xSemaphoreGive(s_lock);
    return rc;
}

static void ws_send_response(const char *id, const char *op, bool ok,
                             const char *code)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *payload;
    char *text;

    if (root == NULL) {
        return;
    }

    cJSON_AddStringToObject(root, "id", (id != NULL) ? id : "");
    cJSON_AddStringToObject(root, "type", ok ? "ack" : "nack");
    cJSON_AddStringToObject(root, "op", (op != NULL) ? op : "");
    payload = cJSON_AddObjectToObject(root, "payload");
    if ((!ok) && (payload != NULL)) {
        cJSON_AddStringToObject(payload, "code",
                                (code != NULL) ? code : "INTERNAL");
        cJSON_AddStringToObject(payload, "message", "command failed");
    }

    text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        (void)ws_send_text(text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void ws_reply(void *ctx, const char *id, const char *op, bool ok,
                     const char *code)
{
    (void)ctx;
    ws_send_response(id, op, ok, code);
}

static int ws_write_registers(void *ctx, uint16_t addr, uint16_t count,
                              const uint16_t *values)
{
    (void)ctx;
    return modbus_client_write_registers(addr, count, values);
}

static int ws_write_coil(void *ctx, uint16_t addr, bool value)
{
    (void)ctx;
    return modbus_client_write_coil(addr, value);
}

static int ws_send_current_state(void *ctx)
{
    (void)ctx;
    if (!s_last_state_valid) {
        return -1;
    }
    return ws_client_send_state(&s_last_state);
}

static int ws_ota_update(void *ctx, const char *version)
{
    (void)ctx;
    return ota_client_request(version);
}

static int ws_reboot(void *ctx)
{
    (void)ctx;
    return ota_client_request_reboot();
}

static void ws_handle_command(const cJSON *root)
{
    const cmd_io_t io = {NULL, ws_write_registers, ws_write_coil};
    const cmd_handler_t handler = {NULL, ws_reply, ws_send_current_state,
                                   ws_ota_update, ws_reboot};

    (void)cmd_handler_handle(root, &io, &handler);
}

static void ws_handle_text(const char *text)
{
    cJSON *root;
    const char *type;

    if (text == NULL) {
        return;
    }
    root = cJSON_Parse(text);
    if (root == NULL) {
        LOG_WARN("ws: invalid JSON");
        return;
    }

    type = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(root, "type"));
    if (type != NULL) {
        if (strcmp(type, "command") == 0) {
            ws_handle_command(root);
        } else if (strcmp(type, "ping") == 0) {
            (void)ws_send_text("{\"type\":\"pong\"}");
        }
    }
    cJSON_Delete(root);
}

int ws_client_send_state(const modbus_light_state_t *state)
{
    cJSON *root;
    cJSON *payload;
    cJSON *color;
    cJSON *sequence;
    char *text;
    int rc;

    if ((state == NULL) || (s_lock == NULL)) {
        return -1;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }

    s_last_state = *state;
    s_last_state_valid = true;

    if (!s_connected) {
        (void)xSemaphoreGive(s_lock);
        return -1;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        (void)xSemaphoreGive(s_lock);
        return -1;
    }

    cJSON_AddStringToObject(root, "type", "state");
    payload = cJSON_AddObjectToObject(root, "payload");
    if (payload != NULL) {
        cJSON_AddBoolToObject(payload, "power", state->power != 0U);
        cJSON_AddNumberToObject(payload, "mode", state->mode);
        cJSON_AddNumberToObject(payload, "brightness", state->brightness);
        color = cJSON_AddObjectToObject(payload, "color");
        if (color != NULL) {
            cJSON_AddNumberToObject(color, "h", state->hue);
            cJSON_AddNumberToObject(color, "s", state->sat);
            cJSON_AddNumberToObject(color, "v", state->val);
        }
        cJSON_AddNumberToObject(payload, "cct", state->cct);
        sequence = cJSON_AddObjectToObject(payload, "sequence");
        if (sequence != NULL) {
            cJSON_AddNumberToObject(sequence, "step", state->seq_step);
        }
        cJSON_AddNumberToObject(payload, "transition_progress",
                                state->transition_progress);
    }

    text = cJSON_PrintUnformatted(root);
    rc = (text != NULL) ? ws_send_text_locked(text) : -1;
    if (text != NULL) {
        cJSON_free(text);
    }
    cJSON_Delete(root);

    (void)xSemaphoreGive(s_lock);
    return rc;
}

static int client_connect(void)
{
    char host[128];
    char path[WS_MAX_URL];
    uint16_t port = 80U;
    modbus_light_state_t state;
    bool have_state;
    cJSON *boot;
    cJSON *payload;
    char *text;

    if (!s_initialized) {
        return -1;
    }
    if (ws_parse_url(s_url, host, sizeof(host), &port, path, sizeof(path)) !=
        0) {
        LOG_ERROR("ws: invalid URL: %s", s_url);
        return -1;
    }
    if (net_transport_open(&s_transport, host, port, 2000U) != 0) {
        LOG_DEBUG("ws: connection failed to %s:%u", host, (unsigned)port);
        return -1;
    }
    if ((ws_send_handshake(host, port, path, s_token) != 0) ||
        (ws_read_handshake_response() != 0)) {
        net_transport_close(&s_transport);
        return -1;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        net_transport_close(&s_transport);
        return -1;
    }
    s_connected = true;
    state = s_last_state;
    have_state = s_last_state_valid;
    (void)xSemaphoreGive(s_lock);

    LOG_INFO("ws: connected to %s:%u", host, (unsigned)port);

    boot = cJSON_CreateObject();
    if (boot != NULL) {
        cJSON_AddStringToObject(boot, "type", "event");
        cJSON_AddStringToObject(boot, "op", "boot");
        payload = cJSON_AddObjectToObject(boot, "payload");
        if (payload != NULL) {
            cJSON_AddStringToObject(payload, "slot", "A");
            cJSON_AddStringToObject(payload, "cause", "ws_connect");
        }
        text = cJSON_PrintUnformatted(boot);
        if (text != NULL) {
            (void)ws_send_text(text);
            cJSON_free(text);
        }
        cJSON_Delete(boot);
    }

    if (have_state) {
        (void)ws_client_send_state(&state);
    }
    return 0;
}

static void client_close(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_connected = false;
    net_transport_close(&s_transport);
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static int client_poll(uint32_t timeout_ms)
{
    uint8_t header[2];
    uint8_t mask[4] = {0};
    uint8_t payload[1024];
    uint8_t fin;
    uint8_t opcode;
    uint8_t masked;
    uint64_t len;
    size_t i;
    int n;

    n = net_transport_recv(&s_transport, header, 2U, timeout_ms);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if (n == 1) {
        if (ws_read_exact(header + 1, 1U, 1000U) != 0) {
            return -1;
        }
    }

    fin = (uint8_t)(header[0] & 0x80U);
    opcode = (uint8_t)(header[0] & 0x0FU);
    masked = (uint8_t)(header[1] & 0x80U);
    len = (uint64_t)(header[1] & 0x7FU);

    if (len == 126U) {
        uint8_t ext[2];
        if (ws_read_exact(ext, 2U, 1000U) != 0) {
            return -1;
        }
        len = ((uint64_t)ext[0] << 8U) | (uint64_t)ext[1];
    } else if (len == 127U) {
        uint8_t ext[8];
        if (ws_read_exact(ext, 8U, 1000U) != 0) {
            return -1;
        }
        len = 0U;
        for (i = 0U; i < 8U; i++) {
            len = (len << 8U) | (uint64_t)ext[i];
        }
    }

    if (len > (sizeof(payload) - 1U)) {
        return -1;
    }
    if (masked != 0U) {
        if (ws_read_exact(mask, 4U, 1000U) != 0) {
            return -1;
        }
    }
    if (len > 0U) {
        if (ws_read_exact(payload, (size_t)len, 1000U) != 0) {
            return -1;
        }
        if (masked != 0U) {
            for (i = 0U; i < (size_t)len; i++) {
                payload[i] ^= mask[i % 4U];
            }
        }
    }
    payload[len] = '\0';

    if (opcode == 0x8U) {
        (void)ws_send_control(0x8U, payload, (size_t)len);
        return -1;
    }
    if (opcode == 0x9U) {
        (void)ws_send_control(0xAU, payload, (size_t)len);
        return 0;
    }
    if (opcode == 0xAU) {
        return 0;
    }
    if (opcode == 0x1U) {
        if (fin == 0U) {
            return -1; /* fragmentation not supported */
        }
        ws_handle_text((const char *)payload);
        return 0;
    }
    return 0;
}

int ws_client_init(const char *url, const char *token)
{
    if ((url == NULL) || (token == NULL)) {
        return -1;
    }
    if ((strlen(url) >= sizeof(s_url)) || (strlen(token) >= sizeof(s_token))) {
        return -1;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return -1;
        }
    }

    (void)strncpy(s_url, url, sizeof(s_url) - 1U);
    s_url[sizeof(s_url) - 1U] = '\0';
    (void)strncpy(s_token, token, sizeof(s_token) - 1U);
    s_token[sizeof(s_token) - 1U] = '\0';

    s_transport.fd = -1;
    s_connected = false;
    s_last_state_valid = false;
    (void)memset(&s_last_state, 0, sizeof(s_last_state));
    s_initialized = true;
    s_rng = ((uint32_t)xTaskGetTickCount()) ^ 0x9E3779B9U;

    (void)signal(SIGPIPE, SIG_IGN);
    return 0;
}

void ws_client_deinit(void)
{
    if (s_initialized) {
        client_close();
        s_initialized = false;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
}

bool ws_client_is_connected(void)
{
    bool connected = false;

    if ((s_lock != NULL) &&
        (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE)) {
        connected = s_connected;
        (void)xSemaphoreGive(s_lock);
    }
    return connected;
}

void v_comms_task(void *pv_parameters)
{
    uint32_t backoff_ms = WS_BACKOFF_MIN_MS;

    (void)pv_parameters;

    if (!s_initialized) {
        LOG_ERROR("ws: client not initialized");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000U));
        }
    }

    for (;;) {
        TickType_t connected_at = xTaskGetTickCount();

        if (client_connect() == 0) {
            while (ws_client_is_connected()) {
                if (client_poll(50U) < 0) {
                    break;
                }
                /* Yield the CPU: without this, the task (priority 2) keeps
                 * the POSIX port thread running continuously and the
                 * priority 1 tasks (monitor, diag, telemetry, commands) never
                 * run. */
                vTaskDelay(pdMS_TO_TICKS(1U));
            }
            client_close();
            if ((xTaskGetTickCount() - connected_at) > pdMS_TO_TICKS(30000U)) {
                backoff_ms = WS_BACKOFF_MIN_MS;
            }
            LOG_WARN("ws: connection lost; reconnecting");
        } else {
            LOG_DEBUG("ws: retry in %lu ms", (unsigned long)backoff_ms);
        }

        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms < WS_BACKOFF_MAX_MS)
                         ? (backoff_ms * 2U)
                         : WS_BACKOFF_MAX_MS;
    }
}
