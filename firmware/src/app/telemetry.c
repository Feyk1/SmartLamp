/**
 * @file telemetry.c
 * @brief REST telemetry: periodic health, state and batched events.
 *
 * Implements channel B of docs/02 §4.1. State travels over WS as the main
 * path; `telemetry_send_state` is the fallback when the WS is down.
 */

#include "telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "FreeRTOS.h"
#include "task.h"

#include "ab.h"
#include "cJSON.h"
#include "fw_version.h"
#include "log.h"
#include "modbus_client.h"
#include "rest_client.h"

#define TELEMETRY_DEVICE_MAX 48U
#define TELEMETRY_EVENT_MAX 8U
#define TELEMETRY_EVENT_LEN 192U
#define TELEMETRY_BATCH_MAX 1800U
#define TELEMETRY_PERIOD_MS 30000U
#define TELEMETRY_FIRST_DELAY_MS 2000U

static char s_device_id[TELEMETRY_DEVICE_MAX] = "lamp-8f2a";
static char s_slot[2] = "A";
static char s_version[24] = FW_VERSION_STRING;
static char s_events[TELEMETRY_EVENT_MAX][TELEMETRY_EVENT_LEN];
static unsigned s_event_count = 0U;
static int s_last_fault = -1;
static bool s_ready = false;

/**
 * @brief Writes the current instant in ISO-8601 UTC (`YYYY-MM-DDTHH:MM:SSZ`).
 */
static void telemetry_now_iso(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_utc;

    if ((buf == NULL) || (size == 0U)) {
        return;
    }
    if (gmtime_r(&now, &tm_utc) == NULL) {
        (void)snprintf(buf, size, "1970-01-01T00:00:00Z");
        return;
    }
    (void)strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

/**
 * @brief Adds the high-water mark (bytes) of each task to the `tasks` object.
 */
static void telemetry_collect_tasks(cJSON *tasks)
{
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *status = (TaskStatus_t *)pvPortMalloc(
        (size_t)count * sizeof(TaskStatus_t));

    if (status == NULL) {
        return;
    }

    count = uxTaskGetSystemState(status, count, NULL);
    for (UBaseType_t i = 0U; i < count; i++) {
        cJSON_AddNumberToObject(
            tasks, status[i].pcTaskName,
            (double)(status[i].usStackHighWaterMark * sizeof(StackType_t)));
    }
    vPortFree(status);
}

/**
 * @brief Serializes the real state with the same schema as the WS.
 */
static cJSON *telemetry_state_object(const modbus_light_state_t *state)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *color;
    cJSON *sequence;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "power", state->power != 0U);
    cJSON_AddNumberToObject(root, "mode", state->mode);
    cJSON_AddNumberToObject(root, "brightness", state->brightness);
    color = cJSON_AddObjectToObject(root, "color");
    if (color != NULL) {
        cJSON_AddNumberToObject(color, "h", state->hue);
        cJSON_AddNumberToObject(color, "s", state->sat);
        cJSON_AddNumberToObject(color, "v", state->val);
    }
    cJSON_AddNumberToObject(root, "cct", state->cct);
    sequence = cJSON_AddObjectToObject(root, "sequence");
    if (sequence != NULL) {
        cJSON_AddNumberToObject(sequence, "step", state->seq_step);
    }
    cJSON_AddNumberToObject(root, "transition_progress",
                            state->transition_progress);
    return root;
}

/**
 * @brief Generic POST with a `cJSON` body; frees the object.
 */
static int telemetry_post_json(const char *path, cJSON *root)
{
    char *text;
    int rc = -1;

    if (root == NULL) {
        return -1;
    }
    text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        rc = rest_client_post(path, text);
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return ((rc >= 200) && (rc < 300)) ? 0 : -1;
}

int telemetry_init(void)
{
    const char *device_id = getenv("DEVICE_ID");
    ab_state_t ab;
    char detail[96];

    if ((device_id != NULL) && (device_id[0] != '\0') &&
        (strlen(device_id) < sizeof(s_device_id))) {
        (void)strncpy(s_device_id, device_id, sizeof(s_device_id) - 1U);
        s_device_id[sizeof(s_device_id) - 1U] = '\0';
    }

    s_event_count = 0U;
    s_last_fault = -1;
    s_ready = true;

    /* Active slot and version from the A/B metadata (set by the bootloader). */
    if (ab_load(&ab) == AB_OK) {
        s_slot[0] = (ab.active_slot == AB_SLOT_B) ? 'B' : 'A';
        (void)snprintf(s_version, sizeof(s_version), "%u.%u.%u",
                       (unsigned)ab.active_version.major,
                       (unsigned)ab.active_version.minor,
                       (unsigned)ab.active_version.patch);
    }
    (void)snprintf(detail, sizeof(detail),
                   "{\"slot\":\"%s\",\"version\":\"%s\",\"cause\":\"power_on\"}",
                   s_slot, s_version);
    telemetry_event("boot", detail);
    return 0;
}

bool telemetry_is_ready(void)
{
    return s_ready;
}

void telemetry_event(const char *type, const char *detail_json)
{
    char ts[24];
    unsigned slot;

    if ((!s_ready) || (type == NULL)) {
        return;
    }

    telemetry_now_iso(ts, sizeof(ts));
    if (s_event_count == TELEMETRY_EVENT_MAX) {
        /* Queue full: the oldest event is dropped. */
        (void)memmove(s_events[0], s_events[1],
                      (TELEMETRY_EVENT_MAX - 1U) * TELEMETRY_EVENT_LEN);
        s_event_count--;
    }
    slot = s_event_count;
    (void)snprintf(s_events[slot], TELEMETRY_EVENT_LEN,
                   "{\"ts\":\"%s\",\"type\":\"%s\",\"detail\":%s}", ts, type,
                   (detail_json != NULL) ? detail_json : "{}");
    s_event_count++;
}

int telemetry_send_health(void)
{
    cJSON *root;
    cJSON *mem;
    cJSON *tasks;
    modbus_diag_t diag;
    char ts[24];
    char serial[20];

    if ((!s_ready) || (!rest_client_is_ready())) {
        return -1;
    }

    telemetry_now_iso(ts, sizeof(ts));
    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }

    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "ts", ts);
    cJSON_AddNumberToObject(
        root, "uptime_s",
        (double)(xTaskGetTickCount() / configTICK_RATE_HZ));
    cJSON_AddStringToObject(root, "fw_version", s_version);
    cJSON_AddStringToObject(root, "slot", s_slot);

    mem = cJSON_AddObjectToObject(root, "mem");
    if (mem != NULL) {
        cJSON_AddNumberToObject(mem, "free_heap",
                                (double)xPortGetFreeHeapSize());
        cJSON_AddNumberToObject(mem, "min_free_heap",
                                (double)xPortGetMinimumEverFreeHeapSize());
    }

    tasks = cJSON_AddObjectToObject(root, "tasks");
    if (tasks != NULL) {
        telemetry_collect_tasks(tasks);
    }

    if ((modbus_client_last_diag(&diag) == 0) && diag.valid) {
        cJSON_AddNumberToObject(root, "led_temp_c",
                                (double)diag.temp_c_x10 / 10.0);
        cJSON_AddNumberToObject(root, "power_estimate_w",
                                (double)diag.power_mw / 1000.0);
        cJSON_AddNumberToObject(root, "fault_code", diag.fault_code);
        cJSON_AddNumberToObject(root, "hw_rev", diag.hw_rev);
        if (diag.serial_text[0] != '\0') {
            cJSON_AddStringToObject(root, "serial", diag.serial_text);
        } else {
            (void)snprintf(serial, sizeof(serial), "SL-%u-%08X",
                           (unsigned)diag.hw_rev, (unsigned)diag.serial_id);
            cJSON_AddStringToObject(root, "serial", serial);
        }
    }

    return telemetry_post_json("/api/v1/telemetry/health", root);
}

int telemetry_send_state(const modbus_light_state_t *state)
{
    cJSON *root;
    cJSON *state_obj;
    char ts[24];

    if ((!s_ready) || (state == NULL)) {
        return -1;
    }

    telemetry_now_iso(ts, sizeof(ts));
    root = cJSON_CreateObject();
    if (root == NULL) {
        return -1;
    }
    cJSON_AddStringToObject(root, "device_id", s_device_id);
    cJSON_AddStringToObject(root, "ts", ts);
    state_obj = telemetry_state_object(state);
    if (state_obj == NULL) {
        cJSON_Delete(root);
        return -1;
    }
    cJSON_AddItemToObject(root, "state", state_obj);

    return telemetry_post_json("/api/v1/telemetry/state", root);
}

int telemetry_flush_events(void)
{
    static char body[TELEMETRY_BATCH_MAX];
    size_t used;
    int n;
    int rc;

    if ((!s_ready) || (!rest_client_is_ready()) || (s_event_count == 0U)) {
        return 0;
    }

    n = snprintf(body, sizeof(body), "{\"device_id\":\"%s\",\"events\":[",
                 s_device_id);
    if (n <= 0) {
        return -1;
    }
    used = (size_t)n;

    for (unsigned i = 0U; i < s_event_count; i++) {
        n = snprintf(body + used, sizeof(body) - used, "%s%s",
                     (i == 0U) ? "" : ",", s_events[i]);
        if ((n <= 0) || ((size_t)n >= (sizeof(body) - used))) {
            return -1; /* does not fit: retried on the next cycle */
        }
        used += (size_t)n;
    }

    n = snprintf(body + used, sizeof(body) - used, "]}");
    if ((n <= 0) || ((size_t)n >= (sizeof(body) - used))) {
        return -1;
    }

    rc = rest_client_post("/api/v1/telemetry/events", body);
    if ((rc >= 200) && (rc < 300)) {
        s_event_count = 0U;
        return 0;
    }
    return -1;
}

void v_telemetry_task(void *pv_parameters)
{
    TickType_t last_wake;
    modbus_diag_t diag;

    (void)pv_parameters;

    /* Give the first diagnostic read some time. */
    vTaskDelay(pdMS_TO_TICKS(TELEMETRY_FIRST_DELAY_MS));
    last_wake = xTaskGetTickCount();

    for (;;) {
        if ((modbus_client_last_diag(&diag) == 0) && diag.valid) {
            if (s_last_fault < 0) {
                s_last_fault = (int)diag.fault_code;
            } else if ((int)diag.fault_code != s_last_fault) {
                char detail[64];

                (void)snprintf(detail, sizeof(detail),
                               "{\"code\":%u,\"previous\":%d}",
                               (unsigned)diag.fault_code, s_last_fault);
                telemetry_event("fault", detail);
                LOG_WARN("telemetry: fault %d -> %u", s_last_fault,
                         (unsigned)diag.fault_code);
                s_last_fault = (int)diag.fault_code;
            }
        }

        if (telemetry_send_health() != 0) {
            LOG_DEBUG("telemetry: health not delivered");
        }
        if (telemetry_flush_events() != 0) {
            LOG_DEBUG("telemetry: pending events (%u)",
                      s_event_count);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
