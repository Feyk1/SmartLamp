#ifndef TELEMETRY_H
#define TELEMETRY_H

#include <stdbool.h>
#include <stdint.h>

#include "modbus_client.h"

/**
 * @brief Initializes telemetry (`DEVICE_ID`) and enqueues the `boot` event.
 *
 * @return 0 on success; -1 otherwise.
 */
int telemetry_init(void);

/** @brief Tells whether telemetry was initialized. */
bool telemetry_is_ready(void);

/**
 * @brief Enqueues an event for the next batch.
 *
 * @param type       Type (`boot`, `fault`, `state_transition`, ...).
 * @param detail_json Already serialized detail (`"{}"` if not applicable).
 */
void telemetry_event(const char *type, const char *detail_json);

/**
 * @brief Sends `POST /telemetry/health` with heap, tasks and diagnostics.
 *
 * @return 0 if the backend answered 2xx; -1 otherwise.
 */
int telemetry_send_health(void);

/**
 * @brief Sends `POST /telemetry/state` (fallback when the WS is down).
 *
 * @return 0 if the backend answered 2xx; -1 otherwise.
 */
int telemetry_send_state(const modbus_light_state_t *state);

/**
 * @brief Drains the batch of pending events.
 *
 * @return 0 if there were no events or they were delivered; -1 if the POST
 *         failed.
 */
int telemetry_flush_events(void);

/**
 * @brief Telemetry task: health every 30 s and events in batches.
 *
 * @param pv_parameters Task parameters (unused).
 */
void v_telemetry_task(void *pv_parameters);

#endif /* TELEMETRY_H */
