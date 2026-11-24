#define _GNU_SOURCE

/**
 * @file main.c
 * @brief POSIX bootloader: applies OTA, decides the slot and execve's the app.
 *
 * Flow (docs/04 §7, ADR-003):
 *  1. Loads the A/B metadata (double buffer + CRC).
 *  2. If there is a valid `.ota` in staging, verifies/decrypts it and applies
 *     it to the inactive slot, marking it as pending confirmation.
 *  3. If the pending app did not confirm on the previous boot, it rolls back.
 *  4. Provisions slot A from the factory app if there is no valid image.
 *  5. Runs the active slot with `memfd_create` + `fexecve` (fallback to /tmp).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "ab.h"
#include "flash.h"
#include "fw_version.h"
#include "ota.h"

#define DEFAULT_FLASH_PATH   "flash.bin"
#define DEFAULT_FACTORY_APP  "firmware"
#define DEFAULT_PUBLIC_KEY   "keys/ota_public.pem"
#define DEFAULT_HW_REV       1U
#define ELF_MAGIC_SIZE       4U

static void blog(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)fprintf(stderr, "[boot] ");
    (void)vfprintf(stderr, format, args);
    (void)fprintf(stderr, "\n");
    va_end(args);
    (void)fflush(stderr);
}

static uint32_t slot_offset(uint16_t slot)
{
    return (slot == AB_SLOT_B) ? FLASH_SLOT_B_OFFSET : FLASH_SLOT_A_OFFSET;
}

static uint32_t slot_capacity(uint16_t slot)
{
    return (slot == AB_SLOT_B) ? FLASH_SLOT_B_SIZE : FLASH_SLOT_A_SIZE;
}

static bool slot_has_elf(uint16_t slot)
{
    uint8_t magic[ELF_MAGIC_SIZE];

    if (flash_read(slot_offset(slot), magic, sizeof(magic)) != FLASH_OK) {
        return false;
    }
    return (magic[0] == 0x7FU) && (magic[1] == 'E') && (magic[2] == 'L') &&
           (magic[3] == 'F');
}

/**
 * @brief Reads the payload length from the staging header (LE, offset 20).
 */
static uint32_t staged_payload_len(const uint8_t *header)
{
    return ((uint32_t)header[OTA_OFF_PAYLOAD_LEN]) |
           ((uint32_t)header[OTA_OFF_PAYLOAD_LEN + 1U] << 8U) |
           ((uint32_t)header[OTA_OFF_PAYLOAD_LEN + 2U] << 16U) |
           ((uint32_t)header[OTA_OFF_PAYLOAD_LEN + 3U] << 24U);
}

static int load_aes_key(uint8_t key[OTA_AES_KEY_SIZE])
{
    const char *hex = getenv("OTA_AES_KEY");
    size_t hex_len;

    if ((hex == NULL) || (hex[0] == '\0')) {
        return -1;
    }
    hex_len = strlen(hex);
    if (sodium_hex2bin(key, OTA_AES_KEY_SIZE, hex, hex_len, NULL, NULL, NULL) !=
        0) {
        return -1;
    }
    return 0;
}

/**
 * @brief Applies the staged `.ota` to the inactive slot if there is one.
 *
 * @return 1 if it was applied (the active slot is now the new one); 0 if
 *         there was nothing or verification failed.
 */
static int apply_staged(ab_state_t *state)
{
    uint8_t header[OTA_HEADER_SIZE];
    uint8_t public_key[OTA_PUBLIC_KEY_SIZE];
    uint8_t aes_key[OTA_AES_KEY_SIZE];
    const char *key_path;
    ota_info_t info;
    ota_keys_t keys;
    ota_status_t status;
    uint32_t payload_len;
    size_t total;
    size_t out_len = 0U;
    uint8_t *artifact;
    uint8_t *payload;
    uint16_t target;
    const char *hw_rev_env;
    long hw_rev = DEFAULT_HW_REV;

    if (flash_read(FLASH_STAGING_OFFSET, header, sizeof(header)) != FLASH_OK) {
        return 0;
    }
    if (memcmp(header + OTA_OFF_MAGIC, OTA_MAGIC, OTA_MAGIC_SIZE) != 0) {
        return 0; /* nothing staged */
    }

    payload_len = staged_payload_len(header);
    total = OTA_HEADER_SIZE + (size_t)payload_len + OTA_GCM_TAG_SIZE +
            OTA_SIGNATURE_SIZE;
    blog("OTA in staging: payload=%lu B, total=%lu B",
         (unsigned long)payload_len, (unsigned long)total);

    if (total > FLASH_STAGING_SIZE) {
        blog("FAULT 12: artifact does not fit in staging");
        (void)flash_erase(FLASH_STAGING_OFFSET, OTA_HEADER_SIZE);
        (void)flash_sync();
        return 0;
    }

    artifact = (uint8_t *)malloc(total);
    payload = (uint8_t *)malloc(payload_len);
    if ((artifact == NULL) || (payload == NULL)) {
        blog("out of memory to verify OTA");
        free(artifact);
        free(payload);
        return 0;
    }
    if (flash_read(FLASH_STAGING_OFFSET, artifact, total) != FLASH_OK) {
        blog("failed to read OTA from staging");
        free(artifact);
        free(payload);
        return 0;
    }

    key_path = getenv("OTA_PUBLIC_KEY_PATH");
    if (key_path == NULL) {
        key_path = DEFAULT_PUBLIC_KEY;
    }
    if (ota_load_public_key(key_path, public_key) != 0) {
        blog("failed to load public key (%s)", key_path);
        free(artifact);
        free(payload);
        return 0;
    }
    if (load_aes_key(aes_key) != 0) {
        blog("OTA_AES_KEY missing or invalid");
        free(artifact);
        free(payload);
        return 0;
    }

    hw_rev_env = getenv("HW_REV");
    if (hw_rev_env != NULL) {
        hw_rev = strtol(hw_rev_env, NULL, 10);
    }
    if ((hw_rev < 1L) || (hw_rev > 32L)) {
        hw_rev = (long)DEFAULT_HW_REV;
    }

    keys.public_key = public_key;
    keys.aes_key = aes_key;
    status = ota_verify_decrypt(artifact, total, &keys, &state->active_version,
                                (uint16_t)hw_rev, payload, payload_len, &out_len,
                                &info);

    /* staging is always consumed (success or failure) to avoid retry loops */
    (void)flash_erase(FLASH_STAGING_OFFSET, OTA_HEADER_SIZE);
    (void)flash_sync();

    if (status != OTA_OK) {
        blog("OTA rejected (FAULT %d)", (int)status);
        free(artifact);
        free(payload);
        return 0;
    }

    target = (uint16_t)ab_other_slot(state->active_slot);
    if (out_len > slot_capacity(target)) {
        blog("FAULT 12: image does not fit in slot");
        free(artifact);
        free(payload);
        return 0;
    }

    blog("verified v%u.%u.%u; writing slot %c (%lu B)",
         (unsigned)info.version.major, (unsigned)info.version.minor,
         (unsigned)info.version.patch, (target == AB_SLOT_A) ? 'A' : 'B',
         (unsigned long)out_len);

    (void)flash_erase(slot_offset(target), out_len);
    if (flash_write(slot_offset(target), payload, out_len) != FLASH_OK) {
        blog("error writing slot %c", (target == AB_SLOT_A) ? 'A' : 'B');
        free(artifact);
        free(payload);
        return 0;
    }

    ab_mark_pending(state, target, &info.version);
    state->active_size = (uint32_t)out_len;
    if (ab_save(state) != AB_OK) {
        blog("failed to save metadata");
        free(artifact);
        free(payload);
        return 0;
    }
    (void)flash_sync();

    blog("OTA applied; active slot %c", (target == AB_SLOT_A) ? 'A' : 'B');
    free(artifact);
    free(payload);
    return 1;
}

/**
 * @brief Provisions slot A with the factory app if there is no valid image.
 *
 * @return 0 if there is an executable image; -1 if it could not be
 *         provisioned.
 */
static int ensure_bootable(ab_state_t *state)
{
    const char *factory_path;
    FILE *fp;
    long file_size;
    uint8_t *image;
    size_t read_bytes;

    if (slot_has_elf(state->active_slot) &&
        (state->active_size > 0U) &&
        (state->active_size <= slot_capacity(state->active_slot))) {
        return 0;
    }

    /* Try the previous slot (e.g. after a failed write). */
    if ((state->previous_size > 0U) &&
        slot_has_elf((uint16_t)ab_other_slot(state->active_slot)) &&
        (state->previous_size <=
         slot_capacity((uint16_t)ab_other_slot(state->active_slot)))) {
        state->active_slot = (uint16_t)ab_other_slot(state->active_slot);
        state->active_size = state->previous_size;
        (void)ab_save(state);
        blog("previous slot recovered (%c)",
             (state->active_slot == AB_SLOT_A) ? 'A' : 'B');
        return 0;
    }

    factory_path = getenv("FACTORY_APP");
    if (factory_path == NULL) {
        factory_path = DEFAULT_FACTORY_APP;
    }
    fp = fopen(factory_path, "rb");
    if (fp == NULL) {
        blog("no valid app and no FACTORY_APP (%s)", factory_path);
        return -1;
    }
    if ((fseek(fp, 0L, SEEK_END) != 0) ||
        ((file_size = ftell(fp)) <= 0L) ||
        ((unsigned long)file_size > FLASH_SLOT_A_SIZE) ||
        (fseek(fp, 0L, SEEK_SET) != 0)) {
        (void)fclose(fp);
        blog("FACTORY_APP invalid or too large");
        return -1;
    }

    image = (uint8_t *)malloc((size_t)file_size);
    if (image == NULL) {
        (void)fclose(fp);
        return -1;
    }
    read_bytes = fread(image, 1U, (size_t)file_size, fp);
    (void)fclose(fp);
    if ((file_size < 4L) || (read_bytes != (size_t)file_size) ||
        (image[0] != 0x7FU) || (image[1] != 'E') || (image[2] != 'L') ||
        (image[3] != 'F')) {
        free(image);
        blog("FACTORY_APP is not a valid ELF");
        return -1;
    }

    (void)flash_erase(FLASH_SLOT_A_OFFSET, (size_t)file_size);
    if (flash_write(FLASH_SLOT_A_OFFSET, image, (size_t)file_size) != FLASH_OK) {
        free(image);
        blog("error writing factory app");
        return -1;
    }
    free(image);

    (void)memset(state, 0, sizeof(*state));
    state->active_slot = AB_SLOT_A;
    state->pending_slot = AB_SLOT_NONE;
    state->active_version.major = FW_VERSION_MAJOR;
    state->active_version.minor = FW_VERSION_MINOR;
    state->active_version.patch = FW_VERSION_PATCH;
    state->active_size = (uint32_t)file_size;
    if (ab_save(state) != AB_OK) {
        blog("failed to save factory metadata");
        return -1;
    }
    (void)flash_sync();
    blog("slot A provisioned with factory app (%ld B, v%s)",
         file_size, FW_VERSION_STRING);
    return 0;
}

/**
 * @brief Runs the image of the active slot (memfd + fexecve, fallback /tmp).
 */
static int boot_slot(const ab_state_t *state)
{
    uint8_t *image;
    size_t size = state->active_size;
    int fd;
    int rc;

    if ((size == 0U) || (size > slot_capacity(state->active_slot))) {
        blog("slot %c has no valid size",
             (state->active_slot == AB_SLOT_A) ? 'A' : 'B');
        return -1;
    }

    image = (uint8_t *)malloc(size);
    if (image == NULL) {
        return -1;
    }
    if (flash_read(slot_offset(state->active_slot), image, size) != FLASH_OK) {
        free(image);
        return -1;
    }

    fd = memfd_create("lamp-slot", MFD_CLOEXEC);
    if (fd >= 0) {
        ssize_t written = write(fd, image, size);

        if (written == (ssize_t)size) {
            (void)fexecve(fd, (char *const[]){"firmware", NULL}, environ);
            /* fexecve only returns if it fails */
        }
        (void)close(fd);
    }

    /* Portable fallback: executable temporary file. */
    {
        char path[128];

        (void)snprintf(path, sizeof(path), "/tmp/smartlamp-slot-%ld",
                       (long)getpid());
        fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0700);
        if (fd >= 0) {
            ssize_t written = write(fd, image, size);

            (void)close(fd);
            free(image);
            if (written == (ssize_t)size) {
                rc = (int)execve(path, (char *const[]){"firmware", NULL},
                                 environ);
                blog("execve failed: %s", strerror(errno));
                (void)rc;
            }
        }
    }

    free(image);
    return -1;
}

int main(void)
{
    const char *flash_path = getenv("FLASH_PATH");
    ab_state_t state;
    ab_boot_action_t action;
    uint16_t fault = 0U;
    int rc;

    if (flash_path == NULL) {
        flash_path = DEFAULT_FLASH_PATH;
    }
    if (flash_init(flash_path) != FLASH_OK) {
        blog("failed to open %s", flash_path);
        return 1;
    }
    if (ab_init() != AB_OK) {
        blog("metadata not initialized");
        flash_deinit();
        return 1;
    }
    (void)ab_load(&state);

    /* The supervisor starts with FORCE_BOOTLOADER=1 (recovery) or 0
     * (normal boot); without the variable the persisted flag is honored. */
    {
        const char *force_env = getenv("FORCE_BOOTLOADER");

        if (force_env != NULL) {
            bool force = (strcmp(force_env, "1") == 0);

            if (force) {
                state.flags |= AB_FLAG_FORCE_BOOTLOADER;
            } else {
                state.flags &= (uint16_t)~AB_FLAG_FORCE_BOOTLOADER;
            }
            (void)ab_save(&state);
            (void)flash_sync();
        }
    }

    action = ab_boot_plan(&state, &fault);
    if (fault != 0U) {
        blog("rollback applied (FAULT %u): slot %c", (unsigned)fault,
             (state.active_slot == AB_SLOT_A) ? 'A' : 'B');
        (void)ab_save(&state);
        (void)flash_sync();
    }
    if (action == AB_BOOT_FORCE) {
        blog("BOOTLOADER_MODE: recovery; waiting for POWER_ON");
        /* The supervisor's SIGTERM ends the process (default action). */
        for (;;) {
            (void)sleep(1U);
        }
    }

    (void)apply_staged(&state);

    rc = ensure_bootable(&state);
    if (rc != 0) {
        blog("no executable image");
        flash_deinit();
        return 1;
    }

    blog("booting slot %c v%u.%u.%u (%u B)",
         (state.active_slot == AB_SLOT_A) ? 'A' : 'B',
         (unsigned)state.active_version.major,
         (unsigned)state.active_version.minor,
         (unsigned)state.active_version.patch,
         (unsigned)state.active_size);
    (void)flash_sync();

    rc = boot_slot(&state);
    blog("failed to exec app: %s", strerror(errno));
    flash_deinit();
    return (rc == 0) ? 0 : 1;
}
