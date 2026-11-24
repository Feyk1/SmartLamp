/**
 * @file ab.c
 * @brief A/B metadata with double buffering and CRC32 (ADR-002, docs/04 §7.2).
 *
 * Each write alternates between two 64-byte copies inside the metadata
 * partition. A power cut in the middle of a write leaves the other copy
 * intact. On real flash the page would have to be erased before writing; the
 * `flash.bin` HAL allows overwriting, so nothing is erased here.
 */

#include "ab.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"
#include "flash.h"

#define AB_RECORD_MAGIC 0x4C4D5041U /* "LMPA" */
#define AB_SCHEMA       1U
#define AB_COPY_OFFSET  64U

/** @brief Record as it is stored in flash. */
typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t active_slot;
    uint16_t pending_slot;
    uint16_t flags;
    uint16_t active_version[3];
    uint16_t previous_version[3];
    uint32_t active_size;
    uint32_t previous_size;
    uint32_t boot_count;
    uint32_t seq;
    uint32_t crc;
} ab_record_t;

/* No padding: the CRC covers exactly the serialized bytes. */
typedef char ab_record_size_must_be_44[(sizeof(ab_record_t) == 44U) ? 1 : -1];

static bool s_initialized = false;

static uint32_t record_crc(const ab_record_t *record)
{
    return crc32_ieee(record, offsetof(ab_record_t, crc));
}

static bool record_valid(const ab_record_t *record)
{
    if (record->magic != AB_RECORD_MAGIC) {
        return false;
    }
    if (record->schema != AB_SCHEMA) {
        return false;
    }
    if ((record->active_slot != AB_SLOT_A) &&
        (record->active_slot != AB_SLOT_B)) {
        return false;
    }
    if ((record->pending_slot != AB_SLOT_A) &&
        (record->pending_slot != AB_SLOT_B) &&
        (record->pending_slot != AB_SLOT_NONE)) {
        return false;
    }
    return record->crc == record_crc(record);
}

static ab_status_t read_copy(unsigned copy, ab_record_t *out)
{
    uint32_t offset = FLASH_METADATA_OFFSET + (copy * AB_COPY_OFFSET);

    if ((copy > 1U) || (out == NULL)) {
        return AB_ERR_ARGUMENT;
    }
    if (flash_read(offset, out, sizeof(*out)) != FLASH_OK) {
        return AB_ERR_IO;
    }
    return record_valid(out) ? AB_OK : AB_ERR_CORRUPT;
}

static void state_defaults(ab_state_t *state)
{
    (void)memset(state, 0, sizeof(*state));
    state->active_slot = AB_SLOT_A;
    state->pending_slot = AB_SLOT_NONE;
}

static void record_to_state(const ab_record_t *record, ab_state_t *state)
{
    state->active_slot = record->active_slot;
    state->pending_slot = record->pending_slot;
    state->flags = record->flags;
    state->active_version.major = record->active_version[0];
    state->active_version.minor = record->active_version[1];
    state->active_version.patch = record->active_version[2];
    state->previous_version.major = record->previous_version[0];
    state->previous_version.minor = record->previous_version[1];
    state->previous_version.patch = record->previous_version[2];
    state->active_size = record->active_size;
    state->previous_size = record->previous_size;
    state->boot_count = record->boot_count;
}

static void state_to_record(const ab_state_t *state, uint32_t seq,
                            ab_record_t *record)
{
    (void)memset(record, 0, sizeof(*record));
    record->magic = AB_RECORD_MAGIC;
    record->schema = AB_SCHEMA;
    record->active_slot = state->active_slot;
    record->pending_slot = state->pending_slot;
    record->flags = state->flags;
    record->active_version[0] = state->active_version.major;
    record->active_version[1] = state->active_version.minor;
    record->active_version[2] = state->active_version.patch;
    record->previous_version[0] = state->previous_version.major;
    record->previous_version[1] = state->previous_version.minor;
    record->previous_version[2] = state->previous_version.patch;
    record->active_size = state->active_size;
    record->previous_size = state->previous_size;
    record->boot_count = state->boot_count;
    record->seq = seq;
    record->crc = record_crc(record);
}

ab_status_t ab_init(void)
{
    s_initialized = flash_is_initialized();
    return s_initialized ? AB_OK : AB_ERR_NOT_INIT;
}

ab_status_t ab_load(ab_state_t *out)
{
    ab_record_t records[2];
    ab_status_t status[2];
    int chosen = -1;

    if (out == NULL) {
        return AB_ERR_ARGUMENT;
    }
    if ((!s_initialized) && (ab_init() != AB_OK)) {
        return AB_ERR_NOT_INIT;
    }

    status[0] = read_copy(0U, &records[0]);
    status[1] = read_copy(1U, &records[1]);

    if ((status[0] == AB_OK) && (status[1] == AB_OK)) {
        chosen = ((int32_t)(records[0].seq - records[1].seq) >= 0) ? 0 : 1;
    } else if (status[0] == AB_OK) {
        chosen = 0;
    } else if (status[1] == AB_OK) {
        chosen = 1;
    }

    if (chosen < 0) {
        state_defaults(out);
        return AB_ERR_CORRUPT;
    }

    record_to_state(&records[chosen], out);
    return AB_OK;
}

ab_status_t ab_save(const ab_state_t *state)
{
    ab_record_t records[2];
    ab_status_t status[2];
    uint32_t next_seq = 1U;
    unsigned target = 0U;

    if (state == NULL) {
        return AB_ERR_ARGUMENT;
    }
    if ((!s_initialized) && (ab_init() != AB_OK)) {
        return AB_ERR_NOT_INIT;
    }

    status[0] = read_copy(0U, &records[0]);
    status[1] = read_copy(1U, &records[1]);

    if ((status[0] == AB_OK) && (status[1] == AB_OK)) {
        if ((int32_t)(records[0].seq - records[1].seq) >= 0) {
            next_seq = records[0].seq + 1U;
            target = 1U; /* the oldest copy */
        } else {
            next_seq = records[1].seq + 1U;
            target = 0U;
        }
    } else if (status[0] == AB_OK) {
        next_seq = records[0].seq + 1U;
        target = 1U;
    } else if (status[1] == AB_OK) {
        next_seq = records[1].seq + 1U;
        target = 0U;
    }

    state_to_record(state, next_seq, &records[target]);
    if (flash_write(FLASH_METADATA_OFFSET + (target * AB_COPY_OFFSET),
                    &records[target], sizeof(records[target])) != FLASH_OK) {
        return AB_ERR_IO;
    }
    if (flash_sync() != FLASH_OK) {
        return AB_ERR_IO;
    }
    return AB_OK;
}

int ab_other_slot(int slot)
{
    if (slot == AB_SLOT_A) {
        return AB_SLOT_B;
    }
    if (slot == AB_SLOT_B) {
        return AB_SLOT_A;
    }
    return AB_SLOT_NONE;
}

void ab_mark_pending(ab_state_t *state, int slot,
                     const ota_version_t *version)
{
    if ((state == NULL) || (version == NULL) ||
        (ab_other_slot(slot) == AB_SLOT_NONE)) {
        return;
    }
    state->previous_version = state->active_version;
    state->previous_size = state->active_size;
    state->active_slot = (uint16_t)slot;
    state->active_version = *version;
    state->pending_slot = (uint16_t)slot;
    state->flags |= AB_FLAG_PENDING_CONFIRMATION;
    state->flags &= (uint16_t)~AB_FLAG_CONFIRMED;
}

void ab_confirm(ab_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->flags &= (uint16_t)~AB_FLAG_PENDING_CONFIRMATION;
    state->flags |= AB_FLAG_CONFIRMED;
}

ab_boot_action_t ab_boot_plan(ab_state_t *state, uint16_t *fault_code)
{
    if (fault_code != NULL) {
        *fault_code = 0U;
    }
    if (state == NULL) {
        return AB_BOOT_ACTIVE;
    }

    state->boot_count++;

    if ((state->flags & AB_FLAG_FORCE_BOOTLOADER) != 0U) {
        return AB_BOOT_FORCE;
    }

    if (((state->flags & AB_FLAG_PENDING_CONFIRMATION) != 0U) &&
        ((state->flags & AB_FLAG_CONFIRMED) == 0U)) {
        /* The new app did not confirm: go back to the previous slot. */
        state->active_slot = (uint16_t)ab_other_slot(state->pending_slot);
        state->active_version = state->previous_version;
        state->active_size = state->previous_size;
        state->pending_slot = AB_SLOT_NONE;
        state->flags &=
            (uint16_t)~(AB_FLAG_PENDING_CONFIRMATION | AB_FLAG_CONFIRMED);
        if (fault_code != NULL) {
            *fault_code = (uint16_t)AB_FAULT_ROLLBACK;
        }
        return AB_BOOT_ROLLBACK;
    }

    if ((state->flags & AB_FLAG_PENDING_CONFIRMATION) != 0U) {
        /* Confirmed: the update is now closed. */
        state->flags &= (uint16_t)~AB_FLAG_PENDING_CONFIRMATION;
    }
    return AB_BOOT_ACTIVE;
}
