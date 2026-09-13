/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef C62_AUDIO_RESAMPLER_H
#define C62_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define C62_RESAMPLER_HISTORY 97
#define C62_RESAMPLER_MAX_OUTPUT 3

struct c62_resampler {
    const float *taps;
    float history[C62_RESAMPLER_HISTORY];
    unsigned int length;
    unsigned int head;
    unsigned int up;
    unsigned int down;
    unsigned int phase;
    uint32_t clipped;
};

/**
 * \brief Initialise a fixed-ratio converter to or from the 16 kHz DSP path.
 *
 * @param state: converter state, including history and clipping counter.
 * @param input_rate: input rate (8000, 16000, 24000 or 48000 Hz).
 * @param output_rate: output rate; one of the two rates must be 16000 Hz.
 * @return true for a supported conversion, false otherwise.
 */
bool c62_resampler_init(struct c62_resampler *state, uint32_t input_rate,
                        uint32_t output_rate);

/**
 * \brief Consume one sample, preserving filter history and rational phase.
 *
 * @param state: initialised converter; reset only at stream boundaries.
 * @param input: signed PCM sample.
 * @param output: storage for at least C62_RESAMPLER_MAX_OUTPUT samples.
 * @return number of output samples (zero to three).
 */
size_t c62_resampler_push(struct c62_resampler *state, int16_t input,
                          int16_t *output);

#ifdef __cplusplus
}
#endif

#endif
