/**
 * @file test_supervisor_core.c
 * @brief Tests for the supervisor state machine (ADR-004).
 */

#include <string.h>

#include "unity.h"

#include "supervisor_core.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_init_is_powered_off(void)
{
    sp_core_t core;

    sp_init(&core);
    TEST_ASSERT_EQUAL_INT(SP_POWERED_OFF, core.state);
    TEST_ASSERT_FALSE(core.forced);
}

static void test_parse_commands(void)
{
    TEST_ASSERT_EQUAL_INT(SP_CMD_STATUS, sp_parse_command("STATUS\n"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_POWER_ON, sp_parse_command("  power_on \r\n"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_POWER_OFF, sp_parse_command("POWER_OFF"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_RESET, sp_parse_command("reset"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_ENTER_BOOTLOADER,
                          sp_parse_command("Enter_Bootloader"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_FACTORY_RESET,
                          sp_parse_command("FACTORY_RESET"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_INVALID, sp_parse_command("EXPLODE"));
    TEST_ASSERT_EQUAL_INT(SP_CMD_INVALID, sp_parse_command(""));
    TEST_ASSERT_EQUAL_INT(SP_CMD_INVALID, sp_parse_command(NULL));
}

static void test_power_on_and_boot_timeout(void)
{
    sp_core_t core;

    sp_init(&core);
    TEST_ASSERT_TRUE(sp_can_power_on(&core));
    TEST_ASSERT_TRUE(sp_on_power_on(&core, false, 1000U));
    TEST_ASSERT_EQUAL_INT(SP_BOOTING, core.state);
    TEST_ASSERT_FALSE(sp_on_boot_timeout(&core, 1100U));
    TEST_ASSERT_EQUAL_INT(SP_BOOTING, core.state);
    TEST_ASSERT_TRUE(sp_on_boot_timeout(&core, 1000U + SP_BOOT_DELAY_MS));
    TEST_ASSERT_EQUAL_INT(SP_APP_RUNNING, core.state);
    TEST_ASSERT_FALSE(sp_can_power_on(&core));
}

static void test_forced_boot_goes_to_bootloader(void)
{
    sp_core_t core;

    sp_init(&core);
    TEST_ASSERT_TRUE(sp_on_power_on(&core, true, 0U));
    TEST_ASSERT_TRUE(sp_on_boot_timeout(&core, SP_BOOT_DELAY_MS));
    TEST_ASSERT_EQUAL_INT(SP_BOOTLOADER_MODE, core.state);
}

static void test_unexpected_exit_is_fault(void)
{
    sp_core_t core;

    sp_init(&core);
    (void)sp_on_power_on(&core, false, 0U);
    (void)sp_on_boot_timeout(&core, SP_BOOT_DELAY_MS);
    TEST_ASSERT_EQUAL_INT(SP_APP_RUNNING, core.state);

    sp_on_child_exit(&core, 139);
    TEST_ASSERT_EQUAL_INT(SP_FAULT, core.state);
    TEST_ASSERT_TRUE(core.has_exit_status);
    TEST_ASSERT_EQUAL_INT(139, core.last_exit_status);

    /* From FAULT it can be powered on again. */
    TEST_ASSERT_TRUE(sp_can_power_on(&core));
    TEST_ASSERT_TRUE(sp_on_power_on(&core, false, 5000U));
    TEST_ASSERT_EQUAL_INT(SP_BOOTING, core.state);
}

static void test_ordered_stop_ignores_exit(void)
{
    sp_core_t core;

    sp_init(&core);
    (void)sp_on_power_on(&core, false, 0U);
    (void)sp_on_boot_timeout(&core, SP_BOOT_DELAY_MS);
    sp_set_powered_off(&core);
    sp_on_child_exit(&core, 0);
    TEST_ASSERT_EQUAL_INT(SP_POWERED_OFF, core.state);
    TEST_ASSERT_FALSE(core.has_exit_status);
}

static void test_power_on_rejected_while_running(void)
{
    sp_core_t core;

    sp_init(&core);
    (void)sp_on_power_on(&core, false, 0U);
    TEST_ASSERT_FALSE(sp_on_power_on(&core, false, 10U));
    TEST_ASSERT_EQUAL_INT(SP_BOOTING, core.state);
}

static void test_state_names(void)
{
    TEST_ASSERT_EQUAL_STRING("POWERED_OFF", sp_state_name(SP_POWERED_OFF));
    TEST_ASSERT_EQUAL_STRING("APP_RUNNING", sp_state_name(SP_APP_RUNNING));
    TEST_ASSERT_EQUAL_STRING("BOOTLOADER_MODE",
                             sp_state_name(SP_BOOTLOADER_MODE));
    TEST_ASSERT_EQUAL_STRING("FAULT", sp_state_name(SP_FAULT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_is_powered_off);
    RUN_TEST(test_parse_commands);
    RUN_TEST(test_power_on_and_boot_timeout);
    RUN_TEST(test_forced_boot_goes_to_bootloader);
    RUN_TEST(test_unexpected_exit_is_fault);
    RUN_TEST(test_ordered_stop_ignores_exit);
    RUN_TEST(test_power_on_rejected_while_running);
    RUN_TEST(test_state_names);
    return UNITY_END();
}
