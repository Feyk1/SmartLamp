/**
 * @file freertos_hooks.c
 * @brief FreeRTOS diagnostic hooks (assert, stack overflow, malloc).
 */

#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

/**
 * @brief Callback invoked by configASSERT() when an assertion fails.
 *
 * @param file File where the assertion failed.
 * @param line Line where the assertion failed.
 */
void v_assert_called(const char *file, long line)
{
    (void)fprintf(stderr, "FreeRTOS ASSERT: %s:%ld\n", file, line);
    abort();
}

/**
 * @brief Hook invoked when a task stack overflow is detected.
 *
 * @param xTask Handle of the task that overflowed (unused).
 * @param pcTaskName Name of the task that overflowed.
 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)fprintf(stderr, "FreeRTOS: stack overflow in '%s'\n", pcTaskName);
    abort();
}

/**
 * @brief Hook invoked when pvPortMalloc() cannot satisfy an allocation.
 */
void vApplicationMallocFailedHook(void)
{
    (void)fprintf(stderr, "FreeRTOS: heap exhausted (pvPortMalloc failed)\n");
    abort();
}
