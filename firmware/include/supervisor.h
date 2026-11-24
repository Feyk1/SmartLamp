#ifndef SUPERVISOR_H
#define SUPERVISOR_H

typedef enum {
    SUPERVISOR_POWERED_OFF = 0,
    SUPERVISOR_BOOTING,
    SUPERVISOR_APP_RUNNING,
    SUPERVISOR_BOOTLOADER_MODE,
    SUPERVISOR_FAULT
} supervisor_state_t;

/**
 * @brief Runs the supervisor lifecycle.
 *
 * Launches the given binary (the bootloader) with fork/execve, waits for it
 * to finish and forwards SIGTERM/SIGINT to the child.
 *
 * @param boot_path Path to the bootloader binary.
 * @return 0 if the cycle ends correctly; -1 on error.
 */
int supervisor_run(const char *boot_path);

/**
 * @brief Returns the human-readable name of a supervisor state.
 *
 * @param state State.
 * @return Constant string with the name.
 */
const char *supervisor_state_name(supervisor_state_t state);

#endif /* SUPERVISOR_H */
