#ifndef SEQUENCE_H
#define SEQUENCE_H

#include <stdbool.h>
#include <stdint.h>

#define SEQ_MAX_STEPS 16U

typedef enum {
    SEQ_KIND_NONE = 0,
    SEQ_KIND_BREATHING,
    SEQ_KIND_RAINBOW,
    SEQ_KIND_STROBE,
    SEQ_KIND_CANDLE,
    SEQ_KIND_POLICE,
    SEQ_KIND_SUNRISE,
    SEQ_KIND_CUSTOM,
    SEQ_KIND_PAIRING
} sequence_kind_t;

/* A step occupies 8 Modbus registers, in this order:
 * hue, sat, val, reserved, brightness, duration_10ms, transition_10ms, cct.
 * The CRC is computed over those 8 values per step (low byte and high byte). */
typedef struct {
    uint16_t hue;          /* 0-360 */
    uint16_t sat;          /* 0-1000 */
    uint16_t val;          /* 0-1000 */
    uint16_t reserved;     /* 0 (map alignment) */
    uint16_t brightness;   /* 0-1000 */
    uint16_t duration_10ms;
    uint16_t transition_10ms;
    uint16_t cct;          /* 2000-6500 K */
} sequence_step_t;

typedef struct {
    uint16_t hue;         /* 0-360 */
    uint16_t sat;         /* 0-1000 */
    uint16_t val;         /* 0-1000 */
    uint16_t brightness;  /* 0-1000 */
    uint16_t cct;         /* K */
    uint16_t step;        /* sequence step */
} sequence_output_t;

typedef struct {
    bool running;
    sequence_kind_t kind;
    uint16_t seq_id;
    uint16_t speed;       /* 1000 = 1x */
    uint16_t direction;   /* 0=forward, 1=reverse, 2=ping-pong */
    uint16_t loop_count;  /* reserved (0=infinite) */
    uint32_t phase;
    uint32_t elapsed_ms;
    int8_t dir;           /* +1 / -1 for ping-pong */
    uint16_t base_hue;
    uint16_t base_sat;
    uint16_t base_val;
    uint16_t base_brightness;
    uint16_t base_cct;
    /* Custom sequences (IDs 100-255). */
    sequence_step_t steps[SEQ_MAX_STEPS];
    uint16_t step_count;
    uint16_t step_index;
    uint32_t step_elapsed_ms;
    uint16_t loops_done;
} sequence_state_t;

void sequence_init(sequence_state_t *state);

/**
 * @brief Starts a predefined sequence (IDs 1-6).
 *
 * @return true if the ID is valid and the sequence started; false otherwise.
 */
bool sequence_start(sequence_state_t *state, uint16_t seq_id, uint16_t speed,
                    uint16_t direction, uint16_t loop_count,
                    uint16_t base_hue, uint16_t base_sat, uint16_t base_val,
                    uint16_t base_brightness, uint16_t base_cct);

/**
 * @brief Starts a custom sequence with already validated steps.
 *
 * @param state Sequence state.
 * @param steps Array of steps (copied into the state).
 * @param count Number of steps, 1..SEQ_MAX_STEPS.
 * @param speed Speed (1000 = 1x).
 * @param direction 0=forward, 1=reverse, 2=ping-pong.
 * @param loop_count 0=infinite.
 * @return true if it started.
 */
bool sequence_start_custom(sequence_state_t *state,
                           const sequence_step_t *steps, uint16_t count,
                           uint16_t speed, uint16_t direction,
                           uint16_t loop_count);

/**
 * @brief CRC-16/CCITT-FALSE over the custom steps.
 *
 * Computed over the 8 registers of each step, low byte then high byte.
 * It must match the CRC written by the backend to register 24.
 *
 * @param steps Steps to validate.
 * @param count Number of steps.
 * @return CRC16 (initial 0xFFFF, polynomial 0x1021).
 */
uint16_t sequence_crc16(const sequence_step_t *steps, uint16_t count);

void sequence_stop(sequence_state_t *state);

/**
 * @brief Advances the sequence.
 *
 * @return true if still active and producing output; false if stopped.
 */
bool sequence_tick(sequence_state_t *state, uint32_t elapsed_ms,
                   sequence_output_t *out);

#endif /* SEQUENCE_H */
