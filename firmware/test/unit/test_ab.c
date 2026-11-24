/**
 * @file test_ab.c
 * @brief A/B metadata tests: double buffering, CRC and rollback (docs/04 §7.2).
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "ab.h"
#include "flash.h"

#define TEST_FLASH_PATH "test_ab_flash.bin"

static ab_state_t s_state;

static ota_version_t version(uint16_t major, uint16_t minor, uint16_t patch)
{
    ota_version_t v;

    v.major = major;
    v.minor = minor;
    v.patch = patch;
    return v;
}

void setUp(void)
{
    ab_status_t status;

    (void)remove(TEST_FLASH_PATH);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, flash_init(TEST_FLASH_PATH));
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_init());
    status = ab_load(&s_state);
    /* Blank flash: there is no valid metadata, but defaults are returned. */
    TEST_ASSERT_TRUE((status == AB_OK) || (status == AB_ERR_CORRUPT));
}

void tearDown(void)
{
    flash_deinit();
    (void)remove(TEST_FLASH_PATH);
}

static void test_defaults_when_blank(void)
{
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_A, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_NONE, s_state.pending_slot);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.flags);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.active_version.major);
}

static void test_save_load_roundtrip(void)
{
    s_state.active_slot = AB_SLOT_A;
    s_state.active_version = version(1U, 0U, 3U);

    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_load(&s_state));

    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_A, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(1U, s_state.active_version.major);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.active_version.minor);
    TEST_ASSERT_EQUAL_UINT16(3U, s_state.active_version.patch);
}

static void test_pending_without_confirm_rolls_back(void)
{
    uint16_t fault = 0U;
    ota_version_t v120 = version(1U, 2U, 0U);

    s_state.active_slot = AB_SLOT_A;
    s_state.active_version = version(1U, 0U, 3U);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));

    ab_mark_pending(&s_state, AB_SLOT_B, &v120);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_load(&s_state));
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_B, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_B, s_state.pending_slot);
    TEST_ASSERT_EQUAL_UINT16(2U, s_state.active_version.minor);

    TEST_ASSERT_EQUAL_INT(AB_BOOT_ROLLBACK, ab_boot_plan(&s_state, &fault));
    TEST_ASSERT_EQUAL_UINT16(AB_FAULT_ROLLBACK, fault);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_A, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_NONE, s_state.pending_slot);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.active_version.minor);
    TEST_ASSERT_EQUAL_UINT16(3U, s_state.active_version.patch);

    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_load(&s_state));
    fault = 99U;
    TEST_ASSERT_EQUAL_INT(AB_BOOT_ACTIVE, ab_boot_plan(&s_state, &fault));
    TEST_ASSERT_EQUAL_UINT16(0U, fault);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_A, s_state.active_slot);
}

static void test_confirmed_pending_boots_new_slot(void)
{
    uint16_t fault = 0U;
    ota_version_t v120 = version(1U, 2U, 0U);

    s_state.active_slot = AB_SLOT_A;
    s_state.active_version = version(1U, 0U, 3U);
    ab_mark_pending(&s_state, AB_SLOT_B, &v120);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));

    ab_confirm(&s_state);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_load(&s_state));

    TEST_ASSERT_EQUAL_INT(AB_BOOT_ACTIVE, ab_boot_plan(&s_state, &fault));
    TEST_ASSERT_EQUAL_UINT16(0U, fault);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_B, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.flags & AB_FLAG_PENDING_CONFIRMATION);
    TEST_ASSERT_EQUAL_UINT16(2U, s_state.active_version.minor);
}

static void test_force_bootloader_skips_rollback(void)
{
    uint16_t fault = 0U;
    ota_version_t v120 = version(1U, 2U, 0U);

    s_state.active_slot = AB_SLOT_A;
    s_state.active_version = version(1U, 0U, 3U);
    ab_mark_pending(&s_state, AB_SLOT_B, &v120);
    s_state.flags |= AB_FLAG_FORCE_BOOTLOADER;

    TEST_ASSERT_EQUAL_INT(AB_BOOT_FORCE, ab_boot_plan(&s_state, &fault));
    TEST_ASSERT_EQUAL_UINT16(0U, fault);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_B, s_state.active_slot);
}

static void test_corrupt_newest_copy_uses_older(void)
{
    uint8_t zeros[36];

    /* First write (copy 0): version 1.0.3. */
    s_state.active_slot = AB_SLOT_A;
    s_state.active_version = version(1U, 0U, 3U);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));

    /* Second write (copy 1): version 2.0.0. */
    s_state.active_version = version(2U, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(AB_OK, ab_save(&s_state));

    /* Invalidate the most recent copy (copy 1, offset +64). */
    (void)memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_EQUAL_INT(
        FLASH_OK,
        flash_write(FLASH_METADATA_OFFSET + 64U, zeros, sizeof(zeros)));

    TEST_ASSERT_EQUAL_INT(AB_OK, ab_load(&s_state));
    TEST_ASSERT_EQUAL_UINT16(1U, s_state.active_version.major);
    TEST_ASSERT_EQUAL_UINT16(0U, s_state.active_version.minor);
}

static void test_both_copies_corrupt_returns_defaults(void)
{
    uint8_t zeros[64];

    (void)memset(zeros, 0, sizeof(zeros));
    TEST_ASSERT_EQUAL_INT(
        FLASH_OK, flash_write(FLASH_METADATA_OFFSET, zeros, sizeof(zeros)));
    TEST_ASSERT_EQUAL_INT(
        FLASH_OK,
        flash_write(FLASH_METADATA_OFFSET + 64U, zeros, sizeof(zeros)));

    TEST_ASSERT_EQUAL_INT(AB_ERR_CORRUPT, ab_load(&s_state));
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_A, s_state.active_slot);
    TEST_ASSERT_EQUAL_UINT16(AB_SLOT_NONE, s_state.pending_slot);
}

static void test_other_slot(void)
{
    TEST_ASSERT_EQUAL_INT(AB_SLOT_B, ab_other_slot(AB_SLOT_A));
    TEST_ASSERT_EQUAL_INT(AB_SLOT_A, ab_other_slot(AB_SLOT_B));
    TEST_ASSERT_EQUAL_INT(AB_SLOT_NONE, ab_other_slot(7));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_defaults_when_blank);
    RUN_TEST(test_save_load_roundtrip);
    RUN_TEST(test_pending_without_confirm_rolls_back);
    RUN_TEST(test_confirmed_pending_boots_new_slot);
    RUN_TEST(test_force_bootloader_skips_rollback);
    RUN_TEST(test_corrupt_newest_copy_uses_older);
    RUN_TEST(test_both_copies_corrupt_returns_defaults);
    RUN_TEST(test_other_slot);
    return UNITY_END();
}
