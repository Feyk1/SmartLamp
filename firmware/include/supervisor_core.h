#ifndef SUPERVISOR_CORE_H
#define SUPERVISOR_CORE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Supervisor state machine (ADR-004), split from the process loop so it can
 * be tested without fork/exec:
 *
 *   POWERED_OFF --power_on--> BOOTING --250 ms--> APP_RUNNING
 *   BOOTING --forcer--> BOOTLOADER_MODE
 *   APP_RUNNING --crash--> FAULT
 *   any state --power_off--> POWERED_OFF
 *
 * The real supervisor performs the effects (fork/exec, signals, flash erase)
 * in supervisor.c; here only the state is decided.
 */

#define SP_BOOT_DELAY_MS 250U

typedef enum {
    SP_POWERED_OFF = 0,
    SP_BOOTING,
    SP_APP_RUNNING,
    SP_BOOTLOADER_MODE,
    SP_FAULT
} sp_state_t;

typedef enum {
    SP_CMD_INVALID = 0,
    SP_CMD_STATUS,
    SP_CMD_POWER_ON,
    SP_CMD_POWER_OFF,
    SP_CMD_RESET,
    SP_CMD_ENTER_BOOTLOADER,
    SP_CMD_FACTORY_RESET
} sp_cmd_t;

typedef struct {
    sp_state_t state;
    bool forced;
    uint32_t boot_deadline_ms;
    int last_exit_status;
    bool has_exit_status;
} sp_core_t;

/** @brief Initializes the machine in POWERED_OFF. */
void sp_init(sp_core_t *core);

/**
 * @brief Parses a control line (no token, no `\n`).
 *
 * @return Recognized command or SP_CMD_INVALID.
 */
sp_cmd_t sp_parse_command(const char *line);

/** @brief Stable name of the state (for JSON and logs). */
const char *sp_state_name(sp_state_t state);

/** @brief Tells whether `power_on` applies in the current state. */
bool sp_can_power_on(const sp_core_t *core);

/**
 * @brief Transition to BOOTING.
 *
 * @param forced true if booting in recovery mode.
 * @return true if applied (OFF or FAULT state); false if a child already
 *         exists.
 */
bool sp_on_power_on(sp_core_t *core, bool forced, uint32_t now_ms);

/**
 * @brief Applies the boot timeout (BOOTING → APP_RUNNING/BOOTLOADER).
 *
 * @return true if the state changed.
 */
bool sp_on_boot_timeout(sp_core_t *core, uint32_t now_ms);

/**
 * @brief Records the death of the child.
 *
 * If it was booting/running it is a crash → FAULT. If it was already powered
 * off (orderly stop), nothing happens.
 */
void sp_on_child_exit(sp_core_t *core, int status);

/** @brief Forces POWERED_OFF (orderly stop). */
void sp_set_powered_off(sp_core_t *core);

/** @brief Forces BOOTLOADER_MODE (forced boot without waiting for a timeout). */
void sp_set_bootloader_mode(sp_core_t *core);

#endif /* SUPERVISOR_CORE_H */
