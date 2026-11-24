/**
 * @file test_cmd_handler.c
 * @brief Command interpreter tests with simulated Modbus IO.
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "cJSON.h"
#include "cmd_handler.h"

#define MAX_WRITES 8

typedef struct {
    unsigned writes;
    uint16_t reg_addr[MAX_WRITES];
    uint16_t reg_count[MAX_WRITES];
    uint16_t regs[MAX_WRITES][4];
    unsigned coils;
    uint16_t coil_addr[MAX_WRITES];
    bool coil_value[MAX_WRITES];
    bool replied;
    bool reply_ok;
    char reply_id[32];
    char reply_op[32];
    char reply_code[32];
    int state_rc;
    bool ota_called;
    int ota_rc;
    char ota_version[32];
    bool reboot_called;
    int reboot_rc;
} test_ctx_t;

static int fake_write_registers(void *ctx, uint16_t addr, uint16_t count,
                                const uint16_t *values)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    if ((t->writes >= MAX_WRITES) || (count > 4U)) {
        return -1;
    }
    t->reg_addr[t->writes] = addr;
    t->reg_count[t->writes] = count;
    (void)memcpy(t->regs[t->writes], values, (size_t)count * sizeof(uint16_t));
    t->writes++;
    return 0;
}

static int fake_write_coil(void *ctx, uint16_t addr, bool value)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    if (t->coils >= MAX_WRITES) {
        return -1;
    }
    t->coil_addr[t->coils] = addr;
    t->coil_value[t->coils] = value;
    t->coils++;
    return 0;
}

static void fake_reply(void *ctx, const char *id, const char *op, bool ok,
                       const char *code)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    t->replied = true;
    t->reply_ok = ok;
    (void)snprintf(t->reply_id, sizeof(t->reply_id), "%s",
                   (id != NULL) ? id : "");
    (void)snprintf(t->reply_op, sizeof(t->reply_op), "%s",
                   (op != NULL) ? op : "");
    (void)snprintf(t->reply_code, sizeof(t->reply_code), "%s",
                   (code != NULL) ? code : "");
}

static int fake_send_state(void *ctx)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    return t->state_rc;
}

static int fake_reboot(void *ctx)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    t->reboot_called = true;
    return t->reboot_rc;
}

static int fake_ota_update(void *ctx, const char *version)
{
    test_ctx_t *t = (test_ctx_t *)ctx;

    t->ota_called = true;
    (void)snprintf(t->ota_version, sizeof(t->ota_version), "%s",
                   (version != NULL) ? version : "");
    return t->ota_rc;
}

static int run_command(const char *json, test_ctx_t *t)
{
    cJSON *root = cJSON_Parse(json);
    const cmd_io_t io = {t, fake_write_registers, fake_write_coil};
    const cmd_handler_t handler = {t, fake_reply, fake_send_state,
                                    fake_ota_update, fake_reboot};
    int rc;

    TEST_ASSERT_NOT_NULL(root);
    rc = cmd_handler_handle(root, &io, &handler);
    cJSON_Delete(root);
    return rc;
}

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_set_power_writes_target_and_apply(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command(
               "{\"id\":\"1\",\"op\":\"set_power\",\"payload\":{\"on\":true}}",
               &t));

    TEST_ASSERT_EQUAL_UINT(1U, t.writes);
    TEST_ASSERT_EQUAL_UINT16(0U, t.reg_addr[0]);
    TEST_ASSERT_EQUAL_UINT16(1U, t.reg_count[0]);
    TEST_ASSERT_EQUAL_UINT16(1U, t.regs[0][0]);
    TEST_ASSERT_EQUAL_UINT(1U, t.coils);
    TEST_ASSERT_EQUAL_UINT16(0U, t.coil_addr[0]);
    TEST_ASSERT_TRUE(t.coil_value[0]);
    TEST_ASSERT_TRUE(t.replied);
    TEST_ASSERT_TRUE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("set_power", t.reply_op);
}

static void test_set_color_writes_hsv(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"2\",\"op\":\"set_color\","
                    "\"payload\":{\"h\":32,\"s\":850,\"v\":600}}",
                    &t));

    TEST_ASSERT_EQUAL_UINT(1U, t.writes);
    TEST_ASSERT_EQUAL_UINT16(2U, t.reg_addr[0]);
    TEST_ASSERT_EQUAL_UINT16(3U, t.reg_count[0]);
    TEST_ASSERT_EQUAL_UINT16(32U, t.regs[0][0]);
    TEST_ASSERT_EQUAL_UINT16(850U, t.regs[0][1]);
    TEST_ASSERT_EQUAL_UINT16(600U, t.regs[0][2]);
    TEST_ASSERT_TRUE(t.reply_ok);
}

static void test_color_clamps_out_of_range(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"3\",\"op\":\"set_color\","
                    "\"payload\":{\"h\":999,\"s\":5000,\"v\":-5}}",
                    &t));

    TEST_ASSERT_EQUAL_UINT16(360U, t.regs[0][0]);
    TEST_ASSERT_EQUAL_UINT16(1000U, t.regs[0][1]);
    TEST_ASSERT_EQUAL_UINT16(0U, t.regs[0][2]);
}

static void test_invalid_param_does_not_write(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command(
               "{\"id\":\"4\",\"op\":\"set_brightness\",\"payload\":{}}", &t));

    TEST_ASSERT_EQUAL_UINT(0U, t.writes);
    TEST_ASSERT_EQUAL_UINT(0U, t.coils);
    TEST_ASSERT_FALSE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("INVALID_PARAM", t.reply_code);
}

static void test_unknown_op_returns_not_supported(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        -1, run_command("{\"id\":\"5\",\"op\":\"explode\",\"payload\":{}}",
                        &t));

    TEST_ASSERT_FALSE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("NOT_SUPPORTED", t.reply_code);
}

static void test_start_sequence_uses_defaults(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command(
               "{\"id\":\"6\",\"op\":\"start_sequence\",\"payload\":{}}", &t));

    TEST_ASSERT_EQUAL_UINT(1U, t.writes);
    TEST_ASSERT_EQUAL_UINT16(10U, t.reg_addr[0]);
    TEST_ASSERT_EQUAL_UINT16(4U, t.reg_count[0]);
    TEST_ASSERT_EQUAL_UINT16(1U, t.regs[0][0]);
    TEST_ASSERT_EQUAL_UINT16(1000U, t.regs[0][1]);
    TEST_ASSERT_EQUAL_UINT16(0U, t.regs[0][2]);
    TEST_ASSERT_EQUAL_UINT16(0U, t.regs[0][3]);
    TEST_ASSERT_EQUAL_UINT16(3U, t.coil_addr[0]);
}

static void test_set_mode_scene(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"7\",\"op\":\"set_mode\","
                    "\"payload\":{\"mode\":\"scene\"}}",
                    &t));

    TEST_ASSERT_EQUAL_UINT16(1U, t.reg_addr[0]);
    TEST_ASSERT_EQUAL_UINT16(3U, t.regs[0][0]);
    TEST_ASSERT_TRUE(t.reply_ok);
}

static void test_clear_faults_writes_coil(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"8\",\"op\":\"clear_faults\",\"payload\":{}}",
                    &t));

    TEST_ASSERT_EQUAL_UINT(0U, t.writes);
    TEST_ASSERT_EQUAL_UINT(1U, t.coils);
    TEST_ASSERT_EQUAL_UINT16(8U, t.coil_addr[0]);
    TEST_ASSERT_TRUE(t.reply_ok);
}

static void test_request_state_ok_and_invalid(void)
{
    test_ctx_t t = {0};

    t.state_rc = 0;
    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"9\",\"op\":\"request_state\",\"payload\":{}}",
                    &t));
    TEST_ASSERT_TRUE(t.reply_ok);

    t.replied = false;
    t.state_rc = -1;
    TEST_ASSERT_EQUAL_INT(
        0,
        run_command("{\"id\":\"10\",\"op\":\"request_state\",\"payload\":{}}",
                    &t));
    TEST_ASSERT_FALSE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("INVALID_STATE", t.reply_code);
}

static void test_ota_update_queues_request(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command("{\"id\":\"12\",\"op\":\"ota_update\","
                       "\"payload\":{\"version\":\"1.2.0\"}}",
                       &t));

    TEST_ASSERT_TRUE(t.ota_called);
    TEST_ASSERT_EQUAL_STRING("1.2.0", t.ota_version);
    TEST_ASSERT_TRUE(t.reply_ok);
}

static void test_ota_update_requires_version(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command("{\"id\":\"13\",\"op\":\"ota_update\","
                       "\"payload\":{}}",
                       &t));

    TEST_ASSERT_FALSE(t.ota_called);
    TEST_ASSERT_FALSE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("INVALID_PARAM", t.reply_code);
}

static void test_ota_update_failure_is_nack(void)
{
    test_ctx_t t = {0};

    t.ota_rc = -1;
    TEST_ASSERT_EQUAL_INT(
        0, run_command("{\"id\":\"14\",\"op\":\"ota_update\","
                       "\"payload\":{\"version\":\"9.9.9\"}}",
                       &t));

    TEST_ASSERT_TRUE(t.ota_called);
    TEST_ASSERT_FALSE(t.reply_ok);
}

static void test_sim_reboot_is_accepted(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(
        0, run_command("{\"id\":\"15\",\"op\":\"sim_reboot\","
                       "\"payload\":{}}",
                       &t));

    TEST_ASSERT_TRUE(t.reboot_called);
    TEST_ASSERT_TRUE(t.reply_ok);
}

static void test_sim_reboot_failure_is_nack(void)
{
    test_ctx_t t = {0};

    t.reboot_rc = -1;
    TEST_ASSERT_EQUAL_INT(
        0, run_command("{\"id\":\"16\",\"op\":\"sim_reboot\","
                       "\"payload\":{}}",
                       &t));

    TEST_ASSERT_TRUE(t.reboot_called);
    TEST_ASSERT_FALSE(t.reply_ok);
}

static void test_missing_op_is_invalid_param(void)
{
    test_ctx_t t = {0};

    TEST_ASSERT_EQUAL_INT(-1,
                          run_command("{\"id\":\"11\",\"payload\":{}}", &t));

    TEST_ASSERT_FALSE(t.reply_ok);
    TEST_ASSERT_EQUAL_STRING("INVALID_PARAM", t.reply_code);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_set_power_writes_target_and_apply);
    RUN_TEST(test_set_color_writes_hsv);
    RUN_TEST(test_color_clamps_out_of_range);
    RUN_TEST(test_invalid_param_does_not_write);
    RUN_TEST(test_unknown_op_returns_not_supported);
    RUN_TEST(test_start_sequence_uses_defaults);
    RUN_TEST(test_set_mode_scene);
    RUN_TEST(test_clear_faults_writes_coil);
    RUN_TEST(test_request_state_ok_and_invalid);
    RUN_TEST(test_ota_update_queues_request);
    RUN_TEST(test_ota_update_requires_version);
    RUN_TEST(test_ota_update_failure_is_nack);
    RUN_TEST(test_sim_reboot_is_accepted);
    RUN_TEST(test_sim_reboot_failure_is_nack);
    RUN_TEST(test_missing_op_is_invalid_param);
    return UNITY_END();
}
