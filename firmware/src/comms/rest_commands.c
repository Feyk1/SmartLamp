/**
 * @file rest_commands.c
 * @brief REST fallback for commands when the device WS is down.
 *
 * Implements docs/02 §4.3: without a WS, the firmware polls
 * `GET /commands/pending` and confirms with `POST /commands/{id}/ack|nack`.
 */

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cJSON.h"
#include "cmd_handler.h"
#include "log.h"
#include "modbus_client.h"
#include "ota_client.h"
#include "rest_client.h"
#include "tasks.h"
#include "telemetry.h"
#include "ws_client.h"

#define REST_PENDING_PATH    "/api/v1/commands/pending"
#define REST_PENDING_BUF     2048U
#define REST_POLL_PERIOD_MS  1000U
#define REST_POLL_IDLE_MS    500U

static int rest_write_registers(void *ctx, uint16_t addr, uint16_t count,
                                const uint16_t *values)
{
    (void)ctx;
    return modbus_client_write_registers(addr, count, values);
}

static int rest_write_coil(void *ctx, uint16_t addr, bool value)
{
    (void)ctx;
    return modbus_client_write_coil(addr, value);
}

static int rest_ota_update(void *ctx, const char *version)
{
    (void)ctx;
    return ota_client_request(version);
}

static int rest_reboot(void *ctx)
{
    (void)ctx;
    return ota_client_request_reboot();
}

static int rest_send_state(void *ctx)
{
    modbus_light_state_t state;

    (void)ctx;
    if (modbus_client_read_light_state(&state) != 0) {
        return -1;
    }
    return telemetry_send_state(&state);
}

/**
 * @brief Confirms a command over REST with the same `id`.
 */
static void rest_reply(void *ctx, const char *id, const char *op, bool ok,
                       const char *code)
{
    char path[160];
    char body[192];
    int n;

    (void)ctx;

    if ((id == NULL) || (id[0] == '\0')) {
        return;
    }

    n = snprintf(path, sizeof(path), "/api/v1/commands/%s/%s", id,
                 ok ? "ack" : "nack");
    if ((n <= 0) || ((size_t)n >= sizeof(path))) {
        return;
    }

    if (ok) {
        n = snprintf(body, sizeof(body), "{\"op\":\"%s\",\"payload\":{}}",
                     (op != NULL) ? op : "");
    } else {
        n = snprintf(body, sizeof(body),
                     "{\"op\":\"%s\",\"payload\":{\"code\":\"%s\"}}",
                     (op != NULL) ? op : "",
                     (code != NULL) ? code : "INTERNAL");
    }
    if ((n <= 0) || ((size_t)n >= sizeof(body))) {
        return;
    }

    if (rest_client_post(path, body) < 0) {
        LOG_DEBUG("rest: failed to confirm %s", id);
    }
}

/**
 * @brief Polls and executes the pending commands.
 */
static void rest_commands_poll(void)
{
    static char response[REST_PENDING_BUF];
    const cmd_io_t io = {NULL, rest_write_registers, rest_write_coil};
    const cmd_handler_t handler = {NULL, rest_reply, rest_send_state,
                                   rest_ota_update, rest_reboot};
    cJSON *root;
    const cJSON *commands;
    const cJSON *command = NULL;
    int status;

    status = rest_client_get(REST_PENDING_PATH, response, sizeof(response));
    if ((status < 200) || (status >= 300)) {
        return;
    }

    root = cJSON_Parse(response);
    if (root == NULL) {
        LOG_DEBUG("rest: non-JSON response at %s", REST_PENDING_PATH);
        return;
    }

    commands = cJSON_GetObjectItemCaseSensitive(root, "commands");
    cJSON_ArrayForEach(command, commands)
    {
        (void)cmd_handler_handle(command, &io, &handler);
    }
    cJSON_Delete(root);
}

void v_command_poll_task(void *pv_parameters)
{
    (void)pv_parameters;

    for (;;) {
        if (ws_client_is_connected() || (!rest_client_is_ready())) {
            vTaskDelay(pdMS_TO_TICKS(REST_POLL_IDLE_MS));
            continue;
        }

        rest_commands_poll();
        vTaskDelay(pdMS_TO_TICKS(REST_POLL_PERIOD_MS));
    }
}
