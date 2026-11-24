/**
 * @file test_register_map.c
 * @brief Unit tests for the driver register map (Unity).
 */

#include "unity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "register_map.h"
#include "sequence.h"

static void clear_identity_env(void)
{
    (void)unsetenv("DEVICE_IDENTITY_PATH");
    (void)unsetenv("DEVICE_SERIAL");
    (void)unsetenv("DEVICE_ID");
    (void)unsetenv("PROVISION_LOT");
    (void)unsetenv("PROVISION_WEEK");
    (void)unsetenv("PROVISION_ID");
}

void setUp(void)
{
    unsetenv("DRIVER_STATE_PATH");
    clear_identity_env();
    register_map_init();
}

void tearDown(void)
{
    unsetenv("DRIVER_STATE_PATH");
    clear_identity_env();
    register_map_init();
}

static void set_target(uint16_t power, uint16_t hue, uint16_t sat,
                       uint16_t val, uint16_t brightness, uint16_t transition)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_TARGET_POWER] = power;
    h[HOLDING_TARGET_MODE] = 0U;
    h[HOLDING_TARGET_HUE] = hue;
    h[HOLDING_TARGET_SAT] = sat;
    h[HOLDING_TARGET_VAL] = val;
    h[HOLDING_TARGET_BRIGHTNESS] = brightness;
    h[HOLDING_TARGET_CCT] = 2700U;
    h[HOLDING_TARGET_TRANSITION_MS] = transition;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
}

static void test_defaults(void)
{
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(2700U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_CCT));
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MAX_BRIGHTNESS));
}

static void test_apply_target_without_transition(void)
{
    set_target(1U, 100U, 800U, 600U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(100U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
    TEST_ASSERT_EQUAL_UINT16(800U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_SAT));
    TEST_ASSERT_EQUAL_UINT16(600U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_VAL));
    TEST_ASSERT_EQUAL_UINT16(800U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_out_of_range_writes_are_clamped(void)
{
    set_target(1U, 9999U, 5000U, 5000U, 5000U, 0U);

    TEST_ASSERT_EQUAL_UINT16(360U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_HUE));
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_SAT));
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_VAL));
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_BRIGHTNESS));

    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(360U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_invalid_config_values_fall_back(void)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_TARGET_MODE] = 9U;
    h[HOLDING_TARGET_TRANSITION_MS] = 60000U;
    h[HOLDING_CFG_GAMMA] = 9U;
    h[HOLDING_CFG_POWER_ON_BEHAVIOR] = 9U;
    h[HOLDING_SIM_FAULT_CODE] = 9U;
    h[HOLDING_SIM_FAULT_DURATION_S] = 9999U;
    h[HOLDING_CFG_OVERTEMP_WARN_C] = 0xFE70U; /* -400 */
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);

    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_MODE));
    TEST_ASSERT_EQUAL_UINT16(10000U,
        register_map_get_holding_reg(
            (uint16_t)HOLDING_TARGET_TRANSITION_MS));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_GAMMA));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_holding_reg(
            (uint16_t)HOLDING_CFG_POWER_ON_BEHAVIOR));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_holding_reg((uint16_t)HOLDING_SIM_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(3600U,
        register_map_get_holding_reg(
            (uint16_t)HOLDING_SIM_FAULT_DURATION_S));
    TEST_ASSERT_EQUAL_UINT16(0xFE70U,
        register_map_get_holding_reg(
            (uint16_t)HOLDING_CFG_OVERTEMP_WARN_C));
}

static void test_brightness_config_limits(void)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_MIN_BRIGHTNESS] = 200U;
    h[HOLDING_CFG_MAX_BRIGHTNESS] = 700U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);

    set_target(1U, 0U, 1000U, 1000U, 1000U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(700U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));

    set_target(1U, 0U, 1000U, 1000U, 50U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(200U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_brightness_limits_are_sorted(void)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_MIN_BRIGHTNESS] = 800U;
    h[HOLDING_CFG_MAX_BRIGHTNESS] = 200U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);

    set_target(1U, 0U, 1000U, 1000U, 1000U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(800U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));

    set_target(1U, 0U, 1000U, 1000U, 100U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(200U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_negative_overtemp_threshold(void)
{
    uint16_t h[HOLDING_COUNT];

    set_target(1U, 0U, 1000U, 1000U, 0U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_OVERTEMP_WARN_C] = 0xFF9CU; /* -100 */
    h[HOLDING_CFG_OVERTEMP_CRIT_C] = 1000U;   /* 100.0 C */
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_OVERTEMP_WARNING));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERTEMP_WARN,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
}

static void test_transition_interpolates(void)
{
    uint16_t hue;

    set_target(1U, 100U, 800U, 600U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    set_target(1U, 200U, 800U, 600U, 800U, 1000U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(500U);
    hue = register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE);
    TEST_ASSERT_TRUE(hue >= 140U);
    TEST_ASSERT_TRUE(hue <= 160U);

    register_map_tick(600U);
    TEST_ASSERT_EQUAL_UINT16(200U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
}

static void test_power_off(void)
{
    set_target(1U, 100U, 800U, 600U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_apply_command((uint16_t)COIL_CMD_POWER_OFF);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_VAL));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_sequence_rainbow(void)
{
    uint16_t h[HOLDING_COUNT];
    uint16_t hue1;
    uint16_t hue2;

    set_target(1U, 0U, 1000U, 1000U, 1000U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_SEQ_ID] = 2U;
    h[HOLDING_SEQ_SPEED] = 1000U;
    h[HOLDING_SEQ_DIRECTION] = 0U;
    h[HOLDING_SEQ_LOOP_COUNT] = 0U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);

    register_map_apply_command((uint16_t)COIL_CMD_START_SEQUENCE);
    register_map_tick(100U);
    hue1 = register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE);
    register_map_tick(100U);
    hue2 = register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE);

    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_SEQUENCE_RUNNING));
    TEST_ASSERT_TRUE(hue1 != hue2);

    register_map_apply_command((uint16_t)COIL_CMD_STOP_SEQUENCE);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_SEQUENCE_RUNNING));
}

static void upload_custom(const sequence_step_t *steps, uint16_t count,
                         uint16_t crc)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_SEQ_TOTAL_STEPS] = count;
    for (uint16_t i = 0U; i < count; i++) {
        h[HOLDING_SEQ_STEP_INDEX] = i;
        h[HOLDING_SEQ_STEP_HUE] = steps[i].hue;
        h[HOLDING_SEQ_STEP_SAT] = steps[i].sat;
        h[HOLDING_SEQ_STEP_VAL] = steps[i].val;
        h[HOLDING_SEQ_STEP_RESERVED] = 0U;
        h[HOLDING_SEQ_STEP_BRIGHTNESS] = steps[i].brightness;
        h[HOLDING_SEQ_STEP_DURATION] = steps[i].duration_10ms;
        h[HOLDING_SEQ_STEP_TRANSITION] = steps[i].transition_10ms;
        h[HOLDING_SEQ_STEP_CCT] = steps[i].cct;
        register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    }
    h[HOLDING_SEQ_CRC] = crc;
    h[HOLDING_SEQ_ID] = 100U;
    h[HOLDING_SEQ_SPEED] = 1000U;
    h[HOLDING_SEQ_DIRECTION] = 0U;
    h[HOLDING_SEQ_LOOP_COUNT] = 0U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
}

static void test_custom_sequence_runs(void)
{
    const sequence_step_t steps[2] = {
        {10U, 900U, 1000U, 0U, 800U, 100U, 0U, 2700U},
        {200U, 900U, 1000U, 0U, 600U, 100U, 0U, 4000U},
    };
    uint16_t crc = sequence_crc16(steps, 2U);

    upload_custom(steps, 2U, crc);
    register_map_apply_command((uint16_t)COIL_CMD_START_SEQUENCE);
    register_map_tick(100U);

    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_SEQUENCE_RUNNING));
    TEST_ASSERT_EQUAL_UINT16(10U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_SEQUENCE_INVALID));

    register_map_tick(1000U);
    TEST_ASSERT_EQUAL_UINT16(200U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
}

static void test_custom_sequence_rejects_bad_crc(void)
{
    const sequence_step_t steps[1] = {
        {10U, 900U, 1000U, 0U, 800U, 100U, 0U, 2700U},
    };

    upload_custom(steps, 1U, (uint16_t)(sequence_crc16(steps, 1U) ^ 0x1234U));
    register_map_apply_command((uint16_t)COIL_CMD_START_SEQUENCE);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_SEQUENCE_RUNNING));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_SEQUENCE_INVALID));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_SEQ_INVALID,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
}

static void test_state_persists_across_restart(void)
{
    char path[] = "/tmp/lamp-state-XXXXXX";
    int fd = mkstemp(path);

    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    (void)unlink(path);
    TEST_ASSERT_EQUAL_INT(0, setenv("DRIVER_STATE_PATH", path, 1));

    register_map_init();
    set_target(1U, 123U, 800U, 700U, 650U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(3000U); /* forces the periodic save */

    register_map_init(); /* simulates a driver restart */

    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(123U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
    TEST_ASSERT_EQUAL_UINT16(650U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_holding_reg((uint16_t)HOLDING_TARGET_POWER));

    (void)unlink(path);
}

static void test_serial_from_env(void)
{
    uint16_t lo;
    uint16_t hi;

    (void)setenv("DEVICE_SERIAL", "0x12345678", 1);
    register_map_init();
    TEST_ASSERT_EQUAL_UINT16(0x5678U,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_LO));
    TEST_ASSERT_EQUAL_UINT16(0x1234U,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_HI));

    /* Text values are hashed and stay stable across restarts. */
    (void)setenv("DEVICE_SERIAL", "unit-42", 1);
    register_map_init();
    lo = register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_LO);
    hi = register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_HI);
    register_map_init();
    TEST_ASSERT_EQUAL_UINT16(lo,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_LO));
    TEST_ASSERT_EQUAL_UINT16(hi,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_HI));

    (void)unsetenv("DEVICE_SERIAL");
    (void)unsetenv("DEVICE_ID");
    register_map_init();
    TEST_ASSERT_EQUAL_UINT16(0x0001U,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_LO));
    TEST_ASSERT_EQUAL_UINT16(0x8F2AU,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_HI));
}

static void read_serial_text(char *out, size_t size)
{
    size_t used = 0U;
    uint16_t i;

    for (i = 0U; i < 12U; i++) {
        uint16_t reg = register_map_get_input_reg(
            (uint16_t)(INPUT_DIAG_SERIAL_TEXT + i));
        char high = (char)(reg >> 8U);
        char low = (char)(reg & 0xFFU);

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
}

static void test_identity_is_provisioned_once(void)
{
    char path[] = "/tmp/lamp-identity-XXXXXX";
    char first[25];
    char second[25];
    struct stat info;
    int fd = mkstemp(path);

    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    (void)unlink(path);
    TEST_ASSERT_EQUAL_INT(0, setenv("DEVICE_IDENTITY_PATH", path, 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("PROVISION_LOT", "LB", 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("PROVISION_WEEK", "2647", 1));
    TEST_ASSERT_EQUAL_INT(0, setenv("PROVISION_ID", "0x12345678", 1));

    register_map_init();
    TEST_ASSERT_EQUAL_UINT16(0x5678U,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_LO));
    TEST_ASSERT_EQUAL_UINT16(0x1234U,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_SERIAL_HI));
    read_serial_text(first, sizeof(first));
    TEST_ASSERT_EQUAL_STRING("SL-1-LB-2647-12345678", first);
    TEST_ASSERT_EQUAL_INT(0, stat(path, &info));
    TEST_ASSERT_EQUAL_UINT32(0444U, (uint32_t)(info.st_mode & 0777U));

    /* A later boot reads the existing identity and ignores new env values. */
    TEST_ASSERT_EQUAL_INT(0, setenv("PROVISION_ID", "0xDEADBEEF", 1));
    register_map_init();
    read_serial_text(second, sizeof(second));
    TEST_ASSERT_EQUAL_STRING(first, second);

    /* Factory reset keeps the factory-programmed identity. */
    register_map_apply_command((uint16_t)COIL_CMD_FACTORY_RESET);
    read_serial_text(second, sizeof(second));
    TEST_ASSERT_EQUAL_STRING(first, second);

    (void)unlink(path);
    (void)unsetenv("DEVICE_IDENTITY_PATH");
    (void)unsetenv("PROVISION_LOT");
    (void)unsetenv("PROVISION_WEEK");
    (void)unsetenv("PROVISION_ID");
}

static void test_config_persists_across_restart(void)
{
    char path[] = "/tmp/lamp-state-cfg-XXXXXX";
    int fd = mkstemp(path);
    uint16_t h[HOLDING_COUNT];

    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    (void)unlink(path);
    TEST_ASSERT_EQUAL_INT(0, setenv("DRIVER_STATE_PATH", path, 1));

    register_map_init();
    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_MAX_BRIGHTNESS] = 600U;
    h[HOLDING_CFG_MIN_BRIGHTNESS] = 150U;
    h[HOLDING_CFG_GAMMA] = 2U;
    h[HOLDING_CFG_OVERTEMP_WARN_C] = 550U;
    h[HOLDING_CFG_OVERTEMP_CRIT_C] = 750U;
    h[HOLDING_CFG_MAX_CURRENT_MA] = 900U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_tick(3000U); /* forces the periodic save */

    register_map_init(); /* simulates a driver restart */

    TEST_ASSERT_EQUAL_UINT16(600U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MAX_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT16(150U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MIN_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT16(2U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_GAMMA));
    TEST_ASSERT_EQUAL_UINT16(550U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_OVERTEMP_WARN_C));
    TEST_ASSERT_EQUAL_UINT16(750U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_OVERTEMP_CRIT_C));
    TEST_ASSERT_EQUAL_UINT16(900U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MAX_CURRENT_MA));

    (void)unlink(path);
}

static uint16_t test_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t bit;

    for (i = 0U; i < length; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 0x8000U) != 0U) {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void test_state_12reg_migrates(void)
{
    static const uint16_t values[12] = {
        1U, 0U, 123U, 800U, 700U, 3500U, 650U, 1U, 1U, 1000U, 0U, 0U
    };
    char path[] = "/tmp/lamp-state-12reg-XXXXXX";
    uint8_t buffer[8U + (2U * 12U) + 2U];
    uint16_t crc;
    FILE *file;
    int fd = mkstemp(path);
    size_t i;

    TEST_ASSERT_TRUE(fd >= 0);
    (void)close(fd);
    (void)memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0x4DU; /* STATE_MAGIC, little-endian */
    buffer[1] = 0x41U;
    buffer[2] = 0x4CU;
    buffer[3] = 0x53U;
    buffer[4] = 1U; /* version */
    buffer[5] = 0U;
    buffer[6] = 12U; /* count */
    buffer[7] = 0U;
    for (i = 0U; i < 12U; i++) {
        buffer[8U + (2U * i)] = (uint8_t)(values[i] & 0xFFU);
        buffer[9U + (2U * i)] = (uint8_t)(values[i] >> 8);
    }
    crc = test_crc16(buffer, 8U + (2U * 12U));
    buffer[8U + (2U * 12U)] = (uint8_t)(crc & 0xFFU);
    buffer[9U + (2U * 12U)] = (uint8_t)(crc >> 8);

    file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT((int)sizeof(buffer),
                          (int)fwrite(buffer, 1U, sizeof(buffer), file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(0, setenv("DRIVER_STATE_PATH", path, 1));

    register_map_init();

    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(123U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_HUE));
    TEST_ASSERT_EQUAL_UINT16(3500U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_CCT));
    TEST_ASSERT_EQUAL_UINT16(650U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
    /* Configuration absent from 12-register files keeps the defaults. */
    TEST_ASSERT_EQUAL_UINT16(1000U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MAX_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_holding_reg((uint16_t)HOLDING_CFG_MIN_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_CONFIG_SAFE_MODE));

    (void)unlink(path);
}

static void test_heartbeat_watchdog(void)
{
    uint16_t h[HOLDING_COUNT];
    uint8_t coils[COIL_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_HEARTBEAT_TIMEOUT_S] = 1U;
    h[HOLDING_HEARTBEAT_COUNTER] = 1U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);

    for (uint16_t i = 0U; i < (uint16_t)COIL_COUNT; i++) {
        coils[i] = 0U;
    }
    coils[COIL_ENABLE_HEARTBEAT_WATCHDOG] = 1U;
    register_map_set_coils(coils, (uint16_t)COIL_COUNT);

    register_map_watchdog_tick();
    register_map_watchdog_tick();
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_HEARTBEAT_COUNTER] = 2U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_watchdog_tick();

    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
}

static void test_coil_edge_and_auto_reset(void)
{
    uint8_t coils[COIL_COUNT];

    for (uint16_t i = 0U; i < (uint16_t)COIL_COUNT; i++) {
        coils[i] = 0U;
    }
    coils[COIL_CMD_POWER_ON] = 1U;
    register_map_set_coils(coils, (uint16_t)COIL_COUNT);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_coil_bit((uint16_t)COIL_CMD_POWER_ON));
}

static void test_overtemp_warning(void)
{
    uint16_t h[HOLDING_COUNT];

    set_target(1U, 0U, 1000U, 1000U, 1000U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_OVERTEMP_WARN_C] = 200U; /* 20.0 C: below simulated temp */
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_OVERTEMP_WARNING));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERTEMP_WARN,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_TRUE(register_map_get_input_reg(
        (uint16_t)INPUT_ACTUAL_BRIGHTNESS) <= 500U);
}

static void set_sim_fault(uint16_t code, uint16_t duration_s)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_SIM_FAULT_CODE] = code;
    h[HOLDING_SIM_FAULT_DURATION_S] = duration_s;
    h[HOLDING_SIM_FAULT_MAGIC] = (uint16_t)SIM_FAULT_MAGIC;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
}

static void test_sim_fault_forced(void)
{
    set_sim_fault((uint16_t)FAULT_OVERTEMP_CRIT, 0U);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERTEMP_CRIT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_OVERTEMP_CRITICAL));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_sim_fault_clears(void)
{
    set_sim_fault((uint16_t)FAULT_OVERCURRENT, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERCURRENT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));

    set_sim_fault(0U, 0U);
    register_map_tick(50U);
    /* The latch survives the end of the injection. */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERCURRENT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));

    register_map_apply_command((uint16_t)COIL_CMD_CLEAR_FAULTS);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_ACTIVE));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));
}

static void test_sim_fault_duration_expires(void)
{
    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    set_sim_fault((uint16_t)FAULT_OVERTEMP_WARN, 1U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERTEMP_WARN,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_TRUE(register_map_get_input_reg(
        (uint16_t)INPUT_ACTUAL_BRIGHTNESS) <= 500U);

    register_map_tick(1000U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(800U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_sim_fault_needs_magic(void)
{
    uint16_t h[HOLDING_COUNT];

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_SIM_FAULT_CODE] = (uint16_t)FAULT_OVERTEMP_CRIT;
    h[HOLDING_SIM_FAULT_MAGIC] = 0U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
}

static void test_sim_critical_latches_and_blocks_power(void)
{
    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    set_sim_fault((uint16_t)FAULT_OVERTEMP_CRIT, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));

    register_map_apply_command((uint16_t)COIL_CMD_POWER_ON);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    set_sim_fault(0U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_CLEAR_FAULTS);
    register_map_tick(50U);
    register_map_apply_command((uint16_t)COIL_CMD_POWER_ON);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
}

static void test_sim_led_open_latches(void)
{
    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    set_sim_fault((uint16_t)FAULT_LED_OPEN, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_LED_OPEN,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));
}

static void test_real_overcurrent_latches(void)
{
    uint16_t h[HOLDING_COUNT];

    set_target(1U, 0U, 1000U, 1000U, 1000U, 0U);
    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_MAX_CURRENT_MA] = 1U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_OVERCURRENT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));
}

static void test_sim_fault_count_increments(void)
{
    uint16_t before;
    uint16_t after;

    before = register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_COUNT);
    set_sim_fault((uint16_t)FAULT_OVERTEMP_WARN, 0U);
    register_map_tick(50U);
    after = register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_COUNT);
    TEST_ASSERT_TRUE(after > before);
}

static void test_heartbeat_recovers_output(void)
{
    uint16_t h[HOLDING_COUNT];
    uint8_t coils[COIL_COUNT] = {0};

    set_target(1U, 42U, 900U, 800U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_HEARTBEAT_TIMEOUT_S] = 1U;
    h[HOLDING_HEARTBEAT_COUNTER] = 1U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    coils[COIL_ENABLE_HEARTBEAT_WATCHDOG] = 1U;
    register_map_set_coils(coils, (uint16_t)COIL_COUNT);

    register_map_watchdog_tick();
    register_map_watchdog_tick();
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_HEARTBEAT_COUNTER] = 2U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_watchdog_tick();
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(800U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_heartbeat_recovery_respects_latch(void)
{
    uint16_t h[HOLDING_COUNT];
    uint8_t coils[COIL_COUNT] = {0};

    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_CFG_HEARTBEAT_TIMEOUT_S] = 1U;
    h[HOLDING_HEARTBEAT_COUNTER] = 1U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    coils[COIL_ENABLE_HEARTBEAT_WATCHDOG] = 1U;
    register_map_set_coils(coils, (uint16_t)COIL_COUNT);

    register_map_watchdog_tick();
    register_map_watchdog_tick();
    set_sim_fault((uint16_t)FAULT_OVERTEMP_CRIT, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));

    register_map_get_holding(h, (uint16_t)HOLDING_COUNT);
    h[HOLDING_HEARTBEAT_COUNTER] = 2U;
    register_map_set_holding(h, (uint16_t)HOLDING_COUNT);
    register_map_watchdog_tick();
    register_map_tick(50U);

    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_FAULT_LATCHED));
}

static void test_sim_heartbeat_lost_recovers(void)
{
    set_target(1U, 120U, 800U, 700U, 750U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    set_sim_fault((uint16_t)FAULT_HEARTBEAT_LOST, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    set_sim_fault(0U, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit((uint16_t)DISCRETE_STATUS_HEARTBEAT_LOST));
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(750U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
}

static void test_config_corrupt_enters_safe_mode(void)
{
    char path[] = "/tmp/lamp-config-XXXXXX";
    int fd = mkstemp(path);

    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(3, write(fd, "bad", 3));
    (void)close(fd);
    TEST_ASSERT_EQUAL_INT(0, setenv("DRIVER_STATE_PATH", path, 1));

    register_map_init();
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_CONFIG_CORRUPT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_CONFIG_SAFE_MODE));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_POWER_ON);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    register_map_apply_command((uint16_t)COIL_CMD_CLEAR_FAULTS);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_CONFIG_SAFE_MODE));

    set_target(1U, 77U, 800U, 700U, 650U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);
    register_map_apply_command((uint16_t)COIL_CMD_SAVE_CONFIG);

    register_map_init();
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
    TEST_ASSERT_EQUAL_UINT16(650U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_BRIGHTNESS));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));

    (void)unlink(path);
}

static void test_sim_config_corrupt_safe_mode(void)
{
    set_target(1U, 0U, 1000U, 1000U, 800U, 0U);
    register_map_apply_command((uint16_t)COIL_CMD_APPLY_TARGET);
    register_map_tick(50U);

    set_sim_fault((uint16_t)FAULT_CONFIG_CORRUPT, 0U);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_CONFIG_CORRUPT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(1U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_CONFIG_SAFE_MODE));
    TEST_ASSERT_EQUAL_UINT16(0U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));

    set_sim_fault(0U, 0U);
    register_map_tick(50U);
    /* The corrupt configuration does not disappear when the injection ends. */
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_CONFIG_CORRUPT,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));

    register_map_apply_command((uint16_t)COIL_CMD_CLEAR_FAULTS);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)FAULT_NONE,
        register_map_get_input_reg((uint16_t)INPUT_DIAG_FAULT_CODE));
    TEST_ASSERT_EQUAL_UINT8(0U,
        register_map_get_discrete_bit(
            (uint16_t)DISCRETE_STATUS_CONFIG_SAFE_MODE));

    register_map_apply_command((uint16_t)COIL_CMD_POWER_ON);
    register_map_tick(50U);
    TEST_ASSERT_EQUAL_UINT16(1U,
        register_map_get_input_reg((uint16_t)INPUT_ACTUAL_POWER));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_apply_target_without_transition);
    RUN_TEST(test_out_of_range_writes_are_clamped);
    RUN_TEST(test_invalid_config_values_fall_back);
    RUN_TEST(test_brightness_config_limits);
    RUN_TEST(test_brightness_limits_are_sorted);
    RUN_TEST(test_negative_overtemp_threshold);
    RUN_TEST(test_transition_interpolates);
    RUN_TEST(test_power_off);
    RUN_TEST(test_sequence_rainbow);
    RUN_TEST(test_heartbeat_watchdog);
    RUN_TEST(test_coil_edge_and_auto_reset);
    RUN_TEST(test_custom_sequence_runs);
    RUN_TEST(test_custom_sequence_rejects_bad_crc);
    RUN_TEST(test_state_persists_across_restart);
    RUN_TEST(test_serial_from_env);
    RUN_TEST(test_identity_is_provisioned_once);
    RUN_TEST(test_config_persists_across_restart);
    RUN_TEST(test_state_12reg_migrates);
    RUN_TEST(test_overtemp_warning);
    RUN_TEST(test_sim_fault_forced);
    RUN_TEST(test_sim_fault_clears);
    RUN_TEST(test_sim_fault_duration_expires);
    RUN_TEST(test_sim_fault_needs_magic);
    RUN_TEST(test_sim_critical_latches_and_blocks_power);
    RUN_TEST(test_sim_led_open_latches);
    RUN_TEST(test_real_overcurrent_latches);
    RUN_TEST(test_sim_fault_count_increments);
    RUN_TEST(test_heartbeat_recovers_output);
    RUN_TEST(test_heartbeat_recovery_respects_latch);
    RUN_TEST(test_sim_heartbeat_lost_recovers);
    RUN_TEST(test_config_corrupt_enters_safe_mode);
    RUN_TEST(test_sim_config_corrupt_safe_mode);
    return UNITY_END();
}
