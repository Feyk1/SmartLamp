/**
 * @file test_flash.c
 * @brief Unit tests for the flash.bin HAL and CRC32 (Unity + CTest).
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "crc32.h"
#include "flash.h"

#define TEST_FLASH_PATH "test_flash.bin"

void setUp(void)
{
    (void)remove(TEST_FLASH_PATH);
    TEST_ASSERT_EQUAL_INT(FLASH_OK, flash_init(TEST_FLASH_PATH));
}

void tearDown(void)
{
    flash_deinit();
    (void)remove(TEST_FLASH_PATH);
}

static void test_init_creates_1mb(void)
{
    TEST_ASSERT_TRUE(flash_is_initialized());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)FLASH_TOTAL_SIZE, flash_size());
}

static void test_write_read_roundtrip(void)
{
    const uint8_t pattern[16] = {
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU
    };
    uint8_t out[16];

    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_write(FLASH_SLOT_A_OFFSET, pattern, sizeof(pattern)));
    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_read(FLASH_SLOT_A_OFFSET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(pattern, out, sizeof(pattern));
}

static void test_erase_sets_ff(void)
{
    const uint8_t data[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
    uint8_t out[8];
    size_t i;

    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_write(FLASH_CONFIG_OFFSET, data, sizeof(data)));
    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_erase(FLASH_CONFIG_OFFSET, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_read(FLASH_CONFIG_OFFSET, out, sizeof(out)));
    for (i = 0U; i < sizeof(out); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFFU, out[i]);
    }
}

static void test_crc32_known_vector(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U, crc32_ieee("123456789", 9U));
}

static void test_persist_after_reopen(void)
{
    const char msg[] = "persist";
    char out[sizeof(msg)] = {0};

    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_write(FLASH_METADATA_OFFSET, msg, sizeof(msg)));
    flash_deinit();
    TEST_ASSERT_EQUAL_INT(FLASH_OK, flash_init(TEST_FLASH_PATH));
    TEST_ASSERT_EQUAL_INT(FLASH_OK,
        flash_read(FLASH_METADATA_OFFSET, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING(msg, out);
}

static void test_out_of_range(void)
{
    uint8_t byte = 0U;

    TEST_ASSERT_EQUAL_INT(FLASH_ERR_RANGE,
        flash_write(FLASH_TOTAL_SIZE, &byte, 1U));
    TEST_ASSERT_EQUAL_INT(FLASH_ERR_RANGE,
        flash_read(FLASH_TOTAL_SIZE - 1U, &byte, 2U));
}

static void test_read_before_init_fails(void)
{
    uint8_t byte = 0U;

    flash_deinit();
    TEST_ASSERT_FALSE(flash_is_initialized());
    TEST_ASSERT_EQUAL_INT(FLASH_ERR_NOT_INIT, flash_read(0U, &byte, 1U));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_creates_1mb);
    RUN_TEST(test_write_read_roundtrip);
    RUN_TEST(test_erase_sets_ff);
    RUN_TEST(test_crc32_known_vector);
    RUN_TEST(test_persist_after_reopen);
    RUN_TEST(test_out_of_range);
    RUN_TEST(test_read_before_init_fails);
    return UNITY_END();
}
