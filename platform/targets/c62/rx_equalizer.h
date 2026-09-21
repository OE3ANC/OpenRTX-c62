/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_RX_EQUALIZER_H
#define C62_RX_EQUALIZER_H

#include <stdint.h>
#include <string.h>

#define C62_RX_EQUALIZER_TAPS 81

/*
 * 81-tap 24 kHz RX equalizer, applied after resampling and LF boost=30.
 * Keep this stage separate so its response and saturation are explicit.
 *
 * Ridge fit to the CRC-valid AF1 LSF (modem CRC cfadae31), samples 100..849
 * at offset 52278, with a 0.01 regularization factor. Causal tap order;
 * centered training adds 40 output samples of delay. Verified with profile
 * 4 captures; occasional decode errors remain. Do not normalize these taps.
 */
static const float c62_rx_equalizer_taps[C62_RX_EQUALIZER_TAPS] = {
    -5.7971640124e-02f, 1.9275920687e-02f,  1.5990877395e-02f,
    -6.7964430793e-03f, -1.7486956902e-02f, -1.4055680826e-02f,
    -7.7003850764e-03f, -6.1599560885e-03f, -8.1009967915e-03f,
    -8.2988628883e-03f, -5.3060268367e-03f, -3.2501339735e-03f,
    -6.4170537328e-03f, -1.3236981146e-02f, -1.7283566691e-02f,
    -1.4287692488e-02f, -6.6310210650e-03f, -5.2769673885e-04f,
    -5.7814257753e-04f, -7.5013361061e-03f, -1.8188457054e-02f,
    -2.5732543086e-02f, -2.2267307448e-02f, -6.5532835738e-03f,
    1.0958327242e-02f,  1.4694661225e-02f,  -1.4150934028e-03f,
    -2.6049217505e-02f, -3.8609805382e-02f, -2.6471981051e-02f,
    3.3712170108e-03f,  2.7545718845e-02f,  2.2585920884e-02f,
    -1.6005871820e-02f, -6.5063268305e-02f, -8.3944503669e-02f,
    -4.0097282002e-02f, 6.5977199097e-02f,  1.9469325554e-01f,
    2.8690236062e-01f,  2.9772691733e-01f,  2.2289737155e-01f,
    1.0058412046e-01f,  -1.1795402023e-02f, -7.0279081503e-02f,
    -6.6316995231e-02f, -2.5768690761e-02f, 1.2280471407e-02f,
    2.1552540379e-02f,  2.1894849921e-03f,  -2.5066594514e-02f,
    -3.6920996151e-02f, -2.4094816280e-02f, 3.5590249115e-03f,
    2.5395502034e-02f,  2.5977875328e-02f,  6.8206531769e-03f,
    -1.6151894431e-02f, -2.6735751539e-02f, -2.0284782316e-02f,
    -4.0735899629e-03f, 9.9275790172e-03f,  1.2392057733e-02f,
    1.9225674252e-03f,  -1.3120430393e-02f, -2.0180272307e-02f,
    -1.3417698340e-02f, 7.9193708487e-04f,  1.0283221608e-02f,
    8.2863994287e-03f,  -2.0172006809e-03f, -1.1724187044e-02f,
    -1.3509470483e-02f, -6.5992446920e-03f, 3.0322208093e-03f,
    7.4522423185e-03f,  3.2663824853e-03f,  -5.5823643373e-03f,
    -1.0842450405e-02f, -5.9857751013e-03f, 8.8971995330e-03f,
};

struct c62_rx_equalizer {
    float history[2 * C62_RX_EQUALIZER_TAPS];
    unsigned int head;
};

/**
 * \brief Clear the C62 experimental equalizer at RX stream start.
 *
 * @param state: history retained across audio buffer boundaries.
 */
static inline void c62_rx_equalizer_reset(struct c62_rx_equalizer *state)
{
    memset(state, 0, sizeof(*state));
}

/**
 * \brief Filter one 24 kHz RX sample after LF boost=30.
 *
 * @param state: initialized equalizer state.
 * @param sample: compensated signed PCM.
 * @return equalized PCM, saturated to prevent signed wraparound.
 */
static inline int16_t c62_rx_equalizer_sample(struct c62_rx_equalizer *state,
                                              int16_t sample)
{
    state->head = state->head == 0 ? C62_RX_EQUALIZER_TAPS - 1 :
                                     state->head - 1;
    state->history[state->head] = sample;
    state->history[state->head + C62_RX_EQUALIZER_TAPS] = sample;
    float output = 0.0f;
    for (unsigned int i = 0; i < C62_RX_EQUALIZER_TAPS; i++)
        output += state->history[state->head + i] * c62_rx_equalizer_taps[i];
    if (output > INT16_MAX)
        return INT16_MAX;
    if (output < INT16_MIN)
        return INT16_MIN;
    return (int16_t)output;
}

#endif
