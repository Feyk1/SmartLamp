#ifndef AB_H
#define AB_H

#include <stdbool.h>
#include <stdint.h>

#include "ota.h"

/*
 * A/B metadata in the flash.bin metadata partition (ADR-002).
 *
 * Two copies with CRC32 and a sequence number are stored; reads use the most
 * recent valid copy. Each write alternates copies (double buffering), so a
 * power cut in the middle of a write never invalidates both.
 */

#define AB_SLOT_A 0U
#define AB_SLOT_B 1U
#define AB_SLOT_NONE 0xFFU

#define AB_FLAG_PENDING_CONFIRMATION 0x01U
#define AB_FLAG_CONFIRMED            0x02U
#define AB_FLAG_FORCE_BOOTLOADER     0x04U

/** @brief Fault code when the pending app did not confirm (docs/04 §7.1). */
#define AB_FAULT_ROLLBACK 13U

/** @brief Logical state of the metadata. */
typedef struct {
    uint16_t active_slot;
    uint16_t pending_slot;
    uint16_t flags;
    ota_version_t active_version;
    ota_version_t previous_version;
    uint32_t active_size;   /* image bytes in the active slot */
    uint32_t previous_size; /* bytes of the previous image (rollback) */
    uint32_t boot_count;
} ab_state_t;

typedef enum {
    AB_OK = 0,
    AB_ERR_ARGUMENT = -1,
    AB_ERR_IO = -2,
    AB_ERR_CORRUPT = -3,
    AB_ERR_NOT_INIT = -4
} ab_status_t;

/** @brief Boot action decided by the bootloader. */
typedef enum {
    AB_BOOT_ACTIVE = 0,  /* boot `active_slot` */
    AB_BOOT_ROLLBACK = 1, /* rollback was applied; boot `active_slot` */
    AB_BOOT_FORCE = 2     /* stay in the bootloader (recovery) */
} ab_boot_action_t;

/**
 * @brief Initializes the A/B layer (requires a prior `flash_init`).
 *
 * @return AB_OK or AB_ERR_NOT_INIT.
 */
ab_status_t ab_init(void);

/**
 * @brief Reads the metadata (the most recent valid copy).
 *
 * If no valid copy exists it returns AB_ERR_CORRUPT and fills `out` with
 * factory values (slot A, version 0.0.0), so the caller can carry on and
 * write a new state.
 *
 * @param out State read.
 * @return AB_OK, AB_ERR_CORRUPT or AB_ERR_ARGUMENT.
 */
ab_status_t ab_load(ab_state_t *out);

/**
 * @brief Persists the state into the oldest copy (double buffering).
 *
 * @param state State to write.
 * @return AB_OK or an I/O error.
 */
ab_status_t ab_save(const ab_state_t *state);

/**
 * @brief Returns the opposite slot (A<->B).
 *
 * @param slot AB_SLOT_A or AB_SLOT_B.
 * @return The other slot, or AB_SLOT_NONE if `slot` is not valid.
 */
int ab_other_slot(int slot);

/**
 * @brief Marks a freshly applied OTA as pending confirmation.
 *
 * @param state   State to modify.
 * @param slot    Slot the image was applied to (the previously inactive one).
 * @param version Version of the new image.
 */
void ab_mark_pending(ab_state_t *state, int slot,
                     const ota_version_t *version);

/**
 * @brief Confirms the active image (the app booted and passed self-test).
 *
 * @param state State to modify.
 */
void ab_confirm(ab_state_t *state);

/**
 * @brief Decides what to boot and applies the rollback if needed.
 *
 * Rules (docs/04 §7.2):
 *  - force_bootloader → AB_BOOT_FORCE (nothing else is touched).
 *  - pending_confirmation without confirmed → rollback to the previous slot,
 *    fault 13.
 *  - in any other case → AB_BOOT_ACTIVE.
 *
 * @param state      State (modified when a rollback happens).
 * @param fault_code Output with 0 or AB_FAULT_ROLLBACK.
 * @return Boot action.
 */
ab_boot_action_t ab_boot_plan(ab_state_t *state, uint16_t *fault_code);

#endif /* AB_H */
