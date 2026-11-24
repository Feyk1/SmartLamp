/**
 * @file boot_confirm.c
 * @brief Slot confirmation after an OTA (docs/04 §7.2).
 *
 * The app confirms once it has been up for a few seconds and the Modbus link
 * to the driver is operational (minimal self-test). If it does not confirm,
 * the bootloader rolls back on the next boot.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "ab.h"
#include "log.h"
#include "modbus_client.h"
#include "tasks.h"

#define CONFIRM_DELAY_MS 3000U

void v_confirm_task(void *pv_parameters)
{
    ab_state_t state;

    (void)pv_parameters;

    vTaskDelay(pdMS_TO_TICKS(CONFIRM_DELAY_MS));

    if (!modbus_client_is_connected()) {
        LOG_WARN("ota: no Modbus, slot not confirmed");
        vTaskDelete(NULL);
        return;
    }
    if (ab_load(&state) != AB_OK) {
        LOG_WARN("ota: unreadable metadata, slot not confirmed");
        vTaskDelete(NULL);
        return;
    }
    if ((state.flags & AB_FLAG_PENDING_CONFIRMATION) == 0U) {
        LOG_DEBUG("ota: no pending update");
        vTaskDelete(NULL);
        return;
    }

    ab_confirm(&state);
    if (ab_save(&state) == AB_OK) {
        LOG_INFO("ota: slot %c confirmed (v%u.%u.%u)",
                 (state.active_slot == AB_SLOT_A) ? 'A' : 'B',
                 (unsigned)state.active_version.major,
                 (unsigned)state.active_version.minor,
                 (unsigned)state.active_version.patch);
    } else {
        LOG_ERROR("ota: failed to save confirmation");
    }

    vTaskDelete(NULL);
}
