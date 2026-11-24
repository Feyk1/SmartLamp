/**
 * @file sequence.c
 * @brief Predefined animation sequences (breathing, rainbow, etc.).
 */

#include "sequence.h"

#include <string.h>

/**
 * @brief CRC-16/CCITT-FALSE step (MSB first, polynomial 0x1021).
 */
static uint16_t crc_update(uint16_t crc, uint8_t byte)
{
    uint8_t bit;

    crc ^= (uint16_t)((uint16_t)byte << 8);
    for (bit = 0U; bit < 8U; bit++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((crc << 1) ^ 0x1021U);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint16_t sequence_crc16(const sequence_step_t *steps, uint16_t count)
{
    uint16_t crc = 0xFFFFU;
    uint16_t i;

    if ((steps == NULL) || (count > (uint16_t)SEQ_MAX_STEPS)) {
        return crc;
    }
    for (i = 0U; i < count; i++) {
        const uint16_t fields[8] = {
            steps[i].hue,       steps[i].sat,         steps[i].val,
            steps[i].reserved,  steps[i].brightness,  steps[i].duration_10ms,
            steps[i].transition_10ms, steps[i].cct
        };
        uint16_t j;

        for (j = 0U; j < 8U; j++) {
            crc = crc_update(crc, (uint8_t)(fields[j] & 0xFFU));
            crc = crc_update(crc, (uint8_t)((fields[j] >> 8) & 0xFFU));
        }
    }
    return crc;
}

/**
 * @brief 16-bit linear interpolation (progress 0..1000).
 */
static uint16_t lerp_u16(uint16_t a, uint16_t b, uint32_t progress)
{
    int32_t delta = (int32_t)b - (int32_t)a;
    int32_t value = (int32_t)a + (delta * (int32_t)progress) / 1000;

    if (value < 0) {
        value = 0;
    }
    if (value > 65535) {
        value = 65535;
    }
    return (uint16_t)value;
}

/**
 * @brief Hue interpolation along the short path.
 */
static uint16_t lerp_hue(uint16_t a, uint16_t b, uint32_t progress)
{
    int32_t delta = (int32_t)b - (int32_t)a;
    int32_t value;

    if (delta > 180) {
        delta -= 360;
    } else if (delta < -180) {
        delta += 360;
    }
    value = (int32_t)a + (delta * (int32_t)progress) / 1000;
    while (value < 0) {
        value += 360;
    }
    while (value > 360) {
        value -= 360;
    }
    return (uint16_t)value;
}

/**
 * @brief Effective duration of a step in ms (0 => 1 s).
 */
static uint32_t step_duration_ms(const sequence_step_t *step)
{
    uint32_t duration = (uint32_t)step->duration_10ms * 10U;

    return (duration == 0U) ? 1000U : duration;
}

/**
 * @brief Previous index used to interpolate the current step's transition.
 */
static uint16_t previous_step_index(const sequence_state_t *state)
{
    uint16_t count = state->step_count;

    if (count < 2U) {
        return state->step_index;
    }
    if (state->direction == 2U) {
        int32_t prev = (int32_t)state->step_index - (int32_t)state->dir;

        if (prev < 0) {
            prev = 0;
        } else if (prev >= (int32_t)count) {
            prev = (int32_t)count - 1;
        }
        return (uint16_t)prev;
    }
    if (state->dir < 0) {
        return (uint16_t)((state->step_index + 1U) % count);
    }
    return (state->step_index == 0U) ? (uint16_t)(count - 1U)
                                     : (uint16_t)(state->step_index - 1U);
}

/**
 * @brief Advances to the next step according to direction and loop count.
 *
 * @return false if the loop limit was reached.
 */
static bool advance_custom_step(sequence_state_t *state)
{
    uint16_t count = state->step_count;

    if (count == 0U) {
        return false;
    }

    if (state->direction == 2U) {
        int32_t next = (int32_t)state->step_index + (int32_t)state->dir;

        if ((next < 0) || (next >= (int32_t)count)) {
            state->dir = (int8_t)(-state->dir);
            next = (int32_t)state->step_index + (int32_t)state->dir;
            state->loops_done++;
        }
        if (next < 0) {
            next = 0;
        } else if (next >= (int32_t)count) {
            next = (int32_t)count - 1;
        }
        state->step_index = (uint16_t)next;
    } else if (state->dir < 0) {
        if (state->step_index == 0U) {
            state->step_index = (uint16_t)(count - 1U);
            state->loops_done++;
        } else {
            state->step_index--;
        }
    } else {
        state->step_index++;
        if (state->step_index >= count) {
            state->step_index = 0U;
            state->loops_done++;
        }
    }

    if ((state->loop_count != 0U) &&
        (state->loops_done >= state->loop_count)) {
        return false;
    }
    return true;
}

/**
 * @brief Advances the phase according to direction (forward/reverse/ping-pong).
 *
 * @param state Sequence state.
 * @param inc Phase increment.
 * @param max Maximum phase (exclusive).
 * @return New phase.
 */
static uint32_t advance_phase(sequence_state_t *state, uint32_t inc,
                              uint32_t max)
{
    if (inc == 0U) {
        return state->phase;
    }

    if (state->direction == 2U) {
        if (state->dir > 0) {
            state->phase += inc;
            if (state->phase >= max) {
                state->phase = (max > 0U) ? (max - 1U) : 0U;
                state->dir = -1;
            }
        } else {
            if (state->phase <= inc) {
                state->phase = 0U;
                state->dir = 1;
            } else {
                state->phase -= inc;
            }
        }
    } else if (state->dir < 0) {
        if (state->phase <= inc) {
            state->phase = max - (inc - state->phase);
            if (state->phase >= max) {
                state->phase = 0U;
            }
        } else {
            state->phase -= inc;
        }
    } else {
        state->phase = (state->phase + inc) % max;
    }
    return state->phase;
}

void sequence_init(sequence_state_t *state)
{
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
    state->running = false;
    state->kind = SEQ_KIND_NONE;
}

bool sequence_start(sequence_state_t *state, uint16_t seq_id, uint16_t speed,
                    uint16_t direction, uint16_t loop_count,
                    uint16_t base_hue, uint16_t base_sat, uint16_t base_val,
                    uint16_t base_brightness, uint16_t base_cct)
{
    if (state == NULL) {
        return false;
    }

    (void)sequence_init(state);
    state->seq_id = seq_id;
    state->speed = (speed == 0U) ? 1000U : speed;
    state->direction = (direction > 2U) ? 0U : direction;
    state->loop_count = loop_count;
    state->dir = (direction == 1U) ? -1 : 1;
    state->base_hue = base_hue;
    state->base_sat = base_sat;
    state->base_val = base_val;
    state->base_brightness = base_brightness;
    state->base_cct = base_cct;

    switch (seq_id) {
    case 1U: state->kind = SEQ_KIND_BREATHING; break;
    case 2U: state->kind = SEQ_KIND_RAINBOW; break;
    case 3U: state->kind = SEQ_KIND_STROBE; break;
    case 4U: state->kind = SEQ_KIND_CANDLE; break;
    case 5U: state->kind = SEQ_KIND_POLICE; break;
    case 6U: state->kind = SEQ_KIND_SUNRISE; break;
    case 7U: state->kind = SEQ_KIND_PAIRING; break;
    default:
        state->kind = SEQ_KIND_NONE;
        return false;
    }

    state->running = true;
    return true;
}

bool sequence_start_custom(sequence_state_t *state,
                           const sequence_step_t *steps, uint16_t count,
                           uint16_t speed, uint16_t direction,
                           uint16_t loop_count)
{
    if ((state == NULL) || (steps == NULL) || (count == 0U) ||
        (count > SEQ_MAX_STEPS)) {
        return false;
    }

    (void)sequence_init(state);
    state->seq_id = 100U;
    state->speed = (speed == 0U) ? 1000U : speed;
    state->direction = (direction > 2U) ? 0U : direction;
    state->loop_count = loop_count;
    state->dir = (state->direction == 1U) ? -1 : 1;
    state->step_count = count;
    state->step_index = (state->direction == 1U) ? (uint16_t)(count - 1U)
                                                 : 0U;
    state->kind = SEQ_KIND_CUSTOM;
    (void)memcpy(state->steps, steps,
                 (size_t)count * sizeof(sequence_step_t));
    state->running = true;
    return true;
}

void sequence_stop(sequence_state_t *state)
{
    if (state == NULL) {
        return;
    }
    state->running = false;
}

bool sequence_tick(sequence_state_t *state, uint32_t elapsed_ms,
                   sequence_output_t *out)
{
    uint32_t inc;
    uint32_t phase;
    uint32_t triangle;
    uint16_t index;

    if ((state == NULL) || (out == NULL) || (!state->running)) {
        return false;
    }

    (void)memset(out, 0, sizeof(*out));
    out->hue = state->base_hue;
    out->sat = state->base_sat;
    out->val = state->base_val;
    out->brightness = state->base_brightness;
    out->cct = state->base_cct;
    out->step = 0U;

    state->elapsed_ms += elapsed_ms;

    switch (state->kind) {
    case SEQ_KIND_BREATHING:
        /* 4 s cycle; phase 0..2000 (triangle 0..1000..0). */
        inc = (uint32_t)(((uint64_t)elapsed_ms * state->speed * 2000U) /
                         (4000U * 1000U));
        phase = advance_phase(state, inc, 2000U);
        triangle = (phase <= 1000U) ? phase : (2000U - phase);
        out->val = (uint16_t)(((uint32_t)state->base_val * triangle) / 1000U);
        break;

    case SEQ_KIND_RAINBOW:
        /* Full 10 s cycle; phase in tenths of a degree (0..3600). */
        inc = (uint32_t)(((uint64_t)elapsed_ms * state->speed * 3600U) /
                         (10000U * 1000U));
        phase = advance_phase(state, inc, 3600U);
        out->hue = (uint16_t)(phase / 10U);
        break;

    case SEQ_KIND_STROBE:
        state->phase += (uint32_t)(((uint64_t)elapsed_ms * state->speed) / 1000U);
        out->val = (((state->phase / 50U) % 2U) == 0U) ? state->base_val : 0U;
        break;

    case SEQ_KIND_CANDLE:
        state->phase = (state->phase * 1103515245U) + 12345U;
        out->hue = (uint16_t)(20U + (state->phase % 30U));
        out->sat = (uint16_t)(800U + ((state->phase >> 8U) % 200U));
        out->val = (uint16_t)(700U + ((state->phase >> 16U) % 300U));
        break;

    case SEQ_KIND_POLICE:
        state->phase += (uint32_t)(((uint64_t)elapsed_ms * state->speed) / 1000U);
        index = (uint16_t)((state->phase / 500U) % 2U);
        out->hue = (index != 0U) ? 240U : 0U;
        out->sat = 1000U;
        out->val = state->base_val;
        break;

    case SEQ_KIND_SUNRISE:
        state->phase += (uint32_t)(((uint64_t)elapsed_ms * state->speed) / 1000U);
        phase = (state->phase > 60000U) ? 60000U : state->phase;
        out->val = (uint16_t)(((uint64_t)phase * 1000U) / 60000U);
        out->hue = 30U;
        out->sat = 800U;
        break;

    case SEQ_KIND_PAIRING:
        /* White pulse of ~2 s for pairing mode. */
        inc = (uint32_t)(((uint64_t)elapsed_ms * state->speed * 2000U) /
                         (2000U * 1000U));
        phase = advance_phase(state, inc, 2000U);
        triangle = (phase <= 1000U) ? phase : (2000U - phase);
        out->hue = 0U;
        out->sat = 0U;
        out->val = (uint16_t)triangle;
        out->brightness = 1000U;
        out->cct = 4000U;
        break;

    case SEQ_KIND_CUSTOM: {
        uint32_t duration;
        uint32_t scaled;
        uint32_t transition;
        uint32_t progress;
        uint16_t previous;
        const sequence_step_t *step;

        if (state->step_count == 0U) {
            state->running = false;
            return false;
        }

        scaled = (uint32_t)(((uint64_t)elapsed_ms * state->speed) / 1000U);
        state->step_elapsed_ms += scaled;

        for (;;) {
            step = &state->steps[state->step_index];
            duration = step_duration_ms(step);
            if (state->step_elapsed_ms < duration) {
                break;
            }
            state->step_elapsed_ms -= duration;
            if (!advance_custom_step(state)) {
                state->running = false;
                return false;
            }
        }

        step = &state->steps[state->step_index];
        out->hue = step->hue;
        out->sat = step->sat;
        out->val = step->val;
        out->brightness = step->brightness;
        out->cct = step->cct;
        out->step = state->step_index;

        transition = (uint32_t)step->transition_10ms * 10U;
        if ((transition > 0U) && (state->step_elapsed_ms < transition)) {
            progress = (state->step_elapsed_ms * 1000U) / transition;
            previous = previous_step_index(state);
            out->hue = lerp_hue(state->steps[previous].hue, step->hue,
                                progress);
            out->sat = lerp_u16(state->steps[previous].sat, step->sat,
                                progress);
            out->val = lerp_u16(state->steps[previous].val, step->val,
                                progress);
            out->brightness = lerp_u16(state->steps[previous].brightness,
                                       step->brightness, progress);
            out->cct = lerp_u16(state->steps[previous].cct, step->cct,
                                progress);
        }
        break;
    }

    default:
        state->running = false;
        return false;
    }

    return true;
}
