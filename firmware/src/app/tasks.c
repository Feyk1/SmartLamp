/**
 * @file tasks.c
 * @brief Base firmware tasks: control, communications and monitor.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "log.h"
#include "tasks.h"

#define CONTROL_PERIOD_MS   100U
#define CONTROL_LOG_EVERY   10U   /* 10 * 100 ms = 1 s */
#define MONITOR_PERIOD_MS   5000U

/**
 * @brief Control task: 100 ms cycle and a log line every second.
 *
 * @param pv_parameters Task parameters (unused).
 */
void v_control_task(void *pv_parameters)
{
    TickType_t last_wake = xTaskGetTickCount();
    unsigned long tick = 0UL;

    (void)pv_parameters;

    for (;;) {
        tick++;
        if ((tick % CONTROL_LOG_EVERY) == 0UL) {
            LOG_INFO("control tick=%lu", tick);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
    }
}

/**
 * @brief Monitor task: free heap and stack high-water mark every 5 s.
 *
 * The heap summary is emitted at INFO; the per-task detail at DEBUG.
 *
 * @param pv_parameters Task parameters (unused).
 */
void v_monitor_task(void *pv_parameters)
{
    (void)pv_parameters;

    for (;;) {
        UBaseType_t count = uxTaskGetNumberOfTasks();
        TaskStatus_t *status = (TaskStatus_t *)pvPortMalloc(
            (size_t)count * sizeof(TaskStatus_t));

        if (status == NULL) {
            LOG_WARN("monitor: out of memory for TaskStatus_t");
        } else {
            UBaseType_t used = uxTaskGetSystemState(status, count, NULL);

            LOG_INFO("monitor: heap=%u B, tareas=%u",
                     (unsigned)xPortGetFreeHeapSize(), (unsigned)used);

            for (UBaseType_t i = 0U; i < used; i++) {
                LOG_DEBUG("  %-12s prio=%u hwm=%u B",
                          status[i].pcTaskName,
                          (unsigned)status[i].uxCurrentPriority,
                          (unsigned)(status[i].usStackHighWaterMark *
                                     sizeof(StackType_t)));
            }
            vPortFree(status);
        }

        vTaskDelay(pdMS_TO_TICKS(MONITOR_PERIOD_MS));
    }
}
