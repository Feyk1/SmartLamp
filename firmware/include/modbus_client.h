#ifndef MODBUS_CLIENT_H
#define MODBUS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t power;
    uint16_t mode;
    uint16_t hue;
    uint16_t sat;
    uint16_t val;
    uint16_t cct;
    uint16_t brightness;
    uint16_t seq_step;
    uint16_t transition_progress;
} modbus_light_state_t;

/** @brief Last diagnostic read from the driver (30011-30026). */
typedef struct {
    bool valid;
    int16_t temp_c_x10;
    uint16_t current_ma;
    uint16_t voltage_mv;
    uint16_t power_mw;
    uint16_t fault_code;
    uint16_t fault_count;
    uint32_t uptime_s;
    uint16_t fw_major;
    uint16_t fw_minor;
    uint16_t fw_patch;
    uint16_t hw_rev;
    uint32_t serial_id;
    char serial_text[25];
} modbus_diag_t;

/* 0-based indices of the relevant discrete inputs (10001 + index). */
#define MODBUS_STATUS_FAULT_ACTIVE   3U
#define MODBUS_STATUS_HEARTBEAT_LOST 8U

int modbus_client_init(const char *host, uint16_t port, uint8_t unit_id);
void modbus_client_deinit(void);
bool modbus_client_is_connected(void);

int modbus_client_read_light_state(modbus_light_state_t *state);
int modbus_client_read_diag(uint16_t *values, uint16_t count);
int modbus_client_read_serial(char *out, size_t size);
int modbus_client_read_status(uint8_t *bits, uint16_t count);

/**
 * @brief Copies the last diagnostic read by `v_diag_poll_task`.
 *
 * @param out Destination; `valid` tells whether there is a previous read.
 * @return 0 if copied; -1 if `out` is NULL.
 */
int modbus_client_last_diag(modbus_diag_t *out);

int modbus_client_write_heartbeat(uint16_t value);
int modbus_client_write_registers(uint16_t addr, uint16_t count,
                                  const uint16_t *values);
int modbus_client_write_coil(uint16_t addr, bool value);

/**
 * @brief Configures the heartbeat watchdog in the driver.
 *
 * The configuration is remembered and re-applied on every reconnection.
 *
 * @param enabled true to enable it.
 * @param timeout_s Timeout in seconds (0 = disabled).
 * @return 0 if applied or left pending; -1 on error.
 */
int modbus_client_set_watchdog(bool enabled, uint16_t timeout_s);

#endif /* MODBUS_CLIENT_H */
