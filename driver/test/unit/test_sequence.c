/**
 * @file test_sequence.c
 * @brief Unit tests for the predefined sequences (Unity).
 */

#include "unity.h"

#include "sequence.h"

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_rainbow_changes_hue(void)
{
    sequence_state_t state;
    sequence_output_t out;
    uint16_t first;
    uint16_t second;

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start(&state, 2U, 1000U, 0U, 0U,
                                    0U, 1000U, 1000U, 1000U, 2700U));

    TEST_ASSERT_TRUE(sequence_tick(&state, 100U, &out));
    first = out.hue;
    TEST_ASSERT_TRUE(first > 0U);

    TEST_ASSERT_TRUE(sequence_tick(&state, 100U, &out));
    second = out.hue;
    TEST_ASSERT_TRUE(second != first);
}

static void test_breathing_modulates_val(void)
{
    sequence_state_t state;
    sequence_output_t out;

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start(&state, 1U, 1000U, 0U, 0U,
                                    0U, 1000U, 1000U, 1000U, 2700U));

    TEST_ASSERT_TRUE(sequence_tick(&state, 500U, &out));
    TEST_ASSERT_TRUE(out.val < 1000U);
    TEST_ASSERT_TRUE(out.val > 0U);
}

static void test_invalid_sequence_id(void)
{
    sequence_state_t state;

    sequence_init(&state);
    TEST_ASSERT_FALSE(sequence_start(&state, 99U, 1000U, 0U, 0U,
                                     0U, 1000U, 1000U, 1000U, 2700U));
    TEST_ASSERT_FALSE(state.running);
}

static void test_stop_sequence(void)
{
    sequence_state_t state;
    sequence_output_t out;

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start(&state, 2U, 1000U, 0U, 0U,
                                    0U, 1000U, 1000U, 1000U, 2700U));
    sequence_stop(&state);
    TEST_ASSERT_FALSE(state.running);
    TEST_ASSERT_FALSE(sequence_tick(&state, 50U, &out));
}

static void test_custom_sequence_steps(void)
{
    sequence_state_t state;
    sequence_output_t out;
    const sequence_step_t steps[2] = {
        {0U, 1000U, 1000U, 0U, 800U, 100U, 0U, 2700U},
        {120U, 1000U, 1000U, 0U, 600U, 100U, 0U, 4000U},
    };

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start_custom(&state, steps, 2U, 1000U, 0U, 0U));

    TEST_ASSERT_TRUE(sequence_tick(&state, 100U, &out));
    TEST_ASSERT_EQUAL_UINT16(0U, out.step);
    TEST_ASSERT_EQUAL_UINT16(0U, out.hue);
    TEST_ASSERT_EQUAL_UINT16(800U, out.brightness);

    TEST_ASSERT_TRUE(sequence_tick(&state, 1000U, &out));
    TEST_ASSERT_EQUAL_UINT16(1U, out.step);
    TEST_ASSERT_EQUAL_UINT16(120U, out.hue);
    TEST_ASSERT_EQUAL_UINT16(600U, out.brightness);

    TEST_ASSERT_TRUE(sequence_tick(&state, 1000U, &out));
    TEST_ASSERT_EQUAL_UINT16(0U, out.step);
}

static void test_custom_sequence_transition(void)
{
    sequence_state_t state;
    sequence_output_t out;
    const sequence_step_t steps[2] = {
        {0U, 1000U, 1000U, 0U, 1000U, 100U, 0U, 2700U},
        {100U, 1000U, 1000U, 0U, 1000U, 100U, 50U, 2700U},
    };

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start_custom(&state, steps, 2U, 1000U, 0U, 0U));
    TEST_ASSERT_TRUE(sequence_tick(&state, 1000U, &out));
    TEST_ASSERT_TRUE(sequence_tick(&state, 250U, &out));
    TEST_ASSERT_EQUAL_UINT16(1U, out.step);
    TEST_ASSERT_TRUE(out.hue > 0U);
    TEST_ASSERT_TRUE(out.hue < 100U);
}

static void test_custom_sequence_crc_changes(void)
{
    const sequence_step_t first[1] = {
        {0U, 1000U, 1000U, 0U, 800U, 100U, 0U, 2700U},
    };
    const sequence_step_t second[1] = {
        {120U, 1000U, 1000U, 0U, 800U, 100U, 0U, 2700U},
    };

    TEST_ASSERT_TRUE(sequence_crc16(first, 1U) !=
                     sequence_crc16(second, 1U));
    TEST_ASSERT_EQUAL_UINT16(sequence_crc16(first, 1U),
                             sequence_crc16(first, 1U));
}

static void test_pairing_is_a_white_pulse(void)
{
    sequence_state_t state;
    sequence_output_t out;

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start(&state, 7U, 1000U, 0U, 0U,
                                    0U, 1000U, 1000U, 1000U, 2700U));
    TEST_ASSERT_TRUE(sequence_tick(&state, 500U, &out));
    TEST_ASSERT_EQUAL_UINT16(0U, out.hue);
    TEST_ASSERT_EQUAL_UINT16(0U, out.sat);
    TEST_ASSERT_EQUAL_UINT16(1000U, out.brightness);
    TEST_ASSERT_TRUE(out.val > 0U);
    TEST_ASSERT_TRUE(out.val < 1000U);
}

static void test_crc_known_value(void)
{
    const sequence_step_t step = {
        0U, 1000U, 1000U, 0U, 800U, 100U, 0U, 2700U,
    };
    const sequence_step_t steps[2] = {
        {0U, 1000U, 1000U, 0U, 800U, 200U, 0U, 2700U},
        {120U, 1000U, 1000U, 0U, 800U, 200U, 0U, 4000U},
    };

    TEST_ASSERT_EQUAL_UINT16(0xD035U, sequence_crc16(&step, 1U));
    TEST_ASSERT_EQUAL_UINT16(0xD330U, sequence_crc16(steps, 2U));
}

static void test_ping_pong_direction(void)
{
    sequence_state_t state;
    sequence_output_t out;
    uint16_t hue;

    sequence_init(&state);
    TEST_ASSERT_TRUE(sequence_start(&state, 2U, 1000U, 2U, 0U,
                                    0U, 1000U, 1000U, 1000U, 2700U));

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_TRUE(sequence_tick(&state, 1000U, &out));
    }
    hue = out.hue;
    TEST_ASSERT_TRUE(hue <= 360U);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rainbow_changes_hue);
    RUN_TEST(test_breathing_modulates_val);
    RUN_TEST(test_invalid_sequence_id);
    RUN_TEST(test_stop_sequence);
    RUN_TEST(test_custom_sequence_steps);
    RUN_TEST(test_custom_sequence_transition);
    RUN_TEST(test_custom_sequence_crc_changes);
    RUN_TEST(test_pairing_is_a_white_pulse);
    RUN_TEST(test_crc_known_value);
    RUN_TEST(test_ping_pong_direction);
    return UNITY_END();
}
