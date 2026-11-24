/**
 * @file register_map.c
 * @brief Modbus register map for the LED driver (target vs actual).
 *
 * The arrays are 0-based: 40001 -> HOLDING_TARGET_POWER, 30001 ->
 * INPUT_ACTUAL_POWER, etc. Access is protected by a mutex because the
 * Modbus server, the animation (20 Hz) and the watchdog run in threads.
 */

#include "register_map.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sequence.h"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t s_coils[COIL_COUNT];
static uint8_t s_discrete[DISCRETE_COUNT];
static uint16_t s_holding[HOLDING_COUNT];
static uint16_t s_input[INPUT_COUNT];

static sequence_state_t s_sequence;
static sequence_step_t s_custom_steps[SEQ_MAX_STEPS];

#define STATE_MAGIC 0x534C414DU /* "SLAM" */
#define STATE_VERSION 2U
#define STATE_VERSION_MIN 1U
#define STATE_REGS 20U
#define STATE_SAVE_INTERVAL_MS 2000U

static const char *s_state_path;
static bool s_state_dirty;
static uint32_t s_state_elapsed_ms;

#define IDENTITY_SERIAL_MAX 24U
#define IDENTITY_TEXT_REGS 12U

/* Product identity, provisioned once into persistent storage (simulated OTP)
 * and only read afterwards. `DEVICE_IDENTITY_PATH` points to the file. */
static char s_identity_serial[IDENTITY_SERIAL_MAX + 1U];
static uint32_t s_identity_id;
static uint16_t s_identity_hw_rev = 1U;

/* Keep the first 12 entries in their original order: 12-register files are
 * migrated by reading that prefix and leaving the newer configuration at its
 * defaults. */
static const uint16_t s_state_regs[STATE_REGS] = {
    HOLDING_TARGET_POWER, HOLDING_TARGET_MODE,   HOLDING_TARGET_HUE,
    HOLDING_TARGET_SAT,   HOLDING_TARGET_VAL,    HOLDING_TARGET_CCT,
    HOLDING_TARGET_BRIGHTNESS, HOLDING_TARGET_SCENE_ID,
    HOLDING_SEQ_ID,       HOLDING_SEQ_SPEED,     HOLDING_SEQ_DIRECTION,
    HOLDING_SEQ_LOOP_COUNT,
    HOLDING_CFG_MAX_BRIGHTNESS, HOLDING_CFG_MIN_BRIGHTNESS,
    HOLDING_CFG_POWER_ON_BEHAVIOR, HOLDING_CFG_GAMMA,
    HOLDING_CFG_HEARTBEAT_TIMEOUT_S, HOLDING_CFG_OVERTEMP_WARN_C,
    HOLDING_CFG_OVERTEMP_CRIT_C, HOLDING_CFG_MAX_CURRENT_MA
};

static bool s_transition_active;
static uint32_t s_transition_elapsed_ms;
static uint32_t s_transition_duration_ms;
static uint16_t s_tr_hue0;
static uint16_t s_tr_hue1;
static uint16_t s_tr_sat0;
static uint16_t s_tr_sat1;
static uint16_t s_tr_val0;
static uint16_t s_tr_val1;
static uint16_t s_tr_cct0;
static uint16_t s_tr_cct1;
static uint16_t s_tr_bri0;
static uint16_t s_tr_bri1;

static bool s_heartbeat_enabled;
static uint16_t s_last_heartbeat;
static uint32_t s_heartbeat_elapsed_s;
static bool s_heartbeat_lost;

static bool s_config_dirty;
static bool s_selftest_running;
static bool s_sequence_invalid;
static bool s_led_open;
static bool s_overtemp_warn;
static bool s_overtemp_crit;
static bool s_overcurrent;
static bool s_fault_latched;
static uint16_t s_fault_latched_code;
static bool s_config_corrupt;
static bool s_derated;
static uint16_t s_sim_counted_code;
static uint16_t s_sim_fault_prev;
static uint32_t s_sim_fault_started_s;
static bool s_sim_heartbeat_lost;

/* Snapshot of the target before the heartbeat was lost, to restore it when
 * the firmware shows signs of life again. */
static bool s_hb_snapshot_valid;
static uint16_t s_hb_power;
static uint16_t s_hb_mode;
static uint16_t s_hb_hue;
static uint16_t s_hb_sat;
static uint16_t s_hb_val;
static uint16_t s_hb_cct;
static uint16_t s_hb_brightness;
static uint16_t s_hb_transition;

#define FAULT_DERATE_BRIGHTNESS 500U

static void force_output_off_locked(void);
static void apply_target_locked(void);
static void enter_config_safe_mode_locked(bool count);
static void heartbeat_force_off_locked(void);
static void heartbeat_try_recover_locked(void);

static uint32_t s_uptime_ms;
static uint32_t s_on_time_ms;
static uint16_t s_fault_count;
static uint16_t s_last_fault_code;
static uint32_t s_last_fault_ts_s;

/**
 * @brief Clamps a value to the range [min, max].
 */
static uint16_t clamp_u16(uint32_t value, uint16_t min, uint16_t max)
{
    if (value < (uint32_t)min) {
        return min;
    }
    if (value > (uint32_t)max) {
        return max;
    }
    return (uint16_t)value;
}

static uint16_t clamp_hue(uint32_t value)
{
    return clamp_u16(value, 0U, 360U);
}

static uint16_t clamp_permille(uint32_t value)
{
    return clamp_u16(value, 0U, 1000U);
}

static uint16_t clamp_cct(uint32_t value)
{
    return clamp_u16(value, 2000U, 6500U);
}

/**
 * @brief Clamps a signed value and returns its 16-bit two's-complement form.
 */
static uint16_t clamp_i16(int32_t value, int16_t min, int16_t max)
{
    if (value < (int32_t)min) {
        return (uint16_t)(int16_t)min;
    }
    if (value > (int32_t)max) {
        return (uint16_t)(int16_t)max;
    }
    return (uint16_t)(int16_t)value;
}

/**
 * @brief Sanitizes one holding register before it is stored.
 *
 * Modbus is the trust boundary: a client can write any 16-bit value, so the
 * driver normalizes the documented ranges instead of trusting the writer.
 * Enum-like registers fall back to their safe default when the value is not
 * defined; numeric ranges are clamped.
 */
static uint16_t sanitize_holding(uint16_t index, uint16_t value)
{
    switch (index) {
    case HOLDING_TARGET_POWER:
        return (value != 0U) ? 1U : 0U;
    case HOLDING_TARGET_MODE:
        return (value <= 3U) ? value : 0U;
    case HOLDING_TARGET_HUE:
        return clamp_hue(value);
    case HOLDING_TARGET_SAT:
    case HOLDING_TARGET_VAL:
    case HOLDING_TARGET_BRIGHTNESS:
        return clamp_permille(value);
    case HOLDING_TARGET_CCT:
        return clamp_cct(value);
    case HOLDING_TARGET_TRANSITION_MS:
        return clamp_u16(value, 0U, 10000U);
    case HOLDING_TARGET_SCENE_ID:
        return clamp_u16(value, 0U, 16U);
    case HOLDING_SEQ_SPEED:
        return clamp_u16(value, 0U, 10000U);
    case HOLDING_SEQ_DIRECTION:
        return (value <= 2U) ? value : 0U;
    case HOLDING_SEQ_STEP_HUE:
        return clamp_hue(value);
    case HOLDING_SEQ_STEP_SAT:
    case HOLDING_SEQ_STEP_VAL:
    case HOLDING_SEQ_STEP_BRIGHTNESS:
        return clamp_permille(value);
    case HOLDING_SEQ_STEP_CCT:
        return clamp_cct(value);
    case HOLDING_CFG_MAX_BRIGHTNESS:
    case HOLDING_CFG_MIN_BRIGHTNESS:
        return clamp_permille(value);
    case HOLDING_CFG_POWER_ON_BEHAVIOR:
        return (value <= 2U) ? value : 0U;
    case HOLDING_CFG_GAMMA:
        return (value <= 2U) ? value : 0U;
    case HOLDING_CFG_OVERTEMP_WARN_C:
    case HOLDING_CFG_OVERTEMP_CRIT_C:
        return clamp_i16((int32_t)(int16_t)value, -400, 1250);
    case HOLDING_SIM_FAULT_CODE:
        return (value <= 7U) ? value : 0U;
    case HOLDING_SIM_FAULT_DURATION_S:
        return clamp_u16(value, 0U, 3600U);
    default:
        return value;
    }
}

/**
 * @brief Sanitizes every holding register in place.
 */
static void sanitize_state_locked(void)
{
    uint16_t i;

    for (i = 0U; i < (uint16_t)HOLDING_COUNT; i++) {
        s_holding[i] = sanitize_holding(i, s_holding[i]);
    }
}

/**
 * @brief Returns the effective brightness interval (min/max may be inverted).
 */
static void brightness_limits_locked(uint16_t *lo, uint16_t *hi)
{
    uint16_t a = clamp_permille(s_holding[HOLDING_CFG_MIN_BRIGHTNESS]);
    uint16_t b = clamp_permille(s_holding[HOLDING_CFG_MAX_BRIGHTNESS]);

    *lo = (a < b) ? a : b;
    *hi = (a < b) ? b : a;
}

/**
 * @brief Clamps a brightness value to the configured limits.
 */
static uint16_t clamp_brightness_locked(uint32_t value)
{
    uint16_t lo;
    uint16_t hi;

    brightness_limits_locked(&lo, &hi);
    return clamp_u16(value, lo, hi);
}

/**
 * @brief FNV-1a hash used to derive a stable serial from a text identifier.
 */
static uint32_t serial_hash(const char *text)
{
    uint32_t hash = 2166136261U;

    while (*text != '\0') {
        hash ^= (uint8_t)*text;
        hash *= 16777619U;
        text++;
    }
    return hash;
}

/**
 * @brief Returns the 32-bit device serial.
 *
 * `DEVICE_SERIAL` may be a number (decimal or `0x`-prefixed) or any text; text
 * is hashed so the same device keeps the same value. `DEVICE_ID` is the
 * fallback and a fixed demo value is the last resort. The serial is public,
 * not a credential: authentication still uses the device token.
 */
static uint32_t serial_from_env(void)
{
    const char *value = getenv("DEVICE_SERIAL");
    char *end = NULL;
    unsigned long parsed;

    if ((value != NULL) && (value[0] != '\0')) {
        parsed = strtoul(value, &end, 0);
        if ((end != NULL) && (*end == '\0') && (parsed <= 0xFFFFFFFFUL)) {
            return (uint32_t)parsed;
        }
        return serial_hash(value);
    }
    value = getenv("DEVICE_ID");
    if ((value != NULL) && (value[0] != '\0')) {
        return serial_hash(value);
    }
    return 0x8F2A0001U;
}

/**
 * @brief Trims leading/trailing spaces and the line ending in place.
 */
static char *trim_text(char *text)
{
    char *end;

    while ((*text == ' ') || (*text == '\t')) {
        text++;
    }
    end = text + strlen(text);
    while ((end > text) && ((end[-1] == '\n') || (end[-1] == '\r') ||
                            (end[-1] == ' ') || (end[-1] == '\t'))) {
        end--;
    }
    *end = '\0';
    return text;
}

/**
 * @brief Reads the identity file written by the provisioning step.
 *
 * @return true if a valid serial could be read.
 */
static bool identity_read_file(const char *path, uint32_t *id,
                               uint16_t *hw_rev, char *serial,
                               size_t serial_size)
{
    char line[160];
    char serial_value[IDENTITY_SERIAL_MAX + 1U] = {0};
    bool have_id = false;
    bool have_hw = false;
    bool have_serial = false;
    FILE *file;

    file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        char *eq = strchr(line, '=');
        char *key;
        char *value;

        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        key = trim_text(line);
        value = trim_text(eq + 1);
        if (strcmp(key, "serial") == 0) {
            size_t length = strlen(value);

            if ((length > 0U) && (length <= IDENTITY_SERIAL_MAX)) {
                (void)memcpy(serial_value, value, length + 1U);
                have_serial = true;
            }
        } else if (strcmp(key, "id") == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 16);

            if ((end != value) && (end != NULL) && (*end == '\0') &&
                (parsed <= 0xFFFFFFFFUL)) {
                *id = (uint32_t)parsed;
                have_id = true;
            }
        } else if (strcmp(key, "hw_rev") == 0) {
            char *end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);

            if ((end != value) && (end != NULL) && (*end == '\0') &&
                (parsed <= 0xFFFFUL)) {
                *hw_rev = (uint16_t)parsed;
                have_hw = true;
            }
        }
    }
    (void)fclose(file);

    if (!have_serial) {
        return false;
    }
    if (!have_hw) {
        *hw_rev = 1U;
    }
    if (!have_id) {
        *id = serial_hash(serial_value);
    }
    (void)strncpy(serial, serial_value, serial_size - 1U);
    serial[serial_size - 1U] = '\0';
    return true;
}

/**
 * @brief Writes the identity once, like factory programming into OTP.
 *
 * `PROVISION_LOT`, `PROVISION_WEEK` and `PROVISION_ID` override the generated
 * lot, ISO week (YYWW) and numeric id. The file is renamed atomically and left
 * read-only.
 */
static bool identity_provision(const char *path, uint32_t *id,
                               uint16_t *hw_rev, char *serial,
                               size_t serial_size)
{
    const char *lot = getenv("PROVISION_LOT");
    const char *week = getenv("PROVISION_WEEK");
    const char *id_env = getenv("PROVISION_ID");
    char week_buf[8];
    char timestamp[32];
    char tmp_path[512];
    uint32_t serial_id = 0U;
    uint16_t hw = 1U;
    time_t now = time(NULL);
    struct tm tm_utc;
    FILE *file;

    if ((lot == NULL) || (lot[0] == '\0')) {
        lot = "LA";
    }
    if ((week == NULL) || (week[0] == '\0')) {
        if (gmtime_r(&now, &tm_utc) != NULL) {
            (void)strftime(week_buf, sizeof(week_buf), "%y%W", &tm_utc);
        } else {
            (void)snprintf(week_buf, sizeof(week_buf), "0000");
        }
        week = week_buf;
    }
    if ((id_env != NULL) && (id_env[0] != '\0')) {
        char *end = NULL;
        unsigned long parsed = strtoul(id_env, &end, 16);

        if ((end != id_env) && (end != NULL) && (*end == '\0') &&
            (parsed <= 0xFFFFFFFFUL)) {
            serial_id = (uint32_t)parsed;
        }
    }
    if (serial_id == 0U) {
        uint8_t random_bytes[4];

        file = fopen("/dev/urandom", "rb");
        if ((file != NULL) &&
            (fread(random_bytes, 1U, sizeof(random_bytes), file) ==
             sizeof(random_bytes))) {
            serial_id = ((uint32_t)random_bytes[0] << 24U) |
                        ((uint32_t)random_bytes[1] << 16U) |
                        ((uint32_t)random_bytes[2] << 8U) |
                        (uint32_t)random_bytes[3];
        }
        if (file != NULL) {
            (void)fclose(file);
        }
    }
    if (serial_id == 0U) {
        char seed[48];

        (void)snprintf(seed, sizeof(seed), "%ld-%d-%s", (long)now,
                       (int)getpid(), lot);
        serial_id = serial_hash(seed);
        if (serial_id == 0U) {
            serial_id = 1U;
        }
    }
    if (snprintf(serial, serial_size, "SL-%u-%s-%s-%08X", (unsigned)hw, lot,
                 week, (unsigned)serial_id) >= (int)serial_size) {
        (void)snprintf(serial, serial_size, "SL-%u-%08X", (unsigned)hw,
                       (unsigned)serial_id);
    }
    if ((gmtime_r(&now, &tm_utc) == NULL) ||
        (strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ",
                  &tm_utc) == 0U)) {
        (void)snprintf(timestamp, sizeof(timestamp), "1970-01-01T00:00:00Z");
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path) >=
        (int)sizeof(tmp_path)) {
        return false;
    }
    file = fopen(tmp_path, "w");
    if (file == NULL) {
        return false;
    }
    (void)fprintf(file,
                  "model=SL\nhw_rev=%u\nlot=%s\nweek=%s\nid=%08X\n"
                  "serial=%s\nprovisioned_at=%s\n",
                  (unsigned)hw, lot, week, (unsigned)serial_id, serial,
                  timestamp);
    if (fclose(file) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        (void)unlink(tmp_path);
        return false;
    }
    (void)chmod(path, 0444);
    *id = serial_id;
    *hw_rev = hw;
    return true;
}

/**
 * @brief Loads the persistent identity or provisions it on first boot.
 *
 * Without `DEVICE_IDENTITY_PATH` it falls back to the environment-derived id,
 * which keeps the driver usable in tests and ad-hoc runs.
 */
static void identity_load_or_provision(void)
{
    const char *path = getenv("DEVICE_IDENTITY_PATH");
    uint32_t id = 0U;
    uint16_t hw = 1U;
    char serial[IDENTITY_SERIAL_MAX + 1U] = {0};

    if ((path != NULL) && (path[0] != '\0')) {
        if (access(path, F_OK) == 0) {
            if (identity_read_file(path, &id, &hw, serial, sizeof(serial))) {
                s_identity_id = id;
                s_identity_hw_rev = hw;
                (void)strncpy(s_identity_serial, serial,
                              sizeof(s_identity_serial) - 1U);
                s_identity_serial[sizeof(s_identity_serial) - 1U] = '\0';
                return;
            }
            (void)fprintf(stderr,
                          "[driver] identity file invalid; using fallback\n");
        } else if (identity_provision(path, &id, &hw, serial, sizeof(serial))) {
            s_identity_id = id;
            s_identity_hw_rev = hw;
            (void)strncpy(s_identity_serial, serial,
                          sizeof(s_identity_serial) - 1U);
            s_identity_serial[sizeof(s_identity_serial) - 1U] = '\0';
            (void)fprintf(stderr, "[driver] identity provisioned: %s\n",
                          s_identity_serial);
            return;
        } else {
            (void)fprintf(stderr,
                          "[driver] identity provisioning failed; using "
                          "fallback\n");
        }
    }

    s_identity_id = serial_from_env();
    s_identity_hw_rev = 1U;
    (void)snprintf(s_identity_serial, sizeof(s_identity_serial), "SL-%u-%08X",
                   (unsigned)s_identity_hw_rev, (unsigned)s_identity_id);
}

/**
 * @brief Copies the step at SEQ_STEP_INDEX from the holding registers.
 *
 * The upload writes the index first and then the step's 8 registers.
 * Copying on every write is idempotent and tolerates partial writes.
 */
static void capture_custom_step_locked(void)
{
    uint16_t index = s_holding[HOLDING_SEQ_STEP_INDEX];
    sequence_step_t *step;

    if (index >= (uint16_t)SEQ_MAX_STEPS) {
        return;
    }
    step = &s_custom_steps[index];
    step->hue = clamp_hue(s_holding[HOLDING_SEQ_STEP_HUE]);
    step->sat = clamp_permille(s_holding[HOLDING_SEQ_STEP_SAT]);
    step->val = clamp_permille(s_holding[HOLDING_SEQ_STEP_VAL]);
    step->reserved = 0U;
    step->brightness = clamp_permille(s_holding[HOLDING_SEQ_STEP_BRIGHTNESS]);
    step->duration_10ms = s_holding[HOLDING_SEQ_STEP_DURATION];
    step->transition_10ms = s_holding[HOLDING_SEQ_STEP_TRANSITION];
    step->cct = clamp_cct(s_holding[HOLDING_SEQ_STEP_CCT]);
}

/**
 * @brief 16-bit linear interpolation (progress 0..1000).
 */
static uint16_t interp_u16(uint16_t a, uint16_t b, uint32_t progress)
{
    int32_t delta = (int32_t)b - (int32_t)a;
    int32_t value = (int32_t)a + (delta * (int32_t)progress) / 1000;

    if (value < 0) {
        value = 0;
    }
    if (value > 65535) {
        value = 65535;
    }
    return (uint16_t)value;
}

/**
 * @brief Hue interpolation along the short path.
 */
static uint16_t interp_hue(uint16_t a, uint16_t b, uint32_t progress)
{
    int32_t delta = (int32_t)b - (int32_t)a;
    int32_t value;

    if (delta > 180) {
        delta -= 360;
    } else if (delta < -180) {
        delta += 360;
    }
    value = (int32_t)a + (delta * (int32_t)progress) / 1000;
    while (value < 0) {
        value += 360;
    }
    while (value > 360) {
        value -= 360;
    }
    return (uint16_t)value;
}

/**
 * @brief CRC-16/CCITT-FALSE over bytes (for the state file).
 */
static uint16_t state_crc16(const uint8_t *data, size_t length)
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

/**
 * @brief Saves the targets and the sequence config atomically.
 *
 * Format: magic(4) + version(2) + count(2) + count*2 values + crc(2),
 * all little-endian. It is written to `<path>.tmp` and then renamed.
 */
static void state_save_locked(void)
{
    uint8_t buffer[8U + (2U * STATE_REGS) + 2U];
    uint32_t magic = (uint32_t)STATE_MAGIC;
    uint16_t version = (uint16_t)STATE_VERSION;
    uint16_t count = (uint16_t)STATE_REGS;
    uint16_t crc;
    char tmp_path[512];
    size_t i;
    FILE *file;

    if ((s_state_path == NULL) || s_config_corrupt) {
        return;
    }
    buffer[0] = (uint8_t)(magic & 0xFFU);
    buffer[1] = (uint8_t)((magic >> 8) & 0xFFU);
    buffer[2] = (uint8_t)((magic >> 16) & 0xFFU);
    buffer[3] = (uint8_t)((magic >> 24) & 0xFFU);
    buffer[4] = (uint8_t)(version & 0xFFU);
    buffer[5] = (uint8_t)((version >> 8) & 0xFFU);
    buffer[6] = (uint8_t)(count & 0xFFU);
    buffer[7] = (uint8_t)((count >> 8) & 0xFFU);
    for (i = 0U; i < (size_t)STATE_REGS; i++) {
        uint16_t value = s_holding[s_state_regs[i]];

        buffer[8U + (2U * i)] = (uint8_t)(value & 0xFFU);
        buffer[9U + (2U * i)] = (uint8_t)((value >> 8) & 0xFFU);
    }
    crc = state_crc16(buffer, 8U + (2U * STATE_REGS));
    buffer[8U + (2U * STATE_REGS)] = (uint8_t)(crc & 0xFFU);
    buffer[9U + (2U * STATE_REGS)] = (uint8_t)((crc >> 8) & 0xFFU);

    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", s_state_path) >=
        (int)sizeof(tmp_path)) {
        return;
    }
    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        return;
    }
    if ((fwrite(buffer, 1U, sizeof(buffer), file) != sizeof(buffer)) ||
        (fclose(file) != 0)) {
        (void)unlink(tmp_path);
        return;
    }
    if (rename(tmp_path, s_state_path) == 0) {
        s_config_corrupt = false;
    }
}

/**
 * @brief Restores the safe configuration limits.
 *
 * It does not touch the diagnostic counters or the simulated fault. It is used
 * when the state file is corrupt and its values cannot be trusted.
 */
static void apply_safe_config_locked(void)
{
    s_holding[HOLDING_CFG_MAX_BRIGHTNESS] = 1000U;
    s_holding[HOLDING_CFG_MIN_BRIGHTNESS] = 0U;
    s_holding[HOLDING_CFG_POWER_ON_BEHAVIOR] = 0U;
    s_holding[HOLDING_CFG_GAMMA] = 0U;
    s_holding[HOLDING_CFG_HEARTBEAT_TIMEOUT_S] = 0U;
    s_holding[HOLDING_CFG_OVERTEMP_WARN_C] = 600U;
    s_holding[HOLDING_CFG_OVERTEMP_CRIT_C] = 800U;
    s_holding[HOLDING_CFG_MAX_CURRENT_MA] = 1000U;
    s_heartbeat_enabled = false;
    s_heartbeat_elapsed_s = 0U;
    s_heartbeat_lost = false;
    s_sim_heartbeat_lost = false;
    s_hb_snapshot_valid = false;
    s_last_heartbeat = s_holding[HOLDING_HEARTBEAT_COUNTER];
}

/**
 * @brief Enters safe mode due to corrupt configuration.
 *
 * The output stays off and save/load/power/start are blocked. The flag is not
 * auto-saved so the corrupt file is not overwritten before the user decides to
 * clear it and save again.
 *
 * @param count true if the entry should count as a new fault.
 */
static void enter_config_safe_mode_locked(bool count)
{
    if (!s_config_corrupt) {
        s_config_corrupt = true;
        if (count) {
            s_fault_count++;
            s_last_fault_ts_s = s_uptime_ms / 1000U;
        }
    }
    apply_safe_config_locked();
    force_output_off_locked();
    s_state_dirty = false;
}

/**
 * @brief Loads the saved state and applies it as the current state.
 *
 * If the file exists but is not valid, it enters safe mode and returns
 * false. A missing file is a normal boot with defaults.
 *
 * @return true if the file existed and was valid.
 */
static bool state_load_locked(void)
{
    uint8_t buffer[8U + (2U * STATE_REGS) + 2U];
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t expected;
    uint16_t crc;
    size_t values_bytes;
    size_t payload_size;
    FILE *file;
    size_t i;

    if (s_state_path == NULL) {
        return false;
    }
    file = fopen(s_state_path, "rb");
    if (file == NULL) {
        return false;
    }
    if (fread(buffer, 1U, 8U, file) != 8U) {
        (void)fclose(file);
        enter_config_safe_mode_locked(true);
        return false;
    }
    magic = ((uint32_t)buffer[0]) | ((uint32_t)buffer[1] << 8) |
            ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
    version = (uint16_t)(buffer[4] | ((uint16_t)buffer[5] << 8));
    count = (uint16_t)(buffer[6] | ((uint16_t)buffer[7] << 8));
    if ((magic != (uint32_t)STATE_MAGIC) ||
        (version < (uint16_t)STATE_VERSION_MIN) ||
        (version > (uint16_t)STATE_VERSION) || (count == 0U) ||
        (count > (uint16_t)STATE_REGS) ||
        ((version == (uint16_t)STATE_VERSION_MIN) && (count != 12U)) ||
        ((version == (uint16_t)STATE_VERSION) &&
         (count != (uint16_t)STATE_REGS))) {
        (void)fclose(file);
        enter_config_safe_mode_locked(true);
        return false;
    }
    values_bytes = (size_t)count * 2U;
    payload_size = 8U + values_bytes + 2U;
    if ((payload_size > sizeof(buffer)) ||
        (fread(buffer + 8U, 1U, payload_size - 8U, file) !=
         (payload_size - 8U))) {
        (void)fclose(file);
        enter_config_safe_mode_locked(true);
        return false;
    }
    (void)fclose(file);
    expected = (uint16_t)(buffer[8U + values_bytes] |
                          ((uint16_t)buffer[9U + values_bytes] << 8));
    crc = state_crc16(buffer, 8U + values_bytes);
    if (crc != expected) {
        enter_config_safe_mode_locked(true);
        return false;
    }

    /* 12-register files only carry the first 12 registers; the configuration
     * registers keep their defaults (or what was set at boot). */
    for (i = 0U; i < (size_t)count; i++) {
        s_holding[s_state_regs[i]] =
            (uint16_t)(buffer[8U + (2U * i)] |
                       ((uint16_t)buffer[9U + (2U * i)] << 8));
    }
    sanitize_state_locked();
    s_holding[HOLDING_TARGET_HUE] = clamp_hue(s_holding[HOLDING_TARGET_HUE]);
    s_holding[HOLDING_TARGET_SAT] = clamp_permille(s_holding[HOLDING_TARGET_SAT]);
    s_holding[HOLDING_TARGET_VAL] = clamp_permille(s_holding[HOLDING_TARGET_VAL]);
    s_holding[HOLDING_TARGET_CCT] = clamp_cct(s_holding[HOLDING_TARGET_CCT]);
    s_holding[HOLDING_TARGET_BRIGHTNESS] =
        clamp_permille(s_holding[HOLDING_TARGET_BRIGHTNESS]);

    sequence_stop(&s_sequence);
    s_transition_active = false;
    s_input[INPUT_ACTUAL_MODE] =
        (s_holding[HOLDING_TARGET_MODE] == 2U)
            ? 0U
            : s_holding[HOLDING_TARGET_MODE];
    s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
    s_input[INPUT_ACTUAL_SEQ_STEP] = 0U;
    if (s_holding[HOLDING_TARGET_POWER] != 0U) {
        s_input[INPUT_ACTUAL_POWER] = 1U;
        s_input[INPUT_ACTUAL_HUE] = s_holding[HOLDING_TARGET_HUE];
        s_input[INPUT_ACTUAL_SAT] = s_holding[HOLDING_TARGET_SAT];
        s_input[INPUT_ACTUAL_VAL] = s_holding[HOLDING_TARGET_VAL];
        s_input[INPUT_ACTUAL_CCT] = s_holding[HOLDING_TARGET_CCT];
        s_input[INPUT_ACTUAL_BRIGHTNESS] =
            clamp_brightness_locked(s_holding[HOLDING_TARGET_BRIGHTNESS]);
    } else {
        s_input[INPUT_ACTUAL_POWER] = 0U;
        s_input[INPUT_ACTUAL_VAL] = 0U;
        s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
    }
    s_config_dirty = false;
    return true;
}

/**
 * @brief Resets all arrays and state to factory values.
 *
 * Must be called with the lock held (or from register_map_init).
 */
static void set_defaults_locked(void)
{
    size_t serial_length = strlen(s_identity_serial);
    uint16_t i;

    (void)memset(s_coils, 0, sizeof(s_coils));
    (void)memset(s_discrete, 0, sizeof(s_discrete));
    (void)memset(s_holding, 0, sizeof(s_holding));
    (void)memset(s_input, 0, sizeof(s_input));

    s_holding[HOLDING_TARGET_POWER] = 0U;
    s_holding[HOLDING_TARGET_MODE] = 0U;
    s_holding[HOLDING_TARGET_HUE] = 0U;
    s_holding[HOLDING_TARGET_SAT] = 0U;
    s_holding[HOLDING_TARGET_VAL] = 0U;
    s_holding[HOLDING_TARGET_CCT] = 2700U;
    s_holding[HOLDING_TARGET_BRIGHTNESS] = 0U;
    s_holding[HOLDING_TARGET_TRANSITION_MS] = 0U;
    s_holding[HOLDING_TARGET_SCENE_ID] = 1U;
    s_holding[HOLDING_SEQ_ID] = 1U;
    s_holding[HOLDING_SEQ_SPEED] = 1000U;
    s_holding[HOLDING_SEQ_DIRECTION] = 0U;
    s_holding[HOLDING_SEQ_LOOP_COUNT] = 0U;
    s_holding[HOLDING_CFG_MAX_BRIGHTNESS] = 1000U;
    s_holding[HOLDING_CFG_MIN_BRIGHTNESS] = 0U;
    s_holding[HOLDING_CFG_POWER_ON_BEHAVIOR] = 0U;
    s_holding[HOLDING_CFG_GAMMA] = 0U;
    s_holding[HOLDING_CFG_HEARTBEAT_TIMEOUT_S] = 0U;
    s_holding[HOLDING_CFG_OVERTEMP_WARN_C] = 600U;
    s_holding[HOLDING_CFG_OVERTEMP_CRIT_C] = 800U;
    s_holding[HOLDING_CFG_MAX_CURRENT_MA] = 1000U;
    s_holding[HOLDING_HEARTBEAT_COUNTER] = 0U;

    s_input[INPUT_ACTUAL_POWER] = 0U;
    s_input[INPUT_ACTUAL_MODE] = 0U;
    s_input[INPUT_ACTUAL_HUE] = 0U;
    s_input[INPUT_ACTUAL_SAT] = 0U;
    s_input[INPUT_ACTUAL_VAL] = 0U;
    s_input[INPUT_ACTUAL_CCT] = 2700U;
    s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
    s_input[INPUT_ACTUAL_SEQ_STEP] = 0U;
    s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 0U;
    s_input[INPUT_DIAG_TEMP_C_X10] = 250U;
    s_input[INPUT_DIAG_VOLTAGE_MV] = 12000U;
    s_input[INPUT_DIAG_FW_MAJOR] = 0U;
    s_input[INPUT_DIAG_FW_MINOR] = 1U;
    s_input[INPUT_DIAG_FW_PATCH] = 0U;
    s_input[INPUT_DIAG_HW_REV] = s_identity_hw_rev;
    s_input[INPUT_DIAG_SERIAL_LO] = (uint16_t)(s_identity_id & 0xFFFFU);
    s_input[INPUT_DIAG_SERIAL_HI] = (uint16_t)(s_identity_id >> 16U);
    for (i = 0U; i < (uint16_t)IDENTITY_TEXT_REGS; i++) {
        size_t first = (size_t)i * 2U;
        uint16_t high = (first < serial_length)
                            ? (uint16_t)(uint8_t)s_identity_serial[first]
                            : 0U;
        uint16_t low = ((first + 1U) < serial_length)
                           ? (uint16_t)(uint8_t)s_identity_serial[first + 1U]
                           : 0U;

        s_input[INPUT_DIAG_SERIAL_TEXT + i] = (uint16_t)((high << 8U) | low);
    }

    sequence_init(&s_sequence);
    (void)memset(s_custom_steps, 0, sizeof(s_custom_steps));
    s_state_dirty = false;
    s_state_elapsed_ms = 0U;

    s_transition_active = false;
    s_transition_elapsed_ms = 0U;
    s_transition_duration_ms = 0U;
    s_heartbeat_enabled = false;
    s_last_heartbeat = 0U;
    s_heartbeat_elapsed_s = 0U;
    s_heartbeat_lost = false;
    s_config_dirty = false;
    s_selftest_running = false;
    s_sequence_invalid = false;
    s_led_open = false;
    s_overtemp_warn = false;
    s_overtemp_crit = false;
    s_overcurrent = false;
    s_fault_latched = false;
    s_fault_latched_code = (uint16_t)FAULT_NONE;
    s_config_corrupt = false;
    s_derated = false;
    s_sim_counted_code = 0U;
    s_sim_fault_prev = 0U;
    s_sim_fault_started_s = 0U;
    s_sim_heartbeat_lost = false;
    s_hb_snapshot_valid = false;
    s_hb_power = 0U;
    s_hb_mode = 0U;
    s_hb_hue = 0U;
    s_hb_sat = 0U;
    s_hb_val = 0U;
    s_hb_cct = 2700U;
    s_hb_brightness = 0U;
    s_hb_transition = 0U;
    s_uptime_ms = 0U;
    s_on_time_ms = 0U;
    s_fault_count = 0U;
    s_last_fault_code = (uint16_t)FAULT_NONE;
    s_last_fault_ts_s = 0U;
}

/**
 * @brief Starts the transition toward the current target values.
 */
static void start_transition_locked(void)
{
    uint16_t duration = s_holding[HOLDING_TARGET_TRANSITION_MS];

    s_tr_hue0 = s_input[INPUT_ACTUAL_HUE];
    s_tr_hue1 = clamp_hue(s_holding[HOLDING_TARGET_HUE]);
    s_tr_sat0 = s_input[INPUT_ACTUAL_SAT];
    s_tr_sat1 = clamp_permille(s_holding[HOLDING_TARGET_SAT]);
    s_tr_val0 = s_input[INPUT_ACTUAL_VAL];
    s_tr_val1 = clamp_permille(s_holding[HOLDING_TARGET_VAL]);
    s_tr_cct0 = s_input[INPUT_ACTUAL_CCT];
    s_tr_cct1 = clamp_cct(s_holding[HOLDING_TARGET_CCT]);
    s_tr_bri0 = s_input[INPUT_ACTUAL_BRIGHTNESS];
    s_tr_bri1 = clamp_brightness_locked(s_holding[HOLDING_TARGET_BRIGHTNESS]);

    s_transition_elapsed_ms = 0U;
    s_transition_duration_ms = duration;
    s_transition_active = (duration > 0U);

    if (!s_transition_active) {
        s_input[INPUT_ACTUAL_HUE] = s_tr_hue1;
        s_input[INPUT_ACTUAL_SAT] = s_tr_sat1;
        s_input[INPUT_ACTUAL_VAL] = s_tr_val1;
        s_input[INPUT_ACTUAL_CCT] = s_tr_cct1;
        s_input[INPUT_ACTUAL_BRIGHTNESS] = s_tr_bri1;
        s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
    } else {
        s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 0U;
    }
}

/**
 * @brief Applies TARGET_POWER and the target values (CMD_APPLY_TARGET).
 */
/**
 * @brief Forces the output off without touching the cause of the fault.
 *
 * Must be called with the lock held. The lamp stays off until the user
 * clears the fault and turns it back on.
 */
static void force_output_off_locked(void)
{
    s_holding[HOLDING_TARGET_POWER] = 0U;
    s_input[INPUT_ACTUAL_POWER] = 0U;
    s_input[INPUT_ACTUAL_VAL] = 0U;
    s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
    s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
    sequence_stop(&s_sequence);
    s_transition_active = false;
}

/**
 * @brief Latches a critical fault and turns the output off.
 *
 * The first cause is kept until `COIL_CMD_CLEAR_FAULTS`; the output cannot
 * be turned back on while the latch is active.
 */
static void latch_fault_locked(uint16_t code)
{
    if (code == (uint16_t)FAULT_NONE) {
        return;
    }
    if (!s_fault_latched) {
        s_fault_latched = true;
        s_fault_latched_code = code;
    }
    force_output_off_locked();
    s_state_dirty = true;
}

/**
 * @brief Counts a simulated injection once per active code.
 */
static void note_sim_fault_locked(uint16_t code)
{
    if (code == 0U || code == s_sim_counted_code) {
        return;
    }
    s_sim_counted_code = code;
    s_fault_count++;
    s_last_fault_ts_s = s_uptime_ms / 1000U;
}

static void apply_target_locked(void)
{
    sequence_stop(&s_sequence);
    s_input[INPUT_ACTUAL_POWER] = (s_holding[HOLDING_TARGET_POWER] != 0U) ? 1U : 0U;
    s_input[INPUT_ACTUAL_MODE] = s_holding[HOLDING_TARGET_MODE];

    if (s_input[INPUT_ACTUAL_POWER] == 0U) {
        s_input[INPUT_ACTUAL_VAL] = 0U;
        s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
        s_transition_active = false;
        s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
    } else {
        start_transition_locked();
    }
}

/**
 * @brief Saves the target and turns the output off due to heartbeat loss.
 *
 * The snapshot is taken only once; if the watchdog and a simulated fault
 * coincide, recovery waits until both have cleared.
 */
static void heartbeat_force_off_locked(void)
{
    if (!s_hb_snapshot_valid) {
        s_hb_power = s_holding[HOLDING_TARGET_POWER];
        s_hb_mode = s_holding[HOLDING_TARGET_MODE];
        s_hb_hue = s_holding[HOLDING_TARGET_HUE];
        s_hb_sat = s_holding[HOLDING_TARGET_SAT];
        s_hb_val = s_holding[HOLDING_TARGET_VAL];
        s_hb_cct = s_holding[HOLDING_TARGET_CCT];
        s_hb_brightness = s_holding[HOLDING_TARGET_BRIGHTNESS];
        s_hb_transition = s_holding[HOLDING_TARGET_TRANSITION_MS];
        s_hb_snapshot_valid = true;
    }
    force_output_off_locked();
    s_state_dirty = true;
}

/**
 * @brief Restores the target if the heartbeat returned with no other fault.
 *
 * A latched critical fault or a configuration in safe mode takes priority:
 * in that case the lamp stays off.
 */
static void heartbeat_try_recover_locked(void)
{
    if (s_heartbeat_lost || s_sim_heartbeat_lost) {
        return;
    }
    if (s_hb_snapshot_valid && !s_fault_latched && !s_config_corrupt) {
        s_holding[HOLDING_TARGET_POWER] = s_hb_power;
        s_holding[HOLDING_TARGET_MODE] = (s_hb_mode == 2U) ? 0U : s_hb_mode;
        s_holding[HOLDING_TARGET_HUE] = s_hb_hue;
        s_holding[HOLDING_TARGET_SAT] = s_hb_sat;
        s_holding[HOLDING_TARGET_VAL] = s_hb_val;
        s_holding[HOLDING_TARGET_CCT] = s_hb_cct;
        s_holding[HOLDING_TARGET_BRIGHTNESS] = s_hb_brightness;
        s_holding[HOLDING_TARGET_TRANSITION_MS] = s_hb_transition;
        apply_target_locked();
        s_state_dirty = true;
    }
    s_hb_snapshot_valid = false;
}

/**
 * @brief Executes a coil command. Must be called with the lock held.
 */
static void apply_command_locked(uint16_t coil_index)
{
    if (((s_fault_latched || s_config_corrupt) &&
         ((coil_index == (uint16_t)COIL_CMD_POWER_ON) ||
          (coil_index == (uint16_t)COIL_CMD_APPLY_TARGET) ||
          (coil_index == (uint16_t)COIL_CMD_START_SEQUENCE) ||
          (coil_index == (uint16_t)COIL_CMD_LOAD_CONFIG))) ||
        (s_config_corrupt && (coil_index == (uint16_t)COIL_CMD_SAVE_CONFIG))) {
        force_output_off_locked();
        s_state_dirty = true;
        return;
    }
    switch (coil_index) {
    case COIL_CMD_APPLY_TARGET:
        apply_target_locked();
        break;

    case COIL_CMD_POWER_ON:
        s_holding[HOLDING_TARGET_POWER] = 1U;
        apply_target_locked();
        break;

    case COIL_CMD_POWER_OFF:
        s_holding[HOLDING_TARGET_POWER] = 0U;
        s_input[INPUT_ACTUAL_POWER] = 0U;
        s_input[INPUT_ACTUAL_VAL] = 0U;
        s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
        s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
        sequence_stop(&s_sequence);
        s_transition_active = false;
        break;

    case COIL_CMD_START_SEQUENCE: {
        uint16_t seq_id = s_holding[HOLDING_SEQ_ID];
        bool started;

        if (seq_id >= 100U) {
            uint16_t total = s_holding[HOLDING_SEQ_TOTAL_STEPS];
            uint16_t expected = s_holding[HOLDING_SEQ_CRC];
            bool valid = (total > 0U) &&
                         (total <= (uint16_t)SEQ_MAX_STEPS) &&
                         (expected == sequence_crc16(s_custom_steps, total));

            started = valid &&
                      sequence_start_custom(
                          &s_sequence, s_custom_steps, total,
                          s_holding[HOLDING_SEQ_SPEED],
                          s_holding[HOLDING_SEQ_DIRECTION],
                          s_holding[HOLDING_SEQ_LOOP_COUNT]);
        } else {
            uint16_t base_sat = s_holding[HOLDING_TARGET_SAT];
            uint16_t base_val = s_holding[HOLDING_TARGET_VAL];
            uint16_t base_bri = s_holding[HOLDING_TARGET_BRIGHTNESS];
            uint16_t base_cct = s_holding[HOLDING_TARGET_CCT];

            if (base_sat == 0U) {
                base_sat = 1000U;
            }
            if (base_val == 0U) {
                base_val = 1000U;
            }
            if (base_bri == 0U) {
                base_bri = 1000U;
            }
            if (base_cct == 0U) {
                base_cct = 2700U;
            }
            started = sequence_start(
                &s_sequence, seq_id, s_holding[HOLDING_SEQ_SPEED],
                s_holding[HOLDING_SEQ_DIRECTION],
                s_holding[HOLDING_SEQ_LOOP_COUNT],
                s_holding[HOLDING_TARGET_HUE], base_sat, base_val, base_bri,
                base_cct);
        }

        if (started) {
            s_sequence_invalid = false;
            s_holding[HOLDING_TARGET_POWER] = 1U;
            s_input[INPUT_ACTUAL_POWER] = 1U;
            s_input[INPUT_ACTUAL_MODE] = 2U;
            s_transition_active = false;
        } else {
            s_sequence_invalid = true;
        }
        break;
    }

    case COIL_CMD_STOP_SEQUENCE:
        sequence_stop(&s_sequence);
        s_input[INPUT_ACTUAL_MODE] = s_holding[HOLDING_TARGET_MODE];
        break;

    case COIL_CMD_SAVE_CONFIG:
        s_config_dirty = false;
        state_save_locked();
        s_state_dirty = false;
        break;

    case COIL_CMD_LOAD_CONFIG:
        s_config_dirty = false;
        (void)state_load_locked();
        s_state_dirty = false;
        break;

    case COIL_CMD_FACTORY_RESET:
        set_defaults_locked();
        if (s_state_path != NULL) {
            (void)unlink(s_state_path);
        }
        s_state_dirty = false;
        break;

    case COIL_CMD_CLEAR_FAULTS:
        s_heartbeat_lost = false;
        s_sequence_invalid = false;
        s_led_open = false;
        s_overtemp_warn = false;
        s_overtemp_crit = false;
        s_overcurrent = false;
        s_fault_latched = false;
        s_fault_latched_code = (uint16_t)FAULT_NONE;
        s_config_corrupt = false;
        s_derated = false;
        s_sim_counted_code = 0U;
        s_sim_heartbeat_lost = false;
        s_hb_snapshot_valid = false;
        s_fault_count = 0U;
        s_last_fault_code = (uint16_t)FAULT_NONE;
        s_last_fault_ts_s = 0U;
        break;

    case COIL_CMD_SELFTEST:
        s_selftest_running = true;
        break;

    case COIL_ENABLE_HEARTBEAT_WATCHDOG:
        s_heartbeat_enabled = true;
        break;

    default:
        break;
    }

    if ((coil_index != (uint16_t)COIL_CMD_SAVE_CONFIG) &&
        (coil_index != (uint16_t)COIL_CMD_LOAD_CONFIG) &&
        (coil_index != (uint16_t)COIL_CMD_FACTORY_RESET)) {
        s_state_dirty = true;
    }
}

void register_map_init(void)
{
    const char *path = getenv("DRIVER_STATE_PATH");

    (void)pthread_mutex_lock(&s_lock);
    s_state_path = ((path != NULL) && (path[0] != '\0')) ? path : NULL;
    identity_load_or_provision();
    set_defaults_locked();
    if (s_state_path != NULL) {
        (void)state_load_locked();
    }
    s_state_dirty = false;
    s_state_elapsed_ms = 0U;
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_lock(void)
{
    (void)pthread_mutex_lock(&s_lock);
}

void register_map_unlock(void)
{
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_apply_command(uint16_t coil_index)
{
    (void)pthread_mutex_lock(&s_lock);
    apply_command_locked(coil_index);
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_tick(uint32_t elapsed_ms)
{
    uint32_t progress;
    uint32_t temp;
    uint32_t current_ma;
    int16_t temp_signed;
    int16_t warn_threshold;
    int16_t crit_threshold;
    bool new_warn;
    bool new_crit;
    bool new_over;
    bool sim_active;
    uint16_t sim_code;
    uint16_t fault_code;
    uint32_t uptime_s;
    uint32_t on_s;

    (void)pthread_mutex_lock(&s_lock);

    s_uptime_ms += elapsed_ms;
    if (s_input[INPUT_ACTUAL_POWER] != 0U) {
        s_on_time_ms += elapsed_ms;
    }

    if (s_sequence.running) {
        sequence_output_t out;

        if (sequence_tick(&s_sequence, elapsed_ms, &out)) {
            s_input[INPUT_ACTUAL_HUE] = clamp_hue(out.hue);
            s_input[INPUT_ACTUAL_SAT] = clamp_permille(out.sat);
            s_input[INPUT_ACTUAL_VAL] = clamp_permille(out.val);
            s_input[INPUT_ACTUAL_BRIGHTNESS] =
                clamp_brightness_locked(out.brightness);
            s_input[INPUT_ACTUAL_CCT] = clamp_cct(out.cct);
            s_input[INPUT_ACTUAL_SEQ_STEP] = out.step;
            s_input[INPUT_ACTUAL_MODE] = 2U;
            s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = 1000U;
        } else {
            s_input[INPUT_ACTUAL_MODE] = s_holding[HOLDING_TARGET_MODE];
            s_input[INPUT_ACTUAL_SEQ_STEP] = 0U;
        }
    } else if (s_transition_active) {
        s_transition_elapsed_ms += elapsed_ms;
        if (s_transition_elapsed_ms >= s_transition_duration_ms) {
            progress = 1000U;
        } else {
            progress = (s_transition_elapsed_ms * 1000U) /
                       s_transition_duration_ms;
        }

        s_input[INPUT_ACTUAL_HUE] = interp_hue(s_tr_hue0, s_tr_hue1, progress);
        s_input[INPUT_ACTUAL_SAT] = interp_u16(s_tr_sat0, s_tr_sat1, progress);
        s_input[INPUT_ACTUAL_VAL] = interp_u16(s_tr_val0, s_tr_val1, progress);
        s_input[INPUT_ACTUAL_CCT] = interp_u16(s_tr_cct0, s_tr_cct1, progress);
        s_input[INPUT_ACTUAL_BRIGHTNESS] = clamp_brightness_locked(
            interp_u16(s_tr_bri0, s_tr_bri1, progress));
        s_input[INPUT_ACTUAL_TRANSITION_PROGRESS] = (uint16_t)progress;

        if (progress >= 1000U) {
            s_transition_active = false;
        }
    } else {
        s_input[INPUT_ACTUAL_SEQ_STEP] = 0U;
    }

    if (s_input[INPUT_ACTUAL_POWER] == 0U) {
        s_input[INPUT_ACTUAL_VAL] = 0U;
        s_input[INPUT_ACTUAL_BRIGHTNESS] = 0U;
    } else if (!s_sequence.running && !s_transition_active) {
        /* In manual mode the current brightness follows the target; that way a
         * temporary derate does not leave the value capped after recovery.
         * The configured min/max limits still apply. */
        s_input[INPUT_ACTUAL_BRIGHTNESS] =
            clamp_brightness_locked(s_holding[HOLDING_TARGET_BRIGHTNESS]);
    }

    /* Simulated fault: evaluated before diagnostics so the derate affects the
     * reported temperature/current. */
    sim_code = s_holding[HOLDING_SIM_FAULT_CODE];
    if (s_holding[HOLDING_SIM_FAULT_MAGIC] != (uint16_t)SIM_FAULT_MAGIC) {
        s_sim_fault_prev = 0U;
    } else if (sim_code != s_sim_fault_prev) {
        s_sim_fault_prev = sim_code;
        s_sim_fault_started_s = s_uptime_ms / 1000U;
    }
    sim_active = (s_holding[HOLDING_SIM_FAULT_MAGIC] ==
                  (uint16_t)SIM_FAULT_MAGIC) &&
                 (sim_code != 0U);
    if (sim_active &&
        (s_holding[HOLDING_SIM_FAULT_DURATION_S] != 0U) &&
        (((s_uptime_ms / 1000U) - s_sim_fault_started_s) >=
         s_holding[HOLDING_SIM_FAULT_DURATION_S])) {
        sim_active = false;
    }
    if (!sim_active) {
        s_sim_counted_code = 0U;
    }

    /* If the heartbeat injection ended or changed to another fault, try to
     * recover the target; the real watchdog may still be down. */
    if (s_sim_heartbeat_lost &&
        (!sim_active || (sim_code != (uint16_t)FAULT_HEARTBEAT_LOST))) {
        s_sim_heartbeat_lost = false;
        heartbeat_try_recover_locked();
    }

    if (sim_active) {
        note_sim_fault_locked(sim_code);
        if (sim_code == (uint16_t)FAULT_OVERTEMP_CRIT ||
            sim_code == (uint16_t)FAULT_OVERCURRENT ||
            sim_code == (uint16_t)FAULT_LED_OPEN) {
            latch_fault_locked(sim_code);
        } else if (sim_code == (uint16_t)FAULT_HEARTBEAT_LOST) {
            if (!s_sim_heartbeat_lost) {
                s_sim_heartbeat_lost = true;
                heartbeat_force_off_locked();
            }
        } else if (sim_code == (uint16_t)FAULT_CONFIG_CORRUPT) {
            if (!s_config_corrupt) {
                enter_config_safe_mode_locked(false);
            }
        } else if (sim_code == (uint16_t)FAULT_OVERTEMP_WARN) {
            s_derated = true;
        }
    }

    if (s_derated) {
        s_input[INPUT_ACTUAL_BRIGHTNESS] = clamp_permille(
            (s_input[INPUT_ACTUAL_BRIGHTNESS] > FAULT_DERATE_BRIGHTNESS)
                ? FAULT_DERATE_BRIGHTNESS
                : s_input[INPUT_ACTUAL_BRIGHTNESS]);
    }

    /* Simulated diagnostics. */
    temp = 250U + (((uint32_t)s_input[INPUT_ACTUAL_BRIGHTNESS] * 20U) / 1000U);
    s_input[INPUT_DIAG_TEMP_C_X10] = (uint16_t)temp;

    current_ma = (((uint32_t)s_input[INPUT_ACTUAL_VAL] *
                   (uint32_t)s_input[INPUT_ACTUAL_BRIGHTNESS]) * 20U) / 1000000U;
    s_input[INPUT_DIAG_CURRENT_MA] = (uint16_t)current_ma;
    s_input[INPUT_DIAG_VOLTAGE_MV] = 12000U;
    s_input[INPUT_DIAG_POWER_MW] =
        (uint16_t)((12000U * current_ma) / 1000U);

    warn_threshold = (int16_t)s_holding[HOLDING_CFG_OVERTEMP_WARN_C];
    crit_threshold = (int16_t)s_holding[HOLDING_CFG_OVERTEMP_CRIT_C];
    if (warn_threshold > crit_threshold) {
        int16_t swap = warn_threshold;

        warn_threshold = crit_threshold;
        crit_threshold = swap;
    }
    temp_signed = (int16_t)temp;
    new_warn = (temp_signed >= warn_threshold) &&
               (temp_signed < crit_threshold);
    new_crit = (temp_signed >= crit_threshold);
    new_over = (s_holding[HOLDING_CFG_MAX_CURRENT_MA] > 0U) &&
               (current_ma > s_holding[HOLDING_CFG_MAX_CURRENT_MA]);

    if ((new_warn && !s_overtemp_warn) || (new_crit && !s_overtemp_crit) ||
        (new_over && !s_overcurrent)) {
        s_fault_count++;
        s_last_fault_ts_s = s_uptime_ms / 1000U;
    }
    s_overtemp_warn = new_warn;
    s_overtemp_crit = new_crit;
    s_overcurrent = new_over;

    if (s_overtemp_crit) {
        latch_fault_locked((uint16_t)FAULT_OVERTEMP_CRIT);
    }
    if (s_overcurrent) {
        latch_fault_locked((uint16_t)FAULT_OVERCURRENT);
    }
    if (s_led_open) {
        latch_fault_locked((uint16_t)FAULT_LED_OPEN);
    }

    fault_code = (uint16_t)FAULT_NONE;
    if (s_heartbeat_lost || s_sim_heartbeat_lost) {
        fault_code = (uint16_t)FAULT_HEARTBEAT_LOST;
    } else if (s_fault_latched) {
        fault_code = s_fault_latched_code;
    } else if (s_config_corrupt) {
        fault_code = (uint16_t)FAULT_CONFIG_CORRUPT;
    } else if (s_overtemp_crit) {
        fault_code = (uint16_t)FAULT_OVERTEMP_CRIT;
    } else if (s_overcurrent) {
        fault_code = (uint16_t)FAULT_OVERCURRENT;
    } else if (s_led_open) {
        fault_code = (uint16_t)FAULT_LED_OPEN;
    } else if (s_sequence_invalid) {
        fault_code = (uint16_t)FAULT_SEQ_INVALID;
    } else if (s_overtemp_warn) {
        fault_code = (uint16_t)FAULT_OVERTEMP_WARN;
    }

    if (sim_active) {
        fault_code = s_fault_latched ? s_fault_latched_code : sim_code;
    }
    if (s_config_corrupt && !s_fault_latched) {
        fault_code = (uint16_t)FAULT_CONFIG_CORRUPT;
    }

    s_derated = s_overtemp_warn ||
                (sim_active && (sim_code == (uint16_t)FAULT_OVERTEMP_WARN));
    if (s_overtemp_warn && !s_fault_latched) {
        s_input[INPUT_ACTUAL_BRIGHTNESS] = clamp_permille(
            (s_input[INPUT_ACTUAL_BRIGHTNESS] > FAULT_DERATE_BRIGHTNESS)
                ? FAULT_DERATE_BRIGHTNESS
                : s_input[INPUT_ACTUAL_BRIGHTNESS]);
    }

    s_input[INPUT_DIAG_FAULT_CODE] = fault_code;
    s_input[INPUT_DIAG_FAULT_COUNT] = s_fault_count;
    if (fault_code != (uint16_t)FAULT_NONE) {
        s_last_fault_code = fault_code;
    }
    s_input[INPUT_DIAG_LAST_FAULT_CODE] = s_last_fault_code;

    uptime_s = s_uptime_ms / 1000U;
    on_s = s_on_time_ms / 1000U;
    s_input[INPUT_DIAG_UPTIME_S_LO] = (uint16_t)(uptime_s & 0xFFFFU);
    s_input[INPUT_DIAG_UPTIME_S_HI] = (uint16_t)((uptime_s >> 16) & 0xFFFFU);
    s_input[INPUT_DIAG_ON_TIME_S_LO] = (uint16_t)(on_s & 0xFFFFU);
    s_input[INPUT_DIAG_ON_TIME_S_HI] = (uint16_t)((on_s >> 16) & 0xFFFFU);
    s_input[INPUT_DIAG_LAST_FAULT_TS_LO] =
        (uint16_t)(s_last_fault_ts_s & 0xFFFFU);
    s_input[INPUT_DIAG_LAST_FAULT_TS_HI] =
        (uint16_t)((s_last_fault_ts_s >> 16) & 0xFFFFU);

    /* Status discrete inputs. */
    s_discrete[DISCRETE_STATUS_POWER_ON] =
        (s_input[INPUT_ACTUAL_POWER] != 0U) ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_SEQUENCE_RUNNING] =
        s_sequence.running ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_CONFIG_DIRTY] = s_config_dirty ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_FAULT_ACTIVE] =
        (fault_code != (uint16_t)FAULT_NONE) ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_FAULT_LATCHED] = s_fault_latched ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_CONFIG_SAFE_MODE] =
        s_config_corrupt ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_OVERTEMP_WARNING] = s_overtemp_warn ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_OVERTEMP_CRITICAL] = s_overtemp_crit ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_OVERCURRENT] = s_overcurrent ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_LED_OPEN] = s_led_open ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_HEARTBEAT_LOST] =
        (s_heartbeat_lost || s_sim_heartbeat_lost) ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_SELFTEST_RUNNING] = s_selftest_running ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_SEQUENCE_INVALID] =
        s_sequence_invalid ? 1U : 0U;

    if (s_fault_latched) {
        s_discrete[DISCRETE_STATUS_FAULT_ACTIVE] = 1U;
        s_discrete[DISCRETE_STATUS_FAULT_LATCHED] = 1U;
        if (s_fault_latched_code == (uint16_t)FAULT_OVERTEMP_CRIT) {
            s_discrete[DISCRETE_STATUS_OVERTEMP_CRITICAL] = 1U;
        } else if (s_fault_latched_code == (uint16_t)FAULT_OVERCURRENT) {
            s_discrete[DISCRETE_STATUS_OVERCURRENT] = 1U;
        } else if (s_fault_latched_code == (uint16_t)FAULT_LED_OPEN) {
            s_discrete[DISCRETE_STATUS_LED_OPEN] = 1U;
        }
    }

    if (sim_active) {
        s_discrete[DISCRETE_STATUS_FAULT_ACTIVE] = 1U;
        if (sim_code == (uint16_t)FAULT_OVERTEMP_WARN) {
            s_discrete[DISCRETE_STATUS_OVERTEMP_WARNING] = 1U;
        } else if (sim_code == (uint16_t)FAULT_OVERTEMP_CRIT) {
            s_discrete[DISCRETE_STATUS_OVERTEMP_CRITICAL] = 1U;
        } else if (sim_code == (uint16_t)FAULT_OVERCURRENT) {
            s_discrete[DISCRETE_STATUS_OVERCURRENT] = 1U;
        } else if (sim_code == (uint16_t)FAULT_LED_OPEN) {
            s_discrete[DISCRETE_STATUS_LED_OPEN] = 1U;
        } else if (sim_code == (uint16_t)FAULT_HEARTBEAT_LOST) {
            s_discrete[DISCRETE_STATUS_HEARTBEAT_LOST] = 1U;
        }
    }

    s_selftest_running = false; /* single-tick self-test */

    if (s_state_path != NULL) {
        if (!s_state_dirty) {
            s_state_elapsed_ms = 0U;
        } else {
            s_state_elapsed_ms += elapsed_ms;
            if (s_state_elapsed_ms >= STATE_SAVE_INTERVAL_MS) {
                state_save_locked();
                s_state_dirty = false;
                s_state_elapsed_ms = 0U;
            }
        }
    }

    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_watchdog_tick(void)
{
    uint16_t timeout;
    uint16_t heartbeat;

    (void)pthread_mutex_lock(&s_lock);

    timeout = s_holding[HOLDING_CFG_HEARTBEAT_TIMEOUT_S];
    heartbeat = s_holding[HOLDING_HEARTBEAT_COUNTER];

    if (s_heartbeat_enabled && (timeout > 0U)) {
        if (heartbeat != s_last_heartbeat) {
            s_last_heartbeat = heartbeat;
            s_heartbeat_elapsed_s = 0U;
            if (s_heartbeat_lost) {
                s_heartbeat_lost = false;
                heartbeat_try_recover_locked();
            }
        } else if (!s_heartbeat_lost) {
            s_heartbeat_elapsed_s++;
            if (s_heartbeat_elapsed_s >= timeout) {
                s_heartbeat_lost = true;
                heartbeat_force_off_locked();
            }
        }
    } else {
        s_last_heartbeat = heartbeat;
        s_heartbeat_elapsed_s = 0U;
        if (s_heartbeat_lost) {
            s_heartbeat_lost = false;
            heartbeat_try_recover_locked();
        }
    }

    s_input[INPUT_DIAG_HEARTBEAT_ECHO] = s_last_heartbeat;
    s_discrete[DISCRETE_STATUS_HEARTBEAT_LOST] =
        (s_heartbeat_lost || s_sim_heartbeat_lost) ? 1U : 0U;
    s_discrete[DISCRETE_STATUS_CONFIG_SAFE_MODE] =
        s_config_corrupt ? 1U : 0U;

    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_get_coils(uint8_t *out, uint16_t count)
{
    uint16_t n;
    if (out == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    n = (count > (uint16_t)COIL_COUNT) ? (uint16_t)COIL_COUNT : count;
    (void)memcpy(out, s_coils, (size_t)n);
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_get_discrete(uint8_t *out, uint16_t count)
{
    uint16_t n;
    if (out == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    n = (count > (uint16_t)DISCRETE_COUNT) ? (uint16_t)DISCRETE_COUNT : count;
    (void)memcpy(out, s_discrete, (size_t)n);
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_get_holding(uint16_t *out, uint16_t count)
{
    uint16_t n;
    if (out == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    n = (count > (uint16_t)HOLDING_COUNT) ? (uint16_t)HOLDING_COUNT : count;
    (void)memcpy(out, s_holding, (size_t)n * sizeof(uint16_t));
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_get_input(uint16_t *out, uint16_t count)
{
    uint16_t n;
    if (out == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&s_lock);
    n = (count > (uint16_t)INPUT_COUNT) ? (uint16_t)INPUT_COUNT : count;
    (void)memcpy(out, s_input, (size_t)n * sizeof(uint16_t));
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_set_coils(const uint8_t *values, uint16_t count)
{
    uint16_t i;

    if (values == NULL) {
        return;
    }

    (void)pthread_mutex_lock(&s_lock);
    for (i = 0U; (i < count) && (i < (uint16_t)COIL_COUNT); i++) {
        if (i == (uint16_t)COIL_ENABLE_HEARTBEAT_WATCHDOG) {
            s_heartbeat_enabled = (values[i] != 0U);
            s_coils[i] = s_heartbeat_enabled ? 1U : 0U;
            continue;
        }

        if ((values[i] != 0U) && (s_coils[i] == 0U)) {
            s_coils[i] = 1U;
            apply_command_locked(i);
        }
        s_coils[i] = 0U; /* command auto-reset */
    }
    (void)pthread_mutex_unlock(&s_lock);
}

void register_map_set_holding(const uint16_t *values, uint16_t count)
{
    uint16_t i;
    bool state_changed = false;

    if (values == NULL) {
        return;
    }

    (void)pthread_mutex_lock(&s_lock);
    for (i = 0U; (i < count) && (i < (uint16_t)HOLDING_COUNT); i++) {
        uint16_t next = sanitize_holding(i, values[i]);

        if ((i == (uint16_t)HOLDING_HEARTBEAT_COUNTER) ||
            (i == (uint16_t)HOLDING_SIM_FAULT_CODE) ||
            (i == (uint16_t)HOLDING_SIM_FAULT_DURATION_S) ||
            (i == (uint16_t)HOLDING_SIM_FAULT_MAGIC)) {
            s_holding[i] = next;
        } else {
            if (s_holding[i] != next) {
                s_config_dirty = true;
                if ((i <= (uint16_t)HOLDING_SEQ_LOOP_COUNT) ||
                    ((i >= (uint16_t)HOLDING_CFG_MAX_BRIGHTNESS) &&
                     (i <= (uint16_t)HOLDING_CFG_MAX_CURRENT_MA))) {
                    state_changed = true;
                }
            }
            s_holding[i] = next;
        }
    }
    capture_custom_step_locked();
    if (state_changed) {
        s_state_dirty = true;
    }
    (void)pthread_mutex_unlock(&s_lock);
}

uint8_t register_map_get_coil_bit(uint16_t index)
{
    uint8_t value = 0U;
    (void)pthread_mutex_lock(&s_lock);
    if (index < (uint16_t)COIL_COUNT) {
        value = s_coils[index];
    }
    (void)pthread_mutex_unlock(&s_lock);
    return value;
}

uint8_t register_map_get_discrete_bit(uint16_t index)
{
    uint8_t value = 0U;
    (void)pthread_mutex_lock(&s_lock);
    if (index < (uint16_t)DISCRETE_COUNT) {
        value = s_discrete[index];
    }
    (void)pthread_mutex_unlock(&s_lock);
    return value;
}

uint16_t register_map_get_holding_reg(uint16_t index)
{
    uint16_t value = 0U;
    (void)pthread_mutex_lock(&s_lock);
    if (index < (uint16_t)HOLDING_COUNT) {
        value = s_holding[index];
    }
    (void)pthread_mutex_unlock(&s_lock);
    return value;
}

uint16_t register_map_get_input_reg(uint16_t index)
{
    uint16_t value = 0U;
    (void)pthread_mutex_lock(&s_lock);
    if (index < (uint16_t)INPUT_COUNT) {
        value = s_input[index];
    }
    (void)pthread_mutex_unlock(&s_lock);
    return value;
}
