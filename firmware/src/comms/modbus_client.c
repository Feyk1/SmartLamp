/**
 * @file modbus_client.c
 * @brief Firmware Modbus TCP client and polling tasks.
 *
 * The firmware is the Modbus master: it reads the real state and diagnostics
 * from the driver, writes the heartbeat and, optionally, configures the
 * watchdog. The libmodbus context is not thread-safe; it is protected with a
 * FreeRTOS mutex.
 */

#include "modbus_client.h"

#include <modbus.h>
#include <modbus-tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "log.h"
#include "tasks.h"
#include "telemetry.h"
#include "ws_client.h"

#define MODBUS_INPUT_LIGHT_ADDR       0
#define MODBUS_INPUT_LIGHT_COUNT      9
#define MODBUS_INPUT_DIAG_ADDR        10
#define MODBUS_INPUT_SERIAL_TEXT_ADDR 31
#define MODBUS_INPUT_SERIAL_TEXT_COUNT 12
#define MODBUS_HOLDING_HEARTBEAT      40  /* 40041 */
#define MODBUS_HOLDING_WD_TIMEOUT     34  /* 40035 (not 40036: that is overtemp warn) */
#define MODBUS_COIL_WD_ENABLE         10  /* 00011 */
#define MODBUS_RETRY_MS               1000U

static modbus_t *s_ctx = NULL;
static SemaphoreHandle_t s_lock = NULL;
static bool s_connected = false;
static TickType_t s_next_retry = 0;
static uint16_t s_heartbeat = 1U;
static bool s_watchdog_enabled = false;
static uint16_t s_watchdog_timeout = 0U;
/* Snapshot of the last diagnostic; v_diag_poll_task writes it and the
 * telemetry task reads it (copies of 16/32-bit fields, no pointers). */
static modbus_diag_t s_diag;

/**
 * @brief Takes the client mutex.
 *
 * @return 0 on success; -1 if the client is not initialized.
 */
static int client_lock(void)
{
    if (s_lock == NULL) {
        return -1;
    }
    return (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) ? 0 : -1;
}

static void client_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGive(s_lock);
    }
}

static void client_disconnect_locked(void)
{
    if (s_connected) {
        (void)modbus_close(s_ctx);
        s_connected = false;
    }
    s_next_retry = xTaskGetTickCount() + pdMS_TO_TICKS(MODBUS_RETRY_MS);
}

/**
 * @brief Applies the remembered watchdog configuration to the driver.
 */
static void client_apply_watchdog_locked(void)
{
    if (!s_connected) {
        return;
    }

    if (s_watchdog_enabled) {
        (void)modbus_write_register(s_ctx, MODBUS_HOLDING_HEARTBEAT,
                                    s_heartbeat);
        (void)modbus_write_register(s_ctx, MODBUS_HOLDING_WD_TIMEOUT,
                                    s_watchdog_timeout);
        (void)modbus_write_bit(s_ctx, MODBUS_COIL_WD_ENABLE, 1);
    } else {
        (void)modbus_write_bit(s_ctx, MODBUS_COIL_WD_ENABLE, 0);
        (void)modbus_write_register(s_ctx, MODBUS_HOLDING_WD_TIMEOUT, 0);
    }
}

static int client_connect_locked(void)
{
    TickType_t now;

    if (s_connected) {
        return 0;
    }

    now = xTaskGetTickCount();
    if (now < s_next_retry) {
        return -1;
    }

    if (modbus_connect(s_ctx) != 0) {
        s_next_retry = now + pdMS_TO_TICKS(MODBUS_RETRY_MS);
        return -1;
    }

    s_connected = true;
    client_apply_watchdog_locked();
    return 0;
}

int modbus_client_init(const char *host, uint16_t port, uint8_t unit_id)
{
    char service[8];

    if ((host == NULL) || (s_ctx != NULL)) {
        return -1;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return -1;
    }

    (void)snprintf(service, sizeof(service), "%u", (unsigned)port);
    /* tcp_pi uses getaddrinfo: it accepts hostnames (e.g. "driver" in Docker)
     * as well as literal IPv4. modbus_new_tcp only accepts literal IPv4. */
    s_ctx = modbus_new_tcp_pi(host, service);
    if (s_ctx == NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return -1;
    }

    (void)modbus_set_slave(s_ctx, (int)unit_id);
    (void)modbus_set_response_timeout(s_ctx, 1U, 0U);

    s_connected = false;
    s_next_retry = 0;
    s_heartbeat = 1U;

    if (client_lock() != 0) {
        return -1;
    }
    (void)client_connect_locked();
    client_unlock();

    return s_connected ? 0 : -1;
}

void modbus_client_deinit(void)
{
    if (s_ctx != NULL) {
        if (s_connected) {
            (void)modbus_close(s_ctx);
        }
        modbus_free(s_ctx);
        s_ctx = NULL;
    }
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_connected = false;
}

bool modbus_client_is_connected(void)
{
    bool connected;

    if (client_lock() != 0) {
        return false;
    }
    connected = s_connected;
    client_unlock();
    return connected;
}

int modbus_client_last_diag(modbus_diag_t *out)
{
    if (out == NULL) {
        return -1;
    }
    *out = s_diag;
    return 0;
}

int modbus_client_read_light_state(modbus_light_state_t *state)
{
    uint16_t regs[MODBUS_INPUT_LIGHT_COUNT];

    if (state == NULL) {
        return -1;
    }
    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    if (modbus_read_input_registers(s_ctx, MODBUS_INPUT_LIGHT_ADDR,
                                    MODBUS_INPUT_LIGHT_COUNT, regs) !=
        MODBUS_INPUT_LIGHT_COUNT) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }

    state->power = regs[0];
    state->mode = regs[1];
    state->hue = regs[2];
    state->sat = regs[3];
    state->val = regs[4];
    state->cct = regs[5];
    state->brightness = regs[6];
    state->seq_step = regs[7];
    state->transition_progress = regs[8];

    client_unlock();
    return 0;
}

int modbus_client_read_diag(uint16_t *values, uint16_t count)
{
    if ((values == NULL) || (count == 0U) || (count > 21U)) {
        return -1;
    }
    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    if (modbus_read_input_registers(s_ctx, MODBUS_INPUT_DIAG_ADDR, (int)count,
                                    values) != (int)count) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    client_unlock();
    return 0;
}

int modbus_client_read_serial(char *out, size_t size)
{
    uint16_t regs[MODBUS_INPUT_SERIAL_TEXT_COUNT];
    size_t i;
    size_t used = 0U;

    if ((out == NULL) || (size == 0U)) {
        return -1;
    }
    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    if (modbus_read_input_registers(s_ctx, MODBUS_INPUT_SERIAL_TEXT_ADDR,
                                    MODBUS_INPUT_SERIAL_TEXT_COUNT, regs) !=
        MODBUS_INPUT_SERIAL_TEXT_COUNT) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    for (i = 0U; i < (size_t)MODBUS_INPUT_SERIAL_TEXT_COUNT; i++) {
        char high = (char)(regs[i] >> 8);
        char low = (char)(regs[i] & 0xFFU);

        if ((high == '\0') || ((used + 1U) >= size)) {
            break;
        }
        out[used++] = high;
        if ((low == '\0') || ((used + 1U) >= size)) {
            break;
        }
        out[used++] = low;
    }
    out[used] = '\0';
    client_unlock();
    return 0;
}

int modbus_client_read_status(uint8_t *bits, uint16_t count)
{
    if ((bits == NULL) || (count == 0U) || (count > 16U)) {
        return -1;
    }
    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    if (modbus_read_input_bits(s_ctx, 0, (int)count, bits) != (int)count) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    client_unlock();
    return 0;
}

int modbus_client_write_heartbeat(uint16_t value)
{
    int rc;

    if (client_lock() != 0) {
        return -1;
    }
    s_heartbeat = value;
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    rc = modbus_write_register(s_ctx, MODBUS_HOLDING_HEARTBEAT, value);
    if (rc != 1) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    client_unlock();
    return 0;
}

int modbus_client_write_registers(uint16_t addr, uint16_t count,
                                  const uint16_t *values)
{
    int rc;

    if ((values == NULL) || (count == 0U)) {
        return -1;
    }
    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    rc = modbus_write_registers(s_ctx, (int)addr, (int)count, values);
    if (rc != (int)count) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    client_unlock();
    return 0;
}

int modbus_client_write_coil(uint16_t addr, bool value)
{
    int rc;

    if (client_lock() != 0) {
        return -1;
    }
    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }
    rc = modbus_write_bit(s_ctx, (int)addr, value ? 1 : 0);
    if (rc != 1) {
        client_disconnect_locked();
        client_unlock();
        return -1;
    }
    client_unlock();
    return 0;
}

int modbus_client_set_watchdog(bool enabled, uint16_t timeout_s)
{
    if (client_lock() != 0) {
        return -1;
    }

    s_watchdog_enabled = enabled;
    s_watchdog_timeout = timeout_s;

    if (client_connect_locked() != 0) {
        client_unlock();
        return -1;
    }

    client_apply_watchdog_locked();
    client_unlock();
    return 0;
}

/* --- Tasks -------------------------------------------------------------- */

void v_light_poll_task(void *pv_parameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    TickType_t last_sent_tick = 0;
    TickType_t last_rest_tick = 0;
    modbus_light_state_t state;
    modbus_light_state_t last_sent = {0};
    bool have_sent = false;
    uint32_t poll_count = 0U;

    (void)pv_parameters;

    for (;;) {
        if (modbus_client_read_light_state(&state) == 0) {
            bool changed;
            TickType_t now = xTaskGetTickCount();

            changed = (!have_sent) ||
                      (state.power != last_sent.power) ||
                      (state.mode != last_sent.mode) ||
                      (abs((int)state.hue - (int)last_sent.hue) >= 2) ||
                      (abs((int)state.sat - (int)last_sent.sat) >= 10) ||
                      (abs((int)state.val - (int)last_sent.val) >= 10) ||
                      (abs((int)state.brightness -
                           (int)last_sent.brightness) >= 10) ||
                      (abs((int)state.cct - (int)last_sent.cct) >= 50) ||
                      ((state.transition_progress == 1000U) &&
                       (last_sent.transition_progress != 1000U));

            if (changed &&
                ((!have_sent) ||
                 (state.power != last_sent.power) ||
                 (state.mode != last_sent.mode) ||
                 ((now - last_sent_tick) >= pdMS_TO_TICKS(200U)))) {
                int rc = ws_client_send_state(&state);

                /* `ws_client_send_state` caches the state even when it
                 * fails, so the current one is resent on WS reconnect. */
                if ((rc != 0) && (!ws_client_is_connected())) {
                    if ((last_rest_tick == 0) ||
                        ((now - last_rest_tick) >= pdMS_TO_TICKS(1000U))) {
                        rc = telemetry_send_state(&state);
                        last_rest_tick = now;
                    }
                }
                if (rc == 0) {
                    last_sent = state;
                    have_sent = true;
                    last_sent_tick = now;
                }
            }

            if ((poll_count % 20U) == 0U) {
                LOG_INFO("light: pwr=%u h=%u s=%u v=%u bri=%u prog=%u",
                         (unsigned)state.power, (unsigned)state.hue,
                         (unsigned)state.sat, (unsigned)state.val,
                         (unsigned)state.brightness,
                         (unsigned)state.transition_progress);
            } else {
                LOG_DEBUG("light: pwr=%u h=%u s=%u v=%u bri=%u prog=%u",
                          (unsigned)state.power, (unsigned)state.hue,
                          (unsigned)state.sat, (unsigned)state.val,
                          (unsigned)state.brightness,
                          (unsigned)state.transition_progress);
            }
            poll_count++;
        } else {
            LOG_DEBUG("light: no Modbus connection");
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50U));
    }
}

void v_heartbeat_task(void *pv_parameters)
{
    uint16_t counter = 1U;

    (void)pv_parameters;

    for (;;) {
        (void)modbus_client_write_heartbeat(counter);
        LOG_DEBUG("heartbeat=%u", (unsigned)counter);
        counter++;
        if (counter == 0U) {
            counter = 1U;
        }
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

void v_diag_poll_task(void *pv_parameters)
{
    /* 30011..30026 (16 registers): includes the hardware revision and the
     * 32-bit serial. The driver exposes nothing past 30031 and an out-of-range
     * read would return a Modbus exception. */
    uint16_t diag[16];

    (void)pv_parameters;

    for (;;) {
        if (modbus_client_read_diag(diag, 16U) == 0) {
            s_diag.valid = true;
            s_diag.temp_c_x10 = (int16_t)diag[0];
            s_diag.current_ma = diag[1];
            s_diag.voltage_mv = diag[2];
            s_diag.power_mw = diag[3];
            s_diag.fault_code = diag[4];
            s_diag.fault_count = diag[5];
            s_diag.uptime_s = ((uint32_t)diag[7] << 16U) | diag[6];
            s_diag.fw_major = diag[10];
            s_diag.fw_minor = diag[11];
            s_diag.fw_patch = diag[12];
            s_diag.hw_rev = diag[13];
            s_diag.serial_id = ((uint32_t)diag[15] << 16U) | diag[14];
            if (modbus_client_read_serial(s_diag.serial_text,
                                          sizeof(s_diag.serial_text)) != 0) {
                s_diag.serial_text[0] = '\0';
            }

            LOG_DEBUG("diag: temp=%d.%dC cur=%umA fault=%u fw=%u.%u.%u "
                      "serial=%s",
                      (int)(s_diag.temp_c_x10 / 10),
                      (int)(s_diag.temp_c_x10 % 10),
                      (unsigned)s_diag.current_ma, (unsigned)s_diag.fault_code,
                      (unsigned)s_diag.fw_major, (unsigned)s_diag.fw_minor,
                      (unsigned)s_diag.fw_patch,
                      (s_diag.serial_text[0] != '\0') ? s_diag.serial_text
                                                       : "?");
        }
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

void v_watchdog_task(void *pv_parameters)
{
    uint8_t status[11];
    bool last_fault = false;
    bool last_lost = false;

    (void)pv_parameters;

    for (;;) {
        if (modbus_client_read_status(status, 11U) == 0) {
            bool fault = status[MODBUS_STATUS_FAULT_ACTIVE] != 0U;
            bool lost = status[MODBUS_STATUS_HEARTBEAT_LOST] != 0U;

            if (fault && !last_fault) {
                LOG_WARN("driver: active fault");
            } else if (!fault && last_fault) {
                LOG_INFO("driver: fault cleared");
            }

            if (lost && !last_lost) {
                LOG_WARN("driver: heartbeat lost");
            } else if (!lost && last_lost) {
                LOG_INFO("driver: heartbeat recovered");
            }

            last_fault = fault;
            last_lost = lost;
        }
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
}
