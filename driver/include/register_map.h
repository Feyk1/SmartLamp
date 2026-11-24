#ifndef REGISTER_MAP_H
#define REGISTER_MAP_H

#include <stdbool.h>
#include <stdint.h>

/* 0-based addresses: 00001 -> index 0, 40001 -> index 0, etc. */

typedef enum {
    COIL_CMD_APPLY_TARGET = 0,
    COIL_CMD_POWER_ON = 1,
    COIL_CMD_POWER_OFF = 2,
    COIL_CMD_START_SEQUENCE = 3,
    COIL_CMD_STOP_SEQUENCE = 4,
    COIL_CMD_SAVE_CONFIG = 5,
    COIL_CMD_LOAD_CONFIG = 6,
    COIL_CMD_FACTORY_RESET = 7,
    COIL_CMD_CLEAR_FAULTS = 8,
    COIL_CMD_SELFTEST = 9,
    COIL_ENABLE_HEARTBEAT_WATCHDOG = 10,
    COIL_COUNT = 16
} register_map_coil_t;

typedef enum {
    DISCRETE_STATUS_POWER_ON = 0,
    DISCRETE_STATUS_SEQUENCE_RUNNING = 1,
    DISCRETE_STATUS_CONFIG_DIRTY = 2,
    DISCRETE_STATUS_FAULT_ACTIVE = 3,
    DISCRETE_STATUS_OVERTEMP_WARNING = 4,
    DISCRETE_STATUS_OVERTEMP_CRITICAL = 5,
    DISCRETE_STATUS_OVERCURRENT = 6,
    DISCRETE_STATUS_LED_OPEN = 7,
    DISCRETE_STATUS_HEARTBEAT_LOST = 8,
    DISCRETE_STATUS_SELFTEST_RUNNING = 9,
    DISCRETE_STATUS_SEQUENCE_INVALID = 10,
    DISCRETE_STATUS_FAULT_LATCHED = 11,
    DISCRETE_STATUS_CONFIG_SAFE_MODE = 12,
    DISCRETE_COUNT = 16
} register_map_discrete_t;

typedef enum {
    HOLDING_TARGET_POWER = 0,
    HOLDING_TARGET_MODE = 1,
    HOLDING_TARGET_HUE = 2,
    HOLDING_TARGET_SAT = 3,
    HOLDING_TARGET_VAL = 4,
    HOLDING_TARGET_CCT = 5,
    HOLDING_TARGET_BRIGHTNESS = 6,
    HOLDING_TARGET_TRANSITION_MS = 7,
    HOLDING_TARGET_SCENE_ID = 8,
    HOLDING_SEQ_ID = 10,
    HOLDING_SEQ_SPEED = 11,
    HOLDING_SEQ_DIRECTION = 12,
    HOLDING_SEQ_LOOP_COUNT = 13,
    HOLDING_SEQ_STEP_INDEX = 14,
    HOLDING_SEQ_TOTAL_STEPS = 15,
    HOLDING_SEQ_STEP_HUE = 16,
    HOLDING_SEQ_STEP_SAT = 17,
    HOLDING_SEQ_STEP_VAL = 18,
    HOLDING_SEQ_STEP_RESERVED = 19,
    HOLDING_SEQ_STEP_BRIGHTNESS = 20,
    HOLDING_SEQ_STEP_DURATION = 21,
    HOLDING_SEQ_STEP_TRANSITION = 22,
    HOLDING_SEQ_STEP_CCT = 23,
    HOLDING_SEQ_CRC = 24,
    HOLDING_CFG_MAX_BRIGHTNESS = 30,
    HOLDING_CFG_MIN_BRIGHTNESS = 31,
    HOLDING_CFG_POWER_ON_BEHAVIOR = 32,
    HOLDING_CFG_GAMMA = 33,
    HOLDING_CFG_HEARTBEAT_TIMEOUT_S = 34,
    HOLDING_CFG_OVERTEMP_WARN_C = 35,
    HOLDING_CFG_OVERTEMP_CRIT_C = 36,
    HOLDING_CFG_MAX_CURRENT_MA = 37,
    HOLDING_HEARTBEAT_COUNTER = 40,
    /* Simulation registers (Sim Panel, docs/07): forced fault. */
    HOLDING_SIM_FAULT_CODE = 45,
    HOLDING_SIM_FAULT_DURATION_S = 46,
    HOLDING_SIM_FAULT_MAGIC = 47,
    HOLDING_COUNT = 50
} register_map_holding_t;

typedef enum {
    INPUT_ACTUAL_POWER = 0,
    INPUT_ACTUAL_MODE = 1,
    INPUT_ACTUAL_HUE = 2,
    INPUT_ACTUAL_SAT = 3,
    INPUT_ACTUAL_VAL = 4,
    INPUT_ACTUAL_CCT = 5,
    INPUT_ACTUAL_BRIGHTNESS = 6,
    INPUT_ACTUAL_SEQ_STEP = 7,
    INPUT_ACTUAL_TRANSITION_PROGRESS = 8,
    INPUT_DIAG_TEMP_C_X10 = 10,
    INPUT_DIAG_CURRENT_MA = 11,
    INPUT_DIAG_VOLTAGE_MV = 12,
    INPUT_DIAG_POWER_MW = 13,
    INPUT_DIAG_FAULT_CODE = 14,
    INPUT_DIAG_FAULT_COUNT = 15,
    INPUT_DIAG_UPTIME_S_LO = 16,
    INPUT_DIAG_UPTIME_S_HI = 17,
    INPUT_DIAG_ON_TIME_S_LO = 18,
    INPUT_DIAG_ON_TIME_S_HI = 19,
    INPUT_DIAG_FW_MAJOR = 20,
    INPUT_DIAG_FW_MINOR = 21,
    INPUT_DIAG_FW_PATCH = 22,
    INPUT_DIAG_HW_REV = 23,
    INPUT_DIAG_SERIAL_LO = 24,
    INPUT_DIAG_SERIAL_HI = 25,
    INPUT_DIAG_MIN_FREE_HEAP = 26,
    INPUT_DIAG_LAST_FAULT_TS_LO = 27,
    INPUT_DIAG_LAST_FAULT_TS_HI = 28,
    INPUT_DIAG_LAST_FAULT_CODE = 29,
    INPUT_DIAG_HEARTBEAT_ECHO = 30,
    /* ASCII product serial: 12 registers, 2 chars each (high byte first). */
    INPUT_DIAG_SERIAL_TEXT = 31,
    INPUT_COUNT = 48
} register_map_input_t;

/* Value for HOLDING_SIM_FAULT_MAGIC to arm a forced fault. */
#define SIM_FAULT_MAGIC 0x5A5AU

typedef enum {
    FAULT_NONE = 0,
    FAULT_OVERTEMP_WARN = 1,
    FAULT_OVERTEMP_CRIT = 2,
    FAULT_OVERCURRENT = 3,
    FAULT_LED_OPEN = 4,
    FAULT_CONFIG_CORRUPT = 5,
    FAULT_SEQ_INVALID = 6,
    FAULT_HEARTBEAT_LOST = 7
} register_map_fault_t;

void register_map_init(void);
void register_map_lock(void);
void register_map_unlock(void);

void register_map_tick(uint32_t elapsed_ms);
void register_map_watchdog_tick(void);
void register_map_apply_command(uint16_t coil_index);

void register_map_get_coils(uint8_t *out, uint16_t count);
void register_map_get_discrete(uint8_t *out, uint16_t count);
void register_map_get_holding(uint16_t *out, uint16_t count);
void register_map_get_input(uint16_t *out, uint16_t count);

void register_map_set_coils(const uint8_t *values, uint16_t count);
void register_map_set_holding(const uint16_t *values, uint16_t count);

uint8_t register_map_get_coil_bit(uint16_t index);
uint8_t register_map_get_discrete_bit(uint16_t index);
uint16_t register_map_get_holding_reg(uint16_t index);
uint16_t register_map_get_input_reg(uint16_t index);

#endif /* REGISTER_MAP_H */
