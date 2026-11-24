/**
 * @file main.c
 * @brief Firmware entry point: FreeRTOS tasks, Modbus client and telemetry.
 *
 * Initializes flash, logging and the Modbus client, creates the base and
 * polling tasks, and starts the FreeRTOS scheduler.
 */

#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#include "flash.h"
#include "log.h"
#include "modbus_client.h"
#include "rest_client.h"
#include "tasks.h"
#include "telemetry.h"
#include "ws_client.h"

/**
 * @brief Creates the base firmware tasks.
 *
 * @return 0 if all of them are created successfully; -1 otherwise.
 */
static int create_tasks(void)
{
    BaseType_t rc;

    rc = xTaskCreate(v_control_task, "control", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_CONTROL, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vControlTask");
        return -1;
    }

    rc = xTaskCreate(v_comms_task, "comms", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_COMMS, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vCommsTask");
        return -1;
    }

    rc = xTaskCreate(v_monitor_task, "monitor", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_MONITOR, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vMonitorTask");
        return -1;
    }

    rc = xTaskCreate(v_light_poll_task, "light", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_LIGHT_POLL, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vLightPollTask");
        return -1;
    }

    rc = xTaskCreate(v_watchdog_task, "watchdog", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_WATCHDOG, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vWatchdogTask");
        return -1;
    }

    rc = xTaskCreate(v_heartbeat_task, "heartbeat", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_HEARTBEAT, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vHeartbeatTask");
        return -1;
    }

    rc = xTaskCreate(v_diag_poll_task, "diag", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_DIAG, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vDiagPollTask");
        return -1;
    }

    rc = xTaskCreate(v_telemetry_task, "telemetry", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_TELEMETRY, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vTelemetryTask");
        return -1;
    }

    rc = xTaskCreate(v_command_poll_task, "commands", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_COMMANDS, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vCommandPollTask");
        return -1;
    }

    rc = xTaskCreate(v_confirm_task, "confirm", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_CONFIRM, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vConfirmTask");
        return -1;
    }

    rc = xTaskCreate(v_ota_task, "ota", TASK_STACK_WORDS, NULL,
                     TASK_PRIO_OTA, NULL);
    if (rc != pdPASS) {
        LOG_ERROR("failed to create vOtaTask");
        return -1;
    }

    return 0;
}

/**
 * @brief Firmware entry point.
 *
 * @return 1 if initialization fails or the scheduler returns.
 */
int main(void)
{
    const char *flash_path;
    const char *modbus_host;
    const char *ws_url;
    const char *http_url;
    const char *device_token;
    const char *env;
    long modbus_port = 5020L;
    long modbus_unit = 1L;
    long watchdog_timeout = 5L;

    log_init();
    LOG_INFO("firmware starting");

    flash_path = getenv("FLASH_PATH");
    if (flash_path == NULL) {
        flash_path = "flash.bin";
    }
    if (flash_init(flash_path) != FLASH_OK) {
        LOG_WARN("failed to initialize %s; continuing without persistence",
                 flash_path);
    } else {
        LOG_INFO("flash.bin ready (%u B) at %s",
                 (unsigned)flash_size(), flash_path);
    }

    modbus_host = getenv("MODBUS_HOST");
    if (modbus_host == NULL) {
        modbus_host = "127.0.0.1";
    }
    env = getenv("MODBUS_PORT");
    if (env != NULL) {
        modbus_port = strtol(env, NULL, 10);
    }
    env = getenv("MODBUS_UNIT_ID");
    if (env != NULL) {
        modbus_unit = strtol(env, NULL, 10);
    }
    env = getenv("MODBUS_WATCHDOG_TIMEOUT_S");
    if (env != NULL) {
        watchdog_timeout = strtol(env, NULL, 10);
    }

    if ((modbus_port <= 0L) || (modbus_port > 65535L)) {
        modbus_port = 5020L;
    }
    if ((modbus_unit <= 0L) || (modbus_unit > 255L)) {
        modbus_unit = 1L;
    }
    if (watchdog_timeout < 0L) {
        watchdog_timeout = 0L;
    }
    if (watchdog_timeout > 65535L) {
        watchdog_timeout = 65535L;
    }

    if (modbus_client_init(modbus_host, (uint16_t)modbus_port,
                           (uint8_t)modbus_unit) != 0) {
        LOG_WARN("Modbus: no connection to %s:%ld; will retry",
                 modbus_host, modbus_port);
    } else {
        LOG_INFO("Modbus: connected to %s:%ld", modbus_host, modbus_port);
    }

    if (watchdog_timeout > 0L) {
        (void)modbus_client_set_watchdog(true, (uint16_t)watchdog_timeout);
        LOG_INFO("Modbus: heartbeat watchdog %ld s", watchdog_timeout);
    } else {
        (void)modbus_client_set_watchdog(false, 0U);
        LOG_INFO("Modbus: heartbeat watchdog disabled");
    }

    ws_url = getenv("ORCH_WS_URL");
    if (ws_url == NULL) {
        ws_url = "ws://127.0.0.1:8080/ws/device";
    }
    device_token = getenv("DEVICE_TOKEN");
    if (device_token == NULL) {
        device_token = "dev-token-local";
    }
    if (ws_client_init(ws_url, device_token) != 0) {
        LOG_WARN("ws: client not initialized (%s)", ws_url);
    } else {
        LOG_INFO("ws: target %s", ws_url);
    }

    http_url = getenv("ORCH_HTTP_URL");
    if (http_url == NULL) {
        http_url = "http://127.0.0.1:8080";
    }
    if (rest_client_init(http_url, device_token) != 0) {
        LOG_WARN("rest: client not initialized (%s)", http_url);
    } else {
        LOG_INFO("rest: target %s", http_url);
    }
    if (telemetry_init() != 0) {
        LOG_WARN("telemetry: not initialized");
    }

    if (create_tasks() != 0) {
        ws_client_deinit();
        modbus_client_deinit();
        flash_deinit();
        return 1;
    }

    LOG_INFO("scheduler starting");
    vTaskStartScheduler();

    LOG_ERROR("scheduler exited unexpectedly");
    ws_client_deinit();
    modbus_client_deinit();
    flash_deinit();
    return 1;
}
