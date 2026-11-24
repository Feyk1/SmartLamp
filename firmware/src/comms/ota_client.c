/**
 * @file ota_client.c
 * @brief OTA client: downloads the `.ota` into staging and reboots into the
 *        bootloader.
 *
 * The WS/REST command only enqueues the request; `v_ota_task` performs the
 * download (streaming HTTP into flash) and the `execv` into the bootloader,
 * which verifies, applies and runs the new slot (docs/04 §7).
 */

#include "ota_client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "flash.h"
#include "log.h"
#include "ota.h"
#include "rest_client.h"

#define OTA_VERSION_MAX  16U
#define OTA_POLL_MS      500U

static SemaphoreHandle_t s_lock = NULL;
static char s_version[OTA_VERSION_MAX];
static bool s_pending = false;
static bool s_reboot_pending = false;

/**
 * @brief Checks that the version is "N.N.N" (digits and dots only).
 */
static bool version_valid(const char *version)
{
    size_t len;
    unsigned dots = 0U;

    if (version == NULL) {
        return false;
    }
    len = strlen(version);
    if ((len < 5U) || (len >= OTA_VERSION_MAX)) {
        return false;
    }
    for (size_t i = 0U; i < len; i++) {
        if (version[i] == '.') {
            dots++;
        } else if ((version[i] < '0') || (version[i] > '9')) {
            return false;
        }
    }
    return dots == 2U;
}

static SemaphoreHandle_t ota_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock;
}

int ota_client_request_reboot(void)
{
    SemaphoreHandle_t lock = ota_lock();

    if (lock == NULL) {
        return -1;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    s_reboot_pending = true;
    (void)xSemaphoreGive(lock);
    return 0;
}

static bool take_reboot_request(void)
{
    SemaphoreHandle_t lock = ota_lock();
    bool pending = false;

    if (lock == NULL) {
        return false;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    pending = s_reboot_pending;
    s_reboot_pending = false;
    (void)xSemaphoreGive(lock);
    return pending;
}

int ota_client_request(const char *version)
{
    SemaphoreHandle_t lock = ota_lock();

    if ((lock == NULL) || (!version_valid(version))) {
        return -1;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    (void)strncpy(s_version, version, sizeof(s_version) - 1U);
    s_version[sizeof(s_version) - 1U] = '\0';
    s_pending = true;
    (void)xSemaphoreGive(lock);
    return 0;
}

/**
 * @brief Takes the pending request (if any).
 *
 * @return true if there was a version and it was copied into `out`.
 */
static bool take_request(char *out, size_t size)
{
    SemaphoreHandle_t lock = ota_lock();
    bool pending = false;

    if ((lock == NULL) || (out == NULL) || (size == 0U)) {
        return false;
    }
    if (xSemaphoreTake(lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (s_pending) {
        (void)snprintf(out, size, "%s", s_version);
        s_pending = false;
        pending = true;
    }
    (void)xSemaphoreGive(lock);
    return pending;
}

/**
 * @brief Sink for `rest_client_get_stream`: writes the body into staging.
 */
static int flash_sink(void *ctx, const uint8_t *data, size_t len)
{
    uint32_t *offset = (uint32_t *)ctx;

    if ((*offset + (uint32_t)len) > FLASH_STAGING_SIZE) {
        LOG_WARN("ota: download larger than staging");
        return -1;
    }
    if (flash_write(FLASH_STAGING_OFFSET + *offset, data, len) != FLASH_OK) {
        return -1;
    }
    *offset += (uint32_t)len;
    return 0;
}

int ota_client_stage(const char *version)
{
    char path[128];
    uint32_t offset = 0U;
    size_t total = 0U;
    int status;

    if ((!version_valid(version)) || (!rest_client_is_ready())) {
        return -1;
    }

    /* Clear the previous header so artifacts are not mixed. */
    (void)flash_erase(FLASH_STAGING_OFFSET, 512U);
    (void)flash_sync();

    if ((size_t)snprintf(path, sizeof(path), "/api/v1/ota/%s/download",
                         version) >= sizeof(path)) {
        return -1;
    }

    status = rest_client_get_stream(path, flash_sink, &offset, &total);
    if ((status != 200) || (offset < OTA_MIN_SIZE)) {
        LOG_WARN("ota: download of v%s failed (HTTP %d, %u B)", version,
                 status, (unsigned)offset);
        (void)flash_erase(FLASH_STAGING_OFFSET, 512U);
        (void)flash_sync();
        return -1;
    }
    (void)flash_sync();
    LOG_INFO("ota: v%s downloaded (%u B) to staging", version,
             (unsigned)offset);
    return 0;
}

int ota_client_reboot(void)
{
    const char *path = getenv("BOOTLOADER_BIN");
    char *const argv[] = {(char *)"bootloader", NULL};
    struct itimerval zero;

    if (path == NULL) {
        path = "bootloader";
    }

    /* The FreeRTOS tick uses SIGALRM + ITIMER_REAL; both survive execve and
     * would kill the bootloader with the default action. */
    (void)memset(&zero, 0, sizeof(zero));
    (void)setitimer(ITIMER_REAL, &zero, NULL);
    (void)signal(SIGALRM, SIG_IGN);

    LOG_INFO("ota: restarting to bootloader (%s)", path);
    (void)execv(path, argv);
    LOG_ERROR("ota: execv %s failed: %s", path, strerror(errno));
    return -1;
}

void v_ota_task(void *pv_parameters)
{
    char version[OTA_VERSION_MAX];

    (void)pv_parameters;

    for (;;) {
        if (take_request(version, sizeof(version))) {
            LOG_INFO("ota: requested v%s", version);
            if (ota_client_stage(version) == 0) {
                (void)ota_client_reboot();
            }
        }
        if (take_reboot_request()) {
            LOG_INFO("ota: restart requested from panel");
            (void)ota_client_reboot();
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_POLL_MS));
    }
}
