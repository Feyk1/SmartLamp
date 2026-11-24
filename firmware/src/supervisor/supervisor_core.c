/**
 * @file supervisor_core.c
 * @brief Supervisor state machine (ADR-004).
 */

#include "supervisor_core.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool equals_ci(const char *a, const char *b)
{
    if ((a == NULL) || (b == NULL)) {
        return false;
    }
    while ((*a != '\0') && (*b != '\0')) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

void sp_init(sp_core_t *core)
{
    if (core == NULL) {
        return;
    }
    (void)memset(core, 0, sizeof(*core));
    core->state = SP_POWERED_OFF;
}

sp_cmd_t sp_parse_command(const char *line)
{
    char buffer[32];
    size_t len;
    size_t start = 0U;
    size_t end;

    if (line == NULL) {
        return SP_CMD_INVALID;
    }
    len = strlen(line);
    end = len;
    while ((start < end) && (isspace((unsigned char)line[start]) != 0)) {
        start++;
    }
    while ((end > start) && (isspace((unsigned char)line[end - 1U]) != 0)) {
        end--;
    }
    if ((end - start) == 0U) {
        return SP_CMD_INVALID;
    }
    if ((end - start) >= sizeof(buffer)) {
        return SP_CMD_INVALID;
    }
    (void)memcpy(buffer, line + start, end - start);
    buffer[end - start] = '\0';

    if (equals_ci(buffer, "STATUS")) {
        return SP_CMD_STATUS;
    }
    if (equals_ci(buffer, "POWER_ON")) {
        return SP_CMD_POWER_ON;
    }
    if (equals_ci(buffer, "POWER_OFF")) {
        return SP_CMD_POWER_OFF;
    }
    if (equals_ci(buffer, "RESET")) {
        return SP_CMD_RESET;
    }
    if (equals_ci(buffer, "ENTER_BOOTLOADER")) {
        return SP_CMD_ENTER_BOOTLOADER;
    }
    if (equals_ci(buffer, "FACTORY_RESET")) {
        return SP_CMD_FACTORY_RESET;
    }
    return SP_CMD_INVALID;
}

const char *sp_state_name(sp_state_t state)
{
    switch (state) {
    case SP_POWERED_OFF:     return "POWERED_OFF";
    case SP_BOOTING:         return "BOOTING";
    case SP_APP_RUNNING:     return "APP_RUNNING";
    case SP_BOOTLOADER_MODE: return "BOOTLOADER_MODE";
    case SP_FAULT:           return "FAULT";
    default:                 return "UNKNOWN";
    }
}

bool sp_can_power_on(const sp_core_t *core)
{
    if (core == NULL) {
        return false;
    }
    return (core->state == SP_POWERED_OFF) || (core->state == SP_FAULT);
}

bool sp_on_power_on(sp_core_t *core, bool forced, uint32_t now_ms)
{
    if ((core == NULL) || (!sp_can_power_on(core))) {
        return false;
    }
    core->state = SP_BOOTING;
    core->forced = forced;
    core->boot_deadline_ms = now_ms + SP_BOOT_DELAY_MS;
    core->has_exit_status = false;
    return true;
}

bool sp_on_boot_timeout(sp_core_t *core, uint32_t now_ms)
{
    if ((core == NULL) || (core->state != SP_BOOTING)) {
        return false;
    }
    if ((int32_t)(now_ms - core->boot_deadline_ms) < 0) {
        return false;
    }
    core->state = core->forced ? SP_BOOTLOADER_MODE : SP_APP_RUNNING;
    return true;
}

void sp_on_child_exit(sp_core_t *core, int status)
{
    if (core == NULL) {
        return;
    }
    if ((core->state == SP_BOOTING) || (core->state == SP_APP_RUNNING) ||
        (core->state == SP_BOOTLOADER_MODE)) {
        core->state = SP_FAULT;
        core->last_exit_status = status;
        core->has_exit_status = true;
    }
}

void sp_set_powered_off(sp_core_t *core)
{
    if (core != NULL) {
        core->state = SP_POWERED_OFF;
    }
}

void sp_set_bootloader_mode(sp_core_t *core)
{
    if (core != NULL) {
        core->state = SP_BOOTLOADER_MODE;
        core->forced = true;
    }
}
