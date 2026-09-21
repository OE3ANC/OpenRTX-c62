/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_RX_BASEBAND_H
#define C62_RX_BASEBAND_H

#include <stdint.h>

struct c62_rx_baseband {
    float accumulator;
};

/**
 * \brief Reset the 24 kHz RX compensation at stream start.
 *
 * @param state: filter state, preserved across audio buffer boundaries.
 */
static inline void c62_rx_baseband_reset(struct c62_rx_baseband *state)
{
    state->accumulator = 0.0f;
}

/**
 * \brief Apply low-frequency compensation to 24 kHz RX PCM.
 *
 * A leaky integrator adds low-frequency content to the original signal.
 * The 0.999 pole limits DC gain; quarter-scale output provides headroom.
 * Applied after RX resampling and before equalization, with fixed boost=30.
 *
 * @param state: initialized filter state.
 * @param sample: resampled signed PCM.
 * @return signed PCM, saturated before conversion to int16_t.
 */
static inline int16_t c62_rx_baseband_sample(struct c62_rx_baseband *state,
                                             int16_t sample)
{
    state->accumulator = 0.999f * state->accumulator + sample;
    float output = 0.25f * (sample + 0.030f * state->accumulator);
    if (output > INT16_MAX)
        return INT16_MAX;
    if (output < INT16_MIN)
        return INT16_MIN;
    return (int16_t)output;
}

#endif
