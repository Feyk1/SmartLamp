#ifndef OTA_CLIENT_H
#define OTA_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Requests an OTA update (`v_ota_task` carries it out).
 *
 * It only validates and stores the request; the download does not block the
 * sender of the command.
 *
 * @param version Version to download ("1.1.0").
 * @return 0 if accepted; -1 if the version is not valid.
 */
int ota_client_request(const char *version);

/**
 * @brief Requests a reboot (the device goes back to the bootloader).
 *
 * `v_ota_task` processes the request so the command ack can be sent before
 * the process is replaced.
 *
 * @return 0 if accepted; -1 otherwise.
 */
int ota_client_request_reboot(void);

/**
 * @brief Downloads `version` into the flash staging partition.
 *
 * @param version Requested version.
 * @return 0 if the download finished with HTTP 200 and a valid minimum size.
 */
int ota_client_stage(const char *version);

/**
 * @brief Does `execv` into the bootloader to apply the OTA.
 *
 * Does not return if `execv` succeeds.
 *
 * @return -1 if it could not be executed (the caller stays alive).
 */
int ota_client_reboot(void);

/**
 * @brief Task that processes OTA requests and reboots into the bootloader.
 *
 * @param pv_parameters Task parameters (unused).
 */
void v_ota_task(void *pv_parameters);

#endif /* OTA_CLIENT_H */
