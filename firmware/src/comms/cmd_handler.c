/**
 * @file cmd_handler.c
 * @brief Command interpreter (WS and REST fallback).
 *
 * Turns every protocol `op` into Modbus writes against the LED driver.
 * The addresses (0-based) are in docs/03.
 */

#include "cmd_handler.h"

#include <stdlib.h>
#include <string.h>

/* Coils (0-based, docs/03). */
#define COIL_APPLY_TARGET   0U
#define COIL_START_SEQUENCE 3U
#define COIL_STOP_SEQUENCE  4U
#define COIL_CLEAR_FAULTS   8U
#define COIL_SELFTEST       9U

/* Holding registers (0-based, docs/03). */
#define HOLD_TARGET_POWER      0U
#define HOLD_TARGET_MODE       1U
#define HOLD_TARGET_HUE        2U
#define HOLD_TARGET_SAT        3U
#define HOLD_TARGET_VAL        4U
#define HOLD_TARGET_CCT        5U
#define HOLD_TARGET_BRIGHTNESS 6U
#define HOLD_TARGET_TRANSITION 7U
#define HOLD_SEQ_ID            10U
#define HOLD_SEQ_SPEED         11U
#define HOLD_SEQ_DIRECTION     12U
#define HOLD_SEQ_LOOP          13U

/**
 * @brief Reads an integer from the payload and clamps it to [min, max].
 */
static int cmd_json_int(const cJSON *obj, const char *key, int def, int min,
                        int max)
{
    const cJSON *item;
    int value;

    if (obj == NULL) {
        return def;
    }
    item = cJSON_GetObjectItemCaseSensitive(obj, key);
    value = cJSON_IsNumber(item) ? item->valueint : def;
    if (value < min) {
        value = min;
    }
    if (value > max) {
        value = max;
    }
    return value;
}

/**
 * @brief Writes registers and applies the target if the write succeeded.
 */
static int cmd_write_and_apply(const cmd_io_t *io, uint16_t addr,
                               uint16_t count, const uint16_t *values)
{
    int rc = io->write_registers(io->ctx, addr, count, values);

    if (rc == 0) {
        rc = io->write_coil(io->ctx, COIL_APPLY_TARGET, true);
    }
    return rc;
}

/**
 * @brief Runs `set_power`/`set_brightness`/... and returns the Modbus rc.
 *
 * @param op      Requested operation.
 * @param payload JSON payload of the command.
 * @param io      Modbus writes.
 * @param handler Reply channel (for `request_state`).
 * @param handled Set to false if the `op` does not exist.
 * @return 0 on success; -1 on error; -2 if the parameters are invalid.
 */
static int cmd_execute(const char *op, const cJSON *payload, const cmd_io_t *io,
                       const cmd_handler_t *handler, bool *handled)
{
    uint16_t regs[4];
    int rc = -1;

    *handled = true;

    if (strcmp(op, "set_power") == 0) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(payload, "on");
        if (!cJSON_IsBool(on)) {
            return -2;
        }
        regs[0] = cJSON_IsTrue(on) ? 1U : 0U;
        rc = cmd_write_and_apply(io, HOLD_TARGET_POWER, 1U, regs);
    } else if (strcmp(op, "set_brightness") == 0) {
        if (!cJSON_IsNumber(
                cJSON_GetObjectItemCaseSensitive(payload, "value"))) {
            return -2;
        }
        regs[0] = (uint16_t)cmd_json_int(payload, "value", 0, 0, 1000);
        rc = cmd_write_and_apply(io, HOLD_TARGET_BRIGHTNESS, 1U, regs);
    } else if (strcmp(op, "set_color") == 0) {
        if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(payload, "h")) ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(payload, "s")) ||
            !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(payload, "v"))) {
            return -2;
        }
        regs[0] = (uint16_t)cmd_json_int(payload, "h", 0, 0, 360);
        regs[1] = (uint16_t)cmd_json_int(payload, "s", 0, 0, 1000);
        regs[2] = (uint16_t)cmd_json_int(payload, "v", 0, 0, 1000);
        rc = io->write_registers(io->ctx, HOLD_TARGET_HUE, 3U, regs);
        if (rc == 0) {
            const cJSON *tr =
                cJSON_GetObjectItemCaseSensitive(payload, "transition_ms");
            if (cJSON_IsNumber(tr)) {
                regs[0] = (uint16_t)cmd_json_int(payload, "transition_ms", 0, 0,
                                                 10000);
                rc = io->write_registers(io->ctx, HOLD_TARGET_TRANSITION, 1U,
                                         regs);
            }
        }
        if (rc == 0) {
            rc = io->write_coil(io->ctx, COIL_APPLY_TARGET, true);
        }
    } else if (strcmp(op, "set_color_temperature") == 0) {
        if (!cJSON_IsNumber(
                cJSON_GetObjectItemCaseSensitive(payload, "kelvin"))) {
            return -2;
        }
        regs[0] = (uint16_t)cmd_json_int(payload, "kelvin", 2700, 2000, 6500);
        rc = cmd_write_and_apply(io, HOLD_TARGET_CCT, 1U, regs);
    } else if (strcmp(op, "set_mode") == 0) {
        const char *mode = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(payload, "mode"));
        if (mode == NULL) {
            return -2;
        }
        if (strcmp(mode, "manual") == 0) {
            regs[0] = 0U;
        } else if (strcmp(mode, "cct") == 0) {
            regs[0] = 1U;
        } else if (strcmp(mode, "sequence") == 0) {
            regs[0] = 2U;
        } else if (strcmp(mode, "scene") == 0) {
            regs[0] = 3U;
        } else {
            return -2;
        }
        rc = cmd_write_and_apply(io, HOLD_TARGET_MODE, 1U, regs);
    } else if (strcmp(op, "start_sequence") == 0) {
        regs[0] = (uint16_t)cmd_json_int(payload, "id", 1, 1, 255);
        regs[1] = (uint16_t)cmd_json_int(payload, "speed", 1000, 1, 10000);
        regs[2] = (uint16_t)cmd_json_int(payload, "direction", 0, 0, 2);
        regs[3] = (uint16_t)cmd_json_int(payload, "loop", 0, 0, 65535);
        rc = io->write_registers(io->ctx, HOLD_SEQ_ID, 4U, regs);
        if (rc == 0) {
            rc = io->write_coil(io->ctx, COIL_START_SEQUENCE, true);
        }
    } else if (strcmp(op, "stop_sequence") == 0) {
        rc = io->write_coil(io->ctx, COIL_STOP_SEQUENCE, true);
    } else if (strcmp(op, "clear_faults") == 0) {
        rc = io->write_coil(io->ctx, COIL_CLEAR_FAULTS, true);
    } else if (strcmp(op, "run_selftest") == 0) {
        rc = io->write_coil(io->ctx, COIL_SELFTEST, true);
    } else if (strcmp(op, "ota_update") == 0) {
        const char *version = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(payload, "version"));
        if ((version == NULL) || (version[0] == '\0')) {
            return -2;
        }
        rc = (handler->ota_update != NULL)
                 ? handler->ota_update(handler->ctx, version)
                 : -1;
    } else if (strcmp(op, "sim_reboot") == 0) {
        rc = (handler->reboot != NULL) ? handler->reboot(handler->ctx) : -1;
    } else if (strcmp(op, "request_state") == 0) {
        rc = (handler->send_state != NULL) ? handler->send_state(handler->ctx)
                                           : -1;
    } else {
        *handled = false;
        return -1;
    }

    return rc;
}

int cmd_handler_handle(const cJSON *root, const cmd_io_t *io,
                       const cmd_handler_t *handler)
{
    const char *id;
    const char *op;
    const cJSON *payload;
    const char *code = "INTERNAL";
    bool handled = false;
    int rc;

    if ((root == NULL) || (io == NULL) || (handler == NULL) ||
        (handler->reply == NULL)) {
        return -1;
    }

    id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
    op = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "op"));
    payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (op == NULL) {
        handler->reply(handler->ctx, id, NULL, false, "INVALID_PARAM");
        return -1;
    }

    rc = cmd_execute(op, payload, io, handler, &handled);
    if (!handled) {
        handler->reply(handler->ctx, id, op, false, "NOT_SUPPORTED");
        return -1;
    }

    if (rc == -2) {
        handler->reply(handler->ctx, id, op, false, "INVALID_PARAM");
        return 0;
    }
    if (rc != 0) {
        code = (strcmp(op, "request_state") == 0) ? "INVALID_STATE"
                                                  : "INTERNAL";
    }
    handler->reply(handler->ctx, id, op, rc == 0, code);
    return 0;
}
