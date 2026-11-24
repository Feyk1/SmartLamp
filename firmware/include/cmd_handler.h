#ifndef CMD_HANDLER_H
#define CMD_HANDLER_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * @brief Modbus writes that execute a command.
 *
 * They are injected so the handler can be tested without the driver or
 * libmodbus; the real firmware passes
 * `modbus_client_write_registers`/`_coil`.
 */
typedef struct {
    void *ctx;
    int (*write_registers)(void *ctx, uint16_t addr, uint16_t count,
                           const uint16_t *values);
    int (*write_coil)(void *ctx, uint16_t addr, bool value);
} cmd_io_t;

/**
 * @brief Command reply channel (WS or REST fallback).
 */
typedef struct {
    void *ctx;
    void (*reply)(void *ctx, const char *id, const char *op, bool ok,
                  const char *code);
    int (*send_state)(void *ctx);
    /* Enqueues an OTA update (op "ota_update"); may be NULL. */
    int (*ota_update)(void *ctx, const char *version);
    /* Enqueues a reboot (op "sim_reboot"); may be NULL. */
    int (*reboot)(void *ctx);
} cmd_handler_t;

/**
 * @brief Executes a `{"id","op","payload"}` command.
 *
 * Always replies through `handler->reply` (ack/nack), unless `op` is missing
 * or the `op` does not exist; in those cases it replies with `INVALID_PARAM`
 * or `NOT_SUPPORTED`.
 *
 * @param root Command already parsed with cJSON.
 * @param io   Modbus writes.
 * @param handler Reply channel.
 * @return 0 if it was handled; -1 if arguments are missing.
 */
int cmd_handler_handle(const cJSON *root, const cmd_io_t *io,
                       const cmd_handler_t *handler);

#endif /* CMD_HANDLER_H */
